// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline dock — pinned + running apps along the bottom, native Win32/Direct2D.
// Event driven: shell hook for windows, WinEvents for focus/moves, nothing polls.
#include "dock.h"
#include "app.h"
#include "menus.h"
#include "settings.h"
#include "icons.h"
#include "startmenu.h"
#include "applist.h"
#include "flyouts.h"
#include <ole2.h>
#include <unordered_map>
#include "nowplaying.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <appmodel.h>
#include <map>
#include <shellscalingapi.h>
#include <cmath>

namespace {

constexpr UINT_PTR T_ANIM = 1, T_REBUILD = 2, T_EVAL = 3, T_HIDEDELAY = 4, T_EXPLORER = 5,
                   T_PREVIEW = 6, T_PREVIEWHIDE = 7, T_MEDIACARD = 8, T_CLOCK = 9, T_MINIS = 10, T_MINIS_LATE = 11;
constexpr wchar_t kBadgeClass[] = L"SkylineDockBadges";
// Where each window's visible part sits inside its rect (Windows 11 has invisible resize borders), noted
// whenever a window comes to the front, so a minimized window's picture can leave them out.
std::unordered_map<HWND, RECT> g_frameMargins;
constexpr wchar_t kDockClass[] = L"SkylineDock";
constexpr wchar_t kTriggerClass[] = L"SkylineDockEdge";
constexpr float kHoverScale = 1.12f;   // "so minimal as possible"
constexpr float kPressScale = 0.94f;

const PROPERTYKEY kAumidKey = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

struct ACCENTPOLICY { int AccentState; int AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };
using SetWCA = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);

void Glass(HWND h) { ApplyGlassEffect(h, Part::Dock); }

std::wstring Lower(std::wstring s) { return ToLower(std::move(s)); }

std::wstring NormPath(std::wstring p) {
    for (auto& c : p) if (c == L'/') c = L'\\';
    return ToLower(p);
}

bool IsAppsFolder(const std::wstring& t) {
    return Lower(t.substr(0, 17)) == L"shell:appsfolder\\";
}

// Which windows belong on a taskbar — same rules Explorer uses.
bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if ((ex & WS_EX_TOOLWINDOW) && !appWindow) return false;
    if ((ex & WS_EX_NOACTIVATE) && !appWindow) return false;
    if (GetWindow(h, GW_OWNER) && !appWindow) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Shell_TrayWnd")) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;
    return GetWindowTextLengthW(h) > 0;
}

struct Ident { std::wstring aumid, exe, exeRaw; bool windowId = false; };
std::map<HWND, Ident> g_identCache;

// Chrome/Edge/Brave web apps (PWAs) carry ids like "Chrome._crx_<id>" or "MSEdge._crx_<id>"
bool IsWebAppId(const std::wstring& aumid) {
    return aumid.find(L"_crx_") != std::wstring::npos;
}

std::wstring WindowAumid(HWND h) {
    std::wstring out;
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(ps.Put())))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kAumidKey, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
        PropVariantClear(&v);
    }
    return out;
}

Ident Identify(HWND h) {
    auto it = g_identCache.find(h);
    if (it != g_identCache.end()) {
        // the window's own id can arrive late (web apps): look again while the cached one came from the process only
        if (!it->second.windowId) {
            std::wstring wid = WindowAumid(h);
            if (!wid.empty()) { it->second.aumid = wid; it->second.windowId = true; }
        }
        return it->second;
    }
    Ident id;
    id.aumid = WindowAumid(h);
    id.windowId = !id.aumid.empty();
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    id.exeRaw = ProcessImagePath(pid);
    // Helper processes that draw windows for their main app
    {
        std::wstring low = NormPath(id.exeRaw);
        size_t bin = low.find(L"\\bin\\cef\\");
        if (bin != std::wstring::npos && low.size() > 18 && low.compare(low.size() - 18, 18, L"steamwebhelper.exe") == 0)
            id.exeRaw = id.exeRaw.substr(0, bin) + L"\\steam.exe";
    }
    id.exe = NormPath(id.exeRaw);
    if (id.aumid.empty()) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (p) {
            UINT32 len = 0;
            if (GetApplicationUserModelId(p, &len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len) {
                std::wstring s(len, L'\0');
                if (GetApplicationUserModelId(p, &len, s.data()) == ERROR_SUCCESS) {
                    s.resize(wcslen(s.c_str()));
                    id.aumid = s;
                }
            }
            CloseHandle(p);
        }
    }
    g_identCache[h] = id;
    return id;
}

std::wstring AppPath(const wchar_t* exe) {
    std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
    wchar_t buf[MAX_PATH * 2];
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD size = sizeof buf;
        if (RegGetValueW(root, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
            std::wstring p = buf;
            if (!p.empty() && p.front() == L'"') p = p.substr(1, p.find(L'"', 1) - 1);
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return L"";
}

std::vector<std::wstring> DefaultPins() {
    std::vector<std::wstring> pins;
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    pins.push_back(std::wstring(win) + L"\\explorer.exe");

    bool browser = false;
    for (const wchar_t* b : {L"brave.exe", L"chrome.exe", L"firefox.exe"}) {
        std::wstring p = AppPath(b);
        if (!p.empty()) { pins.push_back(p); browser = true; }
    }
    if (!browser) {
        std::wstring edge = AppPath(L"msedge.exe");
        if (!edge.empty()) pins.push_back(edge);
    }
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamExe", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        std::wstring p = buf;
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) pins.push_back(p);
    }
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) &&
        GetFileAttributesW((std::wstring(local) + L"\\Discord").c_str()) != INVALID_FILE_ATTRIBUTES)
        pins.push_back(L"shell:AppsFolder\\com.squirrel.Discord.Discord");
    wchar_t roaming[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", roaming, MAX_PATH)) {
        std::wstring sp = std::wstring(roaming) + L"\\Spotify\\Spotify.exe";
        if (GetFileAttributesW(sp.c_str()) != INVALID_FILE_ATTRIBUTES) pins.push_back(sp);
    }
    pins.push_back(L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel");
    return pins;
}


std::vector<HWND> FindTaskbars() {
    std::vector<HWND> out;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        if (lstrcmpW(cls, L"Shell_TrayWnd") && lstrcmpW(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId()) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, (LPARAM)&out);
    return out;
}

bool IsFullscreen(HWND h) {
    if (!h || IsZoomed(h) || IsIconic(h)) return false;   // maximized is not fullscreen
    RECT r;
    if (!GetWindowRect(h, &r)) return false;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

} // namespace

// ===========================================================================
// Taskbar hider
// ===========================================================================
void CALLBACK TaskbarHider::OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h) return;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd")) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return;   // our own tray host
        ShowWindow(h, SW_HIDE);
        Vanish(h);
    }
}

// Hidden isn't always enough: on bright wallpapers the auto-hide strip and the
// taskbar's shadow can still show. An empty window region removes every pixel.
void TaskbarHider::Vanish(HWND h) {
    SetWindowRgn(h, CreateRectRgn(0, 0, 0, 0), TRUE);   // the window owns the region afterwards
}

void TaskbarHider::Hide() {
    wanted_ = true;   // if Explorer's taskbar isn't there yet, it's hidden when it appears (TaskbarCreated)
    auto bars = FindTaskbars();
    if (bars.empty()) return;
    const wchar_t* ini = g_app.settings.path.c_str();
    APPBARDATA abd{sizeof abd};
    abd.hWnd = bars.front();
    UINT state = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
    if (!(state & ABS_AUTOHIDE)) {
        // Auto-hide frees the space the taskbar reserves; remember to undo it.
        abd.lParam = state | ABS_AUTOHIDE;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        WritePrivateProfileStringW(L"Dock", L"RestoreAutohide", L"1", ini);
    }
    for (HWND h : bars) { ShowWindow(h, SW_HIDE); Vanish(h); }
    DWORD pid = 0;
    GetWindowThreadProcessId(bars.front(), &pid);
    if (hook_) UnhookWinEvent(hook_);
    hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, pid, 0, WINEVENT_OUTOFCONTEXT);
    active_ = true;
}

void TaskbarHider::Restore() {
    wanted_ = false;
    if (hook_) { UnhookWinEvent(hook_); hook_ = nullptr; }
    if (!active_) return;
    active_ = false;
    RestoreAfterCrash();
}

// Hand over to the next dock without showing the taskbar in between.
void TaskbarHider::Release() {
    wanted_ = false;
    if (hook_) { UnhookWinEvent(hook_); hook_ = nullptr; }
    active_ = false;
}

void TaskbarHider::RestoreAfterCrash() {
    const wchar_t* ini = g_app.settings.path.c_str();
    auto bars = FindTaskbars();
    for (HWND h : bars) { SetWindowRgn(h, nullptr, TRUE); ShowWindow(h, SW_SHOWNA); }
    if (GetPrivateProfileIntW(L"Dock", L"RestoreAutohide", 0, ini) && !bars.empty()) {
        APPBARDATA abd{sizeof abd};
        abd.hWnd = bars.front();
        UINT state = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
        abd.lParam = state & ~ABS_AUTOHIDE;
        SHAppBarMessage(ABM_SETSTATE, &abd);
        WritePrivateProfileStringW(L"Dock", L"RestoreAutohide", nullptr, ini);
    }
}

void TaskbarHider::ExplorerRestarted() {
    if (active_ || wanted_) Hide();
}

// ===========================================================================
// Dock
// ===========================================================================
// Files dragged from File Explorer onto the dock: only the Recycle Bin tile takes them
class DockDrop final : public IDropTarget {
public:
    explicit DockDrop(Dock* d) : dock_(d) {}
    void Detach() { dock_ = nullptr; }   // the dock is going away; OLE may still hold on to us for a moment
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files_ = d && d->QueryGetData(&fe) == S_OK;
        return DragOver(0, pt, eff);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* eff) override {
        DWORD allowed = *eff;
        bool over = files_ && dock_ && dock_->DropAt({pt.x, pt.y}, false, nullptr);
        *eff = !over ? DROPEFFECT_NONE : (allowed & DROPEFFECT_MOVE) ? DROPEFFECT_MOVE : (allowed & DROPEFFECT_COPY);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override { if (dock_) dock_->DropAt({-100000, -100000}, false, nullptr); return S_OK; }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* eff) override {
        if (files_ && dock_) dock_->DropAt({pt.x, pt.y}, true, d);
        *eff = DROPEFFECT_NONE;   // we put them in the Recycle Bin ourselves: the source must not "finish the move"
        return S_OK;
    }
private:
    Dock* dock_;
    bool files_ = false;
    ULONG ref_ = 1;
};

bool Dock::Create(HMONITOR mon, bool owner) {
    mon_ = mon ? mon : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    owner_ = owner;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDockClass;
        RegisterClassExW(&wc);
        wc.lpfnWndProc = TriggerProc;
        wc.lpszClassName = kTriggerClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kDockClass, L"Skyline Dock", WS_POPUP,
                            0, 0, 100, 60, nullptr, nullptr, g_app.hinst, this);
    if (!hwnd_) return false;
    trigger_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, kTriggerClass, L"",
                               WS_POPUP, 0, 0, 10, 2, nullptr, nullptr, g_app.hinst, this);
    if (trigger_) SetLayeredWindowAttributes(trigger_, 0, 1, LWA_ALPHA);

    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon_, MDT_EFFECTIVE_DPI, &dx, &dy);
    dpi_ = dx;

    BOOL exclude = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
    DWORD corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof corner);
    Glass(hwnd_);

    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                           0, 0, 0, 0, hwnd_, nullptr, g_app.hinst, nullptr);
    if (tip_) {
        TOOLINFOW ti{sizeof ti};
        ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        ti.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    shellMsg_ = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(hwnd_);
    cloakHook_ = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, OnCloak, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (owner_ && g_app.settings.hideTaskbar) taskbar_.Hide();

    LoadPinned();
    Rebuild();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (g_app.settings.dockMinimize) SetTimer(hwnd_, T_MINIS_LATE, 600, nullptr);   // once every dock exists: sort out whose windows are whose
    OnForeground(GetForegroundWindow());
    return true;
}

