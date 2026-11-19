// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Stage Manager — see stage.h
//
// How it works (all event driven, nothing runs while nothing happens):
//  * Per screen there is one "active" app. Its windows stay where they are.
//  * The windows of every other app on that screen are put away (minimised by us) after a picture of each has
//    been taken. They are remembered in g_stash, so only what WE put away is ever shown in the strip or given back.
//  * The strip (one small window per screen, drawn by Direct2D) shows one card per put-away app, newest first.
//    Click a card: that app comes forward and the app that was in front is put away.
//  * Switching with Alt+Tab or the dock works too: the foreground event does the same swap.
//  * Mode 0 (like a Mac): the strip stays at the left edge. It slides away when a window covers it.
//    Mode 1: hidden until the mouse touches the very left edge of the screen.
//  * A screen with a game (any window that covers the whole screen) is left completely alone.
#include "stage.h"
#include "app.h"
#include "icons.h"
#include "enhancedtiling.h"
#include <shellscalingapi.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace {

constexpr UINT_PTR T_REFRESH = 1;   // message window: look again a little later
constexpr UINT_PTR T_SLIDE = 2;     // strip: the slide in / out
constexpr UINT_PTR T_WATCH = 3;     // strip (hidden mode): is the mouse still near?

bool g_on = false;

struct StashInfo {
    HMONITOR mon = nullptr;
    std::wstring exe;
    ULONGLONG seq = 0;   // when it was put away (bigger = later)
    ULONGLONG tick = 0;   // when we asked the window to minimise (it gets a moment to do so)
    bool adopted = false; // already minimised by hand when Stage Manager started: it sits in the strip too, but is never un-minimised on its own
};
std::map<HWND, StashInfo> g_stash;                       // windows we minimised
std::map<HMONITOR, std::wstring> g_active;               // the app in front on each screen
std::map<HMONITOR, std::vector<std::wstring>> g_mru;
std::map<HMONITOR, std::vector<HWND>> g_seenFront;       // the windows on show last time (to notice that the last one was closed)     // apps on each screen, most recently used first
std::map<HWND, std::wstring> g_exeCache;
ULONGLONG g_seq = 1000000;   // (what adopted windows get is lower)
HWND g_msg = nullptr;
HWINEVENTHOOK g_hookDestroy = nullptr, g_hookMove = nullptr, g_hookMin = nullptr, g_hookCloak = nullptr, g_hookShow = nullptr;
std::map<HWND, ULONGLONG> g_noStash;   // windows that would not minimise (elevated or stubborn apps): left alone for a minute, then tried again
std::map<HWND, HMONITOR> g_lastMon;   // the screen each window was last seen on (a minimised window remembers where it was, not where Windows keeps its restore place)
std::set<HWND> g_young;   // windows of another app seen on show once (they are put away when seen a second time: gives a new window time to take the focus)
bool g_refreshPending = false;

std::wstring DirOfIni() {
    std::wstring dir = g_app.settings.path;
    size_t bs = dir.find_last_of(L"\\/");
    return bs == std::wstring::npos ? std::wstring() : dir.substr(0, bs + 1);
}

// A short trail of what Stage Manager did (stage-log.txt next to skyline.ini): to see why something went missing
void Log(const std::wstring& line) {
    static std::wstring path;
    static bool first = true;
    if (path.empty()) path = DirOfIni() + L"stage-log.txt";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, first ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    first = false;
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[32];
    swprintf(ts, 32, L"%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    std::wstring l = ts + line + L"\r\n";
    int len = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(), nullptr, 0, nullptr, nullptr);
    std::string u8(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(), u8.data(), len, nullptr, nullptr);
    DWORD w = 0;
    WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(f);
}
std::wstring Short(HWND h) {
    wchar_t t[60] = {};
    GetWindowTextW(h, t, 60);
    return L"\"" + std::wstring(t) + L"\"";
}

bool IsDesktopClass(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd");
}

// Apps the user wants Stage Manager to leave alone: [StageManager] Exclude=steam.exe;discord.exe in skyline.ini (file names, ; separated)
std::set<std::wstring> g_exclude;
void LoadExclude() {
    g_exclude.clear();
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"StageManager", L"Exclude", L"", buf, 2048, g_app.settings.path.c_str());
    std::wstring all = buf, cur;
    all += L';';
    for (wchar_t c : all) {
        if (c == L';' || c == L',') {
            size_t a = cur.find_first_not_of(L" \t"), b = cur.find_last_not_of(L" \t");
            if (a != std::wstring::npos) {
                std::wstring n = ToLower(cur.substr(a, b - a + 1));
                if (n.size() < 4 || n.compare(n.size() - 4, 4, L".exe") != 0) n += L".exe";
                g_exclude.insert(n);
            }
            cur.clear();
        } else cur += c;
    }
}

// The process a window really belongs to. A Store app's window is owned by ApplicationFrameHost; the app itself lives in the CoreWindow inside it.
// `resolved` = false when that CoreWindow cannot be found (yet): the answer may change later.
DWORD OwningPid(HWND h, bool* resolved = nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (resolved) *resolved = true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"ApplicationFrameWindow")) {
        HWND core = FindWindowExW(h, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        DWORD cp = 0;
        if (core) GetWindowThreadProcessId(core, &cp);
        if (cp) pid = cp;
        else if (resolved) *resolved = false;
    }
    return pid;
}

