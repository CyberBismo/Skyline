// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline tiling — automatic window layout, per monitor.
#include "tiling.h"
#include "version.h"
#include "app.h"
#include "bar.h"
#include "dock.h"
#include <shellscalingapi.h>

namespace {

constexpr UINT_PTR kTimerTile = 0x7110;

enum Hotkey {
    HkToggle = 0x5100, HkFocusL, HkFocusR, HkFocusU, HkFocusD, HkSwapL, HkSwapR, HkSwapU, HkSwapD,
    HkFloat, HkLayout, HkRetile, HkDebug
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
    seen_.clear();
    ratios_.clear();
    memIndex_.clear();
    splits_.clear();
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
    if (GameMode()) { t->pendingRetile_ = true; return; }
    bool automatic = g_app.settings.tilingMode != 2;

    switch (ev) {
    case EVENT_SYSTEM_MOVESIZESTART:
        g_moving = h;
        return;
    case EVENT_SYSTEM_MOVESIZEEND: {
        g_moving = nullptr;
        POINT pt;
        GetCursorPos(&pt);
        if (std::find(t->order_.begin(), t->order_.end(), h) != t->order_.end()) {
            // Dragged an edge (the size changed): the neighbours give way. Dragged the whole window: it swaps places.
            RECT fb = FrameBounds(h);
            auto tg = t->targets_.find(h);
            bool resized = tg != t->targets_.end() &&
                           (std::abs((fb.right - fb.left) - (tg->second.right - tg->second.left)) > 6 ||
                            std::abs((fb.bottom - fb.top) - (tg->second.bottom - tg->second.top)) > 6);
            if (resized) {
                t->ApplyResize(h, fb);
                t->placed_.clear();
                t->Schedule(30);
                return;
            }
            t->SwapAt(h, pt);
        }
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
    RECT a = mi.rcWork;
    // The top comes from our own bar only. Windows' work area can still hold an old reservation at the top (left from
    // when the bar was not auto-hiding, or from another app); with auto-hide on that made tiled windows start far
    // down the screen with an empty strip above them.
    a.top = mi.rcMonitor.top;
    // The strip the top bar takes is kept free, so title bars (with minimize/maximize/close) never end up under it.
    for (auto& b : g_app.bars)
        if (b->Monitor() == mon) {
            int reserved = b->ReservedBottom();   // a screen y; 0 = nothing reserved (auto-hide). Never compare with 0 itself:
            if (reserved != 0) a.top = std::max<LONG>(a.top, reserved);   // a screen above the main one has a negative top
        }
    Dock* dock = g_app.DockOn(mon);
    if (g_app.settings.tilingDockSpace && dock) {
        int top = dock->TopEdge(mon);
        if (top > a.top && top < a.bottom) a.bottom = top;
    }
    return a;
}

std::vector<RECT> Tiler::Layout(int n, const RECT& area, Ratios* rat, std::vector<Split>* splits, const std::vector<HWND>* wins) const {
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

    // Fractions the user has dragged (0 = the default). They start over when the layout or the number of windows changes.
    int layoutNow = g_app.settings.tilingLayout;
    std::vector<float> none;
    std::vector<float>& fr = rat ? rat->fr : none;
    std::vector<HWND> key;
    if (wins) { key = *wins; std::sort(key.begin(), key.end()); }
    if (rat && (rat->layout != layoutNow || rat->key != key || (int)fr.size() != std::max(0, n - 1))) {
        // other windows or another layout: the sizes of the same group of windows (a window minimized and back) come back
        rat->layout = layoutNow; rat->key = key;
        fr.assign(std::max(0, n - 1), 0.0f);
        auto old = rat->saved.find({layoutNow, key});
        if (wins && old != rat->saved.end() && (int)old->second.size() == (int)fr.size()) fr = old->second;
    }
    auto frac = [&](int k, float dflt) { return (k >= 0 && k < (int)fr.size() && fr[k] > 0.05f) ? std::clamp(fr[k], 0.1f, 0.9f) : dflt; };

    auto splitH = [&](RECT& src, int k, float dflt) {   // left part out, src becomes right
        int mid = src.left + (int)((src.right - src.left) * frac(k, dflt));
        RECT a{src.left, src.top, mid - h2, src.bottom};
        if (splits) splits->push_back({k, true, src, mid, mid - h2, mid + (g - h2)});
        src.left = mid + (g - h2);
        return a;
    };
    auto splitV = [&](RECT& src, int k, float dflt) {   // top part out, src becomes bottom
        int mid = src.top + (int)((src.bottom - src.top) * frac(k, dflt));
        RECT a{src.left, src.top, src.right, mid - h2};
        if (splits) splits->push_back({k, false, src, mid, mid - h2, mid + (g - h2)});
        src.top = mid + (g - h2);
        return a;
    };

    switch (layoutNow) {
    case 0: {   // BSP / dwindle: each new window splits the last one along its long side
        RECT cur = r;
        for (int i = 0; i < n; ++i) {
            if (i == n - 1) { out.push_back(cur); break; }
            bool wide = (cur.right - cur.left) >= (cur.bottom - cur.top);
            out.push_back(wide ? splitH(cur, i, 0.5f) : splitV(cur, i, 0.5f));
        }
        break;
    }
    case 1: {   // Master: one big window left, the rest stacked on the right
        if (n == 1) { out.push_back(r); break; }
        RECT rest = r;
        out.push_back(splitH(rest, 0, 0.55f));
        for (int i = 1; i < n; ++i) {
            if (i == n - 1) { out.push_back(rest); break; }
            out.push_back(splitV(rest, i, 1.0f / (n - i)));
        }
        break;
    }
    case 2: {   // Columns
        RECT rest = r;
        for (int i = 0; i < n; ++i) {
            if (i == n - 1) { out.push_back(rest); break; }
            out.push_back(splitH(rest, i, 1.0f / (n - i)));
        }
        break;
    }
    default:    // Monocle: every window full size, stacked
        for (int i = 0; i < n; ++i) out.push_back(r);
        break;
    }
    if (rat && wins) {
        rat->saved[{layoutNow, key}] = fr;
        if (rat->saved.size() > 24) rat->saved.erase(rat->saved.begin());
    }
    return out;
}

// The user dragged an edge of a tiled window: find the divider that edge sits on and move it there.
bool Tiler::ApplyResize(HWND h, const RECT& now) {
    auto tg = targets_.find(h);
    if (tg == targets_.end()) return false;
    const RECT& T = tg->second;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    auto sp = splits_.find(mon);
    auto rt = ratios_.find(mon);
    if (sp == splits_.end() || rt == ratios_.end()) return false;
    auto isAt = [](LONG a, LONG b) { return std::labs(a - b) <= 2; };
    auto moved = [](LONG a, LONG b) { return std::labs(a - b) > 3; };
    bool any = false;
    for (const Split& s : sp->second) {
        if (s.k < 0 || s.k >= (int)rt->second.fr.size()) continue;
        if (T.left < s.parent.left - 2 || T.right > s.parent.right + 2 || T.top < s.parent.top - 2 || T.bottom > s.parent.bottom + 2) continue;
        LONG newMid = 0;
        bool hit = false;
        if (s.horiz) {
            if (isAt(T.right, s.firstEdge) && moved(now.right, T.right)) { newMid = now.right + (s.mid - s.firstEdge); hit = true; }
            else if (isAt(T.left, s.secondEdge) && moved(now.left, T.left)) { newMid = now.left - (s.secondEdge - s.mid); hit = true; }
        } else {
            if (isAt(T.bottom, s.firstEdge) && moved(now.bottom, T.bottom)) { newMid = now.bottom + (s.mid - s.firstEdge); hit = true; }
            else if (isAt(T.top, s.secondEdge) && moved(now.top, T.top)) { newMid = now.top - (s.secondEdge - s.mid); hit = true; }
        }
        if (!hit) continue;
        LONG lo = s.horiz ? s.parent.left : s.parent.top, hi = s.horiz ? s.parent.right : s.parent.bottom;
        if (hi <= lo) continue;
        rt->second.fr[s.k] = std::clamp((float)(newMid - lo) / (float)(hi - lo), 0.1f, 0.9f);
        any = true;
    }
    return any;
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
    {   // an app that moves or resizes itself (e.g. restoring its saved position a moment after opening) gets new tries
        auto seen = seen_.find(h);
        if (seen != seen_.end() && !Near(seen->second, fb)) tries_[h] = 0;
        seen_[h] = fb;
    }
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
    if (GameMode()) { pendingRetile_ = true; return; }   // a game in fullscreen: never move windows now (it would drop out of fullscreen)
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
    for (size_t i = 0; i < order_.size(); ++i) {
        HWND h = order_[i];
        if (std::find(now.begin(), now.end(), h) != now.end()) next.push_back(h);
        else if (IsWindow(h) && IsIconic(h)) memIndex_[h] = (int)i;   // minimized: it comes back to this place
    }
    for (auto it = now.rbegin(); it != now.rend(); ++it)
        if (std::find(next.begin(), next.end(), *it) == next.end()) {
            auto mem = memIndex_.find(*it);
            if (mem != memIndex_.end()) { next.insert(next.begin() + std::min<size_t>(mem->second, next.size()), *it); memIndex_.erase(mem); }
            else next.push_back(*it);
        }
    order_ = next;
    for (auto it = memIndex_.begin(); it != memIndex_.end();) it = IsWindow(it->first) ? std::next(it) : memIndex_.erase(it);

    for (auto it = placed_.begin(); it != placed_.end();) it = IsWindow(it->first) ? std::next(it) : placed_.erase(it);
    for (auto it = floating_.begin(); it != floating_.end();) it = IsWindow(*it) ? std::next(it) : floating_.erase(it);
    for (auto it = known_.begin(); it != known_.end();) it = IsWindow(*it) ? std::next(it) : known_.erase(it);
    for (auto it = exeCache_.begin(); it != exeCache_.end();) it = IsWindow(it->first) ? std::next(it) : exeCache_.erase(it);
    for (auto it = tries_.begin(); it != tries_.end();) it = IsWindow(it->first) ? std::next(it) : tries_.erase(it);
    for (auto it = seen_.begin(); it != seen_.end();) it = IsWindow(it->first) ? std::next(it) : seen_.erase(it);
    targets_.clear();

    // Group per monitor
    std::map<HMONITOR, std::vector<HWND>> perMon;
    for (HWND h : order_) perMon[MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST)].push_back(h);
    splits_.clear();
    for (auto& [mon, wins] : perMon) {
        auto rects = Layout((int)wins.size(), WorkArea(mon), &ratios_[mon], &splits_[mon], &wins);
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
    RegisterHotKey(w, HkDebug, ca, 'D');   // Ctrl+Alt+D: write the debug file
    hotkeys_ = true;
}

void Tiler::UnregisterHotkeys() {
    for (int id = HkToggle; id <= HkDebug; ++id) UnregisterHotKey(g_app.ctlWnd, id);
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
    if (id == HkDebug) { DebugDump(); return; }
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

// ---------------------------------------------------------------------------
// A report for finding bugs: Ctrl+Alt+D (or the button in Settings) writes skyline-debug.txt next to skyline.ini.
void Tiler::DebugDump() {
    std::wstring out = L"Skyline " SKYLINE_VERSION_W L"\r\n";
    wchar_t b[700];
    auto add = [&](const wchar_t* fmt, auto... a) { swprintf(b, 700, fmt, a...); out += b; };
    add(L"settings: tiling=%d layout=%d gap=%d dockSpace=%d barAutohide=%d mode=%d\r\n", (int)g_app.settings.tilingEnabled,
        g_app.settings.tilingLayout, g_app.settings.tilingGap, (int)g_app.settings.tilingDockSpace, (int)g_app.settings.barAutohide, g_app.settings.tilingMode);
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }, (LPARAM)&mons);
    int n = 0;
    for (HMONITOR m : mons) {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(m, &mi);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy);
        RECT wa = WorkArea(m);
        add(L"monitor %d%s: rcMonitor=(%ld,%ld,%ld,%ld) rcWork=(%ld,%ld,%ld,%ld) dpi=%u tilerArea=(%ld,%ld,%ld,%ld)\r\n", ++n,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" PRIMARY" : L"", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
            mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, dx, wa.left, wa.top, wa.right, wa.bottom);
        for (auto& bar : g_app.bars) if (bar->Monitor() == m) out += bar->DebugLine();
        if (Dock* d = g_app.DockOn(m)) add(L"  dock: topEdge=%d\r\n", d->TopEdge(m));
    }
    out += L"windows (visible, bigger than 200 px):\r\n";
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* self = (Tiler*)lp;
        RECT r;
        if (!IsWindowVisible(h) || !GetWindowRect(h, &r) || r.right - r.left < 200 || r.bottom - r.top < 100) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        wchar_t cls[64] = {}, t[64] = {}, line[700];
        GetClassNameW(h, cls, 64);
        GetWindowTextW(h, t, 64);
        RECT fb = FrameBounds(h);
        auto tg = self->targets_.find(h);
        swprintf(line, 700, L"  %ls | %ls | \"%ls\" rect=(%ld,%ld,%ld,%ld) frame=(%ld,%ld,%ld,%ld) style=%08lX ex=%08lX zoomed=%d manageable=%d floating=%d target=%ls\r\n",
                 FileNameOf(ProcessImagePath(pid)).c_str(), cls, t, r.left, r.top, r.right, r.bottom, fb.left, fb.top, fb.right, fb.bottom,
                 (unsigned long)GetWindowLongW(h, GWL_STYLE), (unsigned long)GetWindowLongW(h, GWL_EXSTYLE), (int)IsZoomed(h),
                 (int)self->Manageable(h), (int)self->floating_.count(h),
                 tg == self->targets_.end() ? L"-" : (std::to_wstring(tg->second.left) + L"," + std::to_wstring(tg->second.top) + L"," +
                                                     std::to_wstring(tg->second.right) + L"," + std::to_wstring(tg->second.bottom)).c_str());
        self->dbg_ += line;
        return TRUE;
    }, (LPARAM)this);
    out += dbg_;
    dbg_.clear();
    std::wstring path = g_app.settings.path;
    size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1)) + L"skyline-debug.txt";
    int len = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
    std::string u8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), u8.data(), len, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, u8.data(), (DWORD)u8.size(), &wr, nullptr); CloseHandle(f); }
    MessageBeep(MB_OK);
}
