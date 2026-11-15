// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline tiling — automatic window layout, per monitor.
#include "tiling.h"
#include "app.h"
#include "bar.h"
#include "dock.h"
#include <shellscalingapi.h>

namespace {

constexpr UINT_PTR kTimerTile = 0x7110;

enum Hotkey {
    HkToggle = 0x5100, HkFocusL, HkFocusR, HkFocusU, HkFocusD, HkSwapL, HkSwapR, HkSwapU, HkSwapD,
    HkFloat, HkLayout, HkRetile
};

bool Cloaked(HWND h) {
    BOOL c = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof c);
    return c != FALSE;
}

RECT FrameBounds(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    return r;
}

bool Near(const RECT& a, const RECT& b) {
    return abs(a.left - b.left) <= 2 && abs(a.top - b.top) <= 2 && abs(a.right - b.right) <= 2 && abs(a.bottom - b.bottom) <= 2;
}

POINT Center(const RECT& r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }

HWND g_moving = nullptr;   // window the user is dragging/resizing right now

} // namespace

// ---------------------------------------------------------------------------
void Tiler::Start() {
    if (running_) return;
    running_ = true;
    auto hook = [&](DWORD a, DWORD b) {
        HWINEVENTHOOK h = SetWinEventHook(a, b, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
    };
    hook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);          // destroy, show, hide
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    // LOCATIONCHANGE is hooked per process (only apps we tile) — a global hook
    // would wake us for every mouse move in Windows. See SyncLocationHooks().
    known_.clear();   // everything open right now joins the layout (maximized ones are restored)
    Retile();
}

void Tiler::Stop() {
    if (!running_) return;
    running_ = false;
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    for (auto& [pid, h] : locHooks_) UnhookWinEvent(h);
    locHooks_.clear();
    tries_.clear();
    KillTimer(g_app.ctlWnd, kTimerTile);
    order_.clear();
    placed_.clear();
    targets_.clear();
    // Windows stay where they are.
}

void Tiler::ApplySettings() {
    bool want = g_app.settings.tilingMode >= 1;
    if (want != hotkeys_) {
        if (want) RegisterHotkeys();
        else UnregisterHotkeys();
    }
    if (g_app.settings.tilingEnabled) { Start(); Retile(); }
    else Stop();
}

void Tiler::Schedule(UINT ms) {
    if (running_ && g_app.ctlWnd) SetTimer(g_app.ctlWnd, kTimerTile, ms, nullptr);
}

void Tiler::OnTimer() {
    KillTimer(g_app.ctlWnd, kTimerTile);
    Retile();
}

// ---------------------------------------------------------------------------
void CALLBACK Tiler::OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    Tiler* t = g_app.tiler.get();
    if (!t || !t->running_ || obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    bool automatic = g_app.settings.tilingMode != 2;

    switch (ev) {
    case EVENT_SYSTEM_MOVESIZESTART:
        g_moving = h;
        return;
    case EVENT_SYSTEM_MOVESIZEEND: {
        g_moving = nullptr;
        POINT pt;
        GetCursorPos(&pt);
        if (std::find(t->order_.begin(), t->order_.end(), h) != t->order_.end()) t->SwapAt(h, pt);
        if (automatic) t->Schedule(30);
        return;
    }
    case EVENT_OBJECT_LOCATIONCHANGE:
        // Only care about our own windows being maximized/restored by the user.
        if (g_moving || !automatic) return;
        if (std::find(t->order_.begin(), t->order_.end(), h) != t->order_.end() || (t->known_.count(h) && !IsZoomed(h)))
            t->Schedule(120);
        return;
    default: {
        if (!automatic) return;
        bool ours = std::find(t->order_.begin(), t->order_.end(), h) != t->order_.end();
        // Only windows that could join the layout, or ones already in it, matter.
        if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_UNCLOAKED || ev == EVENT_SYSTEM_MINIMIZEEND) {
            if (GetAncestor(h, GA_ROOT) != h) return;
            if (!ours && !t->Manageable(h)) return;
        } else if (!ours) {
            return;   // (a closing window is often gone already, so no questions asked about it: ours is enough)
        }
        t->Schedule(ev == EVENT_OBJECT_SHOW ? 120 : 60);
        return;
    }
    }
}