// A window Stage Manager may move around (same idea as Mission Control: a normal app window).
bool AppWin(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;   // (a minimised window is still "visible")
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return false;
    if ((ex & WS_EX_NOACTIVATE) && !appWindow) return false;
    if (ex & WS_EX_TOPMOST) return false;                           // always-on-top windows stay out of it
    if (GetWindow(h, GW_OWNER) && !appWindow) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return false;
    if (IsDesktopClass(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    pid = OwningPid(h);
    if (GetWindowTextLengthW(h) <= 0) return false;
    RECT r{};   // small windows (tool panes, popups) are not apps: they stay where they are
    if (IsIconic(h)) { WINDOWPLACEMENT wp{sizeof wp}; if (!GetWindowPlacement(h, &wp)) return false; r = wp.rcNormalPosition; }
    else if (!GetWindowRect(h, &r)) return false;
    if (r.right - r.left < 320 || r.bottom - r.top < 200) return false;
    std::wstring exe = ToLower(ProcessImagePath(pid));
    size_t bs = exe.find_last_of(L'\\');
    if (bs != std::wstring::npos) exe = exe.substr(bs + 1);   // Windows' own shell windows (Start, Search, input panels …)
    static const wchar_t* skip[] = {L"shellexperiencehost.exe", L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe", L"textinputhost.exe",
                                    L"lockapp.exe", L"widgets.exe", L"systemsettingsbroker.exe", L"applicationframehost.exe_"};
    for (const wchar_t* sname : skip) if (exe == sname) return false;
    if (g_exclude.count(exe)) return false;   // an app the user told us to leave alone
    auto ns = g_noStash.find(h);
    if (ns != g_noStash.end()) { if (GetTickCount64() < ns->second) return false; g_noStash.erase(ns); }
    return true;
}

std::set<HWND> g_exeUnresolved;   // Store app windows whose real app was not found yet (asked again next time)
const std::wstring& ExeOf(HWND h) {
    auto it = g_exeCache.find(h);
    if (it == g_exeCache.end()) {
        bool ok = true;
        DWORD pid = OwningPid(h, &ok);
        it = g_exeCache.emplace(h, ToLower(ProcessImagePath(pid))).first;
        if (!ok) g_exeUnresolved.insert(h);
    } else if (!g_exeUnresolved.empty() && g_exeUnresolved.count(h)) {
        bool ok = true;
        DWORD pid = OwningPid(h, &ok);
        if (ok) { it->second = ToLower(ProcessImagePath(pid)); g_exeUnresolved.erase(h); }
    }
    return it->second;
}

HMONITOR MonOf(HWND h) {
    auto s = g_stash.find(h);
    if (s != g_stash.end()) return s->second.mon;
    if (IsIconic(h)) {
        auto lm = g_lastMon.find(h);
        MONITORINFO lmi{sizeof lmi};
        if (lm != g_lastMon.end() && GetMonitorInfoW(lm->second, &lmi)) return lm->second;
        WINDOWPLACEMENT wp{sizeof wp};
        if (GetWindowPlacement(h, &wp)) return MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    }
    return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
}

float ScaleOf(HMONITOR m) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy)) || !dx) dx = 96;
    return dx / 96.0f;
}

std::vector<HMONITOR> Monitors() {
    std::vector<HMONITOR> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }, (LPARAM)&v);
    return v;
}

RECT MonRect(HMONITOR m) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(m, &mi);
    return mi.rcMonitor;
}
RECT WorkRect(HMONITOR m) {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(m, &mi);
    return mi.rcWork;
}

// The cards (in DIP): big enough to see what is in them
constexpr float kCardW = 210, kCardH = 136, kCardGap = 18;
int StripWidthPx(HMONITOR m) { return (int)std::lround((kCardW + 36) * ScaleOf(m)); }

// Any window that covers the whole screen = a game (or a fullscreen video): leave that screen alone.
bool GameOn(HMONITOR m) {
    struct D { RECT mr; bool hit; } d{MonRect(m), false};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        D* d = (D*)lp;
        if (d->hit || !IsWindowVisible(h) || IsIconic(h) || IsZoomed(h)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() || IsDesktopClass(h)) return TRUE;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        if (cloaked) return TRUE;
        RECT r;
        if (!GetWindowRect(h, &r)) return TRUE;
        if (r.left <= d->mr.left && r.top <= d->mr.top && r.right >= d->mr.right && r.bottom >= d->mr.bottom) d->hit = true;
        return TRUE;
    }, (LPARAM)&d);
    if (d.hit) return true;
    QUERY_USER_NOTIFICATION_STATE q{};   // a game in exclusive fullscreen, on the screen that has the focus
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN) {
        HWND fg = GetForegroundWindow();
        if (fg && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == m) return true;
    }
    return false;
}

// Windows only lets the app that got the last input take the foreground (same trick as Mission Control).
void TakeForeground(HWND h) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof in);
    SetForegroundWindow(h);
    if (GetForegroundWindow() == h) return;
    HWND fgw = GetForegroundWindow();
    DWORD fg = GetWindowThreadProcessId(fgw, nullptr), me = GetCurrentThreadId();
    if (fg && fg != me && !(fgw && IsHungAppWindow(fgw)) && AttachThreadInput(me, fg, TRUE)) {
        BringWindowToTop(h);
        SetForegroundWindow(h);
        AttachThreadInput(me, fg, FALSE);
    }
}