// With smart hide off the dock is always there, so it reserves its strip at the
// bottom of the screen like the taskbar does: maximized windows stop above it.
void Dock::UpdateReservation(bool force) {
    bool want = !g_app.settings.smartHide && !IsRectEmpty(&home_);
    APPBARDATA abd{sizeof abd};
    abd.hWnd = hwnd_;
    if (!want) {
        if (appbar_) { SHAppBarMessage(ABM_REMOVE, &abd); appbar_ = false; }
        reserved_ = {};
        return;
    }
    if (!appbar_) {
        abd.uCallbackMessage = WM_APP + 30;
        appbar_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
        if (!appbar_) return;
    }
    static bool busy = false;   // ABM_SETPOS can call us back
    if (busy) return;
    busy = true;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon_, &mi);
    int reserve = mi.rcMonitor.bottom - home_.top;
    abd.uEdge = ABE_BOTTOM;
    abd.rc = {mi.rcMonitor.left, mi.rcMonitor.bottom - reserve, mi.rcMonitor.right, mi.rcMonitor.bottom};
    if (!force && EqualRect(&abd.rc, &reserved_)) { busy = false; return; }   // nothing changed: don't make Explorer re-flow windows
    reserved_ = abd.rc;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - reserve;
    SHAppBarMessage(ABM_SETPOS, &abd);
    busy = false;
}

Dock::~Dock() {
    if (appbar_ && hwnd_) {
        APPBARDATA abd{sizeof abd};
        abd.hWnd = hwnd_;
        SHAppBarMessage(ABM_REMOVE, &abd);
        appbar_ = false;
    }
    if (locHook_) UnhookWinEvent(locHook_);
    if (cloakHook_) UnhookWinEvent(cloakHook_);
    if (hwnd_) DeregisterShellHookWindow(hwnd_);
    if (trashNotify_) SHChangeNotifyDeregister(trashNotify_);
    if (drop_) { static_cast<DockDrop*>(drop_)->Detach(); if (hwnd_) RevokeDragDrop(hwnd_); drop_->Release(); drop_ = nullptr; }
    taskbar_.Restore();
    ReleaseMinis();
    if (badges_) DestroyWindow(badges_);
    if (badgeDC_) { SelectObject(badgeDC_, badgeOld_); DeleteDC(badgeDC_); }
    if (badgeBmp_) DeleteObject(badgeBmp_);
    if (tip_) DestroyWindow(tip_);
    if (trigger_) { SetWindowLongPtrW(trigger_, GWLP_USERDATA, 0); DestroyWindow(trigger_); }
    if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); }
}

// ---------------------------------------------------------------------------
void Dock::LoadPinned() {
    if (!g_app.settings.pinnedSet) {
        g_app.settings.pinned = DefaultPins();
        g_app.settings.pinnedSet = true;
        SavePinned();
    }
}

void Dock::SavePinned() {
    std::wstring joined;
    std::vector<std::wstring> list;
    if (!apps_.empty()) {
        for (auto& a : apps_) if (a.pinned && !a.start) list.push_back(a.target);
        g_app.settings.pinned = list;
    } else {
        list = g_app.settings.pinned;
    }
    for (size_t i = 0; i < list.size(); ++i) { if (i) joined += L"|"; joined += list[i]; }
    WritePrivateProfileStringW(L"Dock", L"Pinned", joined.c_str(), g_app.settings.path.c_str());
    for (auto& d : g_app.docks) if (d.get() != this) d->RebuildSoon();   // docks on other screens follow
}

void Dock::RebuildSoon() { if (hwnd_) SetTimer(hwnd_, T_REBUILD, 60, nullptr); }

void Dock::LoadIcon(App& a) {
    if (a.start || a.img) return;
    // Two exact sizes: normal (drawn pixel-for-pixel) and hover (12 % bigger)
    int px = (int)std::lround(g_app.settings.dockIconDip * dpi_ / 96.0f);
    int pxH = (int)std::lround(g_app.settings.dockIconDip * kHoverScale * dpi_ / 96.0f);
    a.img = LoadAppImage(a.target, px);
    a.imgHover = LoadAppImage(a.target, pxH);
    // No picture for that target? Some apps give their windows an id that has no Start-menu entry to take an
    // icon from (Steam's "Valve.Steam.Client" on many PCs): then the icon was simply empty. Try the exe behind
    // the window, and last of all the icon the window itself shows.
    if (!a.img && !a.exe.empty() && Lower(a.exe) != Lower(a.target)) {
        a.img = LoadAppImage(a.exe, px);
        a.imgHover = LoadAppImage(a.exe, pxH);
    }
    if (!a.img) {
        for (HWND h : a.windows) {
            HICON ic = nullptr;
            DWORD_PTR r = 0;
            if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &r) && r) ic = (HICON)r;
            if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
            if (!ic && SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &r) && r) ic = (HICON)r;
            if (!ic) continue;
            a.img = ImageFromIcon(ic, px);
            a.imgHover = ImageFromIcon(ic, pxH);
            if (a.img) break;
        }
    }
    if (a.img && !a.imgHover) a.imgHover = a.img;
    if (a.name.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.target.c_str(), nullptr, IID_PPV_ARGS(item.Put())))) {
            PWSTR dn = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) { a.name = dn; CoTaskMemFree(dn); }
        }
    }
    if (!a.exe.empty() && !IsAppsFolder(a.target)) {
        std::wstring d = FileDescriptionOf(a.target);
        if (!d.empty()) a.name = d;
    }
    if (a.name.empty()) a.name = FileNameOf(a.target);
}

void Dock::Rebuild() {
    // Mid-press or mid-drag: indices into apps_ are in use. Try again shortly.
    if (dragging_ || downIdx_ >= 0) { SetTimer(hwnd_, T_REBUILD, 200, nullptr); return; }

    // Forget identities of windows that are gone
    for (auto it = g_identCache.begin(); it != g_identCache.end();)
        it = IsWindow(it->first) ? std::next(it) : g_identCache.erase(it);

    std::vector<App> old = std::move(apps_);
    apps_.clear();
    auto takeOld = [&](App& a, auto match) {
        for (auto& o : old) {
            if (match(o)) {
                a.img = o.img; a.imgHover = o.imgHover;
                a.bmp = o.bmp; a.bmpHover = o.bmpHover;
                if (a.name.empty()) a.name = o.name;
                a.scale = o.scale;
                a.attention = o.attention;
                return;
            }
        }
    };

    App start;
    start.start = true;
    start.pinned = true;
    start.name = L"Start";
    takeOld(start, [](const App& o) { return o.start; });
    apps_.push_back(std::move(start));

    for (const auto& t : g_app.settings.pinned) {
        App a;
        a.target = t;
        a.pinned = true;
        if (IsAppsFolder(t)) a.aumid = Lower(t.substr(17));
        else a.exe = NormPath(t);
        takeOld(a, [&](const App& o) { return !o.start && Lower(o.target) == Lower(t); });
        apps_.push_back(std::move(a));
    }

    // Running windows, in z-order
    struct Win { HWND h; Ident id; };
    std::vector<Win> wins;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (IsTaskbarWindow(h)) reinterpret_cast<std::vector<Win>*>(lp)->push_back({h, Identify(h)});
        return TRUE;
    }, (LPARAM)&wins);

    // A browser window without its own id yet may be a web app that's still starting: look again shortly (a few times only)
    {
        static std::map<HWND, int> rechecks;
        for (auto it = rechecks.begin(); it != rechecks.end();) it = IsWindow(it->first) ? std::next(it) : rechecks.erase(it);
        bool again = false;
        for (auto& w : wins) {
            if (!w.id.aumid.empty() && IsWebAppId(Lower(w.id.aumid))) continue;
            std::wstring n = FileNameOf(w.id.exe);
            if (n != L"chrome.exe" && n != L"msedge.exe" && n != L"brave.exe" && n != L"vivaldi.exe" && n != L"opera.exe") continue;
            if (rechecks[w.h]++ < 4) again = true;
        }
        if (again) SetTimer(hwnd_, T_REBUILD, 700, nullptr);
    }

    std::vector<App> running;
    std::map<size_t, std::wstring> pinExe;   // exe behind a Start-menu (AppsFolder) pin, looked up only when needed
    for (auto& w : wins) {
        std::wstring aumid = Lower(w.id.aumid);
        // A web app (PWA) runs as the browser's exe but has its own id: it belongs to its own icon, never the browser's
        bool webApp = IsWebAppId(aumid);
        App* owner = nullptr;
        for (auto& a : apps_)   // its own id first
            if (!a.start && !aumid.empty() && a.aumid == aumid) { owner = &a; break; }
        if (!owner && !webApp)
            for (auto& a : apps_)
                if (!a.start && !a.exe.empty() && a.exe == w.id.exe && (a.aumid.empty() || !IsWebAppId(a.aumid))) { owner = &a; break; }
        // Not exactly: maybe a pinned app that runs as another exe — a newer version (Discord's app-1.0.x),
        // a helper in its folder (Steam's steamwebhelper.exe), or a Start-menu pin whose shortcut starts a launcher
        if (!owner && !webApp && !w.id.exe.empty()) {
            for (size_t i = 0; i < apps_.size() && !owner; ++i) {
                App& a = apps_[i];
                if (a.start || !a.pinned || IsWebAppId(a.aumid)) continue;
                std::wstring pe = a.exe;
                if (pe.empty() && !a.aumid.empty()) {
                    auto it = pinExe.find(i);
                    if (it == pinExe.end()) it = pinExe.emplace(i, ToLower(EntryFor(a.target).exe)).first;   // asked once per rebuild
                    pe = it->second;
                }
                if (!pe.empty() && SameAppExe(pe, w.id.exe)) owner = &a;
            }
        }
        if (!owner) {
            for (auto& a : running) {
                if (!aumid.empty() ? a.aumid == aumid : (a.aumid.empty() && a.exe == w.id.exe)) { owner = &a; break; }
            }
        }
        if (!owner) {
            App a;
            if (!aumid.empty()) { a.aumid = aumid; a.target = L"shell:AppsFolder\\" + w.id.aumid; a.exe = w.id.exe; }
            else { a.exe = w.id.exe; a.target = w.id.exeRaw; }
            running.push_back(std::move(a));
            owner = &running.back();
        }
        owner->windows.push_back(w.h);
    }

    // Keep the order unpinned apps had before; new ones go at the end
    std::vector<App> ordered;
    for (auto& o : old) {
        if (o.pinned) continue;
        for (auto& r : running) {
            if (!r.target.empty() && Lower(r.target) == Lower(o.target)) { ordered.push_back(std::move(r)); r.target.clear(); break; }
        }
    }
    for (auto& r : running) if (!r.target.empty()) ordered.push_back(std::move(r));
    for (auto& a : ordered) {
        std::wstring t = Lower(a.target);
        takeOld(a, [&](const App& o) { return !o.start && Lower(o.target) == t; });
        apps_.push_back(std::move(a));
    }

    for (auto& a : apps_) LoadIcon(a);
    if (hover_ >= (int)apps_.size()) hover_ = -1;
    Layout();
    Invalidate();
}