// ---------------------------------------------------------------------------
bool Tiler::Manageable(HWND h) const {
    if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return false;
    if (floating_.count(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;

    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return false;
    if ((ex & WS_EX_NOACTIVATE) || (ex & WS_EX_TOPMOST)) return false;
    if (GetWindow(h, GW_OWNER) && !appWindow) return false;     // dialogs, popups
    if (!(style & WS_THICKFRAME)) return false;                 // not resizable: leave it alone (most games)
    if (GetWindowTextLengthW(h) == 0) return false;

    wchar_t cls[96] = {};
    GetClassNameW(h, cls, 96);
    static const wchar_t* skipClasses[] = {L"#32770", L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                           L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
                                           L"TopLevelWindowForOverflowXamlIsland", L"ForegroundStaging"};
    for (auto c : skipClasses) if (!lstrcmpW(cls, c)) return false;

    auto cached = exeCache_.find(h);
    if (cached == exeCache_.end()) cached = exeCache_.emplace(h, ToLower(FileNameOf(ProcessImagePath(pid)))).first;
    const std::wstring& exe = cached->second;
    static const wchar_t* skipExe[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"shellexperiencehost.exe",
                                       L"shellhost.exe", L"lockapp.exe", L"textinputhost.exe"};
    for (auto e : skipExe) if (exe == e) return false;
    for (auto& f : g_app.settings.tilingFloat) if (exe == f) return false;

    // Fullscreen (games, videos). A window we put there ourselves (no gaps) doesn't count.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r;
    GetWindowRect(h, &r);
    // Only without a title bar: a browser that opens exactly as big as the screen is still a normal window to tile.
    // A window we tiled that has since gone fullscreen itself (F11, a video) is left alone too: no pulling it back.
    auto placedHere = targets_.find(h);
    bool inOurSlot = placedHere != targets_.end() && Near(FrameBounds(h), placedHere->second);
    if (!IsZoomed(h) && !inOurSlot && (style & WS_CAPTION) != WS_CAPTION && r.left <= mi.rcMonitor.left &&
        r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom)
        return false;

    // Maximized by the user after we started tiling it: respect that.
    if (IsZoomed(h) && known_.count(h)) return false;
    return true;
}

RECT Tiler::WorkArea(HMONITOR mon) const {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT a = mi.rcWork;   // normally already excludes the Skyline top bar …
    // … but don't count on it: the strip the top bar takes is always kept free, so title bars
    // (with minimize/maximize/close) never end up under it (seen with the classic style).
    for (auto& b : g_app.bars)
        if (b->Monitor() == mon) a.top = std::max<LONG>(a.top, b->ReservedBottom());
    Dock* dock = g_app.DockOn(mon);
    if (g_app.settings.tilingDockSpace && dock) {
        int top = dock->TopEdge(mon);
        if (top > a.top && top < a.bottom) a.bottom = top;
    }
    return a;
}

std::vector<RECT> Tiler::Layout(int n, const RECT& area) const {
    std::vector<RECT> out;
    if (n <= 0) return out;
    UINT dpi = 96;
    {
        HMONITOR m = MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST);
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) dpi = dx;
    }
    int g = MulDiv(g_app.settings.tilingGap, dpi, 96);
    int h2 = g / 2;
    RECT r{area.left + g, area.top + g, area.right - g, area.bottom - g};

    auto splitH = [&](RECT& src, float frac) {   // left part out, src becomes right
        int mid = src.left + (int)((src.right - src.left) * frac);
        RECT a{src.left, src.top, mid - h2, src.bottom};
        src.left = mid + (g - h2);
        return a;
    };
    auto splitV = [&](RECT& src, float frac) {   // top part out, src becomes bottom
        int mid = src.top + (int)((src.bottom - src.top) * frac);
        RECT a{src.left, src.top, src.right, mid - h2};
        src.top = mid + (g - h2);
        return a;
    };

    switch (g_app.settings.tilingLayout) {
    case 0: {   // BSP / dwindle: each new window splits the last one along its long side
        RECT cur = r;
        for (int i = 0; i < n; ++i) {
            if (i == n - 1) { out.push_back(cur); break; }
            bool wide = (cur.right - cur.left) >= (cur.bottom - cur.top);
            out.push_back(wide ? splitH(cur, 0.5f) : splitV(cur, 0.5f));
        }
        break;
    }
    case 1: {   // Master: one big window left, the rest stacked on the right
        if (n == 1) { out.push_back(r); break; }
        RECT rest = r;
        out.push_back(splitH(rest, 0.55f));
        for (int i = 1; i < n; ++i) {
            if (i == n - 1) { out.push_back(rest); break; }
            out.push_back(splitV(rest, 1.0f / (n - i)));
        }
        break;
    }
    case 2: {   // Columns
        RECT rest = r;
        for (int i = 0; i < n; ++i) {
            if (i == n - 1) { out.push_back(rest); break; }
            out.push_back(splitH(rest, 1.0f / (n - i)));
        }
        break;
    }
    default:    // Monocle: every window full size, stacked
        for (int i = 0; i < n; ++i) out.push_back(r);
        break;
    }
    return out;
}

