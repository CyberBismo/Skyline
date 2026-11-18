// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Tray host: becomes the window that Shell_NotifyIcon talks to, so every app's
// tray icon lands in Skyline. Protocol mirrors ManagedShell (Cairo/RetroBar).
#include "app.h"
#include "bar.h"
#include <cstddef>

// One counter for every tray icon picture, so a generation number is never used twice
UINT NextTrayGen() { static UINT s_gen = 0; return ++s_gen; }

namespace {

constexpr UINT kNIN_SELECT     = WM_USER + 0;
constexpr UINT kNIN_POPUPOPEN  = WM_USER + 6;
constexpr UINT kNIN_POPUPCLOSE = WM_USER + 7;
constexpr UINT kForwardPost    = WM_USER + 372;   // must be posted, not sent
constexpr wchar_t kTaskbandProp[] = L"TaskbandHWND";   // where ITaskbarList finds the task band

#pragma pack(push, 4)
// NOTIFYICONDATAW as it travels in WM_COPYDATA: handles are always 32-bit.
struct NID32 {
    DWORD cbSize;
    DWORD hWnd;
    DWORD uID;
    DWORD uFlags;
    DWORD uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    DWORD uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    DWORD hBalloonIcon;
};
struct SHELLTRAYDATA {
    DWORD dwSignature;
    DWORD dwMessage;
    NID32 nid;
};
struct WINNOTIFYICONIDENTIFIER {
    DWORD dwMagic;
    DWORD dwMessage;
    DWORD cbSize;
    DWORD dwPadding;
    DWORD hWnd;
    UINT  uID;
    GUID  guidItem;
};
#pragma pack(pop)

// Handles are sign-extended when widened to 64-bit.
template <class H> H Widen(DWORD v) { return reinterpret_cast<H>(static_cast<LONG_PTR>(static_cast<LONG>(v))); }

bool IsNullGuid(const GUID& g) { static const GUID z{}; return memcmp(&g, &z, sizeof g) == 0; }

std::wstring BoundedString(const WCHAR* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return std::wstring(s, n);
}

} // namespace

// ---------------------------------------------------------------------------
bool TrayHost::Start() {
    if (!CreateWindows()) return false;
    timer_ = SetTimer(nullptr, 0, 1000, MonitorTimer);   // Explorer gone / in front of us: once a second is quick enough
    BroadcastTaskbarCreated();
    // Right after login, apps and Explorer's own tray start at the same time as
    // us and some icons land in Explorer's tray. Ask again a few times.
    startupStep_ = 1;
    ScheduleReannounce(2500);
    return true;
}

void TrayHost::ScheduleReannounce(UINT ms) {
    if (reannTimer_) KillTimer(nullptr, reannTimer_);
    reannTimer_ = SetTimer(nullptr, 0, ms, ReannounceTimer);
}

void CALLBACK TrayHost::ReannounceTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    TrayHost* self = g_app.tray.get();
    if (!self || self->reannTimer_ != id) return;
    self->reannTimer_ = 0;
    if (!self->hwnd_) return;
    self->Reannounce();
    self->lastReannounce_ = GetTickCount();
    static const UINT steps[] = {2500, 8000, 20000, 45000};
    if (self->startupStep_ > 0 && self->startupStep_ < (int)(sizeof steps / sizeof steps[0])) {
        self->ScheduleReannounce(steps[self->startupStep_]);
        self->startupStep_++;
    } else {
        self->startupStep_ = 0;
    }
}

void TrayHost::Stop() {
    if (timer_) { KillTimer(nullptr, timer_); timer_ = 0; }
    if (reannTimer_) { KillTimer(nullptr, reannTimer_); reannTimer_ = 0; }
    DestroyWindows();
    for (auto& t : icons_) if (t.icon) DestroyIcon(t.icon);
    icons_.clear();
    // Let apps re-add their icons to Explorer. (Our own bars must not take this for an Explorer restart.)
    g_app.ignoreTaskbarCreatedUntil = GetTickCount() + 3000;
    SendNotifyMessageW(HWND_BROADCAST, g_app.msgTaskbarCreated, 0, 0);
}