// ---------------------------------------------------------------------------
void Dock::Layout() {
    float S = (float)g_app.settings.dockIconDip;
    float slot = S + 8, sep = 11, pad = 6;
    float H = std::round(S * kHoverScale + 13);

    float x = pad;
    for (size_t i = 0; i < apps_.size(); ++i) {
        App& a = apps_[i];
        if (i > 0 && !a.pinned && apps_[i - 1].pinned) x += sep;       // divider before running apps
        a.rc = D2D1::RectF(x, 0, x + slot, H);
        x += slot;
    }
    // The plugins after the divider (Now Playing, clock, minimized windows, stack, Recycle Bin), in the order the
    // user dragged them into ([Dock] PluginOrder)
    mediaRc_ = clockRc_ = stack_.rc = trash_.rc = D2D1::RectF(0, 0, 0, 0);
    int ipx = (int)std::lround(S * kHoverScale * dpi_ / 96.0f);
    RefreshMinis();
    bool group = false;
    auto place = [&](D2D1_RECT_F& rc) {
        if (!group) { x += sep; group = true; }
        rc = D2D1::RectF(x, 0, x + slot, H);
        x += slot;
    };
    if (pluginOrder_.empty()) LoadPluginOrder();
    for (int kind : pluginOrder_) {
        switch (kind) {
        case PlugMedia: if (NowPlaying::InDock()) place(mediaRc_); break;
        case PlugClock: if (g_app.settings.dockClock) place(clockRc_); break;
        case PlugWindows: for (auto& m : minis_) place(m.rc); break;   // newest last, like on a Mac
        case PlugStack: if (g_app.settings.dockStack) place(stack_.rc); break;
        case PlugTrash: if (g_app.settings.dockTrash) place(trash_.rc); break;
        }
    }
    ArmClock();
    if (g_app.settings.dockStack) {
        if (!stack_.img && !stack_.tried) { stack_.tried = true; stack_.img = LoadAppImage(StackFolder(), ipx); }
    } else {
        stack_.img.Reset();
        stack_.bmp.Reset();
        stack_.tried = false;
    }
    if (g_app.settings.dockTrash) {
        if (!trash_.img && !trash_.tried) { trash_.tried = true; TrashChanged(); }
        if (!trashNotify_ && hwnd_) {   // Windows tells us when the Recycle Bin changes: nothing to check ourselves
            PIDLIST_ABSOLUTE bin = nullptr;
            if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &bin)) && bin) {
                SHChangeNotifyEntry e{bin, TRUE};
                trashNotify_ = SHChangeNotifyRegister(hwnd_, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                                      SHCNE_ALLEVENTS, WM_APP + 47, 1, &e);
                CoTaskMemFree(bin);
            }
        }
        if (!drop_ && hwnd_) {
            drop_ = new DockDrop(this);
            if (FAILED(RegisterDragDrop(hwnd_, drop_))) { drop_->Release(); drop_ = nullptr; }
        }
    } else {
        trash_.img.Reset();
        trash_.bmp.Reset();
        trash_.tried = false;
        if (trashNotify_) { SHChangeNotifyDeregister(trashNotify_); trashNotify_ = 0; }
        if (drop_) { static_cast<DockDrop*>(drop_)->Detach(); if (hwnd_) RevokeDragDrop(hwnd_); drop_->Release(); drop_ = nullptr; }
    }
    float W = x + pad;

    HMONITOR mon = mon_;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int w = (int)(W * dpi_ / 96.0f + 0.5f), h = (int)(H * dpi_ / 96.0f + 0.5f);
    int margin = (int)(6 * dpi_ / 96.0f);
    int left = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
    int top = mi.rcMonitor.bottom - h - margin;
    home_ = {left, top, left + w, top + h};

    int y = top + (int)(slide_ * (h + margin + 2));
    SetWindowPos(hwnd_, flyoutOpen_ ? nullptr : HWND_TOPMOST, left, y, w, h,
                 SWP_NOACTIVATE | (flyoutOpen_ ? SWP_NOZORDER : 0));
    if (trigger_) {
        RECT t = RevealStrip();
        SetWindowPos(trigger_, HWND_TOPMOST, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE);
    }
    UpdateReservation();
}

bool Dock::CreateResources() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              (float)dpi_, (float)dpi_);
    if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                                 rt_.Put())))
        return false;
    rt_->CreateSolidColorBrush(g_app.theme.fg, brush_.Put());
    return true;
}

void Dock::DiscardResources() {
    for (auto& a : apps_) { a.bmp.Reset(); a.bmpHover.Reset(); }
    stack_.bmp.Reset();
    trash_.bmp.Reset();
    for (auto& m : minis_) m.bmp.Reset();
    mediaBmp_.Reset();
    mediaGen_ = 0;
    brush_.Reset();
    rt_.Reset();
}

void Dock::Paint() {
    if (!CreateResources()) return;
    const Theme& th = g_app.theme;
    float S = (float)g_app.settings.dockIconDip;
    D2D1_SIZE_F sz = rt_->GetSize();
    float base = sz.height - 8.5f;        // icons sit on this line

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (D2D1_COLOR_F tint = GlassTint(Part::Dock); tint.a > 0) {   // see-through: the tint, painted over the glass
        brush_->SetColor(tint);
        rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), brush_.Get());
    }
    PaintVibe(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), Part::Dock);   // Vibe: the colours of the hour
    PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), 8, 2);   // Liquid Glass / Nordlys
    {   // a fine light edge along the top, fading out to the sides: gives the glass depth
        ComPtr<ID2D1GradientStopCollection> gsc;
        ComPtr<ID2D1LinearGradientBrush> rim;
        float a = th.dark ? 0.20f : 0.55f;
        D2D1_GRADIENT_STOP gs[3] = {{0, D2D1::ColorF(1, 1, 1, 0)}, {0.5f, D2D1::ColorF(1, 1, 1, a)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
        if (SUCCEEDED(rt_->CreateGradientStopCollection(gs, 3, gsc.Put())) &&
            SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(sz.width, 0)), gsc.Get(), rim.Put())))
            rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, 1.0f), rim.Get());
    }

    for (size_t i = 0; i < apps_.size(); ++i) {
        App& a = apps_[i];
        float cx = (a.rc.left + a.rc.right) / 2;

        if (i > 0 && !a.pinned && apps_[i - 1].pinned) {
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.16f));
            float sx = a.rc.left - 7;
            rt_->FillRectangle(D2D1::RectF(sx - 0.5f, sz.height * 0.22f, sx + 0.5f, sz.height * 0.78f), brush_.Get());
        }

        float s = S * a.scale;
        D2D1_RECT_F ir = D2D1::RectF(cx - s / 2, base - s, cx + s / 2, base);
        float opacity = (dragging_ && removing_ && (int)i == downIdx_) ? 0.35f : 1.0f;

        if (a.start && AuroraOn()) {   // Nordlys: a soft glow of northern light behind the Start button
            D2D1_COLOR_F ac[3];
            AuroraColors(ac);
            D2D1_COLOR_F g0 = ac[0];
            g0.a = 0.45f * g_app.settings.auroraStrength / 100.0f;
            D2D1_COLOR_F g1 = g0;
            g1.a = 0;
            D2D1_GRADIENT_STOP gst[2] = {{0, g0}, {1, g1}};
            ComPtr<ID2D1GradientStopCollection> gsc;
            ComPtr<ID2D1RadialGradientBrush> rb;
            float gcy = (ir.top + ir.bottom) / 2;
            if (SUCCEEDED(rt_->CreateGradientStopCollection(gst, 2, gsc.Put())) &&
                SUCCEEDED(rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, gcy), D2D1::Point2F(0, 0), s * 0.75f, s * 0.75f), gsc.Get(), rb.Put())))
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, gcy), s * 0.75f, s * 0.75f), rb.Get());
        }
        if (a.start && g_app.settings.startButton == 1) {
            // Skyline's own icon, drawn sharp at any size: a dark rounded tile with the top bar (blue) and two menu lines
            float t = s * 0.92f, r = t * 0.23f;   // as big as the other app icons look
            float tx = cx - t / 2, ty = (ir.top + ir.bottom) / 2 - t / 2;
            D2D1_RECT_F tile = D2D1::RectF(tx, ty, tx + t, ty + t);
            ComPtr<ID2D1LinearGradientBrush> bg;
            D2D1_GRADIENT_STOP gs[2] = {{0, D2D1::ColorF(0.17f, 0.17f, 0.19f)}, {1, D2D1::ColorF(0.09f, 0.09f, 0.10f)}};
            ComPtr<ID2D1GradientStopCollection> gsc;
            rt_->CreateGradientStopCollection(gs, 2, gsc.Put());
            if (gsc) rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(tx, ty), D2D1::Point2F(tx, ty + t)), gsc.Get(), bg.Put());
            if (bg) rt_->FillRoundedRectangle(D2D1::RoundedRect(tile, r, r), bg.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, th.dark ? 0.14f : 0.06f));   // a fine light rim, so it never melts into dark glass
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f, tile.right - 0.5f, tile.bottom - 0.5f), r, r), brush_.Get(), 1.0f);
            float bh = t * 0.088f, bx = tx + t * 0.15f;
            auto bar = [&](float y, float w, D2D1_COLOR_F c) {
                brush_->SetColor(c);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, y - bh / 2, bx + w, y + bh / 2), bh / 2, bh / 2), brush_.Get());
            };
            bar(ty + t * 0.25f, t * 0.70f, D2D1::ColorF(0.36f, 0.79f, 0.98f));   // the top bar
            bar(ty + t * 0.47f, t * 0.47f, D2D1::ColorF(1, 1, 1, 0.96f));
            bar(ty + t * 0.65f, t * 0.59f, D2D1::ColorF(1, 1, 1, 0.96f));
            continue;
        }
        if (a.start) {
            // Windows logo: four soft squares
            float q = s * 0.36f, g = s * 0.035f;
            float ox = cx - q - g / 2, oy = (ir.top + ir.bottom) / 2 - q - g / 2;
            ComPtr<ID2D1LinearGradientBrush> lg;
            D2D1_GRADIENT_STOP stops[2] = {{0, D2D1::ColorF(0.33f, 0.77f, 1.0f)}, {1, D2D1::ColorF(0.0f, 0.47f, 0.84f)}};
            ComPtr<ID2D1GradientStopCollection> sc;
            rt_->CreateGradientStopCollection(stops, 2, sc.Put());
            if (sc) rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(ox, oy),
                                                   D2D1::Point2F(ox + 2 * q + g, oy + 2 * q + g)), sc.Get(), lg.Put());
            ID2D1Brush* br = lg ? (ID2D1Brush*)lg.Get() : (ID2D1Brush*)brush_.Get();
            for (int yy = 0; yy < 2; ++yy)
                for (int xx = 0; xx < 2; ++xx) {
                    float l = ox + xx * (q + g), t = oy + yy * (q + g);
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, l + q, t + q), q * 0.12f, q * 0.12f), br);
                }
            continue;
        }

        if (!a.bmp && a.img) rt_->CreateBitmapFromWicBitmap(a.img.Get(), nullptr, a.bmp.Put());
        if (!a.bmpHover && a.imgHover) rt_->CreateBitmapFromWicBitmap(a.imgHover.Get(), nullptr, a.bmpHover.Put());
        // At rest: the exact-size image, pixel for pixel. Growing: the bigger one, scaled down smoothly.
        ID2D1Bitmap* b = (a.scale > 1.001f && a.bmpHover) ? a.bmpHover.Get() : a.bmp.Get();
        if (b) DrawImageHQ(rt_.Get(), b, ir, opacity);

        if (!a.windows.empty()) {
            bool active = std::find(a.windows.begin(), a.windows.end(), fg_) != a.windows.end();
            D2D1_COLOR_F dot = a.attention ? D2D1::ColorF(1.0f, 0.62f, 0.2f, 1)
                                           : D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, active ? 0.95f : 0.5f);
            brush_->SetColor(dot);
            float r = active ? 2.1f : 1.7f;
            if (active) {   // a soft halo behind the dot of the active app
                D2D1_COLOR_F hc = dot; hc.a = 0.22f;
                brush_->SetColor(hc);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, sz.height - 3.5f), r + 2.0f, r + 2.0f), brush_.Get());
                brush_->SetColor(dot);
            }
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, sz.height - 3.5f), r, r), brush_.Get());
        }
    }

    // One divider before the plugins, whichever comes first
    {
        float first = 1e9f;
        auto consider = [&](const D2D1_RECT_F& rc) { if (rc.right > rc.left) first = std::min(first, rc.left); };
        consider(mediaRc_);
        consider(clockRc_);
        consider(stack_.rc);
        consider(trash_.rc);
        for (auto& m : minis_) consider(m.rc);
        if (first < 1e8f) {
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.16f));
            float sx = first - 7;
            rt_->FillRectangle(D2D1::RectF(sx - 0.5f, sz.height * 0.22f, sx + 0.5f, sz.height * 0.78f), brush_.Get());
        }
    }
    // Now Playing tile: the album art as an icon, a thin progress line underneath
    if (mediaRc_.right > mediaRc_.left) {
        const auto& np = NowPlaying::Current();
        float cx = (mediaRc_.left + mediaRc_.right) / 2, s = S * mediaScale_;
        D2D1_RECT_F ir = D2D1::RectF(cx - s / 2, base - s, cx + s / 2, base);
        float rad = s * 0.22f;
        if (mediaGen_ != np.gen) { mediaBmp_.Reset(); mediaGen_ = np.gen; }
        if (!mediaBmp_ && np.art) rt_->CreateBitmapFromWicBitmap(np.art.Get(), nullptr, mediaBmp_.Put());
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        g_gfx.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ir, rad, rad), geo.Put());
        if (mediaBmp_ && geo) {
            ComPtr<ID2D1Layer> layer;
            rt_->CreateLayer(layer.Put());
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get()), layer.Get());
            rt_->DrawBitmap(mediaBmp_.Get(), ir, np.playing ? 1.0f : 0.6f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->PopLayer();
        } else {
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.10f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(ir, rad, rad), brush_.Get());
        }
        if (!np.playing) {   // paused: a small pause sign on the cover
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
            float px = cx, py = (ir.top + ir.bottom) / 2, bw = s * 0.07f, bh = s * 0.26f;
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px - bw * 1.8f, py - bh / 2, px - bw * 0.6f, py + bh / 2), 1, 1), brush_.Get());
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px + bw * 0.6f, py - bh / 2, px + bw * 1.8f, py + bh / 2), 1, 1), brush_.Get());
        }
        if (np.duration > 0) {
            float w = S * 0.62f, y = sz.height - 4.5f;
            D2D1_RECT_F track = D2D1::RectF(cx - w / 2, y - 1, cx + w / 2, y + 1);
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.18f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(track, 1, 1), brush_.Get());
            float f = (float)std::clamp(np.position / np.duration, 0.0, 1.0);
            brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.9f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left, track.top, track.left + w * f, track.bottom), 1, 1), brush_.Get());
        }
    }

    if (clockRc_.right > clockRc_.left) PaintClock(S, base, sz.height);
    if (!minis_.empty()) PaintMinis(S, base, sz.height, false);   // the divider is drawn once, above
    if (stack_.rc.right > stack_.rc.left) PaintTile(stack_, S, base, sz.height, false);
    if (trash_.rc.right > trash_.rc.left) PaintTile(trash_, S, base, sz.height, false);

    static int fails = 0;
    if (FAILED(rt_->EndDraw())) { DiscardResources(); if (++fails < 3) Invalidate(); } else fails = 0;
    RenderBadges();
}