void SaveStash() {
    std::wstring path = DirOfIni() + L"stage-stash.txt";
    std::string out;
    for (auto& [w, si] : g_stash) if (!si.adopted) {   // (the window and its process: a window number can be reused by another window later)
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        out += std::to_string((unsigned long long)(ULONG_PTR)w) + " " + std::to_string((unsigned long)pid) + "\n";
    }
    if (out.empty()) { DeleteFileW(path.c_str()); return; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(f);
}
// Skyline was closed (or crashed) while windows were put away: give them back now
void RestoreLeftovers() {
    std::wstring path = DirOfIni() + L"stage-stash.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[8192] = {};
    DWORD r = 0;
    ReadFile(f, buf, sizeof buf - 1, &r, nullptr);
    CloseHandle(f);
    DeleteFileW(path.c_str());
    const char* p = buf;
    while (*p) {
        char* e1 = nullptr;
        unsigned long long v = strtoull(p, &e1, 10);
        unsigned long want = e1 ? strtoul(e1, nullptr, 10) : 0;
        HWND w = (HWND)(ULONG_PTR)v;
        DWORD pid = 0;
        if (v && IsWindow(w)) GetWindowThreadProcessId(w, &pid);
        // only the very same window (same process): never an unrelated window that got the same number after a restart
        if (v && IsWindow(w) && want && pid == want && IsIconic(w)) { ShowWindowAsync(w, SW_RESTORE); Log(L"leftover restored " + Short(w)); }
        const char* nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
}

void Schedule(UINT ms) {
    if (g_msg) SetTimer(g_msg, T_REFRESH, ms, nullptr);
}

void Refresh();
void ActivateGroup(HMONITOR mon, const std::wstring& exe, HWND prefer);

// One card in the strip: all the put-away windows of one app on one screen.
struct Card {
    std::wstring exe;
    std::vector<HWND> wins;   // the one on top first
};

class Strip {
public:
    explicit Strip(HMONITOR m) : mon_(m) {}
    ~Strip() { Destroy(); }
    HMONITOR Mon() const { return mon_; }
    std::wstring State() const {   // for stage-debug.txt: is the strip there, on show, waiting for the mouse?
        std::wstring t = hwnd_ ? L"window" : L"no window";
        if (hwnd_ && IsWindowVisible(hwnd_)) t += L", visible";
        if (revealed_) t += L", shown";
        if (hoverOnly_) t += L", waits for the mouse";
        if (trigger_ && IsWindowVisible(trigger_)) t += L", edge trigger on";
        return t;
    }

    void Configure(std::vector<Card> cards, bool hoverOnly, bool suppressed) {
        cards_ = std::move(cards);
        hoverOnly_ = hoverOnly;
        if (suppressed || cards_.empty()) { HideAll(); return; }
        if (!Ensure()) return;
        Layout();
        if (!hoverOnly_) {
            if (trigger_) ShowWindow(trigger_, SW_HIDE);
            if (!revealed_) SetShown(true, true);
        } else if (!revealed_) {
            PlaceTrigger();
        } else {   // it is on show but should hide now (something covers it): leave when the mouse is away
            leave_ = 0;
            SetTimer(hwnd_, T_WATCH, 100, nullptr);
        }
        for (auto it = bmps_.begin(); it != bmps_.end();) {   // pictures of windows that are no longer in the strip
            bool keep = false;
            for (const auto& c : cards_) for (HWND w : c.wins) if (w == it->first) keep = true;
            it = keep ? std::next(it) : bmps_.erase(it);
        }
        if (revealed_ && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void HideAll() {
        revealed_ = false;
        sliding_ = false;
        if (hwnd_) { KillTimer(hwnd_, T_SLIDE); KillTimer(hwnd_, T_WATCH); ShowWindow(hwnd_, SW_HIDE); }
        if (trigger_) ShowWindow(trigger_, SW_HIDE);
    }

    void Reveal() {   // the mouse touched the edge (hidden mode)
        if (revealed_ || cards_.empty() || !Ensure()) return;
        Layout();
        SetShown(true, true);
        if (trigger_) ShowWindow(trigger_, SW_HIDE);
        leave_ = 0;
        SetTimer(hwnd_, T_WATCH, 100, nullptr);
    }

private:
    bool Ensure() {
        if (hwnd_) return true;
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"SkylineStageStrip";
            RegisterClassExW(&wc);
            WNDCLASSEXW tc{sizeof tc};
            tc.lpfnWndProc = TriggerProc;
            tc.hInstance = g_app.hinst;
            tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            tc.lpszClassName = L"SkylineStageTrigger";
            RegisterClassExW(&tc);
            reg = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"SkylineStageStrip", L"Stage Manager", WS_POPUP,
                                0, 0, 10, 10, nullptr, nullptr, g_app.hinst, this);
        if (!hwnd_) return false;
        trigger_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"SkylineStageTrigger", L"", WS_POPUP,
                                   0, 0, 2, 10, nullptr, nullptr, g_app.hinst, this);
        if (trigger_) SetLayeredWindowAttributes(trigger_, 0, 1, LWA_ALPHA);
        MakeTransparent();
        return true;
    }

    void Destroy() {
        if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); hwnd_ = nullptr; }
        if (trigger_) { SetWindowLongPtrW(trigger_, GWLP_USERDATA, 0); DestroyWindow(trigger_); trigger_ = nullptr; }
        rt_.Reset(); brush_.Reset(); font_.Reset(); bmps_.clear(); icons_.clear();
    }

    // No background at all: only the cards are seen (clear glass + alpha from Direct2D)
    void MakeTransparent() {
        BOOL dark = g_app.theme.dark;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof dark);
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
        DWORD sbt = 1;
        DwmSetWindowAttribute(hwnd_, 38, &sbt, sizeof sbt);
        DWORD corner = 1;   // DWMWCP_DONOTROUND
        DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof corner);
        DWORD noBorder = 0xFFFFFFFE;   // DWMWA_COLOR_NONE: no thin light line round the window's shape
        DwmSetWindowAttribute(hwnd_, 34, &noBorder, sizeof noBorder);
        struct AP { int s, f; DWORD c; int a; };
        struct WD { int a; PVOID d; SIZE_T n; };
        using Fn = BOOL(WINAPI*)(HWND, WD*);
        static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
        if (!fn) return;
        AP p{2, 2, 0, 0};
        WD d{19, &p, sizeof p};
        fn(hwnd_, &d);
    }

    // Where the cards sit (client pixels) and the window's shape = only the cards (the rest of the edge stays clickable)
    void Layout() {
        float k = ScaleOf(mon_);
        RECT wa = WorkRect(mon_), mr = MonRect(mon_);
        W_ = StripWidthPx(mon_);
        H_ = std::max<int>(100, wa.bottom - wa.top);
        int n = (int)cards_.size();
        float boxW = kCardW * k, boxH = kCardH * k, gap = kCardGap * k;
        float total = n * boxH + (n - 1) * gap, avail = H_ - 32 * k;
        if (total > avail && total > 0) { float s = avail / total; boxW *= s; boxH *= s; gap *= s; total = avail; }
        rects_.assign(n, {});
        float left = (W_ - boxW) / 2 - 4 * k, top0 = (H_ - total) / 2;
        for (int i = 0; i < n; ++i) {
            float t = top0 + i * (boxH + gap);
            rects_[i] = D2D1::RectF(left, t, left + boxW, t + boxH);
        }
        baseX_ = mr.left;
        baseY_ = wa.top;
        if (!revealed_) cur_ = -W_;
        if (revealed_ && !sliding_) Place(cur_);
        else BuildRegion();
        PlaceTriggerRect(mr, wa, k);
        RECT cr;
        GetClientRect(hwnd_, &cr);
        if (rt_ && (cr.right != (LONG)rt_->GetSize().width || cr.bottom != (LONG)rt_->GetSize().height)) rt_->Resize(D2D1::SizeU(cr.right, cr.bottom));
    }

    // The window's shape = only the cards (the rest of the edge stays clickable), shifted by the slide
    void BuildRegion() {
        if (!hwnd_) return;
        float k = ScaleOf(mon_);
        HRGN all = CreateRectRgn(0, 0, 0, 0);
        for (int i = 0; i < (int)rects_.size() && i < (int)cards_.size(); ++i) {
            const D2D1_RECT_F& r = rects_[i];
            int extra = (int)std::min<size_t>(cards_[i].wins.size(), 3) - 1;
            float off = std::max(0, extra) * 7 * k;
            HRGN g = CreateRoundRectRgn((int)(r.left - 14 * k) + cur_, (int)(r.top - off - 14 * k), (int)(r.right + off + 14 * k) + cur_, (int)(r.bottom + 22 * k), (int)(20 * k), (int)(20 * k));
            CombineRgn(all, all, g, RGN_OR);
            DeleteObject(g);
        }
        SetWindowRgn(hwnd_, all, TRUE);   // (the system owns the region now)
    }

    void PlaceTriggerRect(const RECT& mr, const RECT& wa, float k) {
        if (!trigger_) return;
        int w = std::max(2, (int)std::lround(2 * k));
        SetWindowPos(trigger_, HWND_TOPMOST, mr.left, wa.top, w, wa.bottom - wa.top, SWP_NOACTIVATE);
    }
    void PlaceTrigger() {
        if (!trigger_) return;
        RECT mr = MonRect(mon_), wa = WorkRect(mon_);
        PlaceTriggerRect(mr, wa, ScaleOf(mon_));
        ShowWindow(trigger_, SW_SHOWNOACTIVATE);
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }

    // The window itself stays on its own screen (a window half-way over to the next screen looked wrong):
    // only the cards (and the window's shape) slide.
    void Place(int offset) {
        cur_ = offset;
        BuildRegion();
        SetWindowPos(hwnd_, HWND_TOPMOST, baseX_, baseY_, W_, H_, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Slide in (on = true) or out
    void SetShown(bool on, bool animate) {
        target_ = on ? 0 : -W_;
        if (on) revealed_ = true;
        if (!animate) { sliding_ = false; Place(target_); if (!on) { revealed_ = false; ShowWindow(hwnd_, SW_HIDE); } return; }
        from_ = cur_;
        startTick_ = GetTickCount64();
        sliding_ = true;
        if (on) Place(cur_);   // (shows it at the start of the slide)
        SetTimer(hwnd_, T_SLIDE, 10, nullptr);
    }

    void SlideStep() {
        float t = std::clamp((GetTickCount64() - startTick_) / 170.0f, 0.0f, 1.0f);
        float e = 1 - (1 - t) * (1 - t) * (1 - t);
        int off = (int)std::lround(from_ + (target_ - from_) * e);
        if (off < -W_) off = -W_;
        Place(off);
        if (t >= 1.0f) {
            KillTimer(hwnd_, T_SLIDE);
            sliding_ = false;
            if (target_ < 0) {   // gone: back to waiting for the mouse
                revealed_ = false;
                ShowWindow(hwnd_, SW_HIDE);
                if (hoverOnly_ && !cards_.empty()) PlaceTrigger();
            }
        }
    }

    // Hidden mode: leave again when the mouse has been away a little while
    void Watch() {
        POINT c;
        GetCursorPos(&c);
        RECT r{baseX_, baseY_, baseX_ + W_ + (int)(24 * ScaleOf(mon_)), baseY_ + H_};   // (the window itself)
        bool closeBy = PtInRect(&r, c) != 0 || (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (closeBy) { leave_ = 0; return; }
        if (++leave_ >= 4) {
            KillTimer(hwnd_, T_WATCH);
            if (hoverOnly_) SetShown(false, true);
        }
    }

    int CardAt(POINT p) const {
        for (int i = 0; i < (int)rects_.size(); ++i) {
            const D2D1_RECT_F& r = rects_[i];
            float k = ScaleOf(mon_), off = (float)(std::max<int>(0, (int)std::min<size_t>(cards_[i].wins.size(), 3) - 1)) * 7 * k;
            if (p.x >= r.left && p.x < r.right + off && p.y >= r.top - off && p.y < r.bottom) return i;
        }
        return -1;
    }

    bool Resources() {
        if (rt_) return true;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY), rt_.Put())))
            return false;
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
        if (!font_) {
            g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           13.0f * ScaleOf(mon_), L"", font_.Put());
            if (font_) {
                font_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        return true;
    }

    ID2D1Bitmap* Snap(HWND w) {
        auto it = bmps_.find(w);
        if (it != bmps_.end()) return it->second.Get();
        ComPtr<ID2D1Bitmap> b;
        if (auto src = EnhancedSnapshot(w)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
        if (!b) return nullptr;   // (no picture yet: don't remember that, it may arrive)
        return bmps_.emplace(w, b).first->second.Get();
    }
    ID2D1Bitmap* Icon(const std::wstring& exe, int px) {
        auto it = icons_.find(exe);
        if (it != icons_.end()) return it->second.Get();
        ComPtr<ID2D1Bitmap> b;
        if (auto src = LoadAppImage(exe, px)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
        icons_[exe] = b;
        return b.Get();
    }

    // One window's picture as a rounded card (a picture brush gives the round corners)
    void DrawShot(HWND w, const std::wstring& exe, D2D1_RECT_F box, float opacity, float k) {
        float r = 9 * k;
        ID2D1Bitmap* bmp = w ? Snap(w) : nullptr;
        D2D1_RECT_F dr = box;
        if (bmp) {
            D2D1_SIZE_F bs = bmp->GetSize();
            float sc = std::min((box.right - box.left) / std::max(1.0f, bs.width), (box.bottom - box.top) / std::max(1.0f, bs.height));
            float dw = bs.width * sc, dh = bs.height * sc, cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
            dr = D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2);
        }
        brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.28f * opacity));   // soft shadow
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(dr.left - 1 * k, dr.top + 1 * k, dr.right + 1 * k, dr.bottom + 4 * k), r + 1 * k, r + 1 * k), brush_.Get());
        if (bmp) {
            ComPtr<ID2D1BitmapBrush> bb;
            rt_->CreateBitmapBrush(bmp, bb.Put());
            if (bb) {
                D2D1_SIZE_F bs = bmp->GetSize();
                float sc = (dr.right - dr.left) / std::max(1.0f, bs.width);
                bb->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(dr.left, dr.top));
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                bb->SetOpacity(opacity);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(dr, r, r), bb.Get());
            }
        } else {   // no picture (yet): the app's icon on a plain card
            brush_->SetColor(D2D1::ColorF(0.16f, 0.16f, 0.18f, 0.92f * opacity));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(dr, r, r), brush_.Get());
            int px = (int)std::lround(40 * k);
            if (ID2D1Bitmap* ib = Icon(exe, px)) {
                float cx = (dr.left + dr.right) / 2, cy = (dr.top + dr.bottom) / 2, s2 = 20 * k;
                DrawImageHQ(rt_.Get(), ib, D2D1::RectF(cx - s2, cy - s2, cx + s2, cy + s2), opacity);
            }
        }
    }

    void Paint() {
        if (!Resources()) return;
        float k = ScaleOf(mon_);
        const Theme& th = g_app.theme;
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt_->SetTransform(D2D1::Matrix3x2F::Translation((float)cur_, 0));
        const bool glow = g_app.settings.stageHover == 1;
        for (int i = 0; i < (int)cards_.size() && i < (int)rects_.size(); ++i) {
            const Card& c = cards_[i];
            const D2D1_RECT_F& r = rects_[i];
            const float dim = (!glow && hover_ != -1 && i != hover_) ? 0.5f : 1.0f;   // dim style: the others fade, the one under the mouse stays bright
            int n = (int)std::min<size_t>(c.wins.size(), 3);
            for (int j = n - 1; j >= 1; --j) {   // the other windows of the app, stacked behind (up and to the right)
                float off = j * 7 * k;
                DrawShot(c.wins[j], c.exe, D2D1::RectF(r.left + off, r.top - off, r.right + off, r.bottom - off), (0.72f - 0.12f * (j - 1)) * dim, k);
            }
            D2D1_RECT_F box = r;
            if (i == hover_) { float g = (glow ? 3 : 6) * k; box = D2D1::RectF(r.left - g, r.top - g, r.right + g, r.bottom + g); }   // lifts a little under the mouse
            if (glow && i == hover_) {   // soft glow in the accent colour (no hard line)
                for (int q = 6; q >= 1; --q) {
                    float e = q * 2 * k;
                    D2D1_COLOR_F gc = th.accent; gc.a = 0.07f;
                    brush_->SetColor(gc);
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left - e, box.top - e, box.right + e, box.bottom + e), (9 + q * 2) * k, (9 + q * 2) * k), brush_.Get());
                }
            }
            DrawShot(c.wins.empty() ? nullptr : c.wins[0], c.exe, box, dim, k);
            {   // the app's icon on the bottom edge, so a group is known at a glance (the name too under the mouse)
                float isz = 34 * k, ix = box.left + 10 * k, iy = box.bottom - isz * 0.6f;
                float pad = 3 * k;
                brush_->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.92f * dim));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ix - pad, iy - pad, ix + isz + pad, iy + isz + pad), 9 * k, 9 * k), brush_.Get());
                if (ID2D1Bitmap* ib = Icon(c.exe, (int)std::lround(isz)))
                    DrawImageHQ(rt_.Get(), ib, D2D1::RectF(ix, iy, ix + isz, iy + isz), dim);
                if (i == hover_ && font_) {
                    const std::wstring& nm = NameOf(c.exe);
                    float tx = ix + isz + pad + 6 * k, tw = std::max(60 * k, std::min(box.right - tx - 4 * k, 170 * k));
                    D2D1_RECT_F pill = D2D1::RectF(tx - 6 * k, iy + 2 * k, tx + tw, iy + isz - 2 * k);
                    brush_->SetColor(D2D1::ColorF(0.1f, 0.1f, 0.12f, 0.92f));
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(pill, 8 * k, 8 * k), brush_.Get());
                    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                    rt_->DrawTextW(nm.c_str(), (UINT32)nm.size(), font_.Get(), D2D1::RectF(tx, pill.top, pill.right - 4 * k, pill.bottom), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); icons_.clear(); }
    }

    static LRESULT CALLBACK TriggerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        Strip* self = reinterpret_cast<Strip*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN)) { self->Reveal(); return 0; }
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        return DefWindowProcW(h, msg, wp, lp);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        Strip* self = reinterpret_cast<Strip*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SIZE:
            if (self->rt_) self->rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;
        case WM_TIMER:
            if (wp == T_SLIDE) self->SlideStep();
            else if (wp == T_WATCH) self->Watch();
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            int i = self->CardAt({p.x - self->cur_, p.y});
            if (i != self->hover_) { self->hover_ = i; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (self->hover_ != -1) { self->hover_ = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN:
            self->down_ = self->CardAt({p.x - self->cur_, p.y});
            return 0;
        case WM_LBUTTONUP: {
            int i = self->CardAt({p.x - self->cur_, p.y}), d = self->down_;
            self->down_ = -1;
            if (i >= 0 && i == d && i < (int)self->cards_.size() && !self->cards_[i].wins.empty()) {
                std::wstring exe = self->cards_[i].exe;
                HWND w = self->cards_[i].wins[0];
                HMONITOR m = self->mon_;
                self->hover_ = -1;
                ActivateGroup(m, exe, w);   // (this strip may be rebuilt in here: touch nothing of ours afterwards)
            }
            return 0;
        }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    HMONITOR mon_;
    HWND hwnd_ = nullptr, trigger_ = nullptr;
    std::vector<Card> cards_;
    std::vector<D2D1_RECT_F> rects_;
    bool hoverOnly_ = false, revealed_ = false, sliding_ = false;
    int W_ = 0, H_ = 0, baseX_ = 0, baseY_ = 0, cur_ = 0, target_ = 0, from_ = 0, leave_ = 0;
    ULONGLONG startTick_ = 0;
    int hover_ = -1, down_ = -1;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<HWND, ComPtr<ID2D1Bitmap>> bmps_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> icons_;
    std::map<std::wstring, std::wstring> names_;   // the app's name (found once)
    ComPtr<IDWriteTextFormat> font_;

    const std::wstring& NameOf(const std::wstring& exe) {
        auto it = names_.find(exe);
        if (it != names_.end()) return it->second;
        std::wstring n = FileDescriptionOf(exe);
        if (n.empty()) {
            n = exe.substr(exe.find_last_of(L'\\') + 1);
            if (n.size() > 4) n.resize(n.size() - 4);
        }
        return names_.emplace(exe, n).first->second;
    }
};

std::map<HMONITOR, std::unique_ptr<Strip>> g_strips;

// ---------------------------------------------------------------------------
// The logic
// ---------------------------------------------------------------------------
// The app windows on a screen, front to back. `minimised`: also the ones put away.
std::vector<HWND> WindowsOn(HMONITOR m, bool includeIconic) {
    struct Ctx { HMONITOR m; bool ic; std::vector<HWND> v; } c{m, includeIconic, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        if (!AppWin(h)) return TRUE;
        bool iconic = IsIconic(h) != FALSE;
        if (iconic && !c->ic) return TRUE;
        if (MonOf(h) == c->m) c->v.push_back(h);
        return TRUE;
    }, (LPARAM)&c);
    return c.v;
}