bool TrayHost::CreateWindows() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.style = CS_DBLCLKS;
        wc.lpszClassName = L"Shell_TrayWnd";
        if (!RegisterClassW(&wc)) return false;
        wc.lpszClassName = L"TrayNotifyWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    explorer_ = FindExplorerTray();

    RECT rc = hostRect_;
    if (IsRectEmpty(&rc)) rc = {0, 0, GetSystemMetrics(SM_CXSCREEN), 28};
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Shell_TrayWnd", L"",
                            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, g_app.hinst, nullptr);
    if (!hwnd_) return false;
    notify_ = CreateWindowExW(0, L"TrayNotifyWnd", nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, 0, rc.right - rc.left, rc.bottom - rc.top, hwnd_, nullptr, g_app.hinst, nullptr);
    if (!IsRectEmpty(&trayArea_)) SetTrayArea(trayArea_);
    LinkTaskband();
    MakeTopmost();
    return true;
}

void TrayHost::DestroyWindows() {
    if (hwnd_) { RemovePropW(hwnd_, kTaskbandProp); DestroyWindow(hwnd_); hwnd_ = nullptr; notify_ = nullptr; }
}

// Apps find "the taskbar" by looking for the first Shell_TrayWnd, and that is ours (so their tray icons come to us).
// ITaskbarList (taskbar progress, overlay badges, thumbnail buttons) then reads a "TaskbandHWND" property on that
// window to find Explorer's task band. Ours had none, so ITaskbarList answered "not implemented" — most apps shrug
// that off, but WPF apps such as Chris Titus' WinUtil crash on it. Point the property at Explorer's real task band.
void TrayHost::LinkTaskband() {
    if (!hwnd_) return;
    if (!explorer_ || !IsWindow(explorer_)) explorer_ = FindExplorerTray();
    if (!explorer_) return;
    HWND band = (HWND)GetPropW(explorer_, kTaskbandProp);
    if (!band || !IsWindow(band)) {
        band = nullptr;
        EnumChildWindows(explorer_, [](HWND w, LPARAM lp) -> BOOL {
            wchar_t cls[32];
            if (GetClassNameW(w, cls, 32) && !wcscmp(cls, L"MSTaskSwWClass")) { *reinterpret_cast<HWND*>(lp) = w; return FALSE; }
            return TRUE;
        }, (LPARAM)&band);
    }
    if (band && (HWND)GetPropW(hwnd_, kTaskbandProp) != band) SetPropW(hwnd_, kTaskbandProp, band);
}

HWND TrayHost::FindExplorerTray() {
    HWND h = FindWindowW(L"Shell_TrayWnd", nullptr);
    while (h && h == hwnd_) h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr);
    return h;
}