std::wstring StackFolder() {
    const std::wstring& f = g_app.settings.dockStackFolder;
    if (!f.empty()) {
        DWORD a = GetFileAttributesW(f.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return f;
    }
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
    return out;
}

void Dock::PaintTile(Tile& t, float S, float base, float height, bool divider) {
    const Theme& th = g_app.theme;
    if (divider) {
        brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.16f));
        float sx = t.rc.left - 7;
        rt_->FillRectangle(D2D1::RectF(sx - 0.5f, height * 0.22f, sx + 0.5f, height * 0.78f), brush_.Get());
    }
    float cx = (t.rc.left + t.rc.right) / 2, s = S * t.scale;
    D2D1_RECT_F ir = D2D1::RectF(cx - s / 2, base - s, cx + s / 2, base);
    if (&t == &trash_ && dropOnTrash_) {   // files are being dragged over it
        brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.30f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left - 4, ir.top - 4, ir.right + 4, ir.bottom + 4), s * 0.25f, s * 0.25f), brush_.Get());
    }
    if (!t.bmp && t.img) rt_->CreateBitmapFromWicBitmap(t.img.Get(), nullptr, t.bmp.Put());
    if (t.bmp) DrawImageHQ(rt_.Get(), t.bmp.Get(), ir);
}

// ---------------------------------------------------------------------------
// Minimized windows in the dock (setting): little live pictures, like on a Mac
void CALLBACK Dock::OnMinimize(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (!h || obj != OBJID_WINDOW || !g_app.settings.dockMinimize) return;
    for (auto& d : g_app.docks)
        if (d && d->hwnd_) {
            SetTimer(d->hwnd_, T_MINIS, 40, nullptr);         // look once the window has settled
            SetTimer(d->hwnd_, T_MINIS_LATE, 450, nullptr);   // and once more after Windows' own animation
        }
}

void Dock::ReleaseMinis() {
    for (auto& m : minis_) if (m.thumb) DwmUnregisterThumbnail(m.thumb);
    minis_.clear();
    if (badges_) ShowWindow(badges_, SW_HIDE);
}

int Dock::MiniHover() const {
    for (int i = 0; i < (int)minis_.size(); ++i) if (minis_[i].hover) return i;
    return -1;
}

void Dock::RefreshMinis() {
    if (!g_app.settings.dockMinimize) { if (!minis_.empty()) ReleaseMinis(); return; }
    static HWINEVENTHOOK hook = nullptr;   // Windows tells us when something is minimized or comes back
    if (!hook) hook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnMinimize, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    // more than one dock: each shows its own screen's windows (count the dock windows: while the docks are
    // being made, g_app.docks isn't complete yet)
    int dockWindows = 0;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, kDockClass, nullptr)) != nullptr;) ++dockWindows;
    bool several = dockWindows > 1 || g_app.docks.size() > 1;
    std::vector<std::pair<HWND, ComPtr<IWICBitmapSource>>> now;
    for (auto& a : apps_) {
        if (a.start) continue;
        for (HWND h : a.windows)
            if (IsWindow(h) && IsIconic(h) && IsWindowVisible(h) && (!several || MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == mon_)) {
                BOOL cloaked = FALSE;   // apps that hide in the tray when minimized aren't in the dock either
                DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
                if (!cloaked) now.push_back({h, a.img});
            }
    }
    // back on screen or closed: out of the dock
    for (auto it = minis_.begin(); it != minis_.end();) {
        bool still = std::any_of(now.begin(), now.end(), [&](const auto& p) { return p.first == it->h; });
        if (still) { ++it; continue; }
        if (it->thumb) DwmUnregisterThumbnail(it->thumb);
        it = minis_.erase(it);
    }
    for (auto it = g_frameMargins.begin(); it != g_frameMargins.end();) it = IsWindow(it->first) ? std::next(it) : g_frameMargins.erase(it);
    // the window's size and its visible part (measured again each time: the first look can be mid-animation)
    auto measure = [](Mini& m) {
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(m.h, &wp);
        SIZE q{};
        if (m.thumb && SUCCEEDED(DwmQueryThumbnailSourceSize(m.thumb, &q)) && q.cx > 40 && q.cy > 40) m.src = q;
        else m.src = {wp.rcNormalPosition.right - wp.rcNormalPosition.left, wp.rcNormalPosition.bottom - wp.rcNormalPosition.top};
        if (m.src.cx < 40 || m.src.cy < 40) m.src = {800, 600};
        // leave out the invisible resize borders: as measured when it was last in front, or Windows' usual size for them
        RECT mg{};
        auto known = g_frameMargins.find(m.h);
        if (known != g_frameMargins.end()) mg = known->second;
        else if (GetWindowLongW(m.h, GWL_STYLE) & WS_THICKFRAME) {
            UINT wdpi = GetDpiForWindow(m.h);
            if (!wdpi) wdpi = 96;
            LONG b = GetSystemMetricsForDpi(SM_CXSIZEFRAME, wdpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, wdpi);
            mg = {b, 0, b, b};
        }
        // the top edge is visible on a normal window; a maximized one has hidden borders on every side
        mg.top = (wp.flags & WPF_RESTORETOMAXIMIZED) ? std::max(mg.top, mg.left) : 0;
        m.crop = {mg.left, mg.top, m.src.cx - mg.right, m.src.cy - mg.bottom};
        if (m.crop.right - m.crop.left < 40 || m.crop.bottom - m.crop.top < 40) m.crop = {0, 0, m.src.cx, m.src.cy};
    };
    // newly minimized: in at the end
    bool added = false;
    for (auto& p : now) {
        auto f = std::find_if(minis_.begin(), minis_.end(), [&](const Mini& m) { return m.h == p.first; });
        if (f != minis_.end()) {
            if (!f->icon && p.second) { f->icon = p.second; f->badge.Reset(); }
            measure(*f);
            continue;
        }
        Mini m;
        m.h = p.first;
        m.icon = p.second;
        if (FAILED(DwmRegisterThumbnail(hwnd_, m.h, &m.thumb))) m.thumb = nullptr;
        measure(m);
        minis_.push_back(std::move(m));
        added = true;
    }
    if (added) StartAnimating();   // grow in
}

void Dock::PaintMinis(float S, float base, float height, bool divider) {
    const Theme& th = g_app.theme;
    if (divider && !minis_.empty()) {
        brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.16f));
        float sx = minis_.front().rc.left - 7;
        rt_->FillRectangle(D2D1::RectF(sx - 0.5f, height * 0.22f, sx + 0.5f, height * 0.78f), brush_.Get());
    }
    float k = dpi_ / 96.0f;
    for (auto& m : minis_) {
        float cx = (m.rc.left + m.rc.right) / 2;
        float e = 1 - (1 - m.appear) * (1 - m.appear);                  // ease out
        float box = S * m.scale * (0.35f + 0.65f * e);                  // the icon square it fills
        // Every window gets the same card, whatever its shape, so the row looks even: the picture fills the
        // card and is trimmed at the sides or the bottom (never the top, where the title is)
        constexpr float kCard = 1.45f;
        float w = box * 0.96f, h = w / kCard;
        RECT fill = m.crop;
        {
            float cw = (float)(fill.right - fill.left), ch = (float)(fill.bottom - fill.top);
            if (cw / std::max(1.0f, ch) > kCard) {   // wider than the card: trim both sides
                LONG nw = (LONG)std::lround(ch * kCard);
                fill.left += (LONG)((cw - nw) / 2);
                fill.right = fill.left + nw;
            } else {                                 // taller: trim the bottom
                fill.bottom = fill.top + (LONG)std::lround(cw / kCard);
            }
        }
        float cy = base - S * m.scale / 2;                               // centred where an icon would be
        // pixel-aligned, so the live picture is crisp
        D2D1_RECT_F r = D2D1::RectF(std::round((cx - w / 2) * k) / k, std::round((cy - h / 2) * k) / k,
                                    std::round((cx + w / 2) * k) / k, std::round((cy + h / 2) * k) / k);
        m.pic = r;
        // a soft shadow underneath, so it floats like a window
        for (int i = 4; i >= 1; --i) {
            float g = i * 1.1f;
            brush_->SetColor(D2D1::ColorF(0, 0, 0, (th.dark ? 0.085f : 0.06f) * e));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - g, r.top - g + 2, r.right + g, r.bottom + g + 2), 3 + g, 3 + g), brush_.Get());
        }
        if (m.thumb) {
            DWM_THUMBNAIL_PROPERTIES p{};
            p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            p.rcDestination = {(LONG)std::lround(r.left * k), (LONG)std::lround(r.top * k), (LONG)std::lround(r.right * k), (LONG)std::lround(r.bottom * k)};
            p.rcSource = fill;
            p.fVisible = TRUE;
            p.opacity = (BYTE)std::lround(255 * std::clamp(e, 0.0f, 1.0f));
            p.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(m.thumb, &p);
        } else if (m.icon) {   // no live picture to be had: the app's icon instead
            if (!m.bmp) rt_->CreateBitmapFromWicBitmap(m.icon.Get(), nullptr, m.bmp.Put());
            if (m.bmp) DrawImageHQ(rt_.Get(), m.bmp.Get(), D2D1::RectF(cx - box / 2, cy - box / 2, cx + box / 2, cy + box / 2), e);
        }
    }
}