void TouchMru(HMONITOR m, const std::wstring& exe) {
    auto& v = g_mru[m];
    v.erase(std::remove(v.begin(), v.end(), exe), v.end());
    v.insert(v.begin(), exe);
}

// Make room at the left (mode 0): the windows of the app in front move out from under the strip
void MakeRoom(HMONITOR m) {
    if (g_app.settings.stageMode != 0) return;
    float k = ScaleOf(m);
    RECT mr = MonRect(m), wa = WorkRect(m);
    int edge = mr.left + StripWidthPx(m) + (int)(6 * k);
    auto it = g_active.find(m);
    if (it == g_active.end()) return;
    for (HWND w : WindowsOn(m, false)) {
        if (ExeOf(w) != it->second || IsZoomed(w) || g_stash.count(w)) continue;
        RECT r;
        if (!GetWindowRect(w, &r) || r.left >= edge - (int)(10 * k)) continue;
        int nl = edge, nr = std::min<int>(r.right + (edge - r.left), wa.right);
        if (nr - nl < (int)(420 * k)) nl = std::max<int>(edge, nr - (int)(420 * k));
        SetWindowPos(w, nullptr, nl, r.top, std::max<int>(200, nr - nl), r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

// Is a window of the active app over the strip's place (or maximised)? Then the strip hides like in hidden mode.
bool ActiveCovers(HMONITOR m) {
    float k = ScaleOf(m);
    RECT mr = MonRect(m);
    int edge = mr.left + StripWidthPx(m) - (int)(14 * k);
    for (HWND w : WindowsOn(m, false)) {
        if (g_stash.count(w)) continue;
        if (IsZoomed(w)) return true;
        RECT r;
        if (GetWindowRect(w, &r) && r.left < edge) return true;
    }
    return false;
}

int ScreenNo(HMONITOR m) {   // 1, 2, 3 … in the order Windows lists the screens (for the log only)
    auto v = Monitors();
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == m) return (int)i + 1;
    return 0;
}

void StashWindow(HWND w, HMONITOR m) {
    if (g_stash.count(w)) return;
    EnhancedSnapshotNow(w);   // a minimised window can't be drawn any more: the picture is taken now
    StashInfo si;
    si.mon = m;
    si.exe = ExeOf(w);
    si.seq = ++g_seq;
    si.tick = GetTickCount64();
    g_stash[w] = si;
    Log(L"put away " + Short(w) + L" (" + si.exe.substr(si.exe.find_last_of(L'\\') + 1) + L") on screen " + std::to_wstring(ScreenNo(m)));
    if (IsHungAppWindow(w)) ShowWindowAsync(w, SW_SHOWMINNOACTIVE);
    else ShowWindow(w, SW_SHOWMINNOACTIVE);
}

void UnstashWindow(HWND w) {
    auto it = g_stash.find(w);
    if (it == g_stash.end()) return;
    g_stash.erase(it);
    Log(L"given back " + Short(w));
    if (!IsWindow(w) || !IsIconic(w)) return;
    if (IsHungAppWindow(w)) ShowWindowAsync(w, SW_RESTORE);
    else ShowWindow(w, SW_RESTORE);
}

// Make `exe` the app in front on this screen: its put-away windows come back, everything else goes away.
void SwitchGroup(HMONITOR m, const std::wstring& exe, HWND prefer) {
    g_active[m] = exe;
    TouchMru(m, exe);
    std::vector<HWND> bring;
    for (auto& [w, si] : g_stash) if (si.mon == m && si.exe == exe) bring.push_back(w);
    for (HWND w : bring) UnstashWindow(w);
    for (HWND w : WindowsOn(m, false)) {   // what is on show and belongs to another app
        if (ExeOf(w) == exe || g_stash.count(w)) continue;
        StashWindow(w, m);
    }
    MakeRoom(m);
    SaveStash();
    Log(L"group " + exe.substr(exe.find_last_of(L'\\') + 1) + L" in front on screen " + std::to_wstring(ScreenNo(m)) + L"; put away now: " + std::to_wstring(g_stash.size()));
    if (prefer && IsWindow(prefer) && !IsIconic(prefer) && GetForegroundWindow() != prefer) TakeForeground(prefer);
}

void ActivateGroup(HMONITOR mon, const std::wstring& exe, HWND prefer) {
    if (!g_on) return;
    SwitchGroup(mon, exe, prefer);
    Schedule(60);
}

// The cards for a screen: newest-used app first, the window put away last at the front of its stack
std::vector<Card> BuildCards(HMONITOR m) {
    std::map<std::wstring, std::vector<std::pair<ULONGLONG, HWND>>> by;
    for (auto& [w, si] : g_stash) if (si.mon == m) by[si.exe].push_back({si.seq, w});
    std::vector<Card> out;
    auto add = [&](const std::wstring& exe) {
        auto it = by.find(exe);
        if (it == by.end()) return;
        auto v = it->second;
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        Card c;
        c.exe = exe;
        for (auto& pr : v) c.wins.push_back(pr.second);
        out.push_back(std::move(c));
        by.erase(it);
    };
    for (const auto& exe : g_mru[m]) add(exe);
    std::vector<std::wstring> rest;
    for (auto& kv : by) rest.push_back(kv.first);
    for (const auto& exe : rest) add(exe);
    return out;
}

// For finding out why nothing shows: what Stage Manager sees, written next to skyline.ini (stage-debug.txt, overwritten each time)
int StashCount(HMONITOR m) {
    int n = 0;
    for (auto& kv : g_stash) if (kv.second.mon == m) ++n;
    return n;
}
void DebugDump(const std::vector<HMONITOR>& mons) {
    std::wstring dir = g_app.settings.path;
    size_t bs = dir.find_last_of(L"\\/");
    if (bs != std::wstring::npos) dir = dir.substr(0, bs + 1);
    std::wstring out = L"mode=" + std::to_wstring(g_app.settings.stageMode) + L" stash=" + std::to_wstring(g_stash.size()) + L"\r\n";
    for (HMONITOR m : mons) {
        RECT r = MonRect(m);
        out += L"screen " + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L" " + std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top) +
               L" game=" + std::to_wstring((int)GameOn(m)) + L" windows=" + std::to_wstring(WindowsOn(m, false).size()) + L" cards=" + std::to_wstring(BuildCards(m).size()) +
               L" stash=" + std::to_wstring(StashCount(m)) + L" covers=" + std::to_wstring((int)ActiveCovers(m)) + L" active=" + g_active[m] + L"\r\n";
        auto st = g_strips.find(m);
        out += L"    strip: " + std::wstring(st == g_strips.end() ? L"none" : st->second->State()) + L"\r\n";
    }
    for (auto& [w, si] : g_stash) {
        wchar_t t[50] = {};
        if (IsWindow(w)) GetWindowTextW(w, t, 50);
        out += L"  stash: screen " + std::to_wstring(ScreenNo(si.mon)) + L" " + si.exe.substr(si.exe.find_last_of(L'\\') + 1) + L" \"" + t + L"\"" + (si.adopted ? L" (adopted)" : L"") + (IsIconic(w) ? L"" : L" NOT MINIMISED") + L"\r\n";
    }
    int n = 0;
    std::pair<std::wstring*, int*> ctx(&out, &n);
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* o = (std::pair<std::wstring*, int*>*)lp;
        if (*o->second > 40 || !IsWindowVisible(h)) return TRUE;
        wchar_t t[80] = {};
        GetWindowTextW(h, t, 80);
        if (!t[0]) return TRUE;
        RECT r{};
        GetWindowRect(h, &r);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        std::wstring exe = ProcessImagePath(pid);
        size_t b = exe.find_last_of(L'\\');
        if (b != std::wstring::npos) exe = exe.substr(b + 1);
        *o->first += L"  [" + std::wstring(AppWin(h) ? L"APP" : L"no ") + L"] " + exe + L" \"" + t + L"\" " + std::to_wstring(r.right - r.left) + L"x" + std::to_wstring(r.bottom - r.top) + (IsIconic(h) ? L" min" : L"") +
                    (AppWin(h) ? L" -> screen " + std::to_wstring(ScreenNo(MonOf(h))) : L"") + L"\r\n";
        ++*o->second;
        return TRUE;
    }, (LPARAM)&ctx);
    static std::wstring last;
    if (out == last) return;   // (nothing new: no writing)
    last = out;
    HANDLE f = CreateFileW((dir + L"stage-debug.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, "\xEF\xBB\xBF", 3, &w, nullptr);
        int len = WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), nullptr, 0, nullptr, nullptr);
        std::string u8(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, out.c_str(), (int)out.size(), u8.data(), len, nullptr, nullptr);
        WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
        CloseHandle(f);
    }
}