void Tiler::Place(HWND h, const RECT& target) {
    if (IsZoomed(h)) {
        // newly opened maximized windows join the layout (async: a busy app mustn't freeze Skyline); placed on the next pass
        ShowWindowAsync(h, SW_RESTORE);
        Schedule(150);
        return;
    }
    // Already put in this slot: leave it. (Some apps refuse to get smaller than
    // their minimum size; re-placing them forever would make windows jitter.)
    RECT fb = FrameBounds(h);
    auto it = placed_.find(h);
    if (it != placed_.end() && Near(it->second, target)) {
        // Really there (or the app refuses after a couple of tries): leave it.
        if (Near(fb, target)) { tries_[h] = 0; return; }   // there: start counting again if it's ever moved
        if (tries_[h] >= 2) return;
        ++tries_[h];
    } else {
        tries_[h] = 0;
    }

    // Windows 10/11 frames have invisible resize borders; compensate so the
    // visible edges land exactly on the target.
    RECT wr;
    GetWindowRect(h, &wr);
    int l = fb.left - wr.left, t = fb.top - wr.top, r = wr.right - fb.right, b = wr.bottom - fb.bottom;
    SetWindowPos(h, nullptr, target.left - l, target.top - t, (target.right - target.left) + l + r,
                 (target.bottom - target.top) + t + b,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    placed_[h] = target;
}

void Tiler::Retile() {
    if (g_moving && !IsWindow(g_moving)) g_moving = nullptr;   // closed mid-drag: its "move ended" never came
    if (!running_ || retiling_ || g_moving) return;
    retiling_ = true;

    // Current tileable windows (z-order)
    struct Ctx { const Tiler* self; std::vector<HWND> wins; } ctx{this, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (c->self->Manageable(h)) c->wins.push_back(h);
        return TRUE;
    }, (LPARAM)&ctx);
    std::vector<HWND>& now = ctx.wins;

    // A window that was maximized and is now restored must be put back in its slot
    for (HWND h : known_) if (IsWindow(h) && IsZoomed(h)) wasZoomed_.insert(h);
    for (HWND h : now)
        if (wasZoomed_.count(h) && !IsZoomed(h)) { placed_.erase(h); wasZoomed_.erase(h); }
    for (auto it = wasZoomed_.begin(); it != wasZoomed_.end();) it = IsWindow(*it) ? std::next(it) : wasZoomed_.erase(it);

    // Keep existing order, drop vanished windows, append new ones (oldest first)
    std::vector<HWND> next;
    for (HWND h : order_) if (std::find(now.begin(), now.end(), h) != now.end()) next.push_back(h);
    for (auto it = now.rbegin(); it != now.rend(); ++it)
        if (std::find(next.begin(), next.end(), *it) == next.end()) next.push_back(*it);
    order_ = next;

    for (auto it = placed_.begin(); it != placed_.end();) it = IsWindow(it->first) ? std::next(it) : placed_.erase(it);
    for (auto it = floating_.begin(); it != floating_.end();) it = IsWindow(*it) ? std::next(it) : floating_.erase(it);
    for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
    for (auto it = exeCache_.begin(); it != exeCache_.end();) it = IsWindow(it->first) ? std::next(it) : exeCache_.erase(it);
    for (auto it = tries_.begin(); it != tries_.end();) it = IsWindow(it->first) ? std::next(it) : tries_.erase(it);
    targets_.clear();

    // Group per monitor
    std::map<HMONITOR, std::vector<HWND>> perMon;
    for (HWND h : order_) perMon[MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)].push_back(h);
    for (auto& [mon, wins] : perMon) {
        auto rects = Layout((int)wins.size(), WorkArea(mon));
        for (size_t i = 0; i < wins.size() && i < rects.size(); ++i) {
            Place(wins[i], rects[i]);
            targets_[wins[i]] = rects[i];
            known_.insert(wins[i]);
        }
    }
    SyncLocationHooks();
    retiling_ = false;
    // Windows that were just moved/restored must never end up over the top bar or the dock
    if (!order_.empty()) KeepBarsOnTop();
    // A window that opened maximized (browsers like Vivaldi and Brave) can look like a fullscreen app for a
    // moment, which hides the top bar. Now that it has its tile, look again so the bar comes back.
    if (!order_.empty()) RecheckFullscreen();
}

void Tiler::SyncLocationHooks() {
    std::set<DWORD> want;
    for (HWND h : known_) {
        DWORD pid = 0;
        if (IsWindow(h) && GetWindowThreadProcessId(h, &pid) && pid) want.insert(pid);
    }
    for (auto it = locHooks_.begin(); it != locHooks_.end();) {
        if (want.count(it->first)) { ++it; continue; }
        UnhookWinEvent(it->second);
        it = locHooks_.erase(it);
    }
    for (DWORD pid : want) {
        if (locHooks_.count(pid)) continue;
        HWINEVENTHOOK h = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnEvent,
                                          pid, 0, WINEVENT_OUTOFCONTEXT);
        if (h) locHooks_[pid] = h;
    }
}