// The app badges: on a see-through layer just above the dock (DWM puts the live pictures above our own drawing)
void Dock::RenderBadges() {
    bool want = !minis_.empty() && IsWindowVisible(hwnd_);
    if (!want) {
        if (badges_ && IsWindowVisible(badges_)) ShowWindow(badges_, SW_HIDE);
        return;
    }
    if (!badges_) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_app.hinst;
            wc.lpszClassName = kBadgeClass;
            RegisterClassExW(&wc);
            reg = true;
        }
        // owned by the dock: always just above it, topmost exactly when the dock is
        badges_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBadgeClass, L"", WS_POPUP,
                                  0, 0, 1, 1, hwnd_, nullptr, g_app.hinst, nullptr);
        if (!badges_) return;
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(badges_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
    }
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    SIZE sz{wr.right - wr.left, wr.bottom - wr.top};
    if (sz.cx <= 0 || sz.cy <= 0) return;
    if (!badgeDC_ || sz.cx != badgeSize_.cx || sz.cy != badgeSize_.cy) {
        if (badgeDC_) { SelectObject(badgeDC_, badgeOld_); DeleteDC(badgeDC_); badgeDC_ = nullptr; }
        if (badgeBmp_) { DeleteObject(badgeBmp_); badgeBmp_ = nullptr; }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sz.cx, -sz.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        badgeBmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!badgeBmp_) return;
        badgeDC_ = CreateCompatibleDC(nullptr);
        badgeOld_ = (HBITMAP)SelectObject(badgeDC_, badgeBmp_);
        badgeSize_ = sz;
    }
    if (!badgeRt_) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  (float)dpi_, (float)dpi_);
        if (FAILED(g_gfx.d2d->CreateDCRenderTarget(&props, badgeRt_.Put()))) return;
        for (auto& m : minis_) m.badge.Reset();
    }
    RECT rc{0, 0, sz.cx, sz.cy};
    if (FAILED(badgeRt_->BindDC(badgeDC_, &rc))) return;
    float S = (float)g_app.settings.dockIconDip;
    badgeRt_->BeginDraw();
    badgeRt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (auto& m : minis_) {
        if (!m.icon || !m.thumb || m.appear < 0.05f) continue;   // no live picture: the icon itself is shown, no badge needed
        if (!m.badge) badgeRt_->CreateBitmapFromWicBitmap(m.icon.Get(), nullptr, m.badge.Put());
        if (!m.badge) continue;
        float e = 1 - (1 - m.appear) * (1 - m.appear);
        float ic = S * m.scale * 0.40f * (0.6f + 0.4f * e);
        // over the picture's lower right corner, a little past its edges, like on a Mac
        float l = m.pic.right - ic * 0.74f, t = m.pic.bottom - ic * 0.74f;
        float maxB = sz.cy * 96.0f / dpi_ - 1.5f;
        if (t + ic > maxB) t = maxB - ic;
        DrawImageHQ(badgeRt_.Get(), m.badge.Get(), D2D1::RectF(l, t, l + ic, t + ic), e);
    }
    if (FAILED(badgeRt_->EndDraw())) { badgeRt_.Reset(); return; }
    POINT dst{wr.left, wr.top}, src{0, 0};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(badges_, nullptr, &dst, &sz, badgeDC_, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(badges_)) ShowWindow(badges_, SW_SHOWNOACTIVATE);
}

void Dock::MiniMenu(int i, POINT screen) {
    if (i < 0 || i >= (int)minis_.size()) return;
    HWND w = minis_[i].h;
    enum { CShow = 1, CClose };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CShow, TR(L"Vis", L"Show"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CClose, TR(L"Lukk vindu", L"Close window"));
    SetForegroundWindow(hwnd_);
    MenuGuard guard;
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
                               screen.x, home_.top - (int)(4 * dpi_ / 96.0f), hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (!IsWindow(w)) return;
    if (cmd == CShow) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
    else if (cmd == CClose) PostMessageW(w, WM_CLOSE, 0, 0);   // the app may still ask to save first
}

// ---------------------------------------------------------------------------
// The plugins' order (drag them around, within their part of the dock)
void Dock::LoadPluginOrder() {
    static const wchar_t* names[] = {L"media", L"clock", L"windows", L"stack", L"trash"};
    wchar_t buf[128] = {};
    GetPrivateProfileStringW(L"Dock", L"PluginOrder", L"", buf, 128, g_app.settings.path.c_str());
    pluginOrder_.clear();
    std::wstring all = buf;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t c = all.find(L',', pos);
        std::wstring n = all.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos);
        for (int k = 0; k < PlugCount; ++k)
            if (n == names[k] && std::find(pluginOrder_.begin(), pluginOrder_.end(), k) == pluginOrder_.end()) pluginOrder_.push_back(k);
        if (c == std::wstring::npos) break;
        pos = c + 1;
    }
    // anything not mentioned (new plugins, or a fresh start): in the usual place
    for (int k = 0; k < PlugCount; ++k)
        if (std::find(pluginOrder_.begin(), pluginOrder_.end(), k) == pluginOrder_.end()) {
            auto at = pluginOrder_.end();
            for (auto it = pluginOrder_.begin(); it != pluginOrder_.end(); ++it) if (*it > k) { at = it; break; }
            pluginOrder_.insert(at, k);
        }
}

void Dock::SavePluginOrder() {
    static const wchar_t* names[] = {L"media", L"clock", L"windows", L"stack", L"trash"};
    std::wstring v;
    for (int k : pluginOrder_) { if (!v.empty()) v += L","; v += names[k]; }
    IniWrite(L"Dock", L"PluginOrder", v);
    for (auto& d : g_app.docks)   // the other screens' docks follow
        if (d && d.get() != this) { d->pluginOrder_ = pluginOrder_; d->Layout(); d->Invalidate(); }
}

int Dock::PluginAt(float x) const {
    auto in = [&](const D2D1_RECT_F& rc) { return rc.right > rc.left && x >= rc.left && x < rc.right; };
    if (in(mediaRc_)) return PlugMedia;
    if (in(clockRc_)) return PlugClock;
    if (in(stack_.rc)) return PlugStack;
    if (in(trash_.rc)) return PlugTrash;
    for (auto& m : minis_) if (in(m.rc)) return PlugWindows;   // the minimized windows move as one group
    return -1;
}

void Dock::DragPluginTo(float x) {
    // where each visible plugin is now (the minimized windows as one block)
    auto span = [&](int kind, float& l, float& r) {
        l = 1e9f; r = -1e9f;
        auto add = [&](const D2D1_RECT_F& rc) { if (rc.right > rc.left) { l = std::min(l, rc.left); r = std::max(r, rc.right); } };
        switch (kind) {
        case PlugMedia: add(mediaRc_); break;
        case PlugClock: add(clockRc_); break;
        case PlugWindows: for (auto& m : minis_) add(m.rc); break;
        case PlugStack: add(stack_.rc); break;
        case PlugTrash: add(trash_.rc); break;
        }
        return r > l;
    };
    std::vector<int> others, hidden;
    std::vector<float> centres;
    for (int k : pluginOrder_) {
        if (k == plugDown_) continue;
        float l, r;
        if (span(k, l, r)) { others.push_back(k); centres.push_back((l + r) / 2); }
        else hidden.push_back(k);
    }
    size_t at = 0;
    while (at < others.size() && centres[at] < x) ++at;   // past the middle of a neighbour: swap places with it
    std::vector<int> order = others;
    order.insert(order.begin() + at, plugDown_);
    order.insert(order.end(), hidden.begin(), hidden.end());
    if (order == pluginOrder_) return;
    pluginOrder_ = order;
    Layout();
    Invalidate();
}

void Dock::TrashChanged() {
    SHQUERYRBINFO q{sizeof q};
    bool full = SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) && q.i64NumItems > 0;
    if (full == trashFull_ && trash_.img) return;
    trashFull_ = full;
    int ipx = (int)std::lround(g_app.settings.dockIconDip * kHoverScale * dpi_ / 96.0f);
    trash_.img = LoadAppImage(L"shell:RecycleBinFolder", ipx);   // Windows' own empty/full icon
    trash_.bmp.Reset();
    Invalidate();
}

bool Dock::DropAt(POINT screen, bool drop, IDataObject* data) {
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    float x = c.x * 96.0f / dpi_;
    bool over = trash_.rc.right > trash_.rc.left && x >= trash_.rc.left - 4 && x < trash_.rc.right + 4;
    if (over != dropOnTrash_ && !drop) { dropOnTrash_ = over; Invalidate(); }
    if (!drop || !over || !data) return over;
    dropOnTrash_ = false;
    Invalidate();
    // the files go to the Recycle Bin (they can be restored from there)
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(data->GetData(&fe, &sm))) return false;
    auto* files = new std::vector<std::wstring>;
    if (HDROP hd = (HDROP)GlobalLock(sm.hGlobal)) {
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(hd, i, nullptr, 0);   // long paths too
            std::wstring p(len + 1, L'\0');
            if (DragQueryFileW(hd, i, p.data(), len + 1)) { p.resize(len); files->push_back(p); }
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    // Explorer waits inside the drop until we return: do the deleting afterwards, from our own message loop
    if (files->empty() || !PostMessageW(hwnd_, WM_APP + 48, 0, (LPARAM)files)) { delete files; return false; }
    return true;
}

void Dock::RecycleFiles(std::vector<std::wstring>* files) {
    std::unique_ptr<std::vector<std::wstring>> own(files);
    ComPtr<IFileOperation> op;
    if (own->empty() || FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.Put())))) return;
    // To the Recycle Bin without asking; but if something can't go there (too big, network drive), Windows warns first
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING);
    for (const auto& f : *own) {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(it.Put())))) op->DeleteItem(it.Get(), nullptr);
    }
    MenuGuard guard;   // a dialog may come up: don't let a settings change rebuild (free) this dock meanwhile
    op->PerformOperations();
    TrashChanged();
}

void Dock::PluginMenu(bool trash, POINT screen) {
    enum { COpen = 1, CEmpty, CFolder };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, COpen, TR(L"Åpne", L"Open"));
    if (trash) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (trashFull_ ? 0 : MF_GRAYED), CEmpty, TR(L"Tøm papirkurven …", L"Empty Recycle Bin …"));
    } else {
        AppendMenuW(m, MF_STRING, CFolder, TR(L"Åpne i Utforsker", L"Open in File Explorer"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CEmpty, TR(L"Velg mappe …", L"Choose folder …"));
    }
    SetForegroundWindow(hwnd_);
    MenuGuard guard;
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
                               screen.x, home_.top - (int)(4 * dpi_ / 96.0f), hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (trash) {
        if (cmd == COpen) ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
        else if (cmd == CEmpty) { SHEmptyRecycleBinW(hwnd_, nullptr, 0); TrashChanged(); }   // Windows asks first
    } else {
        if (cmd == COpen) {
            RECT r{home_.left + (LONG)(stack_.rc.left * dpi_ / 96.0f), home_.top, home_.left + (LONG)(stack_.rc.right * dpi_ / 96.0f), home_.bottom};
            Flyouts::ShowStack(r, dpi_);
        } else if (cmd == CFolder) ShellExecuteW(nullptr, L"open", StackFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (cmd == CEmpty) ShowSettings(SettingsPage::Plugins);
    }
}

void Dock::ArmClock() {
    if (!hwnd_) return;
    if (!g_app.settings.dockClock) { KillTimer(hwnd_, T_CLOCK); return; }
    SYSTEMTIME st;
    GetLocalTime(&st);
    bool perSecond = g_app.settings.dockClockAnalog && g_app.settings.dockClockSeconds;
    UINT ms = perSecond ? 1000 - st.wMilliseconds : (60 - st.wSecond) * 1000 - st.wMilliseconds;
    SetTimer(hwnd_, T_CLOCK, std::max(ms, 20u) + 5, nullptr);   // just after the boundary
}

void Dock::PaintClock(float S, float base, float height) {
    const Theme& th = g_app.theme;
    SYSTEMTIME st;
    GetLocalTime(&st);
    float cx = (clockRc_.left + clockRc_.right) / 2, s = S * clockScale_;
    D2D1_RECT_F ir = D2D1::RectF(cx - s / 2, base - s, cx + s / 2, base);
    float cy = (ir.top + ir.bottom) / 2;

    if (g_app.settings.dockClockAnalog) {
        // a light watch face (dark in light mode would look like a hole), like the Mac's Clock icon
        float r = s / 2;
        D2D1_COLOR_F face = th.dark ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 1) : D2D1::ColorF(0.13f, 0.13f, 0.14f, 1);
        D2D1_COLOR_F ink = th.dark ? D2D1::ColorF(0.10f, 0.10f, 0.11f, 1) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 1);
        brush_->SetColor(face);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_.Get());
        brush_->SetColor(D2D1::ColorF(0, 0, 0, th.dark ? 0.25f : 0.4f));
        rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 0.25f, r - 0.25f), brush_.Get(), 0.5f);
        brush_->SetColor(ink);
        for (int i = 0; i < 12; ++i) {   // hour marks
            float a = i * 3.14159265f / 6;
            float r1 = r * (i % 3 == 0 ? 0.70f : 0.78f), r2 = r * 0.88f;
            rt_->DrawLine(D2D1::Point2F(cx + r1 * sinf(a), cy - r1 * cosf(a)), D2D1::Point2F(cx + r2 * sinf(a), cy - r2 * cosf(a)),
                          brush_.Get(), i % 3 == 0 ? s * 0.045f : s * 0.025f, nullptr);
        }
        auto hand = [&](float turns, float len, float width, float back) {
            float a = turns * 2 * 3.14159265f;
            D2D1_POINT_2F from = D2D1::Point2F(cx - back * r * sinf(a), cy + back * r * cosf(a));
            D2D1_POINT_2F to = D2D1::Point2F(cx + len * r * sinf(a), cy - len * r * cosf(a));
            ComPtr<ID2D1StrokeStyle> round;
            D2D1_STROKE_STYLE_PROPERTIES sp{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
            g_gfx.d2d->CreateStrokeStyle(sp, nullptr, 0, round.Put());
            rt_->DrawLine(from, to, brush_.Get(), width, round.Get());
        };
        float sec = st.wSecond, min = st.wMinute + sec / 60.0f, hr = (st.wHour % 12) + min / 60.0f;
        hand(hr / 12.0f, 0.50f, s * 0.07f, 0.0f);
        hand(min / 60.0f, 0.74f, s * 0.05f, 0.0f);
        if (g_app.settings.dockClockSeconds) {
            brush_->SetColor(D2D1::ColorF(0.95f, 0.45f, 0.10f, 1));   // orange, like the Mac
            hand(sec / 60.0f, 0.80f, std::max(1.0f, s * 0.022f), 0.18f);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.045f, s * 0.045f), brush_.Get());
        } else {
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), s * 0.045f, s * 0.045f), brush_.Get());
        }
        return;
    }

    // Digital: a glass tile with the time, the weekday and date small underneath
    float rad = s * 0.22f;
    brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, clockHover_ ? 0.16f : 0.11f));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(ir, rad, rad), brush_.Get());
    if (!clockBig_ || clockFmtS_ != S) {
        clockFmtS_ = S;
        clockBig_.Reset();
        clockSmall_.Reset();
        auto make = [](float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out) {
            g_gfx.dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           size, L"", out.Put());
            if (!out) g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", out.Put());
            if (out) {
                out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        };
        make(S * 0.36f, DWRITE_FONT_WEIGHT_SEMI_BOLD, clockBig_);
        make(S * 0.22f, DWRITE_FONT_WEIGHT_NORMAL, clockSmall_);
    }
    if (!clockBig_ || !clockSmall_) return;
    // the time scales with the hover, so draw it through a transform
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(clockScale_, clockScale_, D2D1::Point2F(cx, base)) * old);
    D2D1_RECT_F box = D2D1::RectF(cx - S / 2, base - S, cx + S / 2, base);
    wchar_t t[16];
    swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
    wchar_t d[32] = {};
    GetDateFormatEx(g_norsk ? L"nb-NO" : L"en-US", 0, &st, L"ddd d", d, 32, nullptr);
    std::wstring ds = d;
    if (!ds.empty()) ds[0] = towupper(ds[0]);
    ds.erase(std::remove(ds.begin(), ds.end(), L'.'), ds.end());
    brush_->SetColor(th.fg);
    rt_->DrawTextW(t, (UINT32)wcslen(t), clockBig_.Get(), D2D1::RectF(box.left - 4, box.top + S * 0.10f, box.right + 4, box.top + S * 0.60f), brush_.Get());
    brush_->SetColor(th.fgDim);
    rt_->DrawTextW(ds.c_str(), (UINT32)ds.size(), clockSmall_.Get(), D2D1::RectF(box.left - 4, box.top + S * 0.56f, box.right + 4, box.top + S * 0.90f), brush_.Get());
    rt_->SetTransform(old);
}