// "Include minimised windows" (a choice in the settings): apps minimised by hand are in the strip too
void SyncAdopted();

// The screen a window belongs to, looked up fresh (the screen handles can change when a screen sleeps, is plugged in or out)
HMONITOR FreshMon(HWND w) {
    WINDOWPLACEMENT wp{sizeof wp};
    if (IsWindow(w) && GetWindowPlacement(w, &wp)) return MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

// More than one app on show on a screen (a window dragged over from another screen, or one that opened without taking the focus):
// the app with the focus stays (else the one that was in front) and the others are put away, but only when seen twice
// so that a window that is just opening gets time to take the focus. Returns true when a second look is needed.
bool Reconcile(HMONITOR m) {
    std::vector<HWND> shown;
    std::set<std::wstring> exes;
    for (HWND w : WindowsOn(m, false)) if (!g_stash.count(w)) { shown.push_back(w); exes.insert(ExeOf(w)); }
    std::set<HWND> odd;
    std::wstring keep;
    if (exes.size() >= 2) {
        HWND fg = GetForegroundWindow();
        if (fg && AppWin(fg) && !IsIconic(fg) && !g_stash.count(fg) && MonOf(fg) == m && exes.count(ExeOf(fg))) keep = ExeOf(fg);
        else if (exes.count(g_active[m])) keep = g_active[m];
        else keep = ExeOf(shown[0]);
        for (HWND w : shown) if (ExeOf(w) != keep) odd.insert(w);
    }
    if (exes.size() == 1 && g_active[m] != *exes.begin()) { g_active[m] = *exes.begin(); TouchMru(m, g_active[m]); }   // (only one app on show: that is the one in front)
    for (auto it = g_young.begin(); it != g_young.end();)   // no longer an odd one out (or gone)
        it = (!IsWindow(*it) || (MonOf(*it) == m && !odd.count(*it))) ? g_young.erase(it) : std::next(it);
    bool pending = false, did = false;
    for (HWND w : odd) {
        if (!g_young.count(w)) { g_young.insert(w); pending = true; continue; }
        Log(L"second app on show, put away " + Short(w));
        StashWindow(w, m);
        g_young.erase(w);
        did = true;
    }
    if (did) {
        if (g_active[m] != keep) { g_active[m] = keep; TouchMru(m, keep); }
        MakeRoom(m);
        SaveStash();
    }
    return pending;
}

void Refresh() {
    if (!g_on) return;
    g_refreshPending = false;
    auto mons = Monitors();
    for (auto it = g_strips.begin(); it != g_strips.end();) {   // screens that went away
        if (std::find(mons.begin(), mons.end(), it->first) == mons.end()) it = g_strips.erase(it);
        else ++it;
    }
    for (auto& kv : g_stash)   // a screen handle that no longer exists (screen slept / unplugged / changed): look the screen up again
        if (std::find(mons.begin(), mons.end(), kv.second.mon) == mons.end()) { kv.second.mon = FreshMon(kv.first); Log(L"screen changed, moved " + Short(kv.first)); }
    for (auto it = g_active.begin(); it != g_active.end();) it = std::find(mons.begin(), mons.end(), it->first) == mons.end() ? g_active.erase(it) : std::next(it);
    for (auto it = g_mru.begin(); it != g_mru.end();) it = std::find(mons.begin(), mons.end(), it->first) == mons.end() ? g_mru.erase(it) : std::next(it);
    for (auto it = g_seenFront.begin(); it != g_seenFront.end();) it = std::find(mons.begin(), mons.end(), it->first) == mons.end() ? g_seenFront.erase(it) : std::next(it);
    bool slow = false;
    for (auto it = g_noStash.begin(); it != g_noStash.end();) it = !IsWindow(it->first) ? g_noStash.erase(it) : std::next(it);
    for (auto it = g_stash.begin(); it != g_stash.end();) {   // closed, or brought back by hand
        if (IsWindow(it->first) && !it->second.adopted && !IsIconic(it->first) && IsWindowVisible(it->first)) {
            // asked to minimise but still on show: slow app (look again soon) or one that will not (elevated …): then it is left alone for a minute
            if (GetTickCount64() - it->second.tick < 1500) { slow = true; ++it; continue; }
            Log(L"would not minimise, left alone " + Short(it->first));
            g_noStash[it->first] = GetTickCount64() + 60000;
            it = g_stash.erase(it);
            continue;
        }
        if (!IsWindow(it->first) || !IsIconic(it->first) || !IsWindowVisible(it->first)) {   // (a window that hid itself, e.g. in the tray, can't be shown by a click)
            Log(std::wstring(IsWindow(it->first) ? L"no longer minimised or hidden, dropped " : L"gone, dropped ") + (IsWindow(it->first) ? Short(it->first) : L""));
            it = g_stash.erase(it);
        } else ++it;
    }
    for (auto it = g_lastMon.begin(); it != g_lastMon.end();) it = IsWindow(it->first) ? std::next(it) : g_lastMon.erase(it);
    for (auto it = g_exeCache.begin(); it != g_exeCache.end();) it = IsWindow(it->first) ? std::next(it) : g_exeCache.erase(it);
    for (auto it = g_exeUnresolved.begin(); it != g_exeUnresolved.end();) it = IsWindow(*it) ? std::next(it) : g_exeUnresolved.erase(it);
    bool hiddenMode = g_app.settings.stageMode == 1;
    bool again = false, second = false;
    if (g_app.settings.stageMinimized) SyncAdopted();   // (apps minimised by hand since last time)
    for (HMONITOR m : mons) {
        bool game = GameOn(m);
        if (!game && Reconcile(m)) second = true;
        // nothing left in front on this screen (its last window was closed): the newest put-away app comes forward
        auto wins = WindowsOn(m, false);
        for (HWND w : wins) g_lastMon[w] = m;
        bool closedLast = false;   // only when a window in front was really CLOSED (not just minimised by hand, not a click on the desktop)
        for (HWND w : g_seenFront[m]) if (!IsWindow(w)) closedLast = true;
        g_seenFront[m] = wins;
        if (!game && closedLast && !g_stash.empty()) {
            bool any = false;
            for (auto& [w, si] : g_stash) if (si.mon == m) { any = true; break; }
            if (any && wins.empty()) {
                auto cards = BuildCards(m);
                if (!cards.empty() && !cards[0].wins.empty()) { SwitchGroup(m, cards[0].exe, cards[0].wins[0]); again = true; }
            }
        }
        auto& st = g_strips[m];
        if (!st) st = std::make_unique<Strip>(m);
        bool cover = !hiddenMode && !game && !g_app.settings.stageShowOver && ActiveCovers(m);
        st->Configure(BuildCards(m), hiddenMode || cover, game);
    }
    DebugDump(mons);
    if (again) Schedule(120);
    else if (second) Schedule(600);   // a second look at a window that is not in the strip yet
    else if (slow) Schedule(500);     // a window that was asked to minimise: has it done so?
}

void CALLBACK DestroyHook(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (!g_on || obj != OBJID_WINDOW || !h) return;
    bool was = g_stash.erase(h) != 0;
    if (was) Log(L"window closed while put away");
    if (was || !g_stash.empty()) {   // a window closed somewhere: maybe the last one in front. Look again shortly (one timer for all)
        if (!g_refreshPending) { g_refreshPending = true; Schedule(was ? 120 : 350); }
    }
}
void CALLBACK MoveHook(HWINEVENTHOOK, DWORD, HWND, LONG obj, LONG, DWORD, DWORD) {
    if (!g_on || obj != OBJID_WINDOW) return;
    Schedule(200);   // a window was moved: it may now cover the strip (or no longer), or sit on another screen now
}
void CALLBACK ShowHook(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!g_on || obj != OBJID_WINDOW || child != CHILDID_SELF || !h || g_refreshPending) return;
    if (GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return;   // only top-level windows
    g_refreshPending = true;
    Schedule(500);   // a new window is on show (it may not take the focus): look at it in a moment (once for many)
}
void CALLBACK MinHook(HWINEVENTHOOK, DWORD, HWND, LONG obj, LONG, DWORD, DWORD) {
    if (!g_on || obj != OBJID_WINDOW) return;
    Schedule(250);   // something was minimised or brought back (by hand, from the dock …): look again
}

LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == T_REFRESH) { KillTimer(h, T_REFRESH); Refresh(); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

// "Include minimised windows" (a choice in the settings): apps minimised by hand are in the strip too
void SyncAdopted() {
    if (!g_app.settings.stageMinimized) {   // off: they leave the strip (and stay minimised)
        for (auto it = g_stash.begin(); it != g_stash.end();) it = it->second.adopted ? g_stash.erase(it) : std::next(it);
        return;
    }
    for (HMONITOR m : Monitors()) {
        if (GameOn(m)) continue;
        int n = 0;
        for (HWND w : WindowsOn(m, true)) {
            if (!IsIconic(w) || g_stash.count(w) || ExeOf(w) == g_active[m]) continue;
            StashInfo si;
            si.mon = m;
            si.exe = ExeOf(w);
            si.seq = (ULONGLONG)(++n);   // (older than anything put away by us)
            si.adopted = true;
            g_stash[w] = si;
            Log(L"adopted (minimised by hand) " + Short(w));
        }
    }
}

void Start() {
    if (g_on) return;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = g_app.hinst;
    wc.lpszClassName = L"SkylineStageMsg";
    RegisterClassExW(&wc);
    g_msg = CreateWindowExW(0, L"SkylineStageMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_app.hinst, nullptr);
    if (!g_msg) return;
    g_on = true;
    LoadExclude();
    Log(L"start");
    RestoreLeftovers();
    g_hookDestroy = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, DestroyHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookMove = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, MoveHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookMin = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, MinHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookCloak = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, MinHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_hookShow = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, ShowHook, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // From now on: on every screen the app in front stays, the other apps are put away
    for (HMONITOR m : Monitors()) {
        if (GameOn(m)) continue;
        auto wins = WindowsOn(m, false);
        for (HWND w : wins) g_lastMon[w] = m;
        if (wins.empty()) continue;
        HWND fg = GetForegroundWindow();
        HWND front = wins[0];
        if (fg && std::find(wins.begin(), wins.end(), fg) != wins.end()) front = fg;
        // (back to front, so the app that was used last ends up first in the strip)
        for (auto it = wins.rbegin(); it != wins.rend(); ++it) {
            const std::wstring& e = ExeOf(*it);
            auto& v = g_mru[m];
            if (std::find(v.begin(), v.end(), e) == v.end()) v.insert(v.begin(), e);
        }
        SwitchGroup(m, ExeOf(front), nullptr);
    }
    SyncAdopted();
    Refresh();
}

void Stop() {
    if (!g_on) return;
    Log(L"stop");
    g_on = false;
    if (g_hookDestroy) { UnhookWinEvent(g_hookDestroy); g_hookDestroy = nullptr; }
    if (g_hookMove) { UnhookWinEvent(g_hookMove); g_hookMove = nullptr; }
    if (g_hookMin) { UnhookWinEvent(g_hookMin); g_hookMin = nullptr; }
    if (g_hookCloak) { UnhookWinEvent(g_hookCloak); g_hookCloak = nullptr; }
    if (g_hookShow) { UnhookWinEvent(g_hookShow); g_hookShow = nullptr; }
    g_noStash.clear();
    g_lastMon.clear();
    g_young.clear();
    std::vector<HWND> back;
    for (auto& [w, si] : g_stash) if (!si.adopted) back.push_back(w);
    for (HWND w : back) UnstashWindow(w);   // everything we put away comes back (what was minimised by hand stays so)
    g_stash.clear();
    SaveStash();
    g_active.clear();
    g_seenFront.clear();
    g_mru.clear();
    g_exeCache.clear();
    g_strips.clear();
    if (g_msg) { KillTimer(g_msg, T_REFRESH); DestroyWindow(g_msg); g_msg = nullptr; }
    g_refreshPending = false;
}

} // namespace

namespace StageManager {

bool Enabled() { return g_app.settings.stageEnabled; }

void Apply() {
    if (g_app.settings.stageEnabled && !g_on) Start();
    else if (!g_app.settings.stageEnabled && g_on) Stop();
    else if (g_on) { LoadExclude(); SyncAdopted(); Refresh(); }   // (the way of showing it, or the minimised choice, may have changed)
}

void Shutdown() { Stop(); }

void OnForeground(HWND h) {
    if (!g_on || !h) return;
    if (!AppWin(h) || IsIconic(h)) { Schedule(150); return; }   // (a minimised window that "comes forward" is no work window: some apps do that in the background)
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    g_lastMon[h] = m;
    if (GameOn(m)) { Schedule(150); return; }   // a game screen: hands off
    const std::wstring& exe = ExeOf(h);
    auto it = g_active.find(m);
    if (it != g_active.end() && it->second == exe && !g_stash.count(h)) { Schedule(150); return; }   // same app as before
    SwitchGroup(m, exe, nullptr);   // (it is already coming forward by itself)
    Schedule(100);
}

} // namespace StageManager