// Dragging a tiled window onto another one swaps them.
void Tiler::SwapAt(HWND moved, POINT pt) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    auto from = std::find(order_.begin(), order_.end(), moved);
    if (from == order_.end()) return;
    for (auto& [h, r] : targets_) {
        if (h == moved || !PtInRect(&r, pt) || MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != mon) continue;
        auto to = std::find(order_.begin(), order_.end(), h);
        if (to != order_.end()) std::iter_swap(from, to);
        break;
    }
    placed_.erase(moved);   // force it back into a slot even if the user only nudged it
}

void Tiler::ToggleFloat(HWND h) {
    if (!h) return;
    if (floating_.count(h)) floating_.erase(h);
    else {
        floating_.insert(h);
        order_.erase(std::remove(order_.begin(), order_.end(), h), order_.end());
        placed_.erase(h);
    }
    Schedule(10);
}

// ---------------------------------------------------------------------------
// Hotkeys (only when the user chose them)
void Tiler::RegisterHotkeys() {
    HWND w = g_app.ctlWnd;
    const UINT ca = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, cas = ca | MOD_SHIFT;
    RegisterHotKey(w, HkToggle, ca, 'T');
    RegisterHotKey(w, HkFocusL, ca, VK_LEFT);
    RegisterHotKey(w, HkFocusR, ca, VK_RIGHT);
    RegisterHotKey(w, HkFocusU, ca, VK_UP);
    RegisterHotKey(w, HkFocusD, ca, VK_DOWN);
    RegisterHotKey(w, HkSwapL, cas, VK_LEFT);
    RegisterHotKey(w, HkSwapR, cas, VK_RIGHT);
    RegisterHotKey(w, HkSwapU, cas, VK_UP);
    RegisterHotKey(w, HkSwapD, cas, VK_DOWN);
    RegisterHotKey(w, HkFloat, ca, 'F');
    RegisterHotKey(w, HkLayout, ca, 'L');
    RegisterHotKey(w, HkRetile, ca, VK_SPACE);
    hotkeys_ = true;
}

void Tiler::UnregisterHotkeys() {
    for (int id = HkToggle; id <= HkRetile; ++id) UnregisterHotKey(g_app.ctlWnd, id);
    hotkeys_ = false;
}

HWND Tiler::Neighbor(HWND from, int dx, int dy) const {
    auto it = targets_.find(from);
    if (it == targets_.end()) return nullptr;
    POINT c = Center(it->second);
    HWND best = nullptr;
    long bestScore = LONG_MAX;
    for (auto& [h, r] : targets_) {
        if (h == from) continue;
        POINT o = Center(r);
        long ax = o.x - c.x, ay = o.y - c.y;
        if ((dx && ax * dx <= 0) || (dy && ay * dy <= 0)) continue;
        long score = dx ? labs(ax) + 2 * labs(ay) : labs(ay) + 2 * labs(ax);
        if (score < bestScore) { bestScore = score; best = h; }
    }
    return best;
}

void Tiler::OnHotkey(int id) {
    if (id == HkToggle) {
        g_app.settings.tilingEnabled = !g_app.settings.tilingEnabled;
        IniWrite(L"Tiling", L"Enabled", g_app.settings.tilingEnabled ? 1 : 0);
        ApplySettings();
        g_app.InvalidateAll();
        return;
    }
    if (!running_) return;
    HWND fg = GetForegroundWindow();
    int dx = 0, dy = 0;
    switch (id) {
    case HkFocusL: case HkSwapL: dx = -1; break;
    case HkFocusR: case HkSwapR: dx = 1; break;
    case HkFocusU: case HkSwapU: dy = -1; break;
    case HkFocusD: case HkSwapD: dy = 1; break;
    }
    switch (id) {
    case HkFocusL: case HkFocusR: case HkFocusU: case HkFocusD:
        if (HWND n = Neighbor(fg, dx, dy)) SetForegroundWindow(n);
        break;
    case HkSwapL: case HkSwapR: case HkSwapU: case HkSwapD:
        if (HWND n = Neighbor(fg, dx, dy)) {
            auto a = std::find(order_.begin(), order_.end(), fg), b = std::find(order_.begin(), order_.end(), n);
            if (a != order_.end() && b != order_.end()) { std::iter_swap(a, b); placed_.clear(); Retile(); }
        }
        break;
    case HkFloat:
        ToggleFloat(fg);
        break;
    case HkLayout:
        g_app.settings.tilingLayout = (g_app.settings.tilingLayout + 1) % 4;
        IniWrite(L"Tiling", L"Layout", g_app.settings.tilingLayout);
        placed_.clear();
        Retile();
        break;
    case HkRetile:
        placed_.clear();
        Retile();
        break;
    }
}