void Dock::MediaChanged(bool layout) {
    if (!hwnd_) return;
    bool show = NowPlaying::InDock();
    bool has = mediaRc_.right > mediaRc_.left;
    if (layout || show != has) { Layout(); Invalidate(); return; }
    if (has && shown_ && IsWindowVisible(hwnd_)) Invalidate();   // only redraw when the tile is actually there to see
}

void Dock::UpdateMediaHover(POINT p) {
    float x = p.x * 96.0f / dpi_;
    bool inClock = clockRc_.right > clockRc_.left && x >= clockRc_.left && x < clockRc_.right;
    if (inClock != clockHover_) {
        clockHover_ = inClock;
        clockTarget_ = inClock ? kHoverScale : 1.0f;
        StartAnimating();
    }
    for (Tile* t : {&stack_, &trash_}) {
        bool on = t->rc.right > t->rc.left && x >= t->rc.left && x < t->rc.right;
        if (on != t->hover) { t->hover = on; t->target = on ? kHoverScale : 1.0f; StartAnimating(); }
    }
    bool miniChanged = false;
    for (auto& m : minis_) {
        bool on = x >= m.rc.left && x < m.rc.right;
        if (on != m.hover) { m.hover = on; m.target = on ? kHoverScale : 1.0f; miniChanged = true; }
    }
    if (miniChanged) { StartAnimating(); UpdateLabel(); }
    bool in = mediaRc_.right > mediaRc_.left && x >= mediaRc_.left && x < mediaRc_.right;
    if (in == mediaHover_) return;
    mediaHover_ = in;
    mediaTarget_ = in ? kHoverScale : 1.0f;
    StartAnimating();
    if (in) SetTimer(hwnd_, T_MEDIACARD, 320, nullptr);
    else KillTimer(hwnd_, T_MEDIACARD);
}

// ---------------------------------------------------------------------------
// Animation: exponential ease, timer only runs while something is moving.
void Dock::StartAnimating() {
    if (!animating_) { animating_ = true; SetTimer(hwnd_, T_ANIM, 15, nullptr); }
}

void Dock::Animate() {
    bool moving = false;
    for (auto& a : apps_) {
        float d = a.targetScale - a.scale;
        if (std::fabs(d) > 0.002f) { a.scale += d * 0.35f; moving = true; }
        else a.scale = a.targetScale;
    }
    {
        float md = mediaTarget_ - mediaScale_;
        if (std::fabs(md) > 0.002f) { mediaScale_ += md * 0.35f; moving = true; }
        else mediaScale_ = mediaTarget_;
        float cd = clockTarget_ - clockScale_;
        if (std::fabs(cd) > 0.002f) { clockScale_ += cd * 0.35f; moving = true; }
        else clockScale_ = clockTarget_;
        for (Tile* t : {&stack_, &trash_}) {
            float td = t->target - t->scale;
            if (std::fabs(td) > 0.002f) { t->scale += td * 0.35f; moving = true; }
            else t->scale = t->target;
        }
        for (auto& m : minis_) {
            float td = m.target - m.scale;
            if (std::fabs(td) > 0.002f) { m.scale += td * 0.35f; moving = true; }
            else m.scale = m.target;
            if (m.appear < 0.998f) { m.appear += (1 - m.appear) * 0.22f; moving = true; }
            else m.appear = 1;
        }
    }
    float d = slideTarget_ - slide_;
    bool sliding = std::fabs(d) > 0.004f;
    if (sliding) { slide_ += d * 0.3f; moving = true; }
    else slide_ = slideTarget_;

    HMONITOR mon = mon_;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int h = home_.bottom - home_.top;
    int margin = (int)(6 * dpi_ / 96.0f);
    int y = home_.top + (int)(slide_ * (h + margin + 2));
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    if (wr.top != y || wr.left != home_.left)   // only icons animating: the window itself stays put
        SetWindowPos(hwnd_, flyoutOpen_ ? nullptr : HWND_TOPMOST, home_.left, y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | (flyoutOpen_ ? SWP_NOZORDER : 0));

    if (!sliding && slide_ >= 1.0f && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        if (trigger_ && !fullscreen_) ShowWindow(trigger_, SW_SHOWNOACTIVATE);
    }
    Invalidate();
    if (!moving) { KillTimer(hwnd_, T_ANIM); animating_ = false; }
}

// ---------------------------------------------------------------------------
// Smart hide
void Dock::OnForeground(HWND h) {
    fg_ = h;
    if (g_app.settings.dockMinimize && h && !IsIconic(h)) {
        RECT wr{}, ext{};
        if (GetWindowRect(h, &wr) && SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ext, sizeof ext)))
            g_frameMargins[h] = {std::max(0L, ext.left - wr.left), std::max(0L, ext.top - wr.top), std::max(0L, wr.right - ext.right), std::max(0L, wr.bottom - ext.bottom)};
    }
    for (auto& a : apps_)
        if (std::find(a.windows.begin(), a.windows.end(), h) != a.windows.end()) a.attention = false;

    // Watch only the focused app's windows move (cheap, and no cursor noise)
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (pid != locPid_) {
        if (locHook_) UnhookWinEvent(locHook_);
        locHook_ = nullptr;
        locPid_ = pid;
        if (pid && pid != GetCurrentProcessId())
            locHook_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnLocation, pid, 0,
                                       WINEVENT_OUTOFCONTEXT);
    }
    SetTimer(hwnd_, T_EVAL, 40, nullptr);
    Invalidate();
}

void CALLBACK Dock::OnLocation(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW) return;
    for (auto& d : g_app.docks) if (h == d->fg_) SetTimer(d->hwnd_, T_EVAL, 60, nullptr);
}

void CALLBACK Dock::OnCloak(HWINEVENTHOOK, DWORD, HWND, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW) return;
    for (auto& d : g_app.docks) SetTimer(d->hwnd_, T_REBUILD, 120, nullptr);
}

// Start, Search, Quick settings, notification centre, Win+X …
static bool IsShellFlyout(HWND f) {
    if (!f) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    std::wstring exe = ToLower(FileNameOf(ProcessImagePath(pid)));
    static const wchar_t* hosts[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"searchapp.exe",
                                     L"shellexperiencehost.exe", L"shellhost.exe"};
    for (auto h : hosts) if (exe == h) return true;
    wchar_t cls[64] = {};
    GetClassNameW(f, cls, 64);
    return !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"NotifyIconOverflowWindow") ||
           !lstrcmpW(cls, L"XamlExplorerHostIslandWindow") || !lstrcmpW(cls, L"TopLevelWindowForOverflowXamlIsland");
}

int Dock::TopEdge(HMONITOR mon) const {
    if (mon != mon_ || IsRectEmpty(&home_)) return INT_MAX;
    return home_.top;
}

void Dock::SetFullscreen(bool fs) {
    if (fs == fullscreen_) return;
    fullscreen_ = fs;
    if (fs) {
        SetHover(-1);
        HidePreview();
        shown_ = false;
        slide_ = slideTarget_ = 1.0f;          // no animation over a game
        ShowWindow(hwnd_, SW_HIDE);
        if (tip_) { TOOLINFOW ti{sizeof ti}; ti.hwnd = hwnd_; ti.uId = 1; SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); }
        if (trigger_) ShowWindow(trigger_, SW_HIDE);   // no edge reveal in games
    } else {
        FlushPending();
        Evaluate();
        if (!shown_ && trigger_) ShowWindow(trigger_, SW_SHOWNOACTIVATE);
    }
}

void Dock::FlushPending() {
    if (fullscreen_ || GameMode() || !pendingRes_) return;
    pendingRes_ = false;
    UpdateReservation(true);
}