void TrayHost::MakeTopmost() {
    if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void TrayHost::BroadcastTaskbarCreated() {
    g_app.ignoreTaskbarCreatedUntil = GetTickCount() + 3000;
    SendNotifyMessageW(HWND_BROADCAST, g_app.msgTaskbarCreated, 0, 0);
}

void TrayHost::Reannounce() {
    MakeTopmost();          // make sure apps find our tray window, not Explorer's
    BroadcastTaskbarCreated();
}

void TrayHost::SetHostRect(const RECT& rc) {
    hostRect_ = rc;
    if (hwnd_)
        SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
}

void TrayHost::SetTrayArea(const RECT& screen) {
    if (EqualRect(&screen, &trayArea_) && notify_) return;
    trayArea_ = screen;
    if (!notify_) return;
    RECT host{};
    GetWindowRect(hwnd_, &host);
    SetWindowPos(notify_, nullptr, screen.left - host.left, screen.top - host.top,
                 std::max<LONG>(1, screen.right - screen.left), std::max<LONG>(1, screen.bottom - screen.top),
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

// Runs every 250 ms: keep our Shell_TrayWnd first in z-order (that's what
// FindWindow returns to apps) and survive Explorer restarts.
void CALLBACK TrayHost::MonitorTimer(HWND, UINT, UINT_PTR, DWORD) {
    TrayHost* self = g_app.tray.get();
    if (!self) return;

    if (self->waitingForExplorer_) {
        HWND ex = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (ex) {
            // Explorer is back: give it a moment to finish, then take over again.
            static DWORD seenAt = 0;
            if (!seenAt) seenAt = GetTickCount();
            if (GetTickCount() - seenAt > 1500) {
                seenAt = 0;
                self->waitingForExplorer_ = false;
                self->CreateWindows();
                self->BroadcastTaskbarCreated();
                self->startupStep_ = 1;
                self->ScheduleReannounce(2500);
            }
        }
        return;
    }

    if (!self->explorer_ || !IsWindow(self->explorer_)) {
        HWND ex = self->FindExplorerTray();
        if (!ex) {
            // Explorer died. Step aside so the new Explorer can create its taskbar.
            self->DestroyWindows();
            self->waitingForExplorer_ = true;
            return;
        }
        self->explorer_ = ex;
    }
    {
        HWND band = (HWND)GetPropW(self->hwnd_, kTaskbandProp);
        if (!band || !IsWindow(band)) self->LinkTaskband();   // new Explorer, or its task band wasn't ready yet
    }

    if (FindWindowW(L"Shell_TrayWnd", nullptr) != self->hwnd_ && !GameMode()) {   // (a game in fullscreen: later)
        // Explorer's tray got in front of ours: apps that registered icons just
        // now went there. Take the front spot back and ask them again.
        self->MakeTopmost();
        // (a few times at most: if Explorer's own taskbar keeps coming in front, asking every app again and again helps nobody)
        DWORD now = GetTickCount();
        if (now - self->lastReannounce_ > 600000) self->reannCount_ = 0;
        if (!self->reannTimer_ && now - self->lastReannounce_ > 10000 && self->reannCount_ < 3) {
            ++self->reannCount_;
            self->ScheduleReannounce(1500);
        }
    }
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK TrayHost::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    TrayHost* self = g_app.tray.get();
    if (!self) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_COPYDATA:
        return self->OnCopyData(h, msg, wp, lp);
    case WM_WINDOWPOSCHANGED: {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (pos && (pos->flags & SWP_SHOWWINDOW))   // never become visible
            SetWindowLongW(h, GWL_STYLE, GetWindowLongW(h, GWL_STYLE) & ~WS_VISIBLE);
        break;
    }
    default:
        break;
    }
    if (msg == WM_ACTIVATEAPP || msg == WM_COMMAND || msg >= WM_USER)
        return self->Forward(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT TrayHost::Forward(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!explorer_ || !IsWindow(explorer_)) explorer_ = FindExplorerTray();
    if (!explorer_) return DefWindowProcW(h, msg, wp, lp);
    if (msg == kForwardPost) {
        PostMessageW(explorer_, msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
    DWORD_PTR result = 0;
    SendMessageTimeoutW(explorer_, msg, wp, lp, SMTO_ABORTIFHUNG, 2000, &result);
    return (LRESULT)result;
}

int TrayHost::Find(HWND h, UINT uid, const GUID* guid) {
    if (guid) {   // a GUID names the icon by itself (one window can own several that differ only by GUID)
        for (size_t i = 0; i < icons_.size(); ++i)
            if (icons_[i].hasGuid && IsEqualGUID(icons_[i].guid, *guid)) return (int)i;
    }
    for (size_t i = 0; i < icons_.size(); ++i) {
        const auto& t = icons_[i];
        if (guid && t.hasGuid) continue;   // a different GUID icon of the same window
        if (h && t.hwnd == h && t.uid == uid) return (int)i;
    }
    return -1;
}

LRESULT TrayHost::OnCopyData(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
    if (!cds || !cds->lpData) return Forward(h, msg, wp, lp);

    if (cds->dwData == 1 && cds->cbData >= offsetof(SHELLTRAYDATA, nid) + offsetof(NID32, szTip)) {
        // Shell_NotifyIcon
        SHELLTRAYDATA d{};
        memcpy(&d, cds->lpData, std::min<size_t>(cds->cbData, sizeof d));
        const NID32& n = d.nid;
        HWND owner = Widen<HWND>(n.hWnd);
        bool useGuid = (n.uFlags & NIF_GUID) && !IsNullGuid(n.guidItem);
        int idx = Find(owner, n.uID, useGuid ? &n.guidItem : nullptr);
        bool ok = false;

        auto apply = [&](TrayIcon& t) {
            if (n.uFlags & NIF_MESSAGE) t.callbackMsg = n.uCallbackMessage;
            if (n.uFlags & NIF_ICON) {
                HICON src = Widen<HICON>(n.hIcon);
                HICON copy = src ? CopyIcon(src) : nullptr;   // apps may destroy theirs right after
                if (!copy && src) copy = (HICON)CopyImage(src, IMAGE_ICON, 0, 0, 0);
                // A copy that failed (or an app that sends "no icon" for a moment, seen with Steam) must not
                // make the icon vanish: keep the one we have. Without any, the app's own exe icon is drawn.
                if (copy || !t.icon) {
                    if (t.icon) DestroyIcon(t.icon);
                    t.icon = copy;
                    t.iconGen = NextTrayGen();   // unique: a removed and re-added icon must never reuse an old picture
                }
            }
            if (n.uFlags & NIF_TIP) t.tip = BoundedString(n.szTip, 128);
            if (n.uFlags & NIF_STATE) {
                if (n.dwStateMask & NIS_HIDDEN) t.hidden = (n.dwState & NIS_HIDDEN) != 0;
            }
            t.showTip = (n.uFlags & NIF_SHOWTIP) != 0;
            if (useGuid) { t.guid = n.guidItem; t.hasGuid = true; }
            if (owner) t.hwnd = owner;
        };

        switch (d.dwMessage) {
        case NIM_ADD:
        case NIM_MODIFY:
            if (idx >= 0) {
                apply(icons_[idx]);
                ok = true;
            } else if (d.dwMessage == NIM_ADD && owner) {
                TrayIcon t;
                t.hwnd = owner;
                t.uid = n.uID;
                apply(t);
                GetWindowThreadProcessId(owner, &t.pid);
                t.exe = ToLower(FileNameOf(ProcessImagePath(t.pid)));
                icons_.push_back(std::move(t));
                ok = true;
            }
            break;
        case NIM_DELETE:
            if (idx >= 0) {
                if (icons_[idx].icon) DestroyIcon(icons_[idx].icon);
                icons_.erase(icons_.begin() + idx);
                ok = true;
            }
            break;
        case NIM_SETFOCUS:
            ok = idx >= 0;
            break;
        case NIM_SETVERSION:
            if (idx >= 0) { icons_[idx].version = n.uVersion; ok = true; }
            break;
        }
        if (ok) g_app.InvalidateTray();

        // Moving (not mirroring): when an icon arrives, remove Explorer's
        // older copy of it so it doesn't linger in the bottom tray.
        if (ok && d.dwMessage == NIM_ADD && !g_app.settings.mirrorToExplorer) {
            if (!explorer_ || !IsWindow(explorer_)) explorer_ = FindExplorerTray();
            if (explorer_) {
                SHELLTRAYDATA del = d;
                del.dwMessage = NIM_DELETE;
                COPYDATASTRUCT c{1, (DWORD)std::min<size_t>(cds->cbData, sizeof del), &del};
                DWORD_PTR r = 0;
                SendMessageTimeoutW(explorer_, WM_COPYDATA, (WPARAM)hwnd_, (LPARAM)&c,
                                    SMTO_ABORTIFHUNG, 500, &r);
            }
        }

        if (g_app.settings.mirrorToExplorer || !ok) {
            LRESULT r = Forward(h, msg, wp, lp);
            return ok ? TRUE : r;
        }
        return ok ? TRUE : FALSE;
    }

    if (cds->dwData == 3 && cds->cbData >= sizeof(WINNOTIFYICONIDENTIFIER)) {
        // Shell_NotifyIconGetRect: tell apps where their icon is (for flyouts)
        WINNOTIFYICONIDENTIFIER id{};
        memcpy(&id, cds->lpData, sizeof id);
        HWND owner = Widen<HWND>(id.hWnd);
        int idx = Find(owner, id.uID, IsNullGuid(id.guidItem) ? nullptr : &id.guidItem);
        if (idx >= 0) {
            const RECT& r = icons_[idx].screenRect;
            if (id.dwMessage == 1) return MAKELPARAM(r.left, r.top);
            if (id.dwMessage == 2) return MAKELPARAM(r.right, r.bottom);
        }
        return Forward(h, msg, wp, lp);
    }

    // dwData 0 = SHAppBarMessage and anything else: Explorer handles it.
    return Forward(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
bool TrayHost::Visible(const TrayIcon& t) const {
    if (t.hidden || (!t.icon && t.exe.empty())) return false;   // no icon of its own: the bar draws the app's exe icon
    if (!t.callbackMsg && t.tip.empty()) return false;
    std::wstring tip = ToLower(t.tip);
    for (const auto& f : g_app.settings.hiddenTray) {
        if (f.empty()) continue;
        if (t.exe == f) return false;
        if (tip.find(f) != std::wstring::npos) return false;
    }
    return true;
}

bool TrayHost::PurgeDead() {
    bool changed = false;
    for (size_t i = 0; i < icons_.size();) {
        if (!IsWindow(icons_[i].hwnd)) {
            if (icons_[i].icon) DestroyIcon(icons_[i].icon);
            icons_.erase(icons_.begin() + i);
            changed = true;
        } else {
            ++i;
        }
    }
    return changed;
}

void TrayHost::Send(const TrayIcon& t, UINT mouseMsg, POINT pt) {
    if (!t.callbackMsg || !IsWindow(t.hwnd)) return;
    WPARAM w;
    LPARAM l;
    if (t.version > 3) {
        w = MAKEWPARAM((WORD)(SHORT)pt.x, (WORD)(SHORT)pt.y);
        l = MAKELPARAM(mouseMsg, t.uid);
    } else {
        w = t.uid;
        l = mouseMsg;
    }
    SendNotifyMessageW(t.hwnd, t.callbackMsg, w, l);
}

void TrayHost::MouseButton(TrayIcon& t, UINT msg, POINT pt) {
    // Let the app take focus so its menu closes when you click elsewhere.
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
        msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK) {
        AllowSetForegroundWindow(t.pid ? t.pid : ASFW_ANY);
    }
    Send(t, msg, pt);
    if (msg == WM_LBUTTONUP && t.version >= 3) Send(t, kNIN_SELECT, pt);
    if (msg == WM_RBUTTONUP && t.version >= 3) Send(t, WM_CONTEXTMENU, pt);
}

void TrayHost::HoverEnter(TrayIcon& t, POINT pt) {
    Send(t, WM_MOUSEHOVER, pt);
    if (t.version > 3) Send(t, kNIN_POPUPOPEN, pt);
}

void TrayHost::HoverLeave(TrayIcon& t, POINT pt) {
    Send(t, WM_MOUSELEAVE, pt);
    if (t.version > 3) Send(t, kNIN_POPUPCLOSE, pt);
}

void TrayHost::MouseMove(TrayIcon& t, POINT pt) {
    Send(t, WM_MOUSEMOVE, pt);
}