void Dock::KeepOnTop() {
    if (!hwnd_ || fullscreen_ || flyoutOpen_ || !shown_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Dock::StartMenuChanged(bool open) {
    if (open) {
        if (fullscreen_) return;
        SetShown(true);
        if (!IsShellFlyout(GetForegroundWindow())) KeepOnTop();   // over SkyLauncher too
    } else {
        SetTimer(hwnd_, T_EVAL, 60, nullptr);   // smart hide decides again
    }
}

void Dock::Evaluate() {
    if (fullscreen_) return;
    if (Flyouts::StackOpen()) { SetShown(true); return; }   // its stack is open above it: stay
    HWND f = GetForegroundWindow();
    if (IsShellFlyout(f)) {
        // Start/Search aren't app windows: don't hide for them. Just step behind
        // them so they're drawn on top of the dock, never under it.
        flyoutOpen_ = true;
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(hwnd_, f, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (StartMenuOpen()) SetShown(true);   // Start is open: the dock comes up with it
        return;   // otherwise keep whatever state the dock had
    }
    if (flyoutOpen_) {
        flyoutOpen_ = false;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (StartMenuOpen()) { SetShown(true); return; }   // SkyMenu / SkyLauncher
    if (!g_app.settings.smartHide || mouseInside_ || dragging_ || preview_.MouseInside() || NowPlaying::CardHasMouse() ||
        (shown_ && CursorInRevealZone())) { SetShown(true); return; }
    bool overlap = false;
    if (f && IsWindowVisible(f) && !IsIconic(f)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(f, &pid);
        wchar_t cls[64] = {};
        GetClassNameW(f, cls, 64);
        bool desktop = !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
        if (pid == GetCurrentProcessId()) {
            // Our own popup menus: leave the dock as it is. The Settings window is a
            // normal window though, and the dock should hide under it like any other.
            wchar_t own[64] = {};
            GetClassNameW(f, own, 64);
            if (lstrcmpW(own, L"SkylineSettings")) return;
        }
        if (!desktop) {
            RECT r{};
            if (FAILED(DwmGetWindowAttribute(f, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(f, &r);
            RECT i;
            overlap = IntersectRect(&i, &r, &home_) != FALSE;
        }
    }
    SetShown(!overlap);
}

// The dock plus the gap below it down to the screen edge. While the mouse is
// in here the dock stays up; otherwise hovering the gap made it bounce.
bool Dock::CursorInRevealZone() const {
    if (IsRectEmpty(&home_)) return false;
    POINT c;
    if (!GetCursorPos(&c)) return false;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon_, &mi);
    RECT z = home_;
    z.bottom = mi.rcMonitor.bottom;
    // the gap below the dock, as wide as the reveal strip
    RECT gap = RevealStrip();
    gap.top = home_.bottom;
    return PtInRect(&z, c) || PtInRect(&gap, c);
}

// The invisible 2 px strip along the bottom edge that brings the dock back.
// Wider than the dock so you don't have to aim (Settings → Dock).
RECT Dock::RevealStrip() const {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon_, &mi);
    static const int extraDip[] = {0, 150, 400, 100000};
    int extra = MulDiv(extraDip[std::clamp(g_app.settings.dockRevealWidth, 0, 3)], dpi_, 96);
    // the very corners stay free for hot corners
    RECT r{std::max<LONG>(mi.rcMonitor.left + 4, home_.left - extra), mi.rcMonitor.bottom - 2,
           std::min<LONG>(mi.rcMonitor.right - 4, home_.right + extra), mi.rcMonitor.bottom};
    return r;
}

void Dock::SetShown(bool s) {
    if (s == shown_) return;
    shown_ = s;
    slideTarget_ = s ? 0.0f : 1.0f;
    if (s) {
        if (trigger_) ShowWindow(trigger_, SW_HIDE);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    } else {
        if (hover_ >= 0) SetHover(-1);
        HidePreview();
    }
    StartAnimating();
}

// ---------------------------------------------------------------------------
int Dock::HitTest(POINT client) const {
    float x = client.x * 96.0f / dpi_;
    for (int i = 0; i < (int)apps_.size(); ++i)
        if (x >= apps_[i].rc.left && x < apps_[i].rc.right) return i;
    return -1;
}

void Dock::SetHover(int idx) {
    if (idx == hover_) return;
    hover_ = idx;
    for (int i = 0; i < (int)apps_.size(); ++i)
        apps_[i].targetScale = (i == hover_ && !dragging_) ? kHoverScale : 1.0f;
    StartAnimating();

    // Window previews: wait a moment the first time, then follow the mouse instantly
    bool wants = g_app.settings.dockPreview && hover_ >= 0 && !dragging_ && !apps_[hover_].start &&
                 !apps_[hover_].windows.empty();
    if (wants) {
        KillTimer(hwnd_, T_PREVIEWHIDE);
        if (preview_.Visible()) ShowPreview(hover_);
        else SetTimer(hwnd_, T_PREVIEW, 450, nullptr);
    } else {
        KillTimer(hwnd_, T_PREVIEW);
        if (preview_.Visible()) {
            if (hover_ < 0 && !dragging_) SetTimer(hwnd_, T_PREVIEWHIDE, 300, nullptr);   // time to reach it
            else HidePreview();
        }
    }
    UpdateLabel();
}

void Dock::ShowPreview(int idx) {
    if (idx < 0 || idx >= (int)apps_.size()) { HidePreview(); return; }
    if (idx == previewIdx_ && preview_.Visible()) return;
    App& a = apps_[idx];
    std::vector<HWND> live;
    for (HWND h : a.windows) if (IsWindow(h)) live.push_back(h);
    if (live.empty()) { HidePreview(); return; }
    previewIdx_ = idx;
    int cx = home_.left + (int)((a.rc.left + a.rc.right) / 2 * dpi_ / 96.0f);
    int bottom = home_.top - MulDiv(8, dpi_, 96);
    preview_.Show(hwnd_, live, a.img.Get(), cx, bottom, dpi_);
    UpdateLabel();   // the name is in the preview now
}

void Dock::HidePreview() {
    KillTimer(hwnd_, T_PREVIEW);
    KillTimer(hwnd_, T_PREVIEWHIDE);
    previewIdx_ = -1;
    if (preview_.Visible()) { preview_.Hide(); UpdateLabel(); }
}

void Dock::UpdateLabel() {
    if (!tip_) return;
    TOOLINFOW ti{sizeof ti};
    ti.hwnd = hwnd_;
    ti.uId = 1;
    static std::wstring text;
    const D2D1_RECT_F* at = nullptr;
    if (hover_ >= 0 && hover_ < (int)apps_.size() && !dragging_ && !preview_.Visible()) {
        text = apps_[hover_].name;
        at = &apps_[hover_].rc;
    } else if (!dragging_ && !preview_.Visible()) {
        int mi = MiniHover();   // a minimized window: its title
        if (mi >= 0) {
            wchar_t t[256] = {};
            GetWindowTextW(minis_[mi].h, t, 256);
            text = t;
            at = &minis_[mi].rc;
        }
    }
    if (!at || text.empty()) {
        SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
        return;
    }
    ti.lpszText = text.data();
    SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    LRESULT sz = SendMessageW(tip_, TTM_GETBUBBLESIZE, 0, (LPARAM)&ti);
    int tw = LOWORD(sz), th = HIWORD(sz);
    const auto& r = *at;
    int cx = home_.left + (int)((r.left + r.right) / 2 * dpi_ / 96.0f);
    SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(cx - tw / 2, home_.top - th - (int)(6 * dpi_ / 96.0f)));
    SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
}

void Dock::Launch(const App& a) {
    if (a.start) { StartMenus::Toggle(StartMenus::Configured, mon_); return; }
    ShellExecuteW(nullptr, nullptr, a.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Dock::Activate(App& a) {
    if (a.start) { StartMenus::Toggle(StartMenus::Configured, mon_); return; }
    std::vector<HWND> live;
    for (HWND h : a.windows) if (IsWindow(h)) live.push_back(h);
    if (live.empty()) { Launch(a); return; }
    // Clicking the app you're in minimizes it, like the Windows taskbar.
    if (std::find(live.begin(), live.end(), fg_) != live.end() && !IsIconic(fg_)) {
        ShowWindow(fg_, SW_MINIMIZE);
        return;
    }
    HWND w = live.front();   // top of z-order at last scan
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetForegroundWindow(w);
}

void Dock::Click(int idx) {
    if (idx < 0 || idx >= (int)apps_.size()) return;
    Activate(apps_[idx]);
}

void Dock::ContextMenu(int idx, POINT screen) {
    if (idx < 0 || idx >= (int)apps_.size()) return;
    App a = apps_[idx];   // copy: the list may be rebuilt while the menu is open
    if (a.start) { SendHotkey({VK_LWIN, 'X'}); return; }

    enum { CmdOpen = 1, CmdPin, CmdUnpin, CmdClose, CmdWin = 100 };
    HMENU m = CreatePopupMenu();
    std::vector<HWND> wins;
    for (HWND h : a.windows) {
        if (!IsWindow(h)) continue;
        wchar_t title[80] = {};
        GetWindowTextW(h, title, 80);
        wins.push_back(h);
        AppendMenuW(m, MF_STRING | (h == fg_ ? MF_CHECKED : 0), CmdWin + wins.size() - 1, title);
    }
    if (!wins.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CmdOpen, wins.empty() ? TR(L"Åpne", L"Open") : TR(L"Nytt vindu", L"New Window"));
    AppendMenuW(m, MF_STRING, a.pinned ? CmdUnpin : CmdPin, a.pinned ? TR(L"Fjern fra dock", L"Unpin from dock") : TR(L"Fest i dock", L"Pin to dock"));
    if (!wins.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CmdClose, wins.size() > 1 ? TR(L"Lukk alle vinduer", L"Close All Windows") : TR(L"Lukk vindu", L"Close Window"));
    }
    SetForegroundWindow(hwnd_);
    MenuGuard guard;
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
                               screen.x, home_.top - (int)(4 * dpi_ / 96.0f), hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd >= CmdWin && cmd < CmdWin + (int)wins.size()) {
        HWND w = wins[cmd - CmdWin];
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
    } else if (cmd == CmdOpen) {
        Launch(a);
    } else if (cmd == CmdPin || cmd == CmdUnpin) {
        for (auto& x : apps_)
            if (!x.start && Lower(x.target) == Lower(a.target)) x.pinned = cmd == CmdPin;
        // pinning moves the app to the end of the pinned group
        if (cmd == CmdPin) {
            auto it = std::find_if(apps_.begin(), apps_.end(), [&](const App& x) { return Lower(x.target) == Lower(a.target); });
            if (it != apps_.end()) {
                App moved = std::move(*it);
                apps_.erase(it);
                auto pos = std::find_if(apps_.begin(), apps_.end(), [](const App& x) { return !x.pinned; });
                apps_.insert(pos, std::move(moved));
            }
        }
        SavePinned();
        Rebuild();
    } else if (cmd == CmdClose) {
        for (HWND h : wins) PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
    } else if (fg_ && IsWindow(fg_)) {
        SetForegroundWindow(fg_);
    }
}

void Dock::EmptyAreaMenu(POINT screen) {
    enum { CmdSettings = 1, CmdTaskbar };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CmdSettings, TR(L"Dock-innstillinger…", L"Dock Settings…"));
    AppendMenuW(m, MF_STRING, CmdTaskbar,
                g_app.settings.hideTaskbar ? TR(L"Vis Windows-oppgavelinjen igjen", L"Show Windows taskbar again") : TR(L"Skjul Windows-oppgavelinjen", L"Hide Windows taskbar"));
    SetForegroundWindow(hwnd_);
    MenuGuard guard;
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY,
                               screen.x, home_.top - (int)(4 * dpi_ / 96.0f), hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == CmdSettings) {
        ShowSettings(SettingsPage::Dock);
    } else if (cmd == CmdTaskbar) {
        g_app.settings.hideTaskbar = !g_app.settings.hideTaskbar;
        WritePrivateProfileStringW(L"Dock", L"HideTaskbar", g_app.settings.hideTaskbar ? L"1" : L"0",
                                   g_app.settings.path.c_str());
        // only the main dock (the first) looks after Explorer's taskbar, whichever dock was right-clicked
        for (auto& d : g_app.docks)
            if (d && d->owner_) {
                if (g_app.settings.hideTaskbar) d->taskbar_.Hide();
                else d->taskbar_.Restore();
            }
        SettingsChanged();
    }
}

void Dock::ExplorerRestarted() {
    SetTimer(hwnd_, T_EXPLORER, 1500, nullptr);
}

void Dock::ThemeChanged() {
    Glass(hwnd_);
    preview_.Hide();   // recreated with the new look next time
    preview_.Restyle();
    DiscardResources();
    Invalidate();
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK Dock::TriggerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dock* self = reinterpret_cast<Dock*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) static_cast<Dock*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    } else if (msg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    } else if (msg == WM_MOUSEMOVE && self) {
        // Mouse hit the bottom edge: reveal (but never over a fullscreen game)
        if (!self->fullscreen_ && !IsFullscreen(GetForegroundWindow())) {
            self->SetShown(true);
            SetTimer(self->hwnd_, T_HIDEDELAY, 1500, nullptr);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK Dock::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dock* self = reinterpret_cast<Dock*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Dock*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT Dock::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == shellMsg_ && shellMsg_) {
        HWND w = (HWND)lp;
        switch (wp & 0x7FFF) {
        case HSHELL_WINDOWCREATED:
        case HSHELL_WINDOWDESTROYED:
        case HSHELL_WINDOWREPLACED:
        case HSHELL_MONITORCHANGED:
            SetTimer(hwnd_, T_REBUILD, 80, nullptr);
            break;
        case HSHELL_REDRAW:
            if (wp & 0x8000) {   // HSHELL_FLASH: the app wants attention
                for (auto& a : apps_)
                    if (std::find(a.windows.begin(), a.windows.end(), w) != a.windows.end() && w != fg_) a.attention = true;
                Invalidate();
            }
            break;
        }
        return 0;
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        Invalidate();
        return 0;
    case WM_DPICHANGED:
        dpi_ = HIWORD(wp);
        for (auto& a : apps_) { a.img.Reset(); a.imgHover.Reset(); a.bmp.Reset(); a.bmpHover.Reset(); }
        badgeRt_.Reset();
        for (auto& m : minis_) { m.badge.Reset(); m.icon.Reset(); }
        for (Tile* t : {&stack_, &trash_}) { t->img.Reset(); t->bmp.Reset(); t->tried = false; }   // sharp again at the new size
        DiscardResources();
        for (auto& a : apps_) LoadIcon(a);
        Layout();
        return 0;
    case DockPreview::WM_PREVIEW_LEFT:   // mouse left the preview window
        if (hover_ < 0) SetTimer(hwnd_, T_PREVIEWHIDE, 250, nullptr);
        if (!mouseInside_) SetTimer(hwnd_, T_HIDEDELAY, 500, nullptr);
        return 0;
    case WM_APP + 30:   // appbar notifications
        if (wp == ABN_POSCHANGED) { if (!fullscreen_ && !GameMode()) UpdateReservation(true); else pendingRes_ = true; }
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (badges_) {   // the badge layer goes where the dock goes
            const WINDOWPOS* wpos = reinterpret_cast<const WINDOWPOS*>(lp);
            if (wpos->flags & SWP_HIDEWINDOW) ShowWindow(badges_, SW_HIDE);
            else if (!(wpos->flags & SWP_NOSIZE) && (wpos->cx != badgeSize_.cx || wpos->cy != badgeSize_.cy))
                ShowWindow(badges_, SW_HIDE);   // a new size: the next paint puts it back, drawn to fit
            else if (IsWindowVisible(badges_)) {
                if (!(wpos->flags & SWP_NOMOVE))
                    SetWindowPos(badges_, nullptr, wpos->x, wpos->y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (!(wpos->flags & SWP_NOZORDER)) {   // stay right above the dock
                    HWND above = GetWindow(hwnd_, GW_HWNDPREV);
                    if (above != badges_) {
                        bool top = (GetWindowLongW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                        SetWindowPos(badges_, above ? above : (top ? HWND_TOPMOST : HWND_TOP), 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    }
                }
            }
        }
        if (appbar_) {
            APPBARDATA abd{sizeof abd};
            abd.hWnd = hwnd_;
            SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        }
        break;
    case WM_DISPLAYCHANGE:
        if (!fullscreen_ && !GameMode()) Layout();   // during a game: the docks are rebuilt once it's over
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        switch (wp) {
        case T_ANIM: Animate(); break;
        case T_CLOCK:
            ArmClock();
            if (slide_ < 0.99f && IsWindowVisible(hwnd_)) Invalidate();   // hidden dock: nothing to redraw
            break;
        case T_REBUILD: KillTimer(hwnd_, T_REBUILD); Rebuild(); break;
        case T_MINIS: case T_MINIS_LATE: KillTimer(hwnd_, wp); Layout(); Invalidate(); UpdateLabel(); break;   // a window was minimized or came back
        case 0x31: KillTimer(hwnd_, 0x31); TrashChanged(); break;
        case T_EVAL: KillTimer(hwnd_, T_EVAL); Evaluate(); break;
        case T_HIDEDELAY:
            KillTimer(hwnd_, T_HIDEDELAY);
            // Still between the dock and the screen edge? Keep it up and check again.
            if (mouseInside_ || preview_.MouseInside() || NowPlaying::CardHasMouse() || Flyouts::StackOpen() || CursorInRevealZone())
                SetTimer(hwnd_, T_HIDEDELAY, 400, nullptr);
            else Evaluate();
            break;
        case T_MEDIACARD:
            KillTimer(hwnd_, T_MEDIACARD);
            if (mediaHover_ && shown_ && !dragging_) {
                HidePreview();
                RECT r{home_.left + (LONG)(mediaRc_.left * dpi_ / 96.0f), home_.top, home_.left + (LONG)(mediaRc_.right * dpi_ / 96.0f), home_.bottom};
                NowPlaying::ShowDrawer(r, home_, hwnd_, dpi_);   // slides out from the dock's side
            }
            break;
        case T_PREVIEW:
            KillTimer(hwnd_, T_PREVIEW);
            if (hover_ >= 0 && shown_ && !dragging_) ShowPreview(hover_);
            break;
        case T_PREVIEWHIDE:
            KillTimer(hwnd_, T_PREVIEWHIDE);
            if (preview_.MouseInside() || hover_ >= 0) break;   // made it over there, or back on an icon
            HidePreview();
            if (!mouseInside_) SetTimer(hwnd_, T_HIDEDELAY, 300, nullptr);
            break;
        case T_EXPLORER: {
            KillTimer(hwnd_, T_EXPLORER);
            if (owner_) taskbar_.ExplorerRestarted();
            if (appbar_) {   // remove first: a still-registered appbar can't be added again
                APPBARDATA abd{sizeof abd};
                abd.hWnd = hwnd_;
                SHAppBarMessage(ABM_REMOVE, &abd);
                appbar_ = false;
            }
            reserved_ = {};
            UpdateReservation(true);
            break;
        }
        }
        return 0;
    case WM_MOUSEMOVE: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        mouseInside_ = true;
        KillTimer(hwnd_, T_HIDEDELAY);

        // Dragging a plugin to another place among the plugins
        if (plugDown_ >= 0 && (wp & MK_LBUTTON)) {
            if (!plugDragging_ && (abs(p.x - downPt_.x) > 6 || abs(p.y - downPt_.y) > 6)) {
                plugDragging_ = true;
                SetCapture(hwnd_);
                KillTimer(hwnd_, T_MEDIACARD);
                HidePreview();
                if (tip_) { TOOLINFOW ti{sizeof ti}; ti.hwnd = hwnd_; ti.uId = 1; SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); }
            }
            if (plugDragging_) { DragPluginTo(p.x * 96.0f / dpi_); return 0; }
        }
        if (downIdx_ >= (int)apps_.size()) downIdx_ = -1;
        if (downIdx_ >= 0 && (wp & MK_LBUTTON)) {
            if (!dragging_ && (abs(p.x - downPt_.x) > 6 || abs(p.y - downPt_.y) > 6) && !apps_[downIdx_].start) {
                dragging_ = true;
                SetCapture(hwnd_);
                SetHover(-1);
            }
            if (dragging_) {
                removing_ = p.y < -(int)(50 * dpi_ / 96.0f) && apps_[downIdx_].pinned;
                int to = HitTest({p.x, 0});
                if (to > 0 && to != downIdx_) {   // index 0 is Start and stays put
                    App moved = std::move(apps_[downIdx_]);
                    apps_.erase(apps_.begin() + downIdx_);
                    apps_.insert(apps_.begin() + to, std::move(moved));
                    downIdx_ = to;
                    Layout();
                }
                Invalidate();
                return 0;
            }
        }
        SetHover(HitTest(p));
        UpdateMediaHover(p);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (plugDragging_ && (HWND)lp != hwnd_) { plugDragging_ = false; plugDown_ = -1; SavePluginOrder(); }
        if (dragging_ && (HWND)lp != hwnd_) {   // an icon drag was interrupted (Win key, Alt+Tab, a dialog): drop it where it is
            dragging_ = removing_ = false;
            downIdx_ = -1;
            for (auto& a : apps_) a.targetScale = 1.0f;
            SavePinned();
            Rebuild();
            StartAnimating();
        }
        break;
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!dragging_) {
            mouseInside_ = false;
            if (downIdx_ >= 0) {   // pressed, then left without releasing here: forget the press
                if (downIdx_ < (int)apps_.size()) apps_[downIdx_].targetScale = 1.0f;
                downIdx_ = -1;
            }
            SetHover(-1);
            UpdateMediaHover({-100000, 0});
            SetTimer(hwnd_, T_HIDEDELAY, 500, nullptr);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        downIdx_ = HitTest(p);
        downPt_ = p;
        plugDown_ = downIdx_ < 0 ? PluginAt(p.x * 96.0f / dpi_) : -1;
        HidePreview();
        if (downIdx_ >= 0) { apps_[downIdx_].targetScale = kPressScale; StartAnimating(); }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = downIdx_;
        downIdx_ = -1;
        plugDown_ = -1;
        if (plugDragging_) {   // a plugin was moved: keep the new order, no click
            plugDragging_ = false;
            ReleaseCapture();
            SavePluginOrder();
            UpdateMediaHover(p);
            return 0;
        }
        if (dragging_) {
            dragging_ = false;   // first: letting go of the capture below must not look like an interrupted drag
            ReleaseCapture();
            {   // dropped away from the dock (unpinning): the mouse isn't over us any more
                POINT c;
                GetCursorPos(&c);
                if (WindowFromPoint(c) != hwnd_) { mouseInside_ = false; tracking_ = false; SetTimer(hwnd_, T_HIDEDELAY, 500, nullptr); }
            }
            if (idx >= 0 && idx < (int)apps_.size()) {
                App& a = apps_[idx];
                if (removing_) {
                    a.pinned = false;
                } else if (!a.pinned) {
                    // dropped among pinned apps -> pin it
                    for (int j = idx + 1; j < (int)apps_.size(); ++j) if (apps_[j].pinned) { a.pinned = true; break; }
                } else {
                    // a pinned app dragged in among running apps stays pinned, at the end of the pinned group
                    bool afterRunning = false;
                    for (int j = 1; j < idx; ++j) if (!apps_[j].pinned) { afterRunning = true; break; }
                    if (afterRunning) {
                        App moved = std::move(apps_[idx]);
                        apps_.erase(apps_.begin() + idx);
                        auto pos = std::find_if(apps_.begin() + 1, apps_.end(), [](const App& x) { return !x.pinned; });
                        apps_.insert(pos, std::move(moved));
                    }
                }
            }
            removing_ = false;
            SavePinned();
            Rebuild();
            return 0;
        }
        for (auto& a : apps_) a.targetScale = 1.0f;
        if (hover_ >= 0) apps_[hover_].targetScale = kHoverScale;
        StartAnimating();
        if (idx >= 0 && idx == HitTest(p)) Click(idx);
        else if (idx < 0 && mediaHover_) NowPlaying::TogglePlay();   // click the cover: play/pause
        else if (idx < 0 && clockHover_) SendHotkey({VK_LWIN, 'N'});   // the clock: calendar + notifications
        else if (idx < 0 && MiniHover() >= 0) {   // a minimized window: bring it back
            HWND w = minis_[MiniHover()].h;
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        else if (idx < 0 && trash_.hover) ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
        else if (idx < 0 && stack_.hover) {   // the stack opens above its tile
            RECT r{home_.left + (LONG)(stack_.rc.left * dpi_ / 96.0f), home_.top, home_.left + (LONG)(stack_.rc.right * dpi_ / 96.0f), home_.bottom};
            Flyouts::ShowStack(r, dpi_);
        }
        return 0;
    }
    case WM_MBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = HitTest(p);
        if (idx >= 0) Launch(apps_[idx]);   // middle click = new instance
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = HitTest(p);
        POINT s = p;
        ClientToScreen(hwnd_, &s);
        SetHover(-1);
        HidePreview();
        if (idx >= 0) {
            const auto& r = apps_[idx].rc;
            s.x = home_.left + (int)((r.left + r.right) / 2 * dpi_ / 96.0f);
            ContextMenu(idx, s);
        } else if (MiniHover() >= 0) {
            int mi = MiniHover();
            if (tip_) { TOOLINFOW ti{sizeof ti}; ti.hwnd = hwnd_; ti.uId = 1; SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti); }
            s.x = home_.left + (int)((minis_[mi].rc.left + minis_[mi].rc.right) / 2 * dpi_ / 96.0f);
            MiniMenu(mi, s);
        } else if (trash_.hover || stack_.hover) {
            s.x = home_.left + (int)(((trash_.hover ? trash_.rc : stack_.rc).left + (trash_.hover ? trash_.rc : stack_.rc).right) / 2 * dpi_ / 96.0f);
            PluginMenu(trash_.hover, s);
        } else {
            EmptyAreaMenu(s);
        }
        return 0;
    }
    case WM_APP + 48:   // files dropped on the Recycle Bin
        RecycleFiles(reinterpret_cast<std::vector<std::wstring>*>(lp));
        return 0;
    case WM_APP + 47:   // the Recycle Bin changed (Windows' change notification)
        if (HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, nullptr, nullptr)) SHChangeNotification_Unlock(lock);
        SetTimer(hwnd_, 0x31, 300, nullptr);   // a big delete sends many: look once they've settled
        return 0;
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
