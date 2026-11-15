// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// SkyMenu and SkyLauncher — see startmenu.h
#include "startmenu.h"
#include "applist.h"
#include "app.h"
#include "dock.h"
#include "icons.h"
#include "settings.h"
#include "filesearch.h"
#include "nowplaying.h"
#include <shlwapi.h>
#include "mission.h"
#include <shlobj.h>
#include <shellscalingapi.h>
#include <sddl.h>
#include <powrprof.h>
#define SECURITY_WIN32
#include <security.h>
#include <map>
#include <set>
#include <cmath>
#include <algorithm>
#include <atomic>

namespace {

// ===========================================================================
// Shared bits
// ===========================================================================
constexpr UINT_PTR T_ANIM = 1, T_CARET = 3, T_REFRESH = 4, T_EDGE = 5, T_OUTSIDE = 6, T_POWER = 7, T_FILES = 8, T_REORDER = 9;

struct ACCENTPOLICY { int AccentState; int AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };
using SetWCA = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);

// Acrylic glass like the dock. `alpha` = how much tint over the blur (0..255).
void Glass(HWND h, BYTE alpha, bool round, Part part = Part::Start) {
    const Theme& t = g_app.theme;
    BOOL dark = t.dark;
    DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(h, &m);
    DWORD corner = round ? 2 : 1;
    DwmSetWindowAttribute(h, 33, &corner, sizeof corner);
    if (!round) { COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(h, 34, &none, sizeof none); }
    (void)alpha;
    ApplyGlassEffect(h, part);   // see-through: the tint is painted in Paint()
}

// Windows only lets the app that got the last input take the foreground. A
// harmless empty mouse input makes that us (same trick PowerToys uses).
void ForceForeground(HWND h) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof in);
    SetForegroundWindow(h);
    if (GetForegroundWindow() != h) {
        // Still refused: borrow the foreground thread's input state for a moment
        DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr), me = GetCurrentThreadId();
        HWND fgw = GetForegroundWindow();
        if (fg && fg != me && !(fgw && IsHungAppWindow(fgw)) && AttachThreadInput(me, fg, TRUE)) {
            BringWindowToTop(h);
            SetForegroundWindow(h);
            AttachThreadInput(me, fg, FALSE);
        }
    }
    SetFocus(h);
}

const wchar_t* IconFont() {
    static std::wstring name;
    if (name.empty()) {
        name = L"Segoe MDL2 Assets";
        ComPtr<IDWriteFontCollection> fc;
        if (g_gfx.dwrite && SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE)) && fc) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists) name = L"Segoe Fluent Icons";
        }
    }
    return name.c_str();
}

ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    ComPtr<IDWriteTextFormat> f;
    g_gfx.dwrite->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
    if (!f) return f;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ell;
    g_gfx.dwrite->CreateEllipsisTrimmingSign(f.Get(), ell.Put());
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    f->SetTrimming(&trim, ell.Get());
    return f;
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
// Milliseconds, high resolution (GetTickCount only moves every ~16 ms: visible jitter at 165 Hz)
double NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

float Ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }

// Typing into the search field. Returns true if the text changed.
bool EditQuery(std::wstring& q, wchar_t ch) {
    if (ch == 8) { if (q.empty()) return false; q.pop_back(); return true; }
    if (ch == 127) {   // Ctrl+Backspace: the last word
        if (q.empty()) return false;
        while (!q.empty() && q.back() == L' ') q.pop_back();
        while (!q.empty() && q.back() != L' ') q.pop_back();
        return true;
    }
    if (ch == 22) {    // Ctrl+V
        if (!OpenClipboard(nullptr)) return false;
        bool changed = false;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                for (; *p && q.size() < 120; ++p) if (*p >= 32) { q += *p; changed = true; }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        return changed;
    }
    if (ch < 32 || q.size() >= 120) return false;
    q += ch;
    return true;
}

// Right-click menu for an app (shared by both menus). Returns true if something launched.
bool AppContextMenu(HWND owner, const AppEntry& a, POINT screen, bool inSkyMenuPins, std::function<void(bool)> pinSkyMenu,
                    std::function<void()> removeHere = {}, std::function<void()> newFolder = {}, bool vibe = false) {
    enum { COpen = 1, CAdmin, CPinMenu, CUnpinMenu, CPinDock, CLocation, CRemove, CFolder };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, COpen, TR(L"Åpne", L"Open"));
    if (a.file) AppendMenuW(m, MF_STRING, CLocation, TR(L"Åpne filplassering", L"Open file location"));
    if (a.direct) AppendMenuW(m, MF_STRING, CAdmin, TR(L"Kjør som administrator", L"Run as administrator"));
    if (!a.setting && !a.file) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (pinSkyMenu && vibe)   // VibeMenu and SkyMenu share their pinned apps
            AppendMenuW(m, MF_STRING, inSkyMenuPins ? CUnpinMenu : CPinMenu,
                        inSkyMenuPins ? TR(L"Løsne (VibeMenu og SkyMenu)", L"Unpin (VibeMenu and SkyMenu)") : TR(L"Fest (VibeMenu og SkyMenu)", L"Pin (VibeMenu and SkyMenu)"));
        else if (pinSkyMenu) AppendMenuW(m, MF_STRING, inSkyMenuPins ? CUnpinMenu : CPinMenu,
                                    inSkyMenuPins ? TR(L"Løsne fra SkyMenu", L"Unpin from SkyMenu") : TR(L"Fest i SkyMenu", L"Pin to SkyMenu"));
        AppendMenuW(m, MF_STRING, CPinDock, TR(L"Fest i dock", L"Pin to dock"));
        if (a.direct) AppendMenuW(m, MF_STRING, CLocation, TR(L"Åpne filplassering", L"Open file location"));
    }
    if (newFolder) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CFolder, TR(L"Lag mappe", L"Create folder"));
    }
    if (removeHere) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CRemove, TR(L"Fjern fra VibeMenu", L"Remove from VibeMenu"));
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, owner, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case COpen: LaunchApp(a); return true;
    case CRemove: if (removeHere) removeHere(); break;
    case CFolder: if (newFolder) newFolder(); break;
    case CAdmin: LaunchApp(a, true); return true;
    case CPinMenu: if (pinSkyMenu) pinSkyMenu(true); break;
    case CUnpinMenu: if (pinSkyMenu) pinSkyMenu(false); break;
    case CPinDock: PinToDock(a); break;
    case CLocation: OpenFileLocation(a); return true;
    }
    return false;
}

HMONITOR CursorMonitor() {
    POINT p;
    GetCursorPos(&p);
    return MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}

UINT MonitorDpi(HMONITOR m) {
    UINT x = 96, y = 96;
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y);
    return x;
}

std::wstring ReadIniLong(const wchar_t* sec, const wchar_t* key) {
    std::vector<wchar_t> buf(65536);
    GetPrivateProfileStringW(sec, key, L"", buf.data(), (DWORD)buf.size(), g_app.settings.path.c_str());
    return buf.data();
}

// ---------------------------------------------------------------------------
// Base: a popup with Direct2D, an icon cache and a search field
// ---------------------------------------------------------------------------
class Surface {
public:
    virtual ~Surface() { Destroy(); }
    bool Open() const { return hwnd_ && IsWindowVisible(hwnd_) && !closing_; }
    HWND Hwnd() const { return hwnd_; }

    void Close(bool animate = true) {
        if (!hwnd_ || closing_) return;
        OnClosing();
        if (ShowsBars()) OurStartMenuShown(false);   // the dock/bar may tuck away again while we fade out
        if (!animate) { Destroy(); return; }
        closing_ = true;
        animFrom_ = anim_;
        animStart_ = NowMs();
        SetTimer(hwnd_, T_ANIM, 50, nullptr);   // safety net; frames are driven by painting
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

protected:
    virtual const wchar_t* ClassName() const = 0;
    virtual void Paint() = 0;
    virtual LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) = 0;
    virtual DWORD OpenMs() const { return 170; }
    virtual DWORD CloseMs() const { return 110; }
    virtual void OnQuery() {}
    virtual bool WantsFrame() const { return false; }   // the subclass is animating something
    virtual void Tick(double) {}                         // advance subclass animations to `now`
    virtual void OnClosing() {}                          // about to close (clean up floating helpers)
    virtual void OnDestroy() {}                          // the window is about to be destroyed (still valid here)
    virtual bool ShowsBars() const { return true; }      // the top bar and dock come up while it's open (Start menus do)

    bool CreateAt(const RECT& r, DWORD exStyle) {
        Destroy();
        static std::map<std::wstring, bool> registered;
        if (!registered[ClassName()]) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = ClassName();
            RegisterClassExW(&wc);
            registered[ClassName()] = true;
        }
        hwnd_ = CreateWindowExW(exStyle, ClassName(), L"Skyline", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, g_app.hinst, this);
        if (!hwnd_) return false;
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
        anim_ = 0;
        animFrom_ = 0;
        closing_ = false;
        query_.clear();
        caretOn_ = true;
        return true;
    }

    void ShowAnimated() {
        ShowWindow(hwnd_, SW_SHOW);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        ForceForeground(hwnd_);
        if (ShowsBars()) OurStartMenuShown(true);   // the top bar and the dock come up with us (even over a game)
        // Windows refused us the foreground: then we'd never hear about clicks elsewhere. Watch for them.
        if (GetForegroundWindow() != hwnd_) SetTimer(hwnd_, T_OUTSIDE, 50, nullptr);
        animStart_ = NowMs();
        SetTimer(hwnd_, T_ANIM, 50, nullptr);
        SetTimer(hwnd_, T_CARET, 530, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Destroy() {
        if (!hwnd_) return;
        OnClosing();
        OnDestroy();
        HWND h = hwnd_;
        hwnd_ = nullptr;
        if (ShowsBars()) OurStartMenuShown(false);   // (Spotlight never said it was shown: don't undo another menu's "shown")
        rt_.Reset();
        brush_.Reset();
        bmps_.clear();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
        closing_ = false;
        // Re-read the app list now that nobody is looking (it can take a moment), if it's getting old
        SetTimer(nullptr, 0, 1500, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(nullptr, id); AllApps(120000); });
    }

    bool Resources() {
        if (rt_) return true;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  (float)dpi_, (float)dpi_);
        // Present right away and pace frames with DwmFlush: one frame per screen refresh (60, 144, 165 Hz …)
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(
                props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY),
                rt_.Put())))
            return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
        bmps_.clear();
        return true;
    }

    // The app's icon if it's ready; otherwise schedules loading and returns null.
    ID2D1Bitmap* Icon(const AppEntry& a, int px) {
        std::wstring k = a.key + L"|" + std::to_wstring(px);   // one bitmap per size, so a small row never blurs a big tile
        auto it = bmps_.find(k);
        if (it != bmps_.end()) return it->second.Get();
        if (!AppIconCached(a, px)) { RequestIcon(a, px, true); return nullptr; }   // arrives from the loader thread
        ComPtr<ID2D1Bitmap> b;
        if (auto src = AppIcon(a, px)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
        bmps_[k] = b;
        return b.Get();
    }

public:
    void IconsArrived() { if (hwnd_ && !closing_) InvalidateRect(hwnd_, nullptr, FALSE); }
protected:

    void DrawText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        if (!f || s.empty()) return;
        brush_->SetColor(c);
        rt_->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawGlyph(wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        if (!f) return;
        brush_->SetColor(c);
        rt_->DrawTextW(&g, 1, f, r, brush_.Get());
    }

    // The search field with placeholder and caret
    void DrawSearch(D2D1_RECT_F r, const wchar_t* placeholder, IDWriteTextFormat* text, IDWriteTextFormat* glyphs, bool center) {
        const Theme& th = g_app.theme;
        float rad = (r.bottom - r.top) / 2;
        brush_->SetColor(Alpha(th.fg, th.dark ? 0.09f : 0.07f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), brush_.Get());
        brush_->SetColor(Alpha(th.fg, 0.10f));
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad),
                                  brush_.Get(), 1.0f);
        const std::wstring& shown = query_.empty() ? std::wstring(placeholder) : query_;
        float tw = TextWidth(shown, text);
        float left = center ? std::max(r.left + 34, (r.left + r.right) / 2 - (tw + 22) / 2 + 22) : r.left + 38;
        DrawGlyph(0xE721, glyphs, D2D1::RectF(left - 26, r.top, left - 6, r.bottom), th.fgDim);
        DrawText(shown, text, D2D1::RectF(left, r.top, r.right - 14, r.bottom), query_.empty() ? th.fgDim : th.fg);
        if (caretOn_ && !closing_) {
            float cx = query_.empty() ? left : left + tw + 1;
            float h = (r.bottom - r.top) * 0.5f;
            brush_->SetColor(th.fg);
            rt_->FillRectangle(D2D1::RectF(cx, (r.top + r.bottom) / 2 - h / 2, cx + 1.2f, (r.top + r.bottom) / 2 + h / 2), brush_.Get());
        }
    }

    float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
        if (!f || s.empty()) return 0;
        ComPtr<IDWriteTextLayout> tl;
        g_gfx.dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 100, tl.Put());
        DWRITE_TEXT_METRICS m{};
        if (tl) tl->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    // Shared message handling: animation, caret, typing, paint
    LRESULT Common(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
        handled = true;
        switch (msg) {
        case WM_PAINT: {
            // Advance animations to this exact moment, draw, and if anything is still
            // moving, wait for the next screen refresh and ask for another frame.
            double now = NowMs();
            bool moving = false;
            if (closing_ || anim_ < 1) {
                float t = (float)((now - animStart_) / (closing_ ? CloseMs() : OpenMs()));
                if (closing_) {
                    anim_ = animFrom_ * (1 - Ease(t));
                    if (t >= 1) { ValidateRect(hwnd_, nullptr); Destroy(); return 0; }
                } else {
                    anim_ = t >= 1 ? 1.0f : Ease(t);
                }
                moving = true;
            }
            Tick(now);
            moving = moving || WantsFrame();
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            if (Resources()) Paint();
            EndPaint(hwnd_, &ps);
            if (moving && hwnd_) {
                DwmFlush();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (hwnd_) {
                KillTimer(hwnd_, T_ANIM);
            }
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            return 0;
        case WM_TIMER:
            if (wp == T_ANIM) { InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
            if (wp == T_OUTSIDE) {
                if (GetForegroundWindow() == hwnd_) { KillTimer(hwnd_, T_OUTSIDE); return 0; }   // got it after all
                if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) {
                    POINT c;
                    GetCursorPos(&c);
                    RECT r;
                    GetWindowRect(hwnd_, &r);
                    if (!PtInRect(&r, c) && !inMenu_) { KillTimer(hwnd_, T_OUTSIDE); Close(); }
                }
                return 0;
            }   // keeps frames coming if a paint was skipped
            if (wp == T_CARET) { caretOn_ = !caretOn_; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
            break;
        case WM_CHAR:
            if (wp == 27 || wp == 13 || wp == 9) return 0;   // handled as keys
            if (EditQuery(query_, (wchar_t)wp)) {
                caretOn_ = true;
                SetTimer(hwnd_, T_CARET, 530, nullptr);
                OnQuery();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !inMenu_) Close();
            handled = false;   // DefWindowProc still gives us keyboard focus
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        }
        handled = false;
        return 0;
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Surface*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = h;
        }
        auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        // While it fades out it no longer takes input (no double launches)
        if (self->closing_ && ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST)))
            return 0;
        bool handled = false;
        LRESULT r = self->Common(msg, wp, lp, handled);
        if (handled) return r;
        return self->Handle(msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HMONITOR mon_ = nullptr;
    UINT dpi_ = 96;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bmps_;
    std::wstring query_;
    bool caretOn_ = true;
    bool inMenu_ = false;         // our own right-click menu is open: don't close
    bool wantIcons_ = false;
    float anim_ = 0, animFrom_ = 0;
    double animStart_ = 0;
    bool closing_ = false;
};

// ===========================================================================
// SkyMenu pins (its own list; starts as a copy of the dock's)
// ===========================================================================
std::vector<std::wstring> MenuPins() {
    const wchar_t* ini = g_app.settings.path.c_str();
    if (!GetPrivateProfileIntW(L"SkyMenu", L"PinnedSet", 0, ini)) {
        std::vector<std::wstring> pins;
        for (const auto& t : g_app.settings.pinned) pins.push_back(t);
        IniWrite(L"SkyMenu", L"Pinned", JoinList(pins, L"|"));
        IniWrite(L"SkyMenu", L"PinnedSet", 1);
        return pins;
    }
    return SplitList(ReadIniLong(L"SkyMenu", L"Pinned"), L'|');
}

void SaveMenuPins(const std::vector<std::wstring>& pins) {
    IniWrite(L"SkyMenu", L"Pinned", JoinList(pins, L"|"));
    IniWrite(L"SkyMenu", L"PinnedSet", 1);
}

bool IsMenuPinned(const AppEntry& a) {
    for (const auto& t : MenuPins()) {
        std::wstring k = ToLower(t);
        if (k == a.key || (a.direct && k == ToLower(a.exe))) return true;
    }
    return false;
}

void SetMenuPinned(const AppEntry& a, bool pin) {
    auto pins = MenuPins();
    auto same = [&](const std::wstring& t) { std::wstring k = ToLower(t); return k == a.key || (a.direct && k == ToLower(a.exe)); };
    pins.erase(std::remove_if(pins.begin(), pins.end(), same), pins.end());
    if (pin) pins.push_back(a.direct ? a.exe : a.target);
    SaveMenuPins(pins);
}

// ===========================================================================
// Power
// ===========================================================================
bool EnableShutdownPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    bool ok = GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

void RestartPc() {
    EnableShutdownPrivilege();
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

void ShutDownPc() {
    EnableShutdownPrivilege();
    ExitWindowsEx(EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
}

void PowerMenu(HWND owner, POINT at) {
    enum { CLock = 1, CSignOut, CSleep, CRestart, CShutdown };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CLock, TR(L"Lås", L"Lock"));
    AppendMenuW(m, MF_STRING, CSignOut, TR(L"Logg av", L"Sign out"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CSleep, TR(L"Hvilemodus", L"Sleep"));
    AppendMenuW(m, MF_STRING, CRestart, TR(L"Start på nytt", L"Restart"));
    AppendMenuW(m, MF_STRING, CShutdown, TR(L"Slå av", L"Shut down"));
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_NONOTIFY, at.x, at.y, 0, owner, nullptr);
    DestroyMenu(m);
    if (cmd == CSignOut || cmd == CRestart || cmd == CShutdown) {   // a slip of the mouse shouldn't end your session
        const wchar_t* q = cmd == CSignOut ? TR(L"Vil du logge av?", L"Sign out now?")
                         : cmd == CRestart ? TR(L"Vil du starte PC-en på nytt?", L"Restart the PC now?") : TR(L"Vil du slå av PC-en?", L"Shut down the PC now?");
        if (MessageBoxW(owner, q, L"Skyline", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }
    switch (cmd) {
    case CLock: LockWorkStation(); break;
    case CSignOut: ExitWindowsEx(EWX_LOGOFF, 0); break;
    case CSleep: EnableShutdownPrivilege(); SetSuspendState(FALSE, FALSE, FALSE); break;
    case CRestart: RestartPc(); break;
    case CShutdown: ShutDownPc(); break;
    }
}

// ===========================================================================
// The signed-in user
// ===========================================================================
std::wstring UserDisplayName() {
    wchar_t buf[256] = {};
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, buf, &n) && *buf) return buf;
    DWORD m = 256;
    if (GetUserNameW(buf, &m)) return buf;
    return L"";
}

ComPtr<IWICBitmapSource> AccountPicture(int px) {
    ComPtr<IWICBitmapSource> out;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    BYTE buf[256];
    DWORD len = 0;
    std::wstring sid;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len)) {
        LPWSTR s = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &s)) { sid = s; LocalFree(s); }
    }
    CloseHandle(tok);
    if (sid.empty()) return out;
    std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\" + sid;
    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof path;
    for (const wchar_t* v : {L"Image192", L"Image240", L"Image96", L"Image448"}) {
        size = sizeof path;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), v, RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS && *path) break;
        *path = 0;
    }
    if (!*path) return out;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_gfx.wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, dec.Put()))) return out;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.Put()))) return out;
    ComPtr<IWICFormatConverter> conv;
    g_gfx.wic->CreateFormatConverter(conv.Put());
    if (!conv || FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeCustom)))
        return out;
    ComPtr<IWICBitmapScaler> sc;
    g_gfx.wic->CreateBitmapScaler(sc.Put());
    if (!sc || FAILED(sc->Initialize(conv.Get(), px, px, WICBitmapInterpolationModeFant))) return out;
    ComPtr<IWICBitmap> bmp;
    if (SUCCEEDED(g_gfx.wic->CreateBitmapFromSource(sc.Get(), WICBitmapCacheOnLoad, bmp.Put()))) bmp->QueryInterface(IID_PPV_ARGS(out.Put()));
    return out;
}

void OpenKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOWNORMAL);
        CoTaskMemFree(p);
    }
}

// Icon size SkyLauncher uses on a monitor (same math as its layout)
int LauncherIconPx(HMONITOR mon) {
    UINT dpi = MonitorDpi(mon);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    float H = (mi.rcMonitor.bottom - mi.rcMonitor.top) * 96.0f / dpi;
    float icon = std::clamp(H / 11.0f, 56.0f, 84.0f);
    return (int)std::lround(icon * dpi / 96.0f);
}

// ===========================================================================
// SkyLauncher — full-screen grid with pages and folders
// ===========================================================================
// Saved in [SkyLauncher] Layout as tokens separated by '|':
//   an app key · "<page>" (page break) · "<folder>Name*key1*key2…" (a folder)
// None of | * < > can appear in an app key (they're not allowed in paths or app ids).
struct LTile {
    bool folder = false;
    AppEntry app;                 // a single app
    std::wstring name;            // a folder …
    std::vector<AppEntry> apps;   // … and its apps
    const std::wstring& Label() const { return folder ? name : app.name; }
};
using LPage = std::vector<LTile>;

const wchar_t kPageTok[] = L"<page>";
const wchar_t kFolderTok[] = L"<folder>";

std::wstring CleanFolderName(std::wstring s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c == L'|' || c == L'*' || c == L'<' || c == L'>' || c < 32; }), s.end());
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

// The icon you're dragging, in a small see-through window of its own, so it
// can float over the dock (which sits above SkyLauncher) to be pinned there.
class DragGhost {
public:
    ~DragGhost() { Hide(); }
    bool Visible() const { return hwnd_ != nullptr; }

    bool Show(IWICBitmapSource* src, POINT topLeft) {
        Hide();
        if (!src) return false;
        ComPtr<IWICBitmapSource> conv;
        if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, src, conv.Put())) || !conv) return false;
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (!w || !h || w > 1024 || h > 1024) return false;
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_app.hinst;
            wc.lpszClassName = L"SkylineDragGhost";
            RegisterClassW(&wc);
            registered = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SkylineDragGhost",
                                L"", WS_POPUP, topLeft.x, topLeft.y, (int)w, (int)h, nullptr, nullptr, g_app.hinst, nullptr);
        if (!hwnd_) return false;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB};
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        bool ok = dib && bits && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, static_cast<BYTE*>(bits)));
        if (ok) {
            HGDIOBJ old = SelectObject(mem, dib);
            POINT src0{0, 0};
            SIZE sz{(LONG)w, (LONG)h};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 235, AC_SRC_ALPHA};
            ok = UpdateLayeredWindow(hwnd_, screen, &topLeft, &sz, mem, &src0, 0, &bf, ULW_ALPHA) != FALSE;
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!ok) { Hide(); return false; }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        return true;
    }

    void Move(POINT topLeft) {
        if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, topLeft.x, topLeft.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void Hide() {
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

private:
    HWND hwnd_ = nullptr;
};

class Launcher : public Surface {
public:
    void Show(HMONITOR mon) {
        mon_ = mon ? mon : CursorMonitor();
        dpi_ = MonitorDpi(mon_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon_, &mi);
        if (!CreateAt(mi.rcMonitor, WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) return;
        Glass(hwnd_, g_app.theme.dark ? 0x88 : 0x99, false);
        LoadItems();
        page_ = 0;
        pageAnim_ = 0;
        hover_ = sel_ = -1;
        ResetDrag();
        openFolder_ = {};
        renaming_ = false;
        wheel_ = 0;
        swiping_ = false;
        edgeArmed_ = false;
        powerHover_ = armed_ = -1;
        ShowAnimated();
        PrewarmIcons(LauncherIconPx(mon_));   // the other pages load quietly behind the first
        if (needRefresh_) SetTimer(hwnd_, T_REFRESH, 450, nullptr);
    }

protected:
    struct Pos {
        int p = -1, i = -1;
        bool Valid() const { return p >= 0 && i >= 0; }
        bool operator==(const Pos& o) const { return p == o.p && i == o.i; }
    };
    struct Disp { int p, i; bool gap; };   // one cell on screen: a tile of pages_, or the gap where a dragged tile will land

    const wchar_t* ClassName() const override { return L"SkylineLauncher"; }
    DWORD OpenMs() const override { return 200; }
    void OnClosing() override { ghost_.Hide(); }

    // ---- the layout: load, save, tidy ----
    void LoadItems() {
        needRefresh_ = !AllApps(0xFFFFFFFF).empty();   // the list we have shows at once; refreshed right after
        const auto& all = AllApps(0xFFFFFFFF);
        std::map<std::wstring, const AppEntry*> byKey;
        for (const auto& a : all) byKey[a.key] = &a;
        std::map<std::wstring, bool> used;
        auto take = [&](const std::wstring& k, AppEntry& out) {
            auto it = byKey.find(k);
            if (it == byKey.end() || used[k]) return false;
            used[k] = true;
            out = *it->second;
            return true;
        };
        pages_.assign(1, {});
        std::wstring layout = ReadIniLong(L"SkyLauncher", L"Layout");
        bool old = layout.empty();   // 2.3 and older: one long list ("Order")
        for (const auto& tok : SplitList(old ? ReadIniLong(L"SkyLauncher", L"Order") : layout, L'|')) {
            if (tok == kPageTok) {
                if (!pages_.back().empty()) pages_.emplace_back();
                continue;
            }
            if (tok.compare(0, wcslen(kFolderTok), kFolderTok) == 0) {
                auto parts = SplitList(tok.substr(wcslen(kFolderTok)), L'*');
                LTile f;
                f.folder = true;
                size_t first = 0;
                // the name comes first; a folder with an empty name starts right at the apps
                if (!parts.empty() && byKey.find(parts[0]) == byKey.end()) { f.name = parts[0]; first = 1; }
                if (f.name.empty()) f.name = TR(L"Mappe", L"Folder");
                for (size_t j = first; j < parts.size(); ++j) {
                    AppEntry a;
                    if (take(parts[j], a)) f.apps.push_back(a);
                }
                pages_.back().push_back(std::move(f));   // folders you made stay, even with one app or none
                continue;
            }
            LTile t;
            if (take(tok, t.app)) pages_.back().push_back(std::move(t));
        }
        for (const auto& a : all)   // new apps go at the end, like Launchpad
            if (!used[a.key]) { LTile t; t.app = a; pages_.back().push_back(std::move(t)); }
        normPer_ = 0;   // split into pages once the grid size is known
    }

    void SaveLayout() {
        std::vector<std::wstring> toks;
        for (size_t p = 0; p < pages_.size(); ++p) {
            if (pages_[p].empty()) continue;
            if (!toks.empty()) toks.push_back(kPageTok);
            for (const auto& t : pages_[p]) {
                if (!t.folder) { toks.push_back(t.app.key); continue; }
                std::wstring s = kFolderTok + CleanFolderName(t.name);
                for (const auto& a : t.apps) s += L"*" + a.key;
                toks.push_back(s);
            }
        }
        IniWrite(L"SkyLauncher", L"Layout", JoinList(toks, L"|"));
    }

    // Full pages spill their last tiles onto the next page; empty pages go away.
    void Normalize() {
        if (perPage_ <= 0) return;
        for (size_t p = 0; p < pages_.size(); ++p)
            while ((int)pages_[p].size() > perPage_) {
                if (p + 1 == pages_.size()) pages_.emplace_back();
                pages_[p + 1].insert(pages_[p + 1].begin(), std::move(pages_[p].back()));
                pages_[p].pop_back();
            }
        pages_.erase(std::remove_if(pages_.begin(), pages_.end(), [](const LPage& pg) { return pg.empty(); }), pages_.end());
        if (pages_.empty()) pages_.emplace_back();
    }

    // What's on screen, page by page. While dragging there's a gap where the tile
    // will land (full pages push their last tile on), and one empty page at the end
    // so you can start a new page.
    std::vector<std::vector<Disp>> Display() const {
        std::vector<std::vector<Disp>> out;
        int per = std::max(perPage_, 1);
        for (size_t p = 0; p <= pages_.size(); ++p) {
            std::vector<Disp> row;
            if (p < pages_.size())
                for (int i = 0; i < (int)pages_[p].size(); ++i) row.push_back({(int)p, i, false});
            if (dragging_ && fph_ < 0 && ph_.p == (int)p)
                row.insert(row.begin() + std::min<size_t>(ph_.i, row.size()), Disp{-1, -1, true});
            out.push_back(std::move(row));
        }
        for (size_t p = 0; p < out.size(); ++p)
            while ((int)out[p].size() > per) {
                if (p + 1 == out.size()) out.emplace_back();
                out[p + 1].insert(out[p + 1].begin(), out[p].back());
                out[p].pop_back();
            }
        while (out.size() > 1 && out.back().empty()) out.pop_back();
        if (dragging_ && !out.back().empty()) out.emplace_back();
        return out;
    }

    LTile* TileAt(const Pos& at) {
        if (!at.Valid() || at.p >= (int)pages_.size() || at.i >= (int)pages_[at.p].size()) return nullptr;
        return &pages_[at.p][at.i];
    }
    LTile* OpenFolder() {
        LTile* t = TileAt(openFolder_);
        return t && t->folder ? t : nullptr;
    }

    int PageCount() const {
        if (!query_.empty() && !renaming_) return std::max(1, ((int)results_.size() + perPage_ - 1) / std::max(perPage_, 1));
        return (int)Display().size();
    }

    void OnQuery() override {
        if (renaming_) {   // typing goes into the folder's name
            if (LTile* f = OpenFolder()) f->name = query_;
            return;
        }
        openFolder_ = {};
        // with files on, leave room for a few of them on the first page
        int room = g_app.settings.launcherFileSearch ? std::min(6, perPage_ / 3) : 0;
        results_ = SearchApps(query_, false, (size_t)std::max(perPage_ - room, 1));
        ++fileTicket_;
        KillTimer(hwnd_, T_FILES);
        if (g_app.settings.launcherFileSearch && query_.size() >= 2) SetTimer(hwnd_, T_FILES, 150, nullptr);
        sel_ = results_.empty() ? -1 : 0;
        page_ = 0;
        pageAnim_ = 0;
        hover_ = -1;
    }

    // ---- geometry ----
    void Layout(float W, float H) {
        W_ = W;
        H_ = H;
        icon_ = std::clamp(H / 11.0f, 56.0f, 84.0f);
        cellW_ = icon_ * 2.0f;
        cellH_ = icon_ + 50;
        search_ = D2D1::RectF(W / 2 - 150, 64, W / 2 + 150, 64 + 34);
        float top = 140, bottom = H - 96;
        cols_ = std::clamp((int)(W * 0.86f / cellW_), 4, 9);
        rows_ = std::clamp((int)((bottom - top) / cellH_), 2, 6);
        perPage_ = cols_ * rows_;
        gridLeft_ = (W - cols_ * cellW_) / 2;
        gridTop_ = top + ((bottom - top) - rows_ * cellH_) / 2;
        if (perPage_ != normPer_ && !dragging_) { Normalize(); normPer_ = perPage_; }
        pages_n_ = PageCount();
        if (page_ >= pages_n_) page_ = pages_n_ - 1;
    }

    D2D1_RECT_F Cell(int page, int k) const {
        float x = gridLeft_ + (k % cols_) * cellW_ + (page - pageAnim_) * W_;
        float y = gridTop_ + (k / cols_) * cellH_;
        return D2D1::RectF(x, y, x + cellW_, y + cellH_);
    }
    D2D1_RECT_F IconIn(const D2D1_RECT_F& c) const {
        float cx = (c.left + c.right) / 2;
        return D2D1::RectF(cx - icon_ / 2, c.top + 4, cx + icon_ / 2, c.top + 4 + icon_);
    }

    D2D1_RECT_F Dot(int p) const {
        float total = pages_n_ * 18.0f;
        float x = W_ / 2 - total / 2 + p * 18 + 5;
        return D2D1::RectF(x, H_ - 60, x + 8, H_ - 52);
    }

    // Grid cell under the pointer on the current page (-1 outside the grid)
    int SlotAt(float x, float y) const {
        if (x < gridLeft_ || x >= gridLeft_ + cols_ * cellW_ || y < gridTop_ || y >= gridTop_ + rows_ * cellH_) return -1;
        int c = std::clamp((int)((x - gridLeft_) / cellW_), 0, cols_ - 1);
        int r = std::clamp((int)((y - gridTop_) / cellH_), 0, rows_ - 1);
        return r * cols_ + c;
    }

    // A tile (not empty space) under the pointer on the current page
    int HitSlot(float x, float y, int count) const {
        int s = SlotAt(x, y);
        if (s < 0 || s >= count) return -1;
        D2D1_RECT_F c = Cell(page_, s);
        D2D1_RECT_F r = D2D1::RectF(c.left + 8, c.top, c.right - 8, c.bottom - 4);
        return In(r, x, y) ? s : -1;
    }
    int ShownCount() const {   // tiles on the current page
        if (!query_.empty() && !renaming_) return std::clamp((int)results_.size() - page_ * perPage_, 0, perPage_);
        auto d = Display();
        return page_ < (int)d.size() ? (int)d[page_].size() : 0;
    }

    // ---- the open folder ----
    int FolderCount() const {   // apps in the open folder, plus the gap while dragging inside it
        const LTile* f = const_cast<Launcher*>(this)->OpenFolder();
        return f ? (int)f->apps.size() + (dragging_ && fph_ >= 0 ? 1 : 0) : 0;
    }
    int FolderCols() const { return std::clamp(FolderCount(), 3, std::min(6, cols_)); }
    D2D1_RECT_F FolderPanel() const {
        int n = std::max(1, FolderCount()), fc = FolderCols();
        int fr = (n + fc - 1) / fc;
        float w = fc * cellW_ + 56, h = std::min(64 + fr * cellH_ + 28, H_ - 160);
        float l = (W_ - w) / 2, t = std::max(120.0f, (H_ - h) / 2);
        return D2D1::RectF(l, t, l + w, t + h);
    }
    D2D1_RECT_F FolderTitle() const {
        D2D1_RECT_F p = FolderPanel();
        return D2D1::RectF(p.left + 40, p.top + 14, p.right - 40, p.top + 52);
    }
    D2D1_RECT_F FolderCell(int k) const {
        D2D1_RECT_F p = FolderPanel();
        int fc = FolderCols();
        float x = p.left + 28 + (k % fc) * cellW_, y = p.top + 64 + (k / fc) * cellH_ - folderScroll_;
        return D2D1::RectF(x, y, x + cellW_, y + cellH_);
    }
    // A folder with more apps than fit scrolls (mouse wheel)
    float FolderMaxScroll() const {
        D2D1_RECT_F p = FolderPanel();
        int fc = FolderCols(), rows = (FolderCount() + fc - 1) / fc;
        float visible = (p.bottom - 14) - (p.top + 64);
        return std::max(0.0f, rows * cellH_ - visible);
    }
    int FolderSlotAt(float x, float y) const {   // where a dragged app would go inside the folder
        D2D1_RECT_F p = FolderPanel();
        int fc = FolderCols();
        int c = std::clamp((int)((x - p.left - 28) / cellW_), 0, fc - 1);
        int r = std::max(0, (int)((y - p.top - 64 + folderScroll_) / cellH_));
        return r * fc + c;
    }
    int HitFolderApp(float x, float y) const {
        const LTile* f = const_cast<Launcher*>(this)->OpenFolder();
        if (!f) return -1;
        D2D1_RECT_F p = FolderPanel();
        if (y < p.top + 56 || y > p.bottom - 8) return -1;   // the title, or scrolled out of sight
        for (int k = 0; k < (int)f->apps.size(); ++k) {
            D2D1_RECT_F c = FolderCell(k);
            if (In(D2D1::RectF(c.left + 8, c.top, c.right - 8, c.bottom - 4), x, y)) return k;
        }
        return -1;
    }

    void CloseFolder() {
        if (renaming_) FinishRename();
        // you dragged apps out of it and one is left: it becomes that app again (none left: it goes away)
        if (LTile* f = OpenFolder(); f && draggedOut_) {
            bool gone = f->apps.size() <= 1;   // (asked now: f may not exist after this)
            if (f->apps.size() == 1) { LTile t; t.app = f->apps[0]; *f = std::move(t); SaveLayout(); }
            else if (f->apps.empty()) {
                auto& pg = pages_[openFolder_.p];
                pg.erase(pg.begin() + openFolder_.i);
                Normalize();
                normPer_ = perPage_;
                SaveLayout();
            }
            // the folder an app is being dragged out of is gone: don't let the drop find "it" at a place now held by another tile
            if (fromFolder_ && gone) originFolder_ = {};
        }
        draggedOut_ = false;
        folderScroll_ = 0;
        openFolder_ = {};
        hover_ = -1;
    }

    void StartRename() {
        LTile* f = OpenFolder();
        if (!f) return;
        renaming_ = true;
        savedQuery_ = query_;
        query_ = f->name;
        caretOn_ = true;
    }
    void FinishRename() {
        if (!renaming_) return;
        renaming_ = false;
        if (LTile* f = OpenFolder()) {
            f->name = CleanFolderName(query_);
            if (f->name.empty()) f->name = TR(L"Mappe", L"Folder");
        }
        query_ = savedQuery_;
        SaveLayout();
    }

    // ---- paging ----
    void GoPage(int p) {
        p = std::clamp(p, 0, pages_n_ - 1);
        if (p == page_ && !swiping_ && pageAnim_ == (float)p) return;
        swiping_ = false;
        page_ = p;
        hover_ = -1;
        pageFrom_ = pageAnim_;
        pageStart_ = NowMs();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool WantsFrame() const override { return swiping_ || pageAnim_ != (float)page_; }

    void Tick(double now) override {
        if (swiping_) {
            if (now - lastSwipe_ < 110) return;      // fingers still moving
            // Let go: settle on the nearest page, nudged by how fast you were swiping
            float target = std::round(pageAnim_ + std::clamp(swipeVel_ * 0.18f, -0.6f, 0.6f));
            swiping_ = false;
            page_ = std::clamp((int)target, 0, pages_n_ - 1);
            pageFrom_ = pageAnim_;
            pageStart_ = now;
            pageDur_ = 300;
            return;
        }
        if (pageAnim_ == (float)page_) return;
        float t = (float)((now - pageStart_) / pageDur_);
        pageAnim_ = t >= 1 ? (float)page_ : pageFrom_ + (page_ - pageFrom_) * Ease(t);
    }

    // Two-finger swipe on a precision touchpad: the pages follow your fingers
    void Swipe(float pages) {
        double now = NowMs();
        if (!swiping_) { swiping_ = true; swipeVel_ = 0; lastSwipe_ = now; }
        float lo = 0, hi = (float)(pages_n_ - 1);
        if ((pageAnim_ < lo && pages < 0) || (pageAnim_ > hi && pages > 0)) pages *= 0.3f;   // rubber band at the ends
        pageAnim_ = std::clamp(pageAnim_ + pages, lo - 0.25f, hi + 0.25f);
        double dt = std::max(1.0, now - lastSwipe_);
        swipeVel_ = swipeVel_ * 0.6f + (float)(pages / dt * 1000.0) * 0.4f;   // pages per second, smoothed
        lastSwipe_ = now;
        hover_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Mouse wheel notches flip a page; touchpads send small steps and swipe smoothly
    void Wheel(int delta, bool horizontal) {
        if (OpenFolder()) {
            float mx = FolderMaxScroll();
            if (mx <= 0) return;
            folderScroll_ = std::clamp(folderScroll_ - delta / (float)WHEEL_DELTA * cellH_ * 0.6f, 0.0f, mx);
            folderHover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (pages_n_ <= 1) return;
        bool notch = delta % WHEEL_DELTA == 0 && !swiping_;
        float dir = horizontal ? 1.0f : -1.0f;
        if (!notch) { Swipe(dir * delta / 900.0f); return; }
        if (GetTickCount() - lastFlip_ < 180) return;
        lastFlip_ = GetTickCount();
        pageDur_ = 340;
        GoPage(page_ + (dir * delta > 0 ? 1 : -1));
    }

    // ---- dragging ----
    void ResetDrag() {
        dragging_ = false;
        downPos_ = {};
        downFolderApp_ = -1;
        origin_ = ph_ = merge_ = originFolder_ = {};
        originFolderIdx_ = fph_ = -1;
        fromFolder_ = overDock_ = false;
        pendingSlot_ = -1;
        if (hwnd_) KillTimer(hwnd_, T_REORDER);
        ghost_.Hide();
    }

    POINT ScreenPt(float x, float y) const {
        POINT p{(LONG)std::lround(x * dpi_ / 96.0f), (LONG)std::lround(y * dpi_ / 96.0f)};
        ClientToScreen(hwnd_, &p);
        return p;
    }

    POINT GhostTopLeft() const {
        float half = icon_ / 2;
        return ScreenPt(dragPos_.x - grabOff_.x - half, dragPos_.y - grabOff_.y - half);
    }

    void BeginDrag(float x, float y, const D2D1_RECT_F& iconRect) {
        dragging_ = true;
        dragPos_ = {x, y};
        grabOff_ = {x - (iconRect.left + iconRect.right) / 2, y - (iconRect.top + iconRect.bottom) / 2};
        hover_ = -1;
        SetCapture(hwnd_);
        if (!drag_.folder) {
            int px = (int)std::lround(icon_ * dpi_ / 96.0f);
            if (AppIconCached(drag_.app, px)) ghost_.Show(AppIcon(drag_.app, px).Get(), GhostTopLeft());
        }
    }

    void StartDragFromGrid(Pos at, float x, float y) {
        LTile* t = TileAt(at);
        if (!t) return;
        // where it is on screen before we take it out
        auto d = Display();
        D2D1_RECT_F icon{};
        for (int s = 0; page_ < (int)d.size() && s < (int)d[page_].size(); ++s)
            if (d[page_][s].p == at.p && d[page_][s].i == at.i) icon = IconIn(Cell(page_, s));
        drag_ = std::move(*t);
        pages_[at.p].erase(pages_[at.p].begin() + at.i);
        origin_ = ph_ = at;
        fromFolder_ = false;
        BeginDrag(x, y, icon);
    }

    void StartDragFromFolder(int k, float x, float y) {
        LTile* f = OpenFolder();
        if (!f || k < 0 || k >= (int)f->apps.size()) return;
        D2D1_RECT_F icon = IconIn(FolderCell(k));
        drag_ = LTile{};
        drag_.app = f->apps[k];
        f->apps.erase(f->apps.begin() + k);
        draggedOut_ = true;
        fromFolder_ = true;
        originFolder_ = openFolder_;
        originFolderIdx_ = fph_ = k;
        ph_ = {openFolder_.p, openFolder_.i + 1};   // if it leaves the folder: right after it
        BeginDrag(x, y, icon);
    }

    bool DockContains(POINT screen) const {
        if (Dock* d = g_app.DockOn(mon_)) {
            RECT r;
            if (d->ScreenRect(&r) && PtInRect(&r, screen)) return true;
        }
        return false;
    }

    void DragMove(float x, float y) {
        dragPos_ = {x, y};
        if (ghost_.Visible()) ghost_.Move(GhostTopLeft());
        overDock_ = !drag_.folder && DockContains(ScreenPt(x, y));
        merge_ = {};
        if (OpenFolder()) {
            if (In(FolderPanel(), x, y) && !overDock_) {
                const LTile* f = OpenFolder();
                fph_ = std::min(FolderSlotAt(x, y), (int)f->apps.size());
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            // Out of the folder: it closes and the app goes on to the grid
            fph_ = -1;
            CloseFolder();
        }
        fph_ = -1;
        if (overDock_) { InvalidateRect(hwnd_, nullptr, FALSE); return; }

        bool edge = x < 40 || x > W_ - 40;
        if (edge && !edgeArmed_) { SetTimer(hwnd_, T_EDGE, 650, nullptr); edgeArmed_ = true; }
        if (!edge && edgeArmed_) { KillTimer(hwnd_, T_EDGE); edgeArmed_ = false; }

        int slot = SlotAt(x, y);
        if (slot >= 0) {
            auto d = Display();
            if (page_ < (int)d.size()) {
                const auto& row = d[page_];
                // Over the middle of another app's icon: they'll make (or join) a folder
                if (!drag_.folder && slot < (int)row.size() && !row[slot].gap) {
                    D2D1_RECT_F ic = IconIn(Cell(page_, slot));
                    float in = icon_ * 0.2f;
                    if (In(D2D1::RectF(ic.left + in, ic.top + in, ic.right - in, ic.bottom - in), x, y)) {
                        merge_ = {row[slot].p, row[slot].i};
                        KillTimer(hwnd_, T_REORDER);
                        pendingSlot_ = -1;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return;
                    }
                }
                // Over another tile (not its middle yet): wait a moment before making room, so you can
                // still reach its middle to make a folder. Moving the others away at once made that impossible.
                if (slot < (int)row.size() && !row[slot].gap) {
                    if (slot != pendingSlot_) {
                        pendingSlot_ = slot;
                        SetTimer(hwnd_, T_REORDER, 300, nullptr);
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            KillTimer(hwnd_, T_REORDER);
            pendingSlot_ = -1;
            MovePlaceholder(slot);   // an empty spot (or the gap itself): right away
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void MovePlaceholder(int slot) {
        if (page_ < (int)pages_.size()) ph_ = {page_, std::min(slot, (int)pages_[page_].size())};
        else ph_ = {(int)pages_.size(), 0};   // the empty page at the end: a new page
    }

    void DragEnd() {
        ghost_.Hide();
        KillTimer(hwnd_, T_EDGE);
        edgeArmed_ = false;
        bool wasDragging = dragging_;
        dragging_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (!wasDragging) { ResetDrag(); return; }

        LTile* f = OpenFolder();
        if (overDock_ && !drag_.folder) {
            PinToDock(drag_.app);
            // …and the app stays where it was in SkyLauncher
            LTile* home = fromFolder_ ? TileAt(originFolder_) : nullptr;
            if (home && home->folder) home->apps.insert(home->apps.begin() + std::min<size_t>(originFolderIdx_, home->apps.size()), drag_.app);
            else Insert(fromFolder_ ? ph_ : origin_, std::move(drag_));
        } else if (f && fph_ >= 0) {
            f->apps.insert(f->apps.begin() + std::min<size_t>(fph_, f->apps.size()), drag_.app);
        } else if (LTile* t = TileAt(merge_)) {
            if (t->folder) {
                t->apps.push_back(drag_.app);
            } else {
                LTile nf;
                nf.folder = true;
                nf.name = TR(L"Mappe", L"Folder");
                nf.apps = {t->app, drag_.app};
                *t = std::move(nf);
            }
        } else {
            Insert(ph_, std::move(drag_));
        }
        ResetDrag();
        Normalize();
        normPer_ = perPage_;
        pages_n_ = PageCount();
        if (page_ >= pages_n_) GoPage(pages_n_ - 1);
        SaveLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Insert(Pos at, LTile t) {
        if (at.p < 0) at = {(int)pages_.size() - 1, (int)pages_.back().size()};
        while (at.p >= (int)pages_.size()) pages_.emplace_back();
        auto& pg = pages_[at.p];
        pg.insert(pg.begin() + std::clamp(at.i, 0, (int)pg.size()), std::move(t));
    }

    // ---- drawing ----
    void DrawTile(const LTile& t, const D2D1_RECT_F& c, float alpha, bool hot, bool selected, bool target, int px) {
        const Theme& th = g_app.theme;
        if (hot || selected) {
            brush_->SetColor(selected ? Alpha(th.accent, 0.22f * alpha) : Alpha(th.hover, alpha));
            D2D1_RECT_F h = D2D1::RectF(c.left + 10, c.top - 6, c.right - 10, c.bottom - 6);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(h, 14, 14), brush_.Get());
        }
        D2D1_RECT_F ir = IconIn(c);
        if (target) {   // the dragged app will join this one
            float g = icon_ * 0.12f;
            brush_->SetColor(Alpha(th.fg, 0.22f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left - g, ir.top - g, ir.right + g, ir.bottom + g), icon_ * 0.3f, icon_ * 0.3f),
                                      brush_.Get());
        }
        if (t.folder) {
            float rad = icon_ * 0.24f;
            brush_->SetColor(Alpha(th.fg, (th.dark ? 0.16f : 0.12f) * alpha));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(ir, rad, rad), brush_.Get());
            brush_->SetColor(Alpha(th.fg, 0.10f * alpha));
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left + 0.5f, ir.top + 0.5f, ir.right - 0.5f, ir.bottom - 0.5f), rad, rad),
                                      brush_.Get(), 1.0f);
            float pad = icon_ * 0.12f, gap = icon_ * 0.06f;
            float m = (icon_ - 2 * pad - 2 * gap) / 3;
            for (int k = 0; k < (int)t.apps.size() && k < 9; ++k) {
                float x = ir.left + pad + (k % 3) * (m + gap), y = ir.top + pad + (k / 3) * (m + gap);
                if (ID2D1Bitmap* b = Icon(t.apps[k], px)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(x, y, x + m, y + m), alpha);
            }
        } else if (ID2D1Bitmap* b = Icon(t.app, px)) {
            DrawImageHQ(rt_.Get(), b, ir, alpha);
        } else {
            brush_->SetColor(Alpha(th.fg, 0.06f * alpha));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(ir, icon_ * 0.22f, icon_ * 0.22f), brush_.Get());
        }
        DrawText(t.Label(), fLabel_.Get(), D2D1::RectF(c.left + 6, ir.bottom + 6, c.right - 6, ir.bottom + 26), Alpha(th.fg, alpha));
    }

    void Paint() override {
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        Layout(sz.width, sz.height);
        if (!fLabel_) {
            fLabel_ = Format(L"Segoe UI", 12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fSearch_ = Format(L"Segoe UI", 14);
            fGlyph_ = Format(IconFont(), 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fHint_ = Format(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fFolder_ = Format(L"Segoe UI", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fPower_ = Format(IconFont(), 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fPowerLabel_ = Format(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        rt_->BeginDraw();
        {
            rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
            // see-through: the tint, fading in with the rest
            D2D1_COLOR_F tc = GlassTint(Part::Start);
            tc.a *= anim_;
            brush_->SetColor(tc);
            rt_->FillRectangle(D2D1::RectF(0, 0, W_, H_), brush_.Get());
            PaintVibe(rt_.Get(), D2D1::RectF(0, 0, W_, H_), Part::Start, anim_);
        }

        // Launchpad-style entrance: a touch larger, fading in (no layer once fully open)
        ComPtr<ID2D1Layer> layer;
        bool fading = anim_ < 0.999f;
        if (fading) {
            rt_->CreateLayer(layer.Put());
            float s = 1.06f - 0.06f * anim_;
            rt_->SetTransform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(W_ / 2, H_ / 2)));
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), anim_),
                           layer.Get());
        }

        LTile* folder = OpenFolder();
        float gridAlpha = folder ? 0.22f : 1.0f;
        {   // the search field (the folder name takes the typing while renaming)
            std::wstring keep;
            bool caret = caretOn_;
            if (renaming_) { keep.swap(query_); caretOn_ = false; }
            DrawSearch(search_, TR(L"Søk", L"Search"), fSearch_.Get(), fGlyph_.Get(), true);
            if (renaming_) { keep.swap(query_); caretOn_ = caret; }
        }

        int px = (int)std::lround(icon_ * dpi_ / 96.0f);
        bool searching = !query_.empty() && !renaming_;
        if (searching) {
            int first = std::max(0, (int)std::floor(pageAnim_) * perPage_);
            int last = std::min((int)results_.size(), ((int)std::ceil(pageAnim_) + 1) * perPage_);
            for (int i = first; i < last; ++i) {
                D2D1_RECT_F c = Cell(i / perPage_, i % perPage_);
                if (c.right < 0 || c.left > W_) continue;
                LTile t;
                t.app = results_[i];
                DrawTile(t, c, 1.0f, i / perPage_ == page_ && i % perPage_ == hover_, i == sel_, false, px);
            }
            if (results_.empty())
                DrawText(TR(L"Ingen apper funnet. Trykk Enter for å søke i filer.", L"No apps found. Press Enter to search your files."),
                         fHint_.Get(), D2D1::RectF(0, H_ / 2 - 20, W_, H_ / 2 + 20), th.fgDim);
        } else {
            auto d = Display();
            int p0 = std::max(0, (int)std::floor(pageAnim_)), p1 = std::min((int)d.size() - 1, (int)std::ceil(pageAnim_));
            for (int dp = p0; dp <= p1; ++dp)
                for (int s = 0; s < (int)d[dp].size(); ++s) {
                    const Disp& e = d[dp][s];
                    if (e.gap) continue;
                    D2D1_RECT_F c = Cell(dp, s);
                    if (c.right < 0 || c.left > W_) continue;
                    bool open = folder && openFolder_.p == e.p && openFolder_.i == e.i;
                    DrawTile(pages_[e.p][e.i], c, open ? 0.0f : gridAlpha, !dragging_ && !folder && dp == page_ && s == hover_, false,
                             dragging_ && merge_.p == e.p && merge_.i == e.i, px);
                }
            if (dragging_ && page_ == (int)d.size() - 1 && d.back().empty() && !folder)
                DrawText(TR(L"Slipp her for en ny side", L"Drop here for a new page"), fHint_.Get(),
                         D2D1::RectF(0, H_ / 2 - 20, W_, H_ / 2 + 20), th.fgDim);
        }

        if (pages_n_ > 1 && !folder) {
            for (int p = 0; p < pages_n_; ++p) {
                D2D1_RECT_F dot = Dot(p);
                brush_->SetColor(p == page_ ? th.fg : Alpha(th.fg, 0.30f));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((dot.left + dot.right) / 2, (dot.top + dot.bottom) / 2), 4, 4), brush_.Get());
            }
        }

        if (folder) {
            D2D1_RECT_F pr = FolderPanel();
            brush_->SetColor(Alpha(th.dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f) : D2D1::ColorF(0.97f, 0.97f, 0.98f), 0.55f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(pr, 26, 26), brush_.Get());
            brush_->SetColor(Alpha(th.fg, 0.12f));
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pr.left + 0.5f, pr.top + 0.5f, pr.right - 0.5f, pr.bottom - 0.5f), 26, 26),
                                      brush_.Get(), 1.0f);
            D2D1_RECT_F tr = FolderTitle();
            if (renaming_ || titleHover_) {
                brush_->SetColor(Alpha(th.fg, renaming_ ? 0.10f : 0.06f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(tr, 10, 10), brush_.Get());
            }
            DrawText(folder->name, fFolder_.Get(), tr, th.fg);
            if (renaming_ && caretOn_) {
                float tw = TextWidth(folder->name, fFolder_.Get());
                float cx = (tr.left + tr.right) / 2 + tw / 2 + 2, cy = (tr.top + tr.bottom) / 2;
                brush_->SetColor(th.fg);
                rt_->FillRectangle(D2D1::RectF(cx, cy - 11, cx + 1.4f, cy + 11), brush_.Get());
            }
            if (folderScroll_ > FolderMaxScroll()) folderScroll_ = FolderMaxScroll();
            rt_->PushAxisAlignedClip(D2D1::RectF(pr.left, pr.top + 56, pr.right, pr.bottom - 8), D2D1_ANTIALIAS_MODE_ALIASED);   // apps scroll under the title
            for (int k = 0, slot = 0; k < (int)folder->apps.size(); ++k, ++slot) {
                if (dragging_ && fph_ >= 0 && slot == fph_) ++slot;   // the gap for the app being dragged
                LTile t;
                t.app = folder->apps[k];
                DrawTile(t, FolderCell(slot), 1.0f, !dragging_ && k == folderHover_, false, false, px);
            }
            if (folder->apps.empty() && !dragging_) {   // a new, empty folder: say how to fill it
                D2D1_RECT_F c = FolderCell(0);
                DrawText(TR(L"Tom mappe – dra apper oppå mappen for å legge dem til.", L"Empty folder – drag apps onto it to add them."),
                         fLabel_.Get(), D2D1::RectF(pr.left + 20, c.top + 20, pr.right - 20, c.top + 60), th.fgDim);
            }
            rt_->PopAxisAlignedClip();
        }

        // the tile being dragged, when it has no floating window of its own
        if (dragging_ && !ghost_.Visible()) {
            D2D1_RECT_F c = D2D1::RectF(dragPos_.x - grabOff_.x - cellW_ / 2, dragPos_.y - grabOff_.y - icon_ / 2 - 4,
                                        dragPos_.x - grabOff_.x + cellW_ / 2, dragPos_.y - grabOff_.y - icon_ / 2 - 4 + cellH_);
            DrawTile(drag_, c, 0.9f, false, false, false, px);
        }
        if (dragging_ && overDock_) {
            if (Dock* dk = g_app.DockOn(mon_)) {
                RECT r;
                if (dk->ScreenRect(&r)) {
                    POINT tl{r.left, r.top};
                    ScreenToClient(hwnd_, &tl);
                    float top = tl.y * 96.0f / dpi_;
                    DrawText(TR(L"Slipp for å feste i docken", L"Drop to pin to the dock"), fHint_.Get(),
                             D2D1::RectF(0, top - 40, W_, top - 10), th.fg);
                }
            }
        }

        // Restart / shut down
        for (int i = 0; i < 2 && !folder; ++i) {
            D2D1_RECT_F b = PowerBtn(i);
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F((b.left + b.right) / 2, (b.top + b.bottom) / 2), (b.right - b.left) / 2,
                                           (b.bottom - b.top) / 2);
            bool armed = armed_ == i, hov = powerHover_ == i;
            brush_->SetColor(armed ? Alpha(th.red, 0.9f) : Alpha(th.fg, hov ? 0.17f : 0.08f));
            rt_->FillEllipse(e, brush_.Get());
            brush_->SetColor(Alpha(th.fg, 0.12f));
            rt_->DrawEllipse(D2D1::Ellipse(e.point, e.radiusX - 0.5f, e.radiusY - 0.5f), brush_.Get(), 1.0f);
            DrawGlyph(i == 0 ? 0xE72C : 0xE7E8, fPower_.Get(), b, armed ? D2D1::ColorF(1, 1, 1) : th.fg);
        }
        int shownBtn = armed_ >= 0 ? armed_ : powerHover_;
        if (shownBtn >= 0 && !folder) {
            const wchar_t* label = armed_ >= 0
                ? (armed_ == 0 ? TR(L"Klikk igjen for å starte på nytt", L"Click again to restart")
                               : TR(L"Klikk igjen for å slå av", L"Click again to shut down"))
                : (powerHover_ == 0 ? TR(L"Start på nytt", L"Restart") : TR(L"Slå av", L"Shut down"));
            D2D1_RECT_F b1 = PowerBtn(1);
            DrawText(label, fPowerLabel_.Get(), D2D1::RectF(W_ - 460, b1.top - 30, b1.right, b1.top - 8), armed_ >= 0 ? th.fg : th.fgDim);
        }

        if (fading) rt_->PopLayer();
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); }
    }

    // Restart (0) and shut down (1), round buttons in the bottom-right corner
    D2D1_RECT_F PowerBtn(int i) const {
        const float s = 44, gap = 12;
        float x = W_ - 40 - s - (1 - i) * (s + gap);
        float y = H_ - 40 - s;
        return D2D1::RectF(x, y, x + s, y + s);
    }
    int HitPower(float x, float y) const {
        if (const_cast<Launcher*>(this)->OpenFolder()) return -1;
        for (int i = 0; i < 2; ++i) if (In(PowerBtn(i), x, y)) return i;
        return -1;
    }
    // First click arms the button (it turns red and says what will happen), the second does it.
    void PressPower(int i) {
        if (armed_ != i) {
            armed_ = i;
            SetTimer(hwnd_, T_POWER, 4000, nullptr);   // forget it if nobody confirms
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        Close();
        if (i == 0) RestartPc();
        else ShutDownPc();
    }

    void LaunchApp_(const AppEntry& a) {
        AppEntry copy = a;
        Close();
        LaunchApp(copy);
    }

    // Open a folder you just made, with its name ready to type
    void OpenNewFolder(Pos at) {
        openFolder_ = at; folderScroll_ = 0;
        folderHover_ = -1;
        titleHover_ = false;
        draggedOut_ = false;
        StartRename();
        if (GetForegroundWindow() != hwnd_) ForceForeground(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Right-click on empty space in the grid
    void EmptySpaceMenu(POINT screen) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, TR(L"Lag mappe", L"Create folder"));
        inMenu_ = true;
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd_, nullptr);
        inMenu_ = false;
        DestroyMenu(m);
        if (!hwnd_ || cmd != 1) return;
        // on this page if there's room, else at the very end
        Pos at;
        if (page_ < (int)pages_.size() && (int)pages_[page_].size() < perPage_) at = {page_, (int)pages_[page_].size()};
        else if ((int)pages_.back().size() < perPage_) at = {(int)pages_.size() - 1, (int)pages_.back().size()};
        else { pages_.emplace_back(); at = {(int)pages_.size() - 1, 0}; }
        LTile nf;
        nf.folder = true;
        nf.name = TR(L"Mappe", L"Folder");
        Insert(at, std::move(nf));
        SaveLayout();
        pages_n_ = PageCount();
        if (at.p != page_) GoPage(at.p);
        OpenNewFolder(at);
    }

    // Right-click on a folder
    void FolderMenu(Pos at, POINT screen) {
        enum { COpen = 1, CRename, CUngroup };
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, COpen, TR(L"Åpne", L"Open"));
        AppendMenuW(m, MF_STRING, CRename, TR(L"Gi nytt navn", L"Rename"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CUngroup, TR(L"Løs opp mappen", L"Ungroup folder"));
        inMenu_ = true;
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd_, nullptr);
        inMenu_ = false;
        DestroyMenu(m);
        if (!hwnd_) return;
        LTile* t = TileAt(at);
        if (!t || !t->folder) return;
        switch (cmd) {
        case COpen: openFolder_ = at; folderScroll_ = 0; break;
        case CRename: openFolder_ = at; folderScroll_ = 0; StartRename(); break;
        case CUngroup: {
            std::vector<AppEntry> apps = t->apps;
            auto& pg = pages_[at.p];
            pg.erase(pg.begin() + at.i);
            for (size_t k = 0; k < apps.size(); ++k) {
                LTile a;
                a.app = apps[k];
                pg.insert(pg.begin() + at.i + k, std::move(a));
            }
            Normalize();
            normPer_ = perPage_;
            SaveLayout();
            break;
        }
        }
        if (cmd && GetForegroundWindow() != hwnd_) ForceForeground(hwnd_);
    }

    // The tile under the pointer on the grid, in pages_ terms
    Pos GridTileAt(float x, float y) const {
        auto d = Display();
        if (page_ >= (int)d.size()) return {};
        int s = HitSlot(x, y, (int)d[page_].size());
        if (s < 0 || d[page_][s].gap) return {};
        return {d[page_][s].p, d[page_][s].i};
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
        switch (msg) {
        case WM_TIMER:
            if (wp == T_REFRESH) {
                KillTimer(hwnd_, T_REFRESH);
                InvalidateRect(hwnd_, nullptr, FALSE);   // the list itself is refreshed after closing
                return 0;
            }
            if (wp == T_POWER) {
                KillTimer(hwnd_, T_POWER);
                armed_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wp == T_FILES) {
                KillTimer(hwnd_, T_FILES);
                if (!query_.empty() && !renaming_) FileSearch::Query(hwnd_, fileTicket_, query_, 12);
                return 0;
            }
            if (wp == T_REORDER) {   // rested over a tile: make room there
                KillTimer(hwnd_, T_REORDER);
                if (dragging_ && pendingSlot_ >= 0 && !merge_.Valid()) MovePlaceholder(pendingSlot_);
                pendingSlot_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wp == T_EDGE) {   // dragging at the screen edge: next/previous page (or a new one)
                KillTimer(hwnd_, T_EDGE);
                edgeArmed_ = false;
                if (dragging_ && !OpenFolder()) {
                    pages_n_ = PageCount();
                    if (dragPos_.x < 40) GoPage(page_ - 1);
                    else if (dragPos_.x > W_ - 40) GoPage(page_ + 1);
                    DragMove(dragPos_.x, dragPos_.y);
                    if (dragPos_.x < 40 || dragPos_.x > W_ - 40) { SetTimer(hwnd_, T_EDGE, 800, nullptr); edgeArmed_ = true; }   // keep going
                }
                return 0;
            }
            return 0;
        case FileSearch::WM_SKY_FILES: {
            std::unique_ptr<std::vector<AppEntry>> found(reinterpret_cast<std::vector<AppEntry>*>(lp));
            if ((unsigned)wp != fileTicket_ || query_.empty() || renaming_) return 0;
            results_.erase(std::remove_if(results_.begin(), results_.end(), [](const AppEntry& a) { return a.file; }), results_.end());
            for (const auto& f : *found)
                if ((int)results_.size() < perPage_) results_.push_back(f);
            if (sel_ < 0 && !results_.empty()) sel_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(wp), false); return 0;
        case WM_MOUSEHWHEEL: Wheel(GET_WHEEL_DELTA_WPARAM(wp), true); return 0;
        case WM_MOUSEMOVE: {
            if ((wp & MK_LBUTTON) && query_.empty() && !renaming_) {
                if (!dragging_ && (std::fabs(x - downPt_.x) > 8 || std::fabs(y - downPt_.y) > 8)) {
                    if (OpenFolder() && downFolderApp_ >= 0) StartDragFromFolder(downFolderApp_, x, y);
                    else if (!OpenFolder() && downPos_.Valid()) StartDragFromGrid(downPos_, x, y);
                    downPos_ = {};
                    downFolderApp_ = -1;
                }
                if (dragging_) { DragMove(x, y); return 0; }
            }
            int ph = HitPower(x, y);
            if (ph != powerHover_) { powerHover_ = ph; InvalidateRect(hwnd_, nullptr, FALSE); }
            if (OpenFolder()) {
                int fh = HitFolderApp(x, y);
                bool th = In(FolderTitle(), x, y);
                if (fh != folderHover_ || th != titleHover_) { folderHover_ = fh; titleHover_ = th; InvalidateRect(hwnd_, nullptr, FALSE); }
                return 0;
            }
            int h = HitSlot(x, y, ShownCount());
            if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN:
            downPt_ = {x, y};
            downPos_ = {};
            downFolderApp_ = -1;
            if (renaming_ && !In(FolderTitle(), x, y)) FinishRename();
            if (OpenFolder()) downFolderApp_ = HitFolderApp(x, y);
            else if (query_.empty()) downPos_ = GridTileAt(x, y);
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging_ && (HWND)lp != hwnd_) DragEnd();   // capture taken away mid-drag: keep what we have
            return 0;
        case WM_LBUTTONUP: {
            if (dragging_) { DragEnd(); return 0; }
            Pos down = downPos_;
            int downFolder = downFolderApp_;
            downPos_ = {};
            downFolderApp_ = -1;
            if (LTile* f = OpenFolder()) {
                if (!In(FolderPanel(), x, y)) { CloseFolder(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
                if (In(FolderTitle(), x, y)) { if (!renaming_) StartRename(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
                int k = HitFolderApp(x, y);
                if (k >= 0 && k == downFolder) LaunchApp_(f->apps[k]);
                return 0;
            }
            int pw = HitPower(x, y);
            if (pw >= 0) { PressPower(pw); return 0; }
            if (armed_ >= 0) { armed_ = -1; KillTimer(hwnd_, T_POWER); InvalidateRect(hwnd_, nullptr, FALSE); }
            for (int p = 0; p < pages_n_ && pages_n_ > 1; ++p) {
                D2D1_RECT_F dd = Dot(p);
                if (In(D2D1::RectF(dd.left - 5, dd.top - 8, dd.right + 5, dd.bottom + 8), x, y)) { GoPage(p); return 0; }
            }
            if (!query_.empty()) {
                int h = HitSlot(x, y, ShownCount());
                if (h >= 0) { LaunchApp_(results_[page_ * perPage_ + h]); return 0; }
                if (!In(search_, x, y)) Close();
                return 0;
            }
            Pos at = GridTileAt(x, y);
            if (at.Valid() && at == down) {
                LTile* t = TileAt(at);
                if (t->folder) { openFolder_ = at; folderScroll_ = 0; folderHover_ = -1; titleHover_ = false; InvalidateRect(hwnd_, nullptr, FALSE); }
                else LaunchApp_(t->app);
                return 0;
            }
            if (!at.Valid() && !In(search_, x, y)) Close();   // empty space closes, like Launchpad
            return 0;
        }
        case WM_RBUTTONUP: {
            if (dragging_) return 0;
            POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &s);
            AppEntry a;
            if (LTile* f = OpenFolder()) {
                int k = HitFolderApp(x, y);
                if (k < 0) return 0;
                a = f->apps[k];
            } else if (!query_.empty()) {
                int h = HitSlot(x, y, ShownCount());
                if (h < 0) return 0;
                a = results_[page_ * perPage_ + h];
            } else {
                Pos at = GridTileAt(x, y);
                LTile* t = TileAt(at);
                if (!t) {   // empty space: a new, empty folder
                    if (!In(search_, x, y) && HitPower(x, y) < 0) EmptySpaceMenu(s);
                    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (t->folder) { FolderMenu(at, s); if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
                AppEntry one = t->app;
                bool wantFolder = false;
                inMenu_ = true;
                bool launched = AppContextMenu(hwnd_, one, s, IsMenuPinned(one), [one](bool pin) { SetMenuPinned(one, pin); }, {},
                                               [&wantFolder] { wantFolder = true; });
                inMenu_ = false;
                if (!hwnd_) return 0;
                if (wantFolder) {   // the app becomes a folder with it inside; name it straight away
                    if (LTile* tt = TileAt(at); tt && !tt->folder) {
                        LTile nf;
                        nf.folder = true;
                        nf.name = TR(L"Mappe", L"Folder");
                        nf.apps = {tt->app};
                        *tt = std::move(nf);
                        SaveLayout();
                        OpenNewFolder(at);
                    }
                    return 0;
                }
                if (launched || GetForegroundWindow() != hwnd_) Close();
                return 0;
            }
            inMenu_ = true;
            bool launched = AppContextMenu(hwnd_, a, s, IsMenuPinned(a), [a](bool pin) { SetMenuPinned(a, pin); });
            inMenu_ = false;
            if (!hwnd_) return 0;
            if (launched || GetForegroundWindow() != hwnd_) Close();
            return 0;
        }
        case WM_KEYDOWN: {
            if (dragging_) return 0;
            int n = (int)results_.size();
            bool searching = !query_.empty() && !renaming_;
            switch (wp) {
            case VK_ESCAPE:
                if (armed_ >= 0) { armed_ = -1; KillTimer(hwnd_, T_POWER); }
                else if (renaming_) FinishRename();
                else if (OpenFolder()) CloseFolder();
                else if (!query_.empty()) { query_.clear(); OnQuery(); sel_ = -1; }
                else Close();
                break;
            case VK_RETURN:
                if (renaming_) FinishRename();
                else if (searching && n > 0) LaunchApp_(results_[std::max(sel_, 0)]);
                else if (searching) { std::wstring q = query_; Close(); OpenFileSearch(q); }
                break;
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: {
                if (renaming_ || OpenFolder()) break;
                if (!searching) { if (wp == VK_LEFT || wp == VK_RIGHT) GoPage(page_ + (wp == VK_RIGHT ? 1 : -1)); break; }
                if (n == 0) break;
                int step = wp == VK_LEFT ? -1 : wp == VK_RIGHT ? 1 : wp == VK_UP ? -cols_ : cols_;
                sel_ = std::clamp(sel_ < 0 ? page_ * perPage_ : sel_ + step, 0, n - 1);
                if (sel_ / perPage_ != page_) GoPage(sel_ / perPage_);
                break;
            }
            case VK_PRIOR: GoPage(page_ - 1); break;
            case VK_NEXT: GoPage(page_ + 1); break;
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

private:
    std::vector<LPage> pages_;
    std::vector<AppEntry> results_;
    bool needRefresh_ = false;
    float W_ = 0, H_ = 0, icon_ = 64, cellW_ = 128, cellH_ = 110, gridLeft_ = 0, gridTop_ = 0;
    int cols_ = 7, rows_ = 5, perPage_ = 35, normPer_ = 0, pages_n_ = 1, page_ = 0;
    float pageAnim_ = 0, pageFrom_ = 0, swipeVel_ = 0;
    double pageStart_ = 0, lastSwipe_ = 0, pageDur_ = 340;
    DWORD lastFlip_ = 0;
    bool swiping_ = false;
    int wheel_ = 0;
    int hover_ = -1, sel_ = -1, folderHover_ = -1;
    bool titleHover_ = false;
    D2D1_RECT_F search_{};
    // folders
    Pos openFolder_;
    bool draggedOut_ = false;   // an app was dragged out of the open folder this time
    float folderScroll_ = 0;    // how far the open folder's apps are scrolled
    bool renaming_ = false;
    std::wstring savedQuery_;
    // dragging
    bool dragging_ = false, edgeArmed_ = false, fromFolder_ = false, overDock_ = false;
    LTile drag_;
    Pos downPos_, origin_, ph_, merge_, originFolder_;
    int downFolderApp_ = -1, originFolderIdx_ = -1, fph_ = -1;
    int pendingSlot_ = -1;               // tile we're resting over; room is made there after a moment
    D2D1_POINT_2F downPt_{}, dragPos_{}, grabOff_{};
    DragGhost ghost_;
    int powerHover_ = -1, armed_ = -1;   // restart / shut down buttons
    unsigned fileTicket_ = 0;            // the newest file search (when files are on for SkyLauncher)
    ComPtr<IDWriteTextFormat> fLabel_, fSearch_, fGlyph_, fHint_, fFolder_, fPower_, fPowerLabel_;
};

// ===========================================================================
// SkyMenu — the classic menu above the dock
// ===========================================================================
class Menu : public Surface {
public:
    void Show(HMONITOR mon) {
        mon_ = mon ? mon : CursorMonitor();
        dpi_ = MonitorDpi(mon_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon_, &mi);
        float W = 620, H = 580;
        int w = MulDiv((int)W, dpi_, 96), h = MulDiv((int)H, dpi_, 96);
        h = std::min<int>(h, (mi.rcWork.bottom - mi.rcWork.top) - MulDiv(24, dpi_, 96));
        int bottom = mi.rcWork.bottom - MulDiv(12, dpi_, 96);
        if (Dock* d = g_app.DockOn(mon_)) {
            int top = d->TopEdge(mon_);
            if (top != INT_MAX) bottom = std::min(bottom, top - MulDiv(10, dpi_, 96));
        }
        h = std::min(h, bottom - ((int)mi.rcWork.top + MulDiv(12, dpi_, 96)));   // never above the top of the screen
        int left = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
        RECT r{left, bottom - h, left + w, bottom};
        if (!CreateAt(r, WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) return;
        Glass(hwnd_, (BYTE)std::clamp(g_app.settings.tintPercent * 255 / 100 + 30, 0, 255), true);
        mode_ = Pinned;
        scroll_ = 0;
        hover_ = -1;
        sel_ = 0;
        tracking_ = false;
        pendingLetter_ = 0;
        pinDragging_ = false;
        downPin_ = dragPin_ = -1;
        LoadPins();
        if (AllApps(0xFFFFFFFF).empty()) AllApps();
        name_ = UserDisplayName();
        avatar_.Reset();
        int apx = MulDiv(44, dpi_, 96);
        if (!avatarSrc_ || avatarPx_ != apx) { avatarSrc_ = AccountPicture(apx); avatarPx_ = apx; }
        ShowAnimated();
        PrewarmIcons(MulDiv(24, dpi_, 96));   // for "All apps"
        SetTimer(hwnd_, T_REFRESH, 450, nullptr);
    }

protected:
    enum Mode { Pinned, All, Search, Letters };
    enum HitKind { HNone, HApp, HAllButton, HBackButton, HLetter, HLetterPick, HPlace, HUser, HPower, HFileSearch };
    struct Hit { D2D1_RECT_F r; HitKind kind; int idx; };

    const wchar_t* ClassName() const override { return L"SkylineSkyMenu"; }
    DWORD OpenMs() const override { return 160; }

    void LoadPins() {
        pins_.clear();
        pinKeys_ = MenuPins();
        for (const auto& t : pinKeys_) pins_.push_back(EntryFor(t));
    }

    // ---- dragging a pinned app to a new place ----
    float PinRowTop(int i) const { return listTop_ + 34 - scroll_ + i * kAppRow; }

    void PinDragMove(float y) {
        dragY_ = y;
        int n = (int)pins_.size();
        int to = std::clamp((int)std::floor((y - grabDy_ + kAppRow / 2 - (listTop_ + 34) + scroll_) / kAppRow), 0, n - 1);
        if (to != dragPin_ && dragPin_ >= 0 && dragPin_ < n) {
            AppEntry a = pins_[dragPin_];
            std::wstring k = pinKeys_[dragPin_];
            pins_.erase(pins_.begin() + dragPin_);
            pinKeys_.erase(pinKeys_.begin() + dragPin_);
            pins_.insert(pins_.begin() + to, a);
            pinKeys_.insert(pinKeys_.begin() + to, k);
            dragPin_ = to;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void PinDragEnd() {
        if (!pinDragging_) return;
        pinDragging_ = false;
        dragPin_ = downPin_ = -1;
        SaveMenuPins(pinKeys_);
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnQuery() override {
        bool files = g_app.settings.menuFileSearch;
        if (query_.empty()) { mode_ = Pinned; results_.clear(); }
        else { mode_ = Search; results_ = SearchApps(query_, true, files ? 6 : 9); }
        sel_ = 0;
        scroll_ = 0;
        hover_ = -1;
        // Files: asked from Windows' search index a moment after you stop typing
        ++fileTicket_;
        KillTimer(hwnd_, T_FILES);
        if (files && query_.size() >= 2) SetTimer(hwnd_, T_FILES, 150, nullptr);
    }

    struct Place { wchar_t glyph; const wchar_t* no; const wchar_t* en; int id; };
    static const std::vector<Place>& Places() {
        static const std::vector<Place> p = {
            {0xE8A5, L"Dokumenter", L"Documents", 0}, {0xE896, L"Nedlastinger", L"Downloads", 1}, {0xE91B, L"Bilder", L"Pictures", 2},
            {0xE8D6, L"Musikk", L"Music", 3},         {0xE714, L"Videoer", L"Videos", 4},          {0xE7F4, L"Denne PC-en", L"This PC", 5},
            {0, nullptr, nullptr, -1},
            {0xE713, L"Innstillinger", L"Settings", 6}, {0xE771, L"Skyline", L"Skyline", 7},
        };
        return p;
    }

    void OpenPlace(int id) {
        Close();
        switch (id) {
        case 0: OpenKnownFolder(FOLDERID_Documents); break;
        case 1: OpenKnownFolder(FOLDERID_Downloads); break;
        case 2: OpenKnownFolder(FOLDERID_Pictures); break;
        case 3: OpenKnownFolder(FOLDERID_Music); break;
        case 4: OpenKnownFolder(FOLDERID_Videos); break;
        case 5: ShellExecuteW(nullptr, L"open", L"shell:MyComputerFolder", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 6: ShellExecuteW(nullptr, nullptr, L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL); break;
        case 7: ShowSettings(SettingsPage::General); break;
        }
    }

    // All-apps rows: letter headers + apps
    struct Row { bool header; wchar_t letter; int app; };
    std::vector<Row> AllRows() const {
        std::vector<Row> rows;
        const auto& apps = AllApps(0xFFFFFFFF);
        wchar_t cur = 0;
        for (int i = 0; i < (int)apps.size(); ++i) {
            wchar_t c = apps[i].name.empty() ? L'#' : HeaderLetter(apps[i].name[0]);
            if (c != cur) { rows.push_back({true, c, -1}); cur = c; }
            rows.push_back({false, c, i});
        }
        return rows;
    }

    static constexpr float kAppRow = 38, kLetterRow = 30;

    // É files under E; Å stays Å where the language has it as its own letter (it sorts after Z)
    static wchar_t HeaderLetter(wchar_t ch) {
        wchar_t c = (wchar_t)towupper(ch);
        if (!iswalpha(c)) return L'#';
        if (c >= L'A' && c <= L'Z') return c;
        wchar_t one[2] = {c, 0}, z[2] = {L'Z', 0};
        if (CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE, one, 1, z, 1, nullptr, nullptr, 0) == CSTR_GREATER_THAN) return c;
        wchar_t folded[8] = {};
        if (FoldStringW(MAP_COMPOSITE, one, 1, folded, 8) > 0 && folded[0] >= L'A' && folded[0] <= L'Z') return folded[0];
        return c;
    }

    void Paint() override {
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        float W = sz.width, H = sz.height;
        if (!fText_) {
            fText_ = Format(L"Segoe UI", 13.5f);
            fSmall_ = Format(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fDim_ = Format(L"Segoe UI", 12);
            fName_ = Format(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fGlyph_ = Format(IconFont(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fGlyphSmall_ = Format(IconFont(), 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fLetter_ = Format(L"Segoe UI", 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fSearch_ = Format(L"Segoe UI", 14);
        }
        hits_.clear();
        rt_->BeginDraw();
        {
            rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
            brush_->SetColor(GlassTint(Part::Start));
            rt_->FillRectangle(D2D1::RectF(0, 0, W, H), brush_.Get());
            PaintVibe(rt_.Get(), D2D1::RectF(0, 0, W, H), Part::Start);
            PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, W, H), 8);
        }

        // Slides up a little while fading in
        ComPtr<ID2D1Layer> layer;
        bool fading = anim_ < 0.999f;   // no layer once fully open: cheaper frames
        if (fading) {
            rt_->CreateLayer(layer.Put());
            rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, 14 * (1 - anim_)));
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), anim_),
                           layer.Get());
        }

        DrawSearch(D2D1::RectF(16, 16, W - 16, 52), TR(L"Søk etter apper og innstillinger", L"Search apps and settings"),
                   fSearch_.Get(), fGlyph_.Get(), false);

        float split = std::floor(W * 0.62f);
        float top = 68, bottom = H - 58;
        int px = (int)std::lround(24 * dpi_ / 96.0f);

        auto pill = [&](const wchar_t* text, bool rightSide, HitKind k, float y) {
            float tw = TextWidth(text, fDim_.Get()) + 22;
            D2D1_RECT_F r = rightSide ? D2D1::RectF(split - 12 - tw, y, split - 12, y + 26) : D2D1::RectF(20, y, 20 + tw, y + 26);
            bool hov = hover_ >= 0 && hover_ < (int)hitsPrev_.size() && hitsPrev_[hover_].kind == k;
            brush_->SetColor(hov ? Alpha(th.fg, 0.14f) : Alpha(th.fg, 0.07f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get());
            DrawText(text, fDim_.Get(), D2D1::RectF(r.left + 11, r.top, r.right, r.bottom), th.fg);
            hits_.push_back({r, k, 0});
        };
        auto appRow = [&](const AppEntry& a, D2D1_RECT_F r, int idx, bool selected, const wchar_t* tag) {
            int me = (int)hits_.size();
            hits_.push_back({r, HApp, idx});
            bool hov = me == hover_;
            if (hov || selected) {
                brush_->SetColor(selected ? Alpha(th.fg, 0.12f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get());
            }
            float ic = 24, cy = (r.top + r.bottom) / 2;
            D2D1_RECT_F ir = D2D1::RectF(r.left + 10, cy - ic / 2, r.left + 10 + ic, cy + ic / 2);
            if (ID2D1Bitmap* b = Icon(a, px)) DrawImageHQ(rt_.Get(), b, ir);
            DrawText(a.name, fText_.Get(), D2D1::RectF(ir.right + 12, r.top, r.right - (tag ? 90 : 8), r.bottom), th.fg);
            if (tag) DrawText(tag, fDim_.Get(), D2D1::RectF(r.right - 88, r.top, r.right - 10, r.bottom), th.fgDim);
        };

        // ---- left column ----
        rt_->PushAxisAlignedClip(D2D1::RectF(0, top, split, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        if (mode_ == Pinned) {
            DrawText(TR(L"Festet", L"Pinned"), fSmall_.Get(), D2D1::RectF(22, top, split, top + 26), th.fgDim);
            pill(TR(L"Alle apper  ›", L"All apps  ›"), true, HAllButton, top);
            float y = top + 34 - scroll_;
            if (pins_.empty())
                DrawText(TR(L"Høyreklikk en app for å feste den her", L"Right-click an app to pin it here"), fDim_.Get(),
                         D2D1::RectF(22, y, split - 12, y + 30), th.fgDim);
            for (int i = 0; i < (int)pins_.size(); ++i, y += kAppRow) {
                if (pinDragging_ && i == dragPin_) continue;   // drawn last, under the mouse
                if (y + kAppRow > top && y < bottom) appRow(pins_[i], D2D1::RectF(12, y, split - 12, y + kAppRow - 2), i, false, nullptr);
            }
            if (pinDragging_ && dragPin_ >= 0 && dragPin_ < (int)pins_.size()) {
                float dy = std::clamp(dragY_ - grabDy_, top + 34 - kAppRow / 2, bottom - kAppRow / 2);
                appRow(pins_[dragPin_], D2D1::RectF(12, dy, split - 12, dy + kAppRow - 2), dragPin_, true, nullptr);
            }
            contentH_ = 34 + pins_.size() * kAppRow;
        } else if (mode_ == All || mode_ == Letters) {
            DrawText(TR(L"Alle apper", L"All apps"), fSmall_.Get(), D2D1::RectF(22, top, split, top + 26), th.fgDim);
            pill(TR(L"‹  Tilbake", L"‹  Back"), true, HBackButton, top);
            if (mode_ == All) {
                auto rows = AllRows();
                const auto& apps = AllApps(0xFFFFFFFF);
                float y = top + 34 - scroll_;
                for (const Row& r : rows) {
                    float rh = r.header ? kLetterRow : kAppRow;
                    if (y + rh > top && y < bottom) {
                        if (r.header) {
                            D2D1_RECT_F lr = D2D1::RectF(12, y, 52, y + rh - 2);
                            int me = (int)hits_.size();
                            hits_.push_back({lr, HLetter, r.letter});
                            if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(lr, 6, 6), brush_.Get()); }
                            wchar_t s[2] = {r.letter, 0};
                            DrawText(s, fSmall_.Get(), D2D1::RectF(24, y, 60, y + rh), th.accent);
                        } else {
                            appRow(apps[r.app], D2D1::RectF(12, y, split - 12, y + rh - 2), r.app, false, nullptr);
                        }
                    }
                    y += rh;
                }
                contentH_ = 34 + (y + scroll_ - top - 34);
                float before = scroll_;
                ClampScroll();
                if (scroll_ != before) InvalidateRect(hwnd_, nullptr, FALSE);   // jumped near the end: redraw at the limit
            } else {
                // letter picker: tap a letter to jump there
                std::map<wchar_t, bool> have;
                for (const Row& r : AllRows()) if (r.header) have[r.letter] = true;
                std::vector<wchar_t> letters{L'#'};
                for (wchar_t c = L'A'; c <= L'Z'; ++c) letters.push_back(c);
                for (auto& [c, on] : have) if (std::find(letters.begin(), letters.end(), c) == letters.end()) letters.push_back(c);
                int cols = 6;
                float cw = (split - 40) / cols, ch = 46;
                for (int i = 0; i < (int)letters.size(); ++i) {
                    float x = 20 + (i % cols) * cw, y = top + 44 + (i / cols) * ch;
                    D2D1_RECT_F r = D2D1::RectF(x + 2, y + 2, x + cw - 2, y + ch - 2);
                    bool on = have.count(letters[i]) != 0;
                    int me = (int)hits_.size();
                    if (on) hits_.push_back({r, HLetterPick, letters[i]});
                    if (on && me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), brush_.Get()); }
                    wchar_t s[2] = {letters[i], 0};
                    DrawText(s, fLetter_.Get(), r, on ? th.fg : Alpha(th.fg, 0.22f));
                }
                contentH_ = 0;
            }
        } else if (mode_ == Search) {
            DrawText(TR(L"Treff", L"Results"), fSmall_.Get(), D2D1::RectF(22, top, split, top + 26), th.fgDim);
            float y = top + 34;
            for (int i = 0; i < (int)results_.size(); ++i, y += kAppRow) {
                const AppEntry& a = results_[i];
                appRow(a, D2D1::RectF(12, y, split - 12, y + kAppRow - 2), i, i == sel_,
                       a.setting ? TR(L"Innstilling", L"Setting") : a.folder ? TR(L"Mappe", L"Folder") : a.file ? TR(L"Fil", L"File") : TR(L"App", L"App"));
            }
            // last row: hand the search to Windows for files
            D2D1_RECT_F r = D2D1::RectF(12, y, split - 12, y + kAppRow - 2);
            int me = (int)hits_.size();
            hits_.push_back({r, HFileSearch, 0});
            bool selected = sel_ == (int)results_.size();
            if (me == hover_ || selected) {
                brush_->SetColor(selected ? Alpha(th.fg, 0.12f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get());
            }
            DrawGlyph(0xE8B7, fGlyph_.Get(), D2D1::RectF(r.left + 10, r.top, r.left + 34, r.bottom), th.fgDim);
            std::wstring t = std::wstring(TR(L"Søk i filer etter «", L"Search files for “")) + query_ + TR(L"»", L"”");
            DrawText(t, fText_.Get(), D2D1::RectF(r.left + 46, r.top, r.right - 8, r.bottom), th.fg);
            contentH_ = 0;
        }
        rt_->PopAxisAlignedClip();

        // ---- right column ----
        float rx = split + 8, rw = W - 14;
        brush_->SetColor(th.hairline);
        rt_->FillRectangle(D2D1::RectF(split, top + 4, split + 1, bottom - 4), brush_.Get());
        {
            D2D1_RECT_F r = D2D1::RectF(rx, top, rw, top + 56);
            int me = (int)hits_.size();
            hits_.push_back({r, HUser, 0});
            if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), brush_.Get()); }
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(rx + 30, top + 28), 20, 20);
            if (!avatar_ && avatarSrc_) rt_->CreateBitmapFromWicBitmap(avatarSrc_.Get(), nullptr, avatar_.Put());
            if (avatar_) {
                ComPtr<ID2D1BitmapBrush> bb;
                rt_->CreateBitmapBrush(avatar_.Get(), bb.Put());
                if (bb) {
                    D2D1_SIZE_F bs = avatar_->GetSize();
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(40 / bs.width, 40 / bs.height) *
                                     D2D1::Matrix3x2F::Translation(e.point.x - 20, e.point.y - 20));
                    rt_->FillEllipse(e, bb.Get());
                }
            } else {
                brush_->SetColor(Alpha(th.fg, 0.12f));
                rt_->FillEllipse(e, brush_.Get());
                DrawGlyph(0xE77B, fGlyph_.Get(), D2D1::RectF(rx + 10, top + 8, rx + 50, top + 48), th.fg);
            }
            DrawText(name_, fName_.Get(), D2D1::RectF(rx + 60, top, rw - 8, top + 56), th.fg);
        }
        float y = top + 68;
        for (const Place& p : Places()) {
            if (p.id < 0) {
                brush_->SetColor(th.hairline);
                rt_->FillRectangle(D2D1::RectF(rx + 12, y + 6, rw - 12, y + 7), brush_.Get());
                y += 14;
                continue;
            }
            D2D1_RECT_F r = D2D1::RectF(rx, y, rw, y + 36);
            int me = (int)hits_.size();
            hits_.push_back({r, HPlace, p.id});
            if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get()); }
            DrawGlyph(p.glyph, fGlyph_.Get(), D2D1::RectF(r.left + 8, r.top, r.left + 36, r.bottom), th.fg);
            DrawText(TR(p.no, p.en), fText_.Get(), D2D1::RectF(r.left + 46, r.top, r.right - 6, r.bottom), th.fg);
            y += 38;
        }

        // ---- bottom bar ----
        brush_->SetColor(th.hairline);
        rt_->FillRectangle(D2D1::RectF(0, bottom, W, bottom + 1), brush_.Get());
        {
            const wchar_t* label = TR(L"Av/på", L"Power");
            float tw = TextWidth(label, fText_.Get());
            D2D1_RECT_F r = D2D1::RectF(W - 24 - tw - 40, bottom + 11, W - 14, H - 11);
            int me = (int)hits_.size();
            hits_.push_back({r, HPower, 0});
            if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get()); }
            DrawGlyph(0xE7E8, fGlyph_.Get(), D2D1::RectF(r.left + 6, r.top, r.left + 32, r.bottom), th.fg);
            DrawText(label, fText_.Get(), D2D1::RectF(r.left + 36, r.top, r.right, r.bottom), th.fg);
        }
        DrawText(L"Skyline", fDim_.Get(), D2D1::RectF(22, bottom, 200, H), Alpha(th.fgDim, 0.8f));

        if (fading) rt_->PopLayer();
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); avatar_.Reset(); }
        hitsPrev_ = hits_;
        listTop_ = top;
        listBottom_ = bottom;
    }

    int HitAt(float x, float y) const {
        float yy = y - 14 * (1 - anim_);
        for (int i = (int)hitsPrev_.size() - 1; i >= 0; --i)
            if (In(hitsPrev_[i].r, x, yy)) {
                // list rows are clipped to the list area
                if (hitsPrev_[i].kind == HApp || hitsPrev_[i].kind == HLetter || hitsPrev_[i].kind == HFileSearch)
                    if (yy < listTop_ || yy > listBottom_) continue;
                return i;
            }
        return -1;
    }

    const AppEntry* AppOf(const Hit& h) const {
        if (h.kind != HApp) return nullptr;
        if (mode_ == Pinned) return h.idx < (int)pins_.size() ? &pins_[h.idx] : nullptr;
        if (mode_ == Search) return h.idx < (int)results_.size() ? &results_[h.idx] : nullptr;
        const auto& apps = AllApps(0xFFFFFFFF);
        return h.idx < (int)apps.size() ? &apps[h.idx] : nullptr;
    }

    void Activate(const Hit& h) {
        switch (h.kind) {
        case HApp: if (const AppEntry* a = AppOf(h)) { AppEntry copy = *a; Close(); LaunchApp(copy); } break;
        case HAllButton: mode_ = All; scroll_ = 0; hover_ = -1; break;
        case HBackButton: mode_ = Pinned; scroll_ = 0; hover_ = -1; break;
        case HLetter: mode_ = Letters; hover_ = -1; break;
        case HLetterPick: {
            mode_ = All;
            float y = 0;
            for (const Row& r : AllRows()) {
                if (r.header && r.letter == (wchar_t)h.idx) break;
                y += r.header ? kLetterRow : kAppRow;
            }
            scroll_ = y;           // clamped in Paint, once the list height is known
            hover_ = -1;
            break;
        }
        case HPlace: OpenPlace(h.idx); break;
        case HUser: Close(); ShellExecuteW(nullptr, nullptr, L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL); break;
        case HPower: {
            POINT p{(LONG)(h.r.right * dpi_ / 96.0f), (LONG)(h.r.top * dpi_ / 96.0f)};
            ClientToScreen(hwnd_, &p);
            inMenu_ = true;
            PowerMenu(hwnd_, p);
            inMenu_ = false;
            if (hwnd_ && GetForegroundWindow() != hwnd_) Close();
            break;
        }
        case HFileSearch: { std::wstring q = query_; Close(); OpenFileSearch(q); break; }
        default: break;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ClampScroll() {
        float view = listBottom_ - listTop_;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - view + 8));
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
        switch (msg) {
        case WM_TIMER:
            if (wp == T_REFRESH) {
                KillTimer(hwnd_, T_REFRESH);
                if (mode_ == Search) OnQuery();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (wp == T_FILES) {
                KillTimer(hwnd_, T_FILES);
                if (mode_ == Search) FileSearch::Query(hwnd_, fileTicket_, query_, 4);
            }
            return 0;
        case FileSearch::WM_SKY_FILES: {
            std::unique_ptr<std::vector<AppEntry>> found(reinterpret_cast<std::vector<AppEntry>*>(lp));
            if ((unsigned)wp != fileTicket_ || mode_ != Search) return 0;   // typed on since
            results_.erase(std::remove_if(results_.begin(), results_.end(), [](const AppEntry& a) { return a.file; }), results_.end());
            results_.insert(results_.end(), found->begin(), found->end());
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (mode_ == All || mode_ == Pinned) {
                scroll_ -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * kAppRow * 2.5f;
                ClampScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            if (downPin_ >= 0 && (wp & MK_LBUTTON) && mode_ == Pinned) {
                if (!pinDragging_ && std::fabs(y - downPt_.y) > 5) {
                    pinDragging_ = true;
                    dragPin_ = downPin_;
                    grabDy_ = downPt_.y - PinRowTop(dragPin_);
                    SetCapture(hwnd_);
                }
                if (pinDragging_) { PinDragMove(y); return 0; }
            }
            int h = HitAt(x, y);
            if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            downPin_ = -1;
            int h = HitAt(x, y);
            if (mode_ == Pinned && h >= 0 && hitsPrev_[h].kind == HApp) { downPin_ = hitsPrev_[h].idx; downPt_ = {x, y}; }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pinDragging_ && (HWND)lp != hwnd_) PinDragEnd();   // capture taken away: keep what we have
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (pinDragging_) { PinDragEnd(); return 0; }
            downPin_ = -1;
            int h = HitAt(x, y);
            if (h >= 0) Activate(hitsPrev_[h]);
            return 0;
        }
        case WM_RBUTTONUP: {
            int h = HitAt(x, y);
            if (h < 0) return 0;
            const AppEntry* a = AppOf(hitsPrev_[h]);
            if (!a) return 0;
            AppEntry copy = *a;
            POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &s);
            inMenu_ = true;
            bool launched = AppContextMenu(hwnd_, copy, s, IsMenuPinned(copy), [this, copy](bool pin) {
                SetMenuPinned(copy, pin);
                LoadPins();
            });
            inMenu_ = false;
            if (!hwnd_) return 0;   // closed while the menu was open
            if (launched || GetForegroundWindow() != hwnd_) Close();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
            case VK_ESCAPE:
                if (!query_.empty()) { query_.clear(); OnQuery(); }
                else if (mode_ != Pinned) { mode_ = mode_ == Letters ? All : Pinned; scroll_ = mode_ == Pinned ? 0 : scroll_; }
                else Close();
                break;
            case VK_RETURN:
                if (mode_ == Search) {
                    if (sel_ < (int)results_.size()) { AppEntry a = results_[sel_]; Close(); LaunchApp(a); }
                    else { std::wstring q = query_; Close(); OpenFileSearch(q); }
                }
                break;
            case VK_DOWN: if (mode_ == Search) sel_ = std::min(sel_ + 1, (int)results_.size()); break;
            case VK_UP: if (mode_ == Search) sel_ = std::max(sel_ - 1, 0); break;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

private:
    Mode mode_ = Pinned;
    std::vector<AppEntry> pins_, results_;
    std::vector<std::wstring> pinKeys_;   // what's saved for each pin, same order as pins_
    unsigned fileTicket_ = 0;             // the newest file search; older answers are dropped
    int downPin_ = -1, dragPin_ = -1;
    bool pinDragging_ = false;
    D2D1_POINT_2F downPt_{};
    float dragY_ = 0, grabDy_ = 0;
    std::vector<Hit> hits_, hitsPrev_;
    int hover_ = -1, sel_ = 0;
    bool tracking_ = false;
    float scroll_ = 0, contentH_ = 0, listTop_ = 68, listBottom_ = 500;
    std::wstring name_;
    ComPtr<IWICBitmapSource> avatarSrc_;
    int avatarPx_ = 0;
    wchar_t pendingLetter_ = 0;   // jump here once the All list is laid out
    ComPtr<ID2D1Bitmap> avatar_;
    ComPtr<IDWriteTextFormat> fText_, fSmall_, fDim_, fName_, fGlyph_, fGlyphSmall_, fLetter_, fSearch_;
};

// ===========================================================================
// Spotlight — apps, settings and files, from a hotkey (plugin)
// ===========================================================================
// A glass search field in the middle of the screen. Apps and settings are found at
// once (the list Skyline already has); files come from Windows' own search index a
// moment after you stop typing (filesearch.cpp). Always on top, keyboard first:
// type, arrows, Enter. Closes when you click elsewhere or press Esc.
class Spotlight : public Surface {
public:
    void Show() {
        mon_ = CursorMonitor();
        dpi_ = MonitorDpi(mon_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon_, &mi);
        int w = MulDiv((int)kW, dpi_, 96), h = MulDiv((int)kSearchH, dpi_, 96);
        int left = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        int top = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 5;
        maxH_ = (mi.rcWork.bottom - top) * 96.0f / dpi_ - 24;
        if (!CreateAt(RECT{left, top, left + w, top + h}, WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) return;
        Glass(hwnd_, 0, true, Part::Spotlight);
        apps_.clear();
        files_.clear();
        sel_ = 0;
        hover_ = -1;
        if (AllApps(0xFFFFFFFF).empty()) AllApps();
        ShowAnimated();
        KeepOnTop();
    }

protected:
    static constexpr float kW = 680, kSearchH = 62, kHeader = 26, kRow = 44, kPad = 8;
    enum Kind { KHeader, KApp, KFile, KWindows };
    struct Row { Kind kind; int idx; const wchar_t* title; };

    const wchar_t* ClassName() const override { return L"SkylineSpotlight"; }
    DWORD OpenMs() const override { return 140; }
    DWORD CloseMs() const override { return 90; }
    bool ShowsBars() const override { return false; }   // a search box, not a Start menu: the game stays a game

    // Always above everything: re-assert it whenever we're shown or resized
    void KeepOnTop() {
        if (hwnd_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void OnQuery() override {
        std::wstring q = query_;
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        apps_ = q.empty() ? std::vector<AppEntry>{} : SearchApps(q, true, 6);
        if (q.size() < 2) files_.clear();   // keep the old file hits until new ones arrive (no flicker)
        sel_ = 0;
        hover_ = -1;
        ++ticket_;
        KillTimer(hwnd_, T_FILES);
        if (q.size() >= 2) SetTimer(hwnd_, T_FILES, 150, nullptr);
        Resize();
    }

    std::vector<Row> Rows() const {
        std::vector<Row> rows;
        if (query_.empty()) return rows;
        bool head = false;
        for (int i = 0; i < (int)apps_.size(); ++i)
            if (!apps_[i].setting) {
                if (!head) { rows.push_back({KHeader, 0, TR(L"Apper", L"Apps")}); head = true; }
                rows.push_back({KApp, i, nullptr});
            }
        head = false;
        for (int i = 0; i < (int)apps_.size(); ++i)
            if (apps_[i].setting) {
                if (!head) { rows.push_back({KHeader, 0, TR(L"Innstillinger", L"Settings")}); head = true; }
                rows.push_back({KApp, i, nullptr});
            }
        if (!files_.empty()) {
            rows.push_back({KHeader, 0, TR(L"Filer og mapper", L"Files and folders")});
            for (int i = 0; i < (int)files_.size(); ++i) rows.push_back({KFile, i, nullptr});
        }
        rows.push_back({KHeader, 0, nullptr});   // thin line
        rows.push_back({KWindows, 0, nullptr});
        return rows;
    }

    static float RowH(const Row& r) { return r.kind == KHeader ? (r.title ? kHeader : 9.0f) : kRow; }

    float ContentH() const {
        float h = 0;
        for (const Row& r : Rows()) h += RowH(r);
        return h ? h + kPad : 0;
    }

    void Resize() {
        if (!hwnd_) return;
        float h = std::min(kSearchH + ContentH(), maxH_);
        RECT r;
        GetWindowRect(hwnd_, &r);
        int want = MulDiv((int)std::lround(h), dpi_, 96);
        if (r.bottom - r.top != want)
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, r.right - r.left, want, SWP_NOMOVE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // The selectable rows in order (headers left out)
    std::vector<int> Selectable(const std::vector<Row>& rows) const {
        std::vector<int> s;
        for (int i = 0; i < (int)rows.size(); ++i) if (rows[i].kind != KHeader) s.push_back(i);
        return s;
    }

    int RowAt(float y) const {
        auto rows = Rows();
        float top = kSearchH;
        for (int i = 0; i < (int)rows.size(); ++i) {
            float h = RowH(rows[i]);
            if (y >= top && y < top + h) {
                if (rows[i].kind == KHeader) return -1;
                auto sel = Selectable(rows);
                return (int)(std::find(sel.begin(), sel.end(), i) - sel.begin());
            }
            top += h;
        }
        return -1;
    }

    void Activate(int which) {
        auto rows = Rows();
        auto sel = Selectable(rows);
        if (which < 0 || which >= (int)sel.size()) return;
        const Row& r = rows[sel[which]];
        if (r.kind == KWindows) { std::wstring q = query_; Close(); OpenFileSearch(q); return; }
        AppEntry a = r.kind == KApp ? apps_[r.idx] : files_[r.idx];
        Close();
        LaunchApp(a);
    }

    const AppEntry* EntryAt(int which) const {
        auto rows = Rows();
        auto sel = Selectable(rows);
        if (which < 0 || which >= (int)sel.size()) return nullptr;
        const Row& r = rows[sel[which]];
        if (r.kind == KApp) return &apps_[r.idx];
        if (r.kind == KFile) return &files_[r.idx];
        return nullptr;
    }

    // "C:\Users\me\Documents\Notes" -> "Documents › Notes": where a file lives, short
    std::wstring WhereIs(const std::wstring& path) const {
        std::wstring dir = path.substr(0, path.find_last_of(L'\\') == std::wstring::npos ? 0 : path.find_last_of(L'\\'));
        PWSTR prof = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &prof))) {
            std::wstring p = prof;
            CoTaskMemFree(prof);
            if (ToLower(dir).compare(0, p.size(), ToLower(p)) == 0) dir = dir.substr(std::min(dir.size(), p.size() + 1));
        }
        for (size_t i = 0; (i = dir.find(L'\\', i)) != std::wstring::npos; i += 3) dir.replace(i, 1, L" › ");
        return dir.empty() ? std::wstring(TR(L"Brukermappen", L"Your user folder")) : dir;
    }

    void Paint() override {
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        float W = sz.width, H = sz.height;
        if (!fSearch_) {
            fSearch_ = Format(L"Segoe UI", 21);
            fGlyph_ = Format(IconFont(), 19, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fName_ = Format(L"Segoe UI", 14);
            fSub_ = Format(L"Segoe UI", 11.5f);
            fHead_ = Format(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fTag_ = Format(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            fSmallGlyph_ = Format(IconFont(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        D2D1_COLOR_F tint = GlassTint(Part::Spotlight);
        tint.a *= anim_;
        brush_->SetColor(tint);
        rt_->FillRectangle(D2D1::RectF(0, 0, W, H), brush_.Get());
        PaintVibe(rt_.Get(), D2D1::RectF(0, 0, W, H), Part::Spotlight);
        PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, W, H), 8);

        ComPtr<ID2D1Layer> layer;
        bool fading = anim_ < 0.999f;
        if (fading) {
            rt_->CreateLayer(layer.Put());
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), anim_),
                           layer.Get());
        }

        // the search field
        DrawGlyph(0xE721, fGlyph_.Get(), D2D1::RectF(14, 0, 50, kSearchH), th.fgDim);
        const wchar_t* ph = TR(L"Søk etter apper, innstillinger og filer", L"Search apps, settings and files");
        DrawText(query_.empty() ? std::wstring(ph) : query_, fSearch_.Get(), D2D1::RectF(58, 0, W - 20, kSearchH), query_.empty() ? th.fgDim : th.fg);
        if (caretOn_ && !closing_) {
            float cx = 58 + (query_.empty() ? 0 : TextWidth(query_, fSearch_.Get()) + 1);
            brush_->SetColor(th.fg);
            rt_->FillRectangle(D2D1::RectF(cx, kSearchH / 2 - 12, cx + 1.5f, kSearchH / 2 + 12), brush_.Get());
        }

        auto rows = Rows();
        if (!rows.empty()) {
            brush_->SetColor(th.hairline);
            rt_->FillRectangle(D2D1::RectF(0, kSearchH - 0.5f, W, kSearchH + 0.5f), brush_.Get());
        }
        int px = (int)std::lround(28 * dpi_ / 96.0f);
        float y = kSearchH;
        int k = 0;
        rt_->PushAxisAlignedClip(D2D1::RectF(0, kSearchH, W, H), D2D1_ANTIALIAS_MODE_ALIASED);
        for (const Row& r : rows) {
            float h = RowH(r);
            if (r.kind == KHeader) {
                if (r.title) DrawText(r.title, fHead_.Get(), D2D1::RectF(20, y + 4, W - 20, y + h), th.fgDim);
                else { brush_->SetColor(th.hairline); rt_->FillRectangle(D2D1::RectF(14, y + 4, W - 14, y + 5), brush_.Get()); }
                y += h;
                continue;
            }
            D2D1_RECT_F rr = D2D1::RectF(8, y + 2, W - 8, y + h - 2);
            if (k == sel_ || k == hover_) {
                brush_->SetColor(k == sel_ ? Alpha(th.accent, 0.24f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(rr, 8, 8), brush_.Get());
            }
            D2D1_RECT_F ir = D2D1::RectF(20, y + (h - 28) / 2, 48, y + (h + 28) / 2);
            if (r.kind == KWindows) {
                DrawGlyph(0xE8B7, fSmallGlyph_.Get(), ir, th.fgDim);
                std::wstring t = std::wstring(TR(L"Søk i Windows etter «", L"Search Windows for “")) + query_ + TR(L"»", L"”");
                DrawText(t, fName_.Get(), D2D1::RectF(62, y, W - 20, y + h), th.fg);
            } else {
                const AppEntry& a = r.kind == KApp ? apps_[r.idx] : files_[r.idx];
                if (ID2D1Bitmap* b = Icon(a, px)) DrawImageHQ(rt_.Get(), b, ir);
                if (a.file) {
                    DrawText(a.name, fName_.Get(), D2D1::RectF(62, y + 3, W - 110, y + 25), th.fg);
                    DrawText(WhereIs(a.target), fSub_.Get(), D2D1::RectF(62, y + 22, W - 110, y + h - 3), th.fgDim);
                } else {
                    DrawText(a.name, fName_.Get(), D2D1::RectF(62, y, W - 110, y + h), th.fg);
                }
                const wchar_t* tag = a.setting ? TR(L"Innstilling", L"Setting") : a.folder ? TR(L"Mappe", L"Folder") : a.file ? TR(L"Fil", L"File") : L"App";
                DrawText(tag, fTag_.Get(), D2D1::RectF(W - 110, y, W - 22, y + h), th.fgDim);
            }
            y += h;
            ++k;
        }
        rt_->PopAxisAlignedClip();

        if (fading) rt_->PopLayer();
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); }
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
        (void)x;
        switch (msg) {
        case WM_TIMER:
            if (wp == T_FILES) {
                KillTimer(hwnd_, T_FILES);
                if (!query_.empty()) FileSearch::Query(hwnd_, ticket_, query_, 6);
            }
            return 0;
        case FileSearch::WM_SKY_FILES: {
            std::unique_ptr<std::vector<AppEntry>> found(reinterpret_cast<std::vector<AppEntry>*>(lp));
            if ((unsigned)wp != ticket_ || query_.size() < 2) return 0;   // typed on since
            files_ = std::move(*found);
            int n = (int)Selectable(Rows()).size();
            if (sel_ >= n) sel_ = std::max(0, n - 1);
            Resize();
            return 0;
        }
        case WM_MOUSEMOVE: {
            int h = RowAt(y);
            if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: {
            int h = RowAt(y);
            if (h >= 0) Activate(h);
            return 0;
        }
        case WM_RBUTTONUP: {
            const AppEntry* e = EntryAt(RowAt(y));
            if (!e) return 0;
            AppEntry a = *e;
            POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &s);
            std::function<void(bool)> pin;
            if (!a.file) pin = [a](bool p) { SetMenuPinned(a, p); };
            inMenu_ = true;
            bool launched = AppContextMenu(hwnd_, a, s, !a.file && IsMenuPinned(a), pin);
            inMenu_ = false;
            if (!hwnd_) return 0;
            if (launched || GetForegroundWindow() != hwnd_) Close();
            return 0;
        }
        case WM_KEYDOWN: {
            int n = (int)Selectable(Rows()).size();
            switch (wp) {
            case VK_ESCAPE:
                if (!query_.empty()) { query_.clear(); OnQuery(); }
                else Close();
                break;
            case VK_RETURN: Activate(sel_); break;
            case VK_DOWN: if (n) sel_ = (sel_ + 1) % n; break;
            case VK_UP: if (n) sel_ = (sel_ + n - 1) % n; break;
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

private:
    std::vector<AppEntry> apps_, files_;
    int sel_ = 0, hover_ = -1;
    unsigned ticket_ = 0;
    float maxH_ = 600;
    ComPtr<IDWriteTextFormat> fSearch_, fGlyph_, fName_, fSub_, fHead_, fTag_, fSmallGlyph_;
};

// ===========================================================================
// VibeMenu — Claude's own Start menu
// ===========================================================================
// A compact glass card above the dock that learns instead of asking you to keep lists:
//   · a greeting and one search field for everything (apps, settings, files, and "= 12*7" maths)
//   · "Now": the apps you tend to use at this time of day (counted locally, see VibeUsage)
//   · "Pick up where you left off": your latest files, from Windows' own Recent list
//   · keyboard first: 1–9 start a suggestion, arrows + Enter for the rest, Esc closes
//   · lock / restart / shut down at the bottom (restart and shut down ask with a second click)
//
// Usage is counted only while VibeMenu is the chosen Start menu: when an app comes to the
// front, its exe gets +1 in one of six four-hour slots of the day. Kept in vibe.ini next to
// the exe, written when the menu opens and when Skyline quits. No timers, no polling.

// A place that can be slow to ask (a network share, a removable or optical drive that may not be there)
bool SlowPlace(const wchar_t* path) {
    if (PathIsNetworkPathW(path)) return true;
    if (!path[0] || path[1] != L':') return false;
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    UINT t = GetDriveTypeW(root);
    return t == DRIVE_REMOTE || t == DRIVE_REMOVABLE || t == DRIVE_CDROM;   // (a drive letter that's gone answers quickly)
}

namespace VibeUsage {

struct Counts { int slot[6] = {}; };
std::map<std::wstring, Counts> g_counts;   // lowercase exe path -> uses per 4-hour slot
bool g_loaded = false, g_dirty = false;
std::wstring g_last;                        // the app counted last (switching windows inside it doesn't count)
std::set<std::wstring> g_hiddenApps;        // removed from "Now" (lowercase exe path or app key); back once used again
std::map<std::wstring, ULONGLONG> g_hiddenFiles;   // removed from "Pick up …": path -> when it was last opened then

std::wstring File() { return ExeDir() + L"\\vibe.ini"; }

int SlotNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wHour / 4;
}

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::vector<wchar_t> buf(1 << 16);
    DWORD n = GetPrivateProfileSectionW(L"Usage", buf.data(), (DWORD)buf.size(), File().c_str());
    for (const wchar_t* p = buf.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.rfind(L'=');
        if (eq == std::wstring::npos) continue;
        Counts c;
        auto parts = SplitList(line.substr(eq + 1), L',');
        for (int i = 0; i < 6 && i < (int)parts.size(); ++i) c.slot[i] = _wtoi(parts[i].c_str());
        g_counts[ToLower(line.substr(0, eq))] = c;
    }
    n = GetPrivateProfileSectionW(L"HiddenApps", buf.data(), (DWORD)buf.size(), File().c_str());
    for (const wchar_t* p = buf.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.rfind(L'=');
        if (eq != std::wstring::npos) g_hiddenApps.insert(ToLower(line.substr(0, eq)));
    }
    n = GetPrivateProfileSectionW(L"HiddenFiles", buf.data(), (DWORD)buf.size(), File().c_str());
    for (const wchar_t* p = buf.data(); n && *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.rfind(L'=');
        if (eq != std::wstring::npos) g_hiddenFiles[ToLower(line.substr(0, eq))] = _wcstoui64(line.c_str() + eq + 1, nullptr, 10);
    }
}

void Save() {
    if (!g_dirty) return;
    g_dirty = false;
    // One write for the whole section: the file stays small (only apps actually used)
    std::wstring sec;
    for (const auto& [exe, c] : g_counts) {
        sec += exe + L"=";
        for (int i = 0; i < 6; ++i) sec += std::to_wstring(c.slot[i]) + (i < 5 ? L"," : L"");
        sec.push_back(L'\0');
    }
    sec.push_back(L'\0');
    WritePrivateProfileSectionW(L"Usage", sec.c_str(), File().c_str());
    std::wstring ha;
    for (const auto& k : g_hiddenApps) { ha += k + L"=1"; ha.push_back(L'\0'); }
    ha.push_back(L'\0');
    WritePrivateProfileSectionW(L"HiddenApps", ha.c_str(), File().c_str());
    std::wstring hf;
    for (const auto& [k, t] : g_hiddenFiles) { hf += k + L"=" + std::to_wstring(t); hf.push_back(L'\0'); }
    hf.push_back(L'\0');
    WritePrivateProfileSectionW(L"HiddenFiles", hf.c_str(), File().c_str());
}

// Remove an app from "Now": its count starts over; using it again lets it earn its place back
void HideApp(const AppEntry& a) {
    Load();
    for (std::wstring k : {ToLower(a.exe), ToLower(a.key), ToLower(a.target)}) {
        if (k.empty()) continue;
        g_hiddenApps.insert(k);
        g_counts.erase(k);
    }
    g_last.clear();
    g_dirty = true;
    Save();
}

bool AppHidden(const AppEntry& a) {
    Load();
    for (std::wstring k : {ToLower(a.exe), ToLower(a.key), ToLower(a.target)})
        if (!k.empty() && g_hiddenApps.count(k)) return true;
    return false;
}

// Remove a file from "Pick up …": it comes back if you open it again later
void HideFile(const std::wstring& path, ULONGLONG when) {
    Load();
    g_hiddenFiles[ToLower(path)] = when;
    g_dirty = true;
    Save();
}

bool FileHidden(const std::wstring& path, ULONGLONG when) {
    Load();
    auto it = g_hiddenFiles.find(ToLower(path));
    return it != g_hiddenFiles.end() && when <= it->second;
}

// Drop what's in memory (not the file): read again next time. Used before a restore or factory reset.
void Unload() {
    g_counts.clear();
    g_hiddenApps.clear();
    g_hiddenFiles.clear();
    g_last.clear();
    g_loaded = false;
    g_dirty = false;
}

void Forget() {
    g_counts.clear();
    g_hiddenApps.clear();
    g_hiddenFiles.clear();
    WritePrivateProfileSectionW(L"HiddenApps", nullptr, File().c_str());
    WritePrivateProfileSectionW(L"HiddenFiles", nullptr, File().c_str());
    g_last.clear();
    g_loaded = true;
    g_dirty = false;
    WritePrivateProfileSectionW(L"Usage", nullptr, File().c_str());
}

void Note(HWND h, const std::wstring& path) {
    if (!h || path.empty()) return;
    std::wstring p = ToLower(path);
    if (p == g_last) return;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    std::wstring exe = FileNameOf(p);
    // Only real apps: not the desktop, the shell's own surfaces, or Store app internals
    static const wchar_t* skip[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"shellexperiencehost.exe", L"shellhost.exe",
                                    L"textinputhost.exe", L"applicationframehost.exe", L"lockapp.exe", L"searchapp.exe"};
    for (auto s : skip) if (exe == s) return;
    if (exe == L"explorer.exe" && lstrcmpW(cls, L"CabinetWClass")) return;   // File Explorer windows only
    if (p.find(L"\\windowsapps\\") != std::wstring::npos || p.find(L"\\systemapps\\") != std::wstring::npos) return;
    g_last = p;
    Load();
    if (g_hiddenApps.erase(p)) g_dirty = true;   // used again: it may earn its place back
    Counts& c = g_counts[p];
    if (++c.slot[SlotNow()] > 5000)   // keep numbers small: halve everything now and then
        for (auto& [k, v] : g_counts) for (int& x : v.slot) x /= 2;
    g_dirty = true;
}

// The apps for right now: this slot counts most, its neighbours a bit, the rest of the day a little
std::vector<std::wstring> Top(size_t n) {
    Load();
    int s = SlotNow();
    std::vector<std::pair<double, std::wstring>> scored;
    for (const auto& [exe, c] : g_counts) {
        int total = 0;
        for (int x : c.slot) total += x;
        double score = c.slot[s] * 3.0 + c.slot[(s + 1) % 6] + c.slot[(s + 5) % 6] + total * 0.15;
        if (score >= 1 && !g_hiddenApps.count(exe) && (SlowPlace(exe.c_str()) || GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES))
            scored.push_back({score, exe});
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < scored.size() && out.size() < n; ++i) out.push_back(scored[i].second);
    return out;
}

} // namespace VibeUsage

// "= 12*(3+4)/2" -> 42. + - * / ^ % and brackets; a comma works as a decimal point too.
struct Calc {
    const wchar_t* p;
    bool ok = true;
    void Skip() { while (*p == L' ') ++p; }
    double Number() {
        Skip();
        std::wstring s;
        while (iswdigit(*p) || *p == L'.' || *p == L',') s += (*p == L',' ? L'.' : *p), ++p;
        if (s.empty()) { ok = false; return 0; }
        return _wtof(s.c_str());
    }
    double Atom() {
        Skip();
        if (*p == L'(') { ++p; double v = Sum(); Skip(); if (*p == L')') ++p; else ok = false; return v; }
        if (*p == L'-') { ++p; return -Atom(); }
        if (*p == L'+') { ++p; return Atom(); }
        return Number();
    }
    double Power() {
        double v = Atom();
        Skip();
        if (*p == L'^') { ++p; v = std::pow(v, Power()); }
        return v;
    }
    double Product() {
        double v = Power();
        for (;;) {
            Skip();
            if (*p == L'*' || *p == L'x' || *p == L'×') { ++p; v *= Power(); }
            else if (*p == L'/' || *p == L'÷') { ++p; double d = Power(); if (d == 0) ok = false; else v /= d; }
            else if (*p == L'%') { ++p; v /= 100; }
            else return v;
        }
    }
    double Sum() {
        double v = Product();
        for (;;) {
            Skip();
            if (*p == L'+') { ++p; v += Product(); }
            else if (*p == L'-') { ++p; v -= Product(); }
            else return v;
        }
    }
    static bool Eval(const std::wstring& expr, double& out) {
        Calc c{expr.c_str()};
        out = c.Sum();
        c.Skip();
        return c.ok && *c.p == 0 && std::isfinite(out);
    }
    static std::wstring Format(double v) {
        wchar_t b[64];
        swprintf(b, 64, L"%.10g", v);
        std::wstring s = b;
        if (g_norsk) for (auto& ch : s) if (ch == L'.') ch = L',';
        return s;
    }
};

// Windows can be told not to remember the files you open (Settings > Start, or a tweak tool / policy):
// then the Recent folder stays empty and VibeMenu says how to turn it back on
bool RecentTrackingOff() {
    auto dw = [](HKEY root, const wchar_t* path, const wchar_t* name, DWORD& v) {
        DWORD size = sizeof(v);
        return RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS;
    };
    DWORD v = 0;
    const wchar_t* pol = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
    if ((dw(HKEY_CURRENT_USER, pol, L"NoRecentDocsHistory", v) && v) || (dw(HKEY_LOCAL_MACHINE, pol, L"NoRecentDocsHistory", v) && v)) return true;
    if (dw(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_TrackDocs", v) && !v) return true;
    return false;
}

// Your latest files, from Windows' Recent folder (read only when the menu opens), with when you used them
struct RecentFile { AppEntry a; ULONGLONG when = 0; };
std::vector<RecentFile> RecentFiles(size_t n) {
    std::vector<RecentFile> out;
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &dir))) return out;
    std::wstring recent = dir;
    CoTaskMemFree(dir);
    std::vector<std::pair<ULONGLONG, std::wstring>> links;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((recent + L"\\*.lnk").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do links.push_back({((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, fd.cFileName});
    while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(links.begin(), links.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (const auto& [t, name] : links) {
        if (out.size() >= n) break;
        ComPtr<IShellLinkW> sl;
        ComPtr<IPersistFile> pf;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(sl.Put()))) ||
            FAILED(sl->QueryInterface(IID_PPV_ARGS(pf.Put()))) || FAILED(pf->Load((recent + L"\\" + name).c_str(), STGM_READ)))
            continue;
        wchar_t raw[MAX_PATH] = {}, target[MAX_PATH] = {};
        if (FAILED(sl->GetPath(raw, MAX_PATH, nullptr, 0)) || !*raw) {
            // some links only carry the shell item: ask it for its path instead
            PIDLIST_ABSOLUTE id = nullptr;
            if (FAILED(sl->GetIDList(&id)) || !id) continue;
            BOOL ok = SHGetPathFromIDListEx(id, raw, MAX_PATH, GPFIDL_DEFAULT);
            CoTaskMemFree(id);
            if (!ok || !*raw) continue;
        }
        // Windows often keeps these as "%USERPROFILE%\…": expand before looking for the file
        if (!ExpandEnvironmentStringsW(raw, target, MAX_PATH)) lstrcpynW(target, raw, MAX_PATH);
        // files only, and only ones still there – but never ask a network share or a USB stick that may be gone (that can take seconds)
        if (!SlowPlace(target)) {
            DWORD attr = GetFileAttributesW(target);
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        }
        std::wstring low = ToLower(target);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, L".exe") == 0) continue;           // programs belong up top, not here
        if (VibeUsage::FileHidden(target, t)) continue;
        RecentFile r;
        r.a.file = true;
        r.a.target = target;
        r.a.name = FileNameOf(target);
        r.a.key = L"file:" + ToLower(r.a.target);
        r.when = t;
        out.push_back(std::move(r));
    }
    return out;
}

// "5 min ago", "2 h ago", "yesterday", "3 days ago"
std::wstring Ago(ULONGLONG when) {
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    long long min = when && now > when ? (long long)((now - when) / 600000000ULL) : 0;
    wchar_t b[64];
    if (min < 1) return TR(L"akkurat nå", L"just now");
    if (min < 60) { swprintf(b, 64, TR(L"for %lld min siden", L"%lld min ago"), min); return b; }
    if (min < 24 * 60) { swprintf(b, 64, TR(L"for %lld t siden", L"%lld h ago"), min / 60); return b; }
    if (min < 48 * 60) return TR(L"i går", L"yesterday");
    swprintf(b, 64, TR(L"for %lld dager siden", L"%lld days ago"), min / (24 * 60));
    return b;
}

// The main colour of an icon (saturated pixels count most): VibeMenu tints each tile with it
D2D1_COLOR_F IconColour(IWICBitmapSource* src) {
    D2D1_COLOR_F none{0.5f, 0.5f, 0.55f, 1};
    if (!src) return none;
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512) return none;
    std::vector<BYTE> px(w * h * 4);
    if (FAILED(src->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) return none;
    double r = 0, g = 0, b = 0, wsum = 0;
    for (size_t i = 0; i < px.size(); i += 4) {
        double a = px[i + 3] / 255.0;
        if (a < 0.5) continue;
        double B = px[i] / 255.0 / a, G = px[i + 1] / 255.0 / a, R = px[i + 2] / 255.0 / a;   // premultiplied -> straight
        double mx = std::max({R, G, B}), mn = std::min({R, G, B});
        double weight = 0.05 + (mx - mn) * (mx - mn) * 4;   // colourful pixels decide, greys barely count
        r += R * weight; g += G * weight; b += B * weight; wsum += weight;
    }
    if (wsum <= 0) return none;
    return D2D1::ColorF((float)std::min(1.0, r / wsum), (float)std::min(1.0, g / wsum), (float)std::min(1.0, b / wsum), 1);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}


class Vibe : public Surface {
public:
    void Show(HMONITOR mon) {
        mon_ = mon ? mon : CursorMonitor();
        dpi_ = MonitorDpi(mon_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon_, &mi);
        int w = MulDiv((int)kW, dpi_, 96), h = MulDiv((int)kH, dpi_, 96);
        int bottom = mi.rcWork.bottom - MulDiv(12, dpi_, 96);
        if (Dock* d = g_app.DockOn(mon_)) {
            int top = d->TopEdge(mon_);
            if (top != INT_MAX) bottom = std::min(bottom, top - MulDiv(10, dpi_, 96));
        }
        h = std::min(h, bottom - ((int)mi.rcWork.top + MulDiv(12, dpi_, 96)));
        int left = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
        fullscreen_ = g_app.settings.vibeFullscreen;
        RECT where = fullscreen_ ? mi.rcMonitor : RECT{left, bottom - h, left + w, bottom};
        if (!CreateAt(where, WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) return;
        Glass(hwnd_, 0, !fullscreen_, Part::Vibe);
        // Full screen draws the card larger: fetch icons at that size so they stay sharp
        iconScale_ = 1;   // full screen lays itself out at normal size: icons are fetched at their real size
        if (AllApps(0xFFFFFFFF).empty()) AllApps();
        VibeUsage::Save();
        LoadNow();
        recent_ = RecentFiles(RecentCount());
        LoadPins();
        tab_ = GetPrivateProfileIntW(L"Vibe", L"Tab", 0, g_app.settings.path.c_str()) == 1 ? 1 : 0;
        trackOff_ = recent_.empty() && RecentTrackingOff();
        apps_.clear();
        files_.clear();
        calc_.clear();
        allMode_ = filesMode_ = false;
        all_.clear();
        moreFiles_.clear();
        allScroll_ = recentScroll_ = 0;
        if (fullscreen_) LoadAll();   // full screen shows every app next to the recent files
        scroll_ = 0;
        sel_ = hover_ = -1;
        armed_ = -1;
        tracking_ = false;
        artGen_ = (UINT)-1;
        art_.Reset();
        avatar_.Reset();
        first_ = UserDisplayName();
        if (size_t sp = first_.find(L' '); sp != std::wstring::npos) first_ = first_.substr(0, sp);
        int apx = Px(30);
        if (!avatarSrc_ || avatarPx_ != apx) { avatarSrc_ = AccountPicture(apx); avatarPx_ = apx; }
        PickPalette();
        ShowAnimated();
        if (!g_app.settings.performanceMode)
            SetTimer(hwnd_, T_AURORA, 50, nullptr);   // the colours drift slowly, ~20 frames a second, only while open (never in performance mode)
        PrewarmIcons(Px(kIcon));
    }

protected:
    static constexpr float kW = 600, kH = 660, kIcon = 48, kRow = 46, kHero = 122;
    static constexpr UINT_PTR T_AURORA = 21;
    enum Kind { KNow, KRecent, KApp, KFile, KCalc, KWindows, KLock, KRestart, KShutdown, KPrev, KPlay, KNext, KUser, KSignOut, KTrackDocs, KAll, KAllBtn, KMoreFile, KFilesBtn, KTab, KSearchBox };
    struct Hit { D2D1_RECT_F r; Kind kind; int idx; };

    const wchar_t* ClassName() const override { return L"SkylineVibeMenu"; }
    DWORD OpenMs() const override { return 220; }
    bool WantsFrame() const override { return NowMs() - animStart_ < 700; }   // the tiles' entrance

    // ---- colours of the hour: the card's aurora follows the day, mixed with your Windows accent ----
    void PickPalette() { VibePalette(blob_); }   // shared with the "Vibe" style of the top bar and dock

    // The app in your list that a running program belongs to. Many apps run from another exe than
    // their shortcut points at (Discord: Update.exe -> app-1.0.x\Discord.exe; Steam: steamwebhelper.exe),
    // so match the exe, then its description, then the app whose folder it lives inside.
    static AppEntry AppForExe(const std::wstring& exe) {
        std::wstring k = ToLower(exe);
        const auto& all = AllApps(0xFFFFFFFF);
        for (const auto& a : all)
            if (!a.setting && !a.file && (a.key == k || ToLower(a.exe) == k)) return a;
        std::wstring desc = ToLower(FileDescriptionOf(exe));
        if (!desc.empty())
            for (const auto& a : all)
                if (!a.setting && !a.file && ToLower(a.name) == desc) return a;
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring winDir = ToLower(win) + L"\\";
        std::wstring myDir = k.substr(0, k.find_last_of(L'\\') + 1);
        const AppEntry* best = nullptr;
        size_t bestLen = 0;
        for (const auto& a : all) {
            if (a.setting || a.file || a.exe.empty()) continue;
            std::wstring e = ToLower(a.exe);
            std::wstring dir = e.substr(0, e.find_last_of(L'\\') + 1);
            // strictly inside the app's folder (not beside it), never Windows' own folders, never a game library
            if (dir.size() < 4 || dir.size() >= myDir.size() || k.compare(0, dir.size(), dir) != 0) continue;
            if (dir.compare(0, winDir.size(), winDir) == 0 || myDir.find(L"\\steamapps\\", dir.size() - 1) != std::wstring::npos) continue;
            if (std::count(dir.begin(), dir.end(), L'\\') < 2) continue;   // not a bare drive
            if (dir.size() > bestLen) { best = &a; bestLen = dir.size(); }
        }
        return best ? *best : EntryFor(exe);
    }

    void LoadNow() {
        now_.clear();
        learned_ = false;
        auto dup = [this](const AppEntry& a) {
            for (const auto& n : now_)
                if (n.key == a.key || (!a.exe.empty() && ToLower(n.exe) == ToLower(a.exe)) || ToLower(n.name) == ToLower(a.name)) return true;
            return false;
        };
        for (const auto& exe : VibeUsage::Top(16)) {   // several exes can be one app: the first one counts
            if (now_.size() >= 8) break;
            AppEntry a = AppForExe(exe);
            if (a.name.empty() || dup(a) || VibeUsage::AppHidden(a)) continue;
            now_.push_back(a);
            learned_ = true;
        }
        if (now_.size() < 8)
            for (const auto& t : g_app.settings.pinned) {
                if (now_.size() >= 8) break;
                AppEntry a = EntryFor(t);
                if (VibeUsage::AppHidden(a)) continue;
                if (!dup(a) && !a.name.empty()) now_.push_back(a);
            }
    }

    void OnQuery() override {
        std::wstring q = query_;
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        calc_.clear();
        if (!q.empty() && q[0] == L'=') {   // a sum
            double v;
            if (q.size() > 1 && Calc::Eval(q.substr(1), v)) calc_ = Calc::Format(v);
            apps_.clear();
            files_.clear();
        } else {
            apps_ = q.empty() ? std::vector<AppEntry>{} : SearchApps(q, true, 5);
            if (q.size() < 2) files_.clear();
        }
        ++ticket_;
        KillTimer(hwnd_, T_FILES);
        if (q.size() >= 2 && q[0] != L'=') SetTimer(hwnd_, T_FILES, 150, nullptr);
        sel_ = q.empty() ? -1 : 0;
        hover_ = -1;
        resultsAt_ = NowMs();
    }

    std::wstring Greeting() const {
        SYSTEMTIME t;
        GetLocalTime(&t);
        const wchar_t* g = t.wHour < 5 ? TR(L"God natt", L"Good night") : t.wHour < 10 ? TR(L"God morgen", L"Good morning")
                         : t.wHour < 18 ? TR(L"God dag", L"Good afternoon") : TR(L"God kveld", L"Good evening");
        return first_.empty() ? std::wstring(g) : std::wstring(g) + L", " + first_;
    }

    std::wstring Today() const {
        wchar_t b[96] = {};
        GetDateFormatEx(g_norsk ? L"nb-NO" : L"en-US", 0, nullptr, g_norsk ? L"dddd d. MMMM" : L"dddd, MMMM d", b, 96, nullptr);
        std::wstring s = b;
        if (!s.empty()) s[0] = (wchar_t)towupper(s[0]);
        // ISO week number
        SYSTEMTIME t;
        GetLocalTime(&t);
        struct tm tmv{};
        tmv.tm_year = t.wYear - 1900; tmv.tm_mon = t.wMonth - 1; tmv.tm_mday = t.wDay; tmv.tm_hour = 12;
        mktime(&tmv);
        int wday = (tmv.tm_wday + 6) % 7;   // Monday = 0
        int week = (tmv.tm_yday - wday + 10) / 7;
        if (week < 1) {   // the first days of January can be in last year's week 52 or 53
            struct tm p{};
            p.tm_year = t.wYear - 1 - 1900; p.tm_mon = 0; p.tm_mday = 1; p.tm_hour = 12;
            mktime(&p);
            int jd = (p.tm_wday + 6) % 7;   // Monday = 0
            int py = t.wYear - 1;
            bool pleap = (py % 4 == 0 && py % 100 != 0) || py % 400 == 0;
            week = (jd == 3 || (pleap && jd == 2)) ? 53 : 52;   // a year has 53 weeks if it starts on a Thursday (or a Wednesday in a leap year)
        }
        else if (week == 53) {   // only if the year really has one
            int jan1 = (wday - tmv.tm_yday % 7 + 7) % 7;
            bool leap = (t.wYear % 4 == 0 && t.wYear % 100 != 0) || t.wYear % 400 == 0;
            if (!(jan1 == 3 || (leap && jan1 == 2))) week = 1;
        }
        return s + TR(L"  ·  uke ", L"  ·  week ") + std::to_wstring(week);
    }

    std::wstring Clock() const {
        wchar_t b[32] = {};
        GetTimeFormatEx(nullptr, TIME_NOSECONDS, nullptr, nullptr, b, 32);
        return b;
    }

    // The hit index the keyboard selection points at (sel_ counts selectable hits)
    static bool Selectable(Kind k) { return k == KNow || k == KRecent || k == KApp || k == KFile || k == KCalc || k == KWindows || k == KAll || k == KMoreFile; }
    static const wchar_t* ArmedText(int which) {
        switch (which) {
        case 0: return TR(L"Klikk igjen for å starte på nytt", L"Click again to restart");
        case 1: return TR(L"Klikk igjen for å slå av", L"Click again to shut down");
        case 2: return TR(L"Klikk igjen for å låse", L"Click again to lock");
        default: return TR(L"Klikk igjen for å logge av", L"Click again to sign out");
        }
    }
    int SelectableCount() const {
        int n = 0;
        for (const Hit& h : hitsPrev_) n += Selectable(h.kind);
        return n;
    }
    int SelHit() const {
        if (sel_ < 0) return -1;
        int k = 0;
        for (int i = 0; i < (int)hitsPrev_.size(); ++i)
            if (Selectable(hitsPrev_[i].kind) && k++ == sel_) return i;
        return -1;
    }
    bool Hot(int me) const { return me == SelHit() || me == hover_; }

    void Launch(const AppEntry& a) {
        AppEntry copy = a;
        Close();
        LaunchApp(copy);
    }

    void Activate(const Hit& h) {
        switch (h.kind) {
        case KNow: if (h.idx < (int)Tiles().size()) Launch(Tiles()[h.idx]); break;
        case KTab: SetTab(h.idx); break;
        case KSearchBox: break;
        case KRecent: if (h.idx < (int)recent_.size()) Launch(recent_[h.idx].a); break;
        case KApp: if (h.idx < (int)apps_.size()) Launch(apps_[h.idx]); break;
        case KFile: if (h.idx < (int)files_.size()) Launch(files_[h.idx]); break;
        case KCalc: CopyText(calc_); copiedAt_ = NowMs(); InvalidateRect(hwnd_, nullptr, FALSE); break;
        case KWindows: { std::wstring q = query_; Close(); OpenFileSearch(q); break; }
        case KPrev: NowPlaying::Previous(); break;
        case KPlay: NowPlaying::TogglePlay(); break;
        case KNext: NowPlaying::Next(); break;
        case KUser: Close(); ShellExecuteW(nullptr, nullptr, L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL); break;
        case KTrackDocs: Close(); ShellExecuteW(nullptr, nullptr, L"ms-settings:personalization-start", nullptr, nullptr, SW_SHOWNORMAL); break;
        case KAll: if (h.idx < (int)all_.size()) Launch(all_[h.idx]); break;
        case KAllBtn: ShowAllApps(!allMode_); break;
        case KMoreFile: if (h.idx < (int)moreFiles_.size()) Launch(moreFiles_[h.idx].a); break;
        case KFilesBtn: ShowMoreFiles(!filesMode_); break;
        case KLock: case KSignOut: case KRestart: case KShutdown: {
            // Every one of these asks twice: the first click turns it red and says what will happen
            int which = h.kind == KRestart ? 0 : h.kind == KShutdown ? 1 : h.kind == KLock ? 2 : 3;
            if (armed_ != which) {
                armed_ = which;
                SetTimer(hwnd_, T_POWER, 4000, nullptr);
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
            armed_ = -1;
            Close();
            if (which == 0) RestartPc();
            else if (which == 1) ShutDownPc();
            else if (which == 2) LockWorkStation();
            else ExitWindowsEx(EWX_LOGOFF, 0);
            break;
        }
        }
    }

    void CopyText(const std::wstring& s) {
        if (s.empty() || !OpenClipboard(hwnd_)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }

    const AppEntry* EntryOf(const Hit& h) const {
        switch (h.kind) {
        case KNow: return h.idx < (int)Tiles().size() ? &Tiles()[h.idx] : nullptr;
        case KRecent: return h.idx < (int)recent_.size() ? &recent_[h.idx].a : nullptr;
        case KApp: return h.idx < (int)apps_.size() ? &apps_[h.idx] : nullptr;
        case KFile: return h.idx < (int)files_.size() ? &files_[h.idx] : nullptr;
        case KAll: return h.idx < (int)all_.size() ? &all_[h.idx] : nullptr;
        case KMoreFile: return h.idx < (int)moreFiles_.size() ? &moreFiles_[h.idx].a : nullptr;
        default: return nullptr;
        }
    }

    // "All apps": every app, A to Z, in the same card (typing searches as always)
    // "Show more" files: a longer list of what you opened last, in its own scrolling view
    void ShowMoreFiles(bool on) {
        filesMode_ = on;
        allMode_ = false;
        scroll_ = 0;
        sel_ = hover_ = -1;
        moreFiles_.clear();
        if (on) moreFiles_ = RecentFiles(40);
        resultsAt_ = NowMs();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void LoadAll() {
        all_.clear();
        for (const auto& a : AllApps())
            if (!a.setting && !a.file && !a.name.empty()) all_.push_back(a);
    }

    void ShowAllApps(bool on) {
        filesMode_ = false;
        allMode_ = on;
        allScroll_ = 0;
        sel_ = hover_ = -1;
        if (on) LoadAll();
        else if (!fullscreen_) all_.clear();
        resultsAt_ = NowMs();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Pinned apps: the very same list as SkyMenu's
    void LoadPins() {
        pins_.clear();
        for (const auto& t : MenuPins()) {
            if (pins_.size() >= 8) break;
            AppEntry a = EntryFor(t);
            if (!a.name.empty()) pins_.push_back(a);
        }
    }
    const std::vector<AppEntry>& Tiles() const { return tab_ == 1 ? pins_ : now_; }
    void SetTab(int t) {
        if (t == tab_) return;
        tab_ = t;
        tabAt_ = NowMs();   // the tiles arrive again, one after the other
        sel_ = hover_ = -1;
        IniWrite(L"Vibe", L"Tab", t);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    size_t RecentCount() const { return 10; }   // the list scrolls; "Show more" has up to 40

    // ---- drawing helpers ----
    D2D1_COLOR_F TileColour(const AppEntry& a) {
        auto it = tileColour_.find(a.key);
        if (it != tileColour_.end()) return it->second;
        int px = Px(kIcon);
        if (!AppIconCached(a, px)) return D2D1::ColorF(0.5f, 0.5f, 0.55f, 1);   // not loaded yet: ask again next frame
        return tileColour_[a.key] = IconColour(AppIcon(a, px).Get());
    }

    // Text with the part that matches what you typed in bold and in colour
    void DrawMatch(const std::wstring& text, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, D2D1_COLOR_F hi) {
        if (!f || text.empty()) return;
        ComPtr<IDWriteTextLayout> tl;
        g_gfx.dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, r.right - r.left, r.bottom - r.top, tl.Put());
        if (!tl) return;
        std::wstring q = query_;
        while (!q.empty() && iswspace(q.back())) q.pop_back();
        size_t at = q.empty() ? std::wstring::npos : ToLower(text).find(ToLower(q));
        ComPtr<ID2D1SolidColorBrush> hb;
        if (at != std::wstring::npos && SUCCEEDED(rt_->CreateSolidColorBrush(hi, hb.Put()))) {
            DWRITE_TEXT_RANGE range{(UINT32)at, (UINT32)q.size()};
            tl->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
            tl->SetDrawingEffect(hb.Get(), range);
        }
        brush_->SetColor(c);
        rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), tl.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // A soft glowing blob for the aurora
    void Blob(float cx, float cy, float rad, D2D1_COLOR_F c, float alpha) {
        D2D1_GRADIENT_STOP stops[2] = {{0, D2D1::ColorF(c.r, c.g, c.b, alpha)}, {1, D2D1::ColorF(c.r, c.g, c.b, 0)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        if (FAILED(rt_->CreateGradientStopCollection(stops, 2, sc.Put()))) return;
        ComPtr<ID2D1RadialGradientBrush> rb;
        if (FAILED(rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rad, rad),
                                                  sc.Get(), rb.Put())))
            return;
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), rb.Get());
    }

    // 0..1: how far an item's entrance has come (they arrive one after the other)
    float Enter(int i, double base) const {
        float t = (float)((NowMs() - base - 40 - i * 32) / 260);
        return Ease(t);
    }

    void Section(const wchar_t* title, float y, float W, const wchar_t* right = nullptr) {
        const Theme& th = g_app.theme;
        DrawText(title, fHead_.Get(), D2D1::RectF(26, y, W - 26, y + 22), th.fg);
        if (right) DrawText(right, fTagR_.Get(), D2D1::RectF(W / 2, y, W - 26, y + 22), th.fgDim);
    }

    void Paint() override {
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        float RW = sz.width, RH = sz.height;   // the window
        // Normal: the card is the window. Full screen: its own layout across the whole screen (PaintFull).
        // (Nordlys at night: a faint starry sky behind the full-screen layout, drawn after the aurora below.)
        float W = RW, H = RH;
        s_ = 1;
        ox_ = oy_ = 0;
        double now = NowMs();
        if (!fClock_) {
            fClock_ = Format(L"Segoe UI Variable Display", 54, DWRITE_FONT_WEIGHT_SEMI_LIGHT);
            fHello_ = Format(L"Segoe UI", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fDate_ = Format(L"Segoe UI", 12.5f);
            fSearch_ = Format(L"Segoe UI", 14.5f);
            fGlyph_ = Format(IconFont(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fHead_ = Format(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fText_ = Format(L"Segoe UI", 14);
            fSub_ = Format(L"Segoe UI", 11.5f);
            fTagR_ = Format(L"Segoe UI", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
            fPill_ = Format(L"Segoe UI", 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fLabel_ = Format(L"Segoe UI", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fBadge_ = Format(L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fCalc_ = Format(L"Segoe UI Variable Display", 44, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fHint_ = Format(L"Segoe UI", 12.5f);
            fRing_ = Format(L"Segoe UI", 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fRingSub_ = Format(L"Segoe UI", 10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
            fMediaT_ = Format(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fName_ = Format(L"Segoe UI", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            fClockBig_ = Format(L"Segoe UI Variable Display", 96, DWRITE_FONT_WEIGHT_SEMI_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER);
            fClockMid_ = Format(L"Segoe UI Variable Display", 66, DWRITE_FONT_WEIGHT_SEMI_LIGHT, DWRITE_TEXT_ALIGNMENT_CENTER);
            fHelloBig_ = Format(L"Segoe UI", 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            fDateC_ = Format(L"Segoe UI", 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        hits_.clear();
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        {
            D2D1_COLOR_F tint = GlassTint(Part::Vibe);
            tint.a *= fullscreen_ ? anim_ : 1;
            brush_->SetColor(tint);
            rt_->FillRectangle(D2D1::RectF(0, 0, RW, RH), brush_.Get());
            if (!fullscreen_) PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, RW, RH), 8);
        }

        // ---- the aurora: three soft colours drifting slowly behind everything (the whole screen in full screen)
        {
            float t = (float)(now / 1000.0);
            float a = (th.dark ? 0.42f : 0.30f) * anim_;
            Blob(RW * 0.18f + std::sin(t * 0.21f) * RW * 0.10f, RH * 0.12f + std::cos(t * 0.17f) * RH * 0.06f, RW * 0.62f, blob_[0], a);
            Blob(RW * 0.86f + std::cos(t * 0.16f + 1) * RW * 0.10f, RH * 0.30f + std::sin(t * 0.23f) * RH * 0.09f, RW * 0.55f, blob_[1], a * 0.85f);
            Blob(RW * 0.45f + std::sin(t * 0.12f + 2) * RW * 0.14f, RH * 0.95f + std::cos(t * 0.19f) * RH * 0.05f, RW * 0.70f, blob_[2], a * 0.75f);
        }
        if (fullscreen_) PaintStars(rt_.Get(), D2D1::RectF(0, 0, RW, RH), 0.8f * anim_);

        // Everything else rises a little and fades in as it opens
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, 14 * (1 - anim_)));
        ComPtr<ID2D1Layer> layer;
        bool fading = anim_ < 0.999f;
        if (fading) {
            rt_->CreateLayer(layer.Put());
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), anim_),
                           layer.Get());
        }
        if (fullscreen_) PaintFull(RW, RH);
        else PaintCard(W, H);
        if (fading) rt_->PopLayer();
        rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); art_.Reset(); avatar_.Reset(); }
        hitsPrev_ = hits_;
    }

    // ---- the search field: a pill with a thin glowing edge in the colours of the hour
    void PaintSearch(D2D1_RECT_F sr) {
        const Theme& th = g_app.theme;
        hits_.push_back({sr, KSearchBox, 0});   // clicking it is not "empty space"
        float rad = (sr.bottom - sr.top) / 2;
        brush_->SetColor(Alpha(th.fg, th.dark ? 0.08f : 0.06f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(sr, rad, rad), brush_.Get());
        D2D1_GRADIENT_STOP st[2] = {{0, Mix(blob_[0], th.fg, 0.15f)}, {1, Mix(blob_[1], th.fg, 0.15f)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lb;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, sc.Put())) &&
            SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(sr.left, 0), D2D1::Point2F(sr.right, 0)),
                                                     sc.Get(), lb.Put()))) {
            lb->SetOpacity(query_.empty() ? 0.55f : 0.95f);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.75f, sr.top + 0.75f, sr.right - 0.75f, sr.bottom - 0.75f), rad, rad),
                                      lb.Get(), 1.5f);
        }
        DrawGlyph(0xE721, fGlyph_.Get(), D2D1::RectF(sr.left + 12, sr.top, sr.left + 40, sr.bottom), th.fgDim);
        const wchar_t* ph = TR(L"Søk etter alt  ·  skriv = for å regne", L"Search everything  ·  type = to do maths");
        DrawText(query_.empty() ? std::wstring(ph) : query_, fSearch_.Get(), D2D1::RectF(sr.left + 46, sr.top, sr.right - 16, sr.bottom),
                 query_.empty() ? th.fgDim : th.fg);
        if (caretOn_ && !closing_) {
            float cx = sr.left + 46 + (query_.empty() ? 0 : TextWidth(query_, fSearch_.Get()) + 1);
            brush_->SetColor(th.fg);
            rt_->FillRectangle(D2D1::RectF(cx, sr.top + 11, cx + 1.4f, sr.bottom - 11), brush_.Get());
        }
    }

    // Full screen: no card. The time big in the middle at the top, what's playing (or the day) at the top right,
    // the search in the middle, "Now | Pinned" as one wide row, then recent files and all apps side by side.
    void PaintFull(float RW, float RH) {
        const Theme& th = g_app.theme;
        float lift = 14 * (1 - anim_);
        auto at = [&](float x, float y) { rt_->SetTransform(D2D1::Matrix3x2F::Translation(x, y + lift)); };
        // hits are drawn in a panel's own coordinates: move them to the screen's
        auto shift = [&](size_t from, float x, float y) {
            for (size_t i = from; i < hits_.size(); ++i) { auto& r = hits_[i].r; r.left += x; r.right += x; r.top += y; r.bottom += y; }
        };
        bool compact = RH < 820;   // laptops with scaling: a smaller clock leaves room for the rest
        float T = compact ? 26 : std::max(44.0f, RH * 0.05f);
        at(0, 0);
        DrawText(Clock(), (compact ? fClockMid_ : fClockBig_).Get(), D2D1::RectF(0, T, RW, T + (compact ? 76 : 108)), th.fg);
        float y = T + (compact ? 78 : 112);
        DrawText(Greeting(), fHelloBig_.Get(), D2D1::RectF(0, y, RW, y + 26), th.fg);
        y += 27;
        DrawText(Today(), fDateC_.Get(), D2D1::RectF(0, y, RW, y + 20), th.fgDim);
        y += compact ? 30 : 42;
        if (RW >= 1100) {   // what's playing, or how far the day has come: top right
            size_t h0 = hits_.size();
            float hx = RW - 620, hy = T - 4;
            at(hx, hy);
            PaintHeroRight(600);
            shift(h0, hx, hy);
            at(0, 0);
        }
        float sw = std::min(720.0f, RW - 80);
        PaintSearch(D2D1::RectF((RW - sw) / 2, y, (RW + sw) / 2, y + 48));
        y += 48 + (compact ? 22 : 34);

        float bottom = RH - 84;
        float Wc = std::min(RW - 120, 1180.0f), cx0 = (RW - Wc) / 2;
        allCol_ = leftCol_ = recentCol_ = D2D1::RectF(0, 0, 0, 0);
        rt_->PushAxisAlignedClip(D2D1::RectF(0, y - 6, RW, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        if (!calc_.empty() || !query_.empty()) {
            float cw = std::min(760.0f, RW - 80), x0 = (RW - cw) / 2;
            size_t h0 = hits_.size();
            at(x0, 0);
            if (!calc_.empty()) PaintCalc(cw, y);
            else PaintResults(cw, y);
            shift(h0, x0, 0);
        } else {
            size_t h0 = hits_.size();
            at(cx0, 0);
            float y2 = PaintTiles(Wc, y, 8, false);
            shift(h0, cx0, 0);
            if (filesMode_) {   // "Show more": the long list, in a calm column of its own
                float cw = std::min(760.0f, Wc), x0 = (RW - cw) / 2;
                h0 = hits_.size();
                at(x0, 0);
                PaintMoreFiles(cw, y2, bottom);
                shift(h0, x0, 0);
                leftCol_ = D2D1::RectF(x0, y2, x0 + cw, bottom);
            } else {
                // recent files as a row of cards, then every app as a quiet grid of icons that scrolls
                h0 = hits_.size();
                at(cx0, 0);
                float y3 = PaintRecentCards(Wc, y2);
                PaintAllGrid(Wc, y3, bottom);
                shift(h0, cx0, 0);
                allCol_ = D2D1::RectF(cx0, y3, cx0 + Wc, bottom);
            }
        }
        at(0, 0);
        rt_->PopAxisAlignedClip();

        // you and the power buttons along the bottom, under the content column
        size_t h0 = hits_.size();
        at(cx0, 0);
        PaintBottom(Wc, RH - 8, bottom + 8);
        shift(h0, cx0, 0);
        at(0, 0);
    }

    // Full screen: the last files you opened as a row of cards (one line each: icon, name, folder · when)
    float PaintRecentCards(float W, float y) {
        const Theme& th = g_app.theme;
        Section(TR(L"Fortsett der du slapp", L"Pick up where you left off"), y, W);
        if (!recent_.empty()) AllAppsPill(W, y, TR(L"Vis flere  \u203a", L"Show more  \u203a"), KFilesBtn);
        y += 30;
        if (recent_.empty()) {
            if (trackOff_) {
                D2D1_RECT_F r = D2D1::RectF(14, y, std::min(W - 14, 560.0f), y + kRow - 4);
                int me = (int)hits_.size();
                hits_.push_back({r, KTrackDocs, 0});
                brush_->SetColor(Hot(me) ? th.hover : Alpha(th.fg, 0.05f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
                float cy = (r.top + r.bottom) / 2;
                DrawText(TR(L"Windows husker ikke filene du åpner.", L"Windows isn't remembering the files you open."), fText_.Get(),
                         D2D1::RectF(r.left + 16, r.top + 2, r.right - 16, cy + 4), th.fg);
                DrawText(TR(L"Klikk for å slå på «Vis nylig åpnede elementer».", L"Click to turn on \u201cShow recently opened items\u201d."), fSub_.Get(),
                         D2D1::RectF(r.left + 16, cy + 2, r.right - 16, r.bottom - 2), th.fgDim);
            } else {
                DrawText(TR(L"Filene du åpner, havner her.", L"The files you open end up here."), fHint_.Get(), D2D1::RectF(26, y, W - 26, y + 30), th.fgDim);
            }
            return y + kRow + 22;
        }
        int n = std::min<int>((int)recent_.size(), W >= 1000 ? 4 : 3);
        float gap = 12, cw = (W - 28 - gap * (n - 1)) / n, ch = 58;
        int spx = Px(28);
        for (int i = 0; i < n; ++i) {
            float e = Enter(9 + i, animStart_);
            float x = 14 + i * (cw + gap);
            D2D1_RECT_F r = D2D1::RectF(x, y + (1 - e) * 10, x + cw, y + ch + (1 - e) * 10);
            int me = (int)hits_.size();
            hits_.push_back({r, KRecent, i});
            brush_->SetColor(Hot(me) ? (me == SelHit() ? Alpha(th.accent, 0.24f) : th.hover) : Alpha(th.fg, th.dark ? 0.05f : 0.07f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), brush_.Get());
            const AppEntry& a = recent_[i].a;
            float cy = (r.top + r.bottom) / 2;
            if (ID2D1Bitmap* b = Icon(a, spx)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(r.left + 14, cy - 14, r.left + 42, cy + 14), e);
            DrawText(a.name, fText_.Get(), D2D1::RectF(r.left + 54, r.top + 7, r.right - 12, cy + 2), Alpha(th.fg, e));
            std::wstring dir = a.target.substr(0, a.target.find_last_of(L'\\'));
            DrawText(FileNameOf(dir) + L"  \u00b7  " + Ago(recent_[i].when), fSub_.Get(), D2D1::RectF(r.left + 54, cy + 1, r.right - 12, r.bottom - 6),
                     Alpha(th.fgDim, e));
        }
        return y + ch + 26;
    }

    // Full screen: every app as a calm grid of icons with their names, scrolling with the mouse wheel
    void PaintAllGrid(float W, float y, float bottom) {
        const Theme& th = g_app.theme;
        Section(TR(L"Alle apper", L"All apps"), y, W);
        y += 30;
        float top = y;
        const float cellW = 112, cellH = 96;
        int cols = std::max(1, (int)((W - 28) / cellW));
        float x0 = 14 + ((W - 28) - cols * cellW) / 2;
        int rows = ((int)all_.size() + cols - 1) / cols;
        allMax_ = std::max(0.0f, rows * cellH - (bottom - top) + 8);
        if (allScroll_ > allMax_) allScroll_ = allMax_;
        int px = Px(40);
        rt_->PushAxisAlignedClip(D2D1::RectF(0, top - 4, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)all_.size(); ++i) {
            float cx = x0 + (i % cols) * cellW, cy = top + (i / cols) * cellH - allScroll_;
            if (cy + cellH <= top - 4 || cy >= bottom) continue;   // only what you can see
            D2D1_RECT_F r = D2D1::RectF(cx + 4, cy, cx + cellW - 4, cy + cellH - 6);
            int me = (int)hits_.size();
            hits_.push_back({D2D1::RectF(r.left, std::max(r.top, top - 4), r.right, std::min(r.bottom, bottom)), KAll, i});
            if (Hot(me)) {
                brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.24f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 14, 14), brush_.Get());
            }
            float mx = (r.left + r.right) / 2;
            if (ID2D1Bitmap* b = Icon(all_[i], px)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(mx - 20, r.top + 10, mx + 20, r.top + 50));
            DrawText(all_[i].name, fLabel_.Get(), D2D1::RectF(r.left + 4, r.top + 56, r.right - 4, r.top + 78), th.fg);
        }
        rt_->PopAxisAlignedClip();
        // more below: a small chevron says the grid scrolls
        if (allScroll_ < allMax_ - 1) DrawGlyph(0xE70D, fGlyph_.Get(), D2D1::RectF(W / 2 - 20, bottom - 18, W / 2 + 20, bottom), th.fgDim);
    }

    // Normal: the glass card above the dock
    void PaintCard(float W, float H) {
        const Theme& th = g_app.theme;
        // a whisper of darkness/lightness at the bottom so the text there stays easy to read
        brush_->SetColor(th.dark ? D2D1::ColorF(0, 0, 0, 0.18f) : D2D1::ColorF(1, 1, 1, 0.18f));
        rt_->FillRectangle(D2D1::RectF(0, H - 60, W, H), brush_.Get());

        // ---- hero: the time, big; a greeting; and on the right what's playing, or how far the day has come
        DrawText(Clock(), fClock_.Get(), D2D1::RectF(24, 6, W / 2 + 40, 78), th.fg);
        DrawText(Greeting(), fHello_.Get(), D2D1::RectF(26, 74, W / 2 + 60, 96), th.fg);
        DrawText(Today(), fDate_.Get(), D2D1::RectF(26, 95, W / 2 + 80, 114), th.fgDim);
        PaintHeroRight(W);

        D2D1_RECT_F sr = D2D1::RectF(18, kHero + 6, W - 18, kHero + 48);
        PaintSearch(sr);

        float y = sr.bottom + 18, bottom = H - 60;
        rt_->PushAxisAlignedClip(D2D1::RectF(0, y - 6, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        if (!calc_.empty()) PaintCalc(W, y);
        else if (!query_.empty()) PaintResults(W, y);
        else if (allMode_) PaintAllApps(W, y, bottom);
        else if (filesMode_) PaintMoreFiles(W, y, bottom);
        else PaintHome(W, y, bottom);
        rt_->PopAxisAlignedClip();

        PaintBottom(W, H, bottom);
    }

    void PaintHeroRight(float W) {
        const Theme& th = g_app.theme;
        const NowPlaying::State& np = NowPlaying::Current();
        D2D1_RECT_F box = D2D1::RectF(W / 2 + 30, 18, W - 18, 108);
        if (np.active) {
            // what's playing: cover, title, artist, prev / play / next, and a thin progress line
            float rad = 16;
            D2D1_COLOR_F tint = np.tint.a > 0 ? np.tint : blob_[1];
            brush_->SetColor(Alpha(Mix(tint, th.dark ? D2D1::ColorF(0, 0, 0) : D2D1::ColorF(1, 1, 1), 0.35f), 0.55f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(box, rad, rad), brush_.Get());
            if (np.gen != artGen_ || (!art_ && np.art)) {
                art_.Reset();
                if (np.art) rt_->CreateBitmapFromWicBitmap(np.art.Get(), nullptr, art_.Put());
                artGen_ = np.gen;
            }
            D2D1_RECT_F cover = D2D1::RectF(box.left + 12, box.top + 12, box.left + 12 + 54, box.top + 12 + 54);
            if (art_) {
                ComPtr<ID2D1BitmapBrush> bb;
                rt_->CreateBitmapBrush(art_.Get(), bb.Put());
                if (bb) {
                    D2D1_SIZE_F bs = art_->GetSize();
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(54 / bs.width, 54 / bs.height) * D2D1::Matrix3x2F::Translation(cover.left, cover.top));
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(cover, 8, 8), bb.Get());
                }
            } else {
                brush_->SetColor(Alpha(th.fg, 0.12f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(cover, 8, 8), brush_.Get());
                DrawGlyph(0xE8D6, fGlyph_.Get(), cover, th.fg);
            }
            DrawText(np.title, fMediaT_.Get(), D2D1::RectF(cover.right + 10, box.top + 12, box.right - 10, box.top + 32), th.fg);
            DrawText(np.artist.empty() ? np.app : np.artist, fSub_.Get(), D2D1::RectF(cover.right + 10, box.top + 31, box.right - 10, box.top + 48), th.fgDim);
            struct B { Kind k; wchar_t g; } bs[] = {{KPrev, 0xE892}, {KPlay, (wchar_t)(np.playing ? 0xE769 : 0xE768)}, {KNext, 0xE893}};
            for (int i = 0; i < 3; ++i) {
                float bx = cover.right + 6 + i * 32, by = box.top + 48;
                D2D1_RECT_F r = D2D1::RectF(bx, by, bx + 28, by + 24);
                int me = (int)hits_.size();
                hits_.push_back({r, bs[i].k, 0});
                if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), brush_.Get()); }
                DrawGlyph(bs[i].g, fGlyph_.Get(), r, th.fg);
            }
            if (np.duration > 0) {
                float p = (float)std::clamp(np.position / np.duration, 0.0, 1.0);
                D2D1_RECT_F line = D2D1::RectF(box.left + 14, box.bottom - 9, box.right - 14, box.bottom - 6);
                brush_->SetColor(Alpha(th.fg, 0.15f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(line, 1.5f, 1.5f), brush_.Get());
                line.right = line.left + (line.right - line.left) * p;
                brush_->SetColor(th.fg);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(line, 1.5f, 1.5f), brush_.Get());
            }
            return;
        }
        // nothing playing: a ring showing how much of the day has gone, or at night (22–06) how much of the night
        SYSTEMTIME t;
        GetLocalTime(&t);
        int nowMin = t.wHour * 60 + t.wMinute;
        bool night = nowMin >= 22 * 60 || nowMin < 6 * 60;
        int nightGone = night ? (nowMin >= 22 * 60 ? nowMin - 22 * 60 : nowMin + 120) : 0;   // minutes since 22:00
        float frac = night ? nightGone / 480.0f : nowMin / 1440.0f;
        D2D1_POINT_2F c = D2D1::Point2F(box.right - 46, (box.top + box.bottom) / 2);
        float r = 36;
        brush_->SetColor(Alpha(th.fg, 0.12f));
        rt_->DrawEllipse(D2D1::Ellipse(c, r, r), brush_.Get(), 5);
        ComPtr<ID2D1PathGeometry> arc;
        if (frac > 0.002f && SUCCEEDED(g_gfx.d2d->CreatePathGeometry(arc.Put()))) {
            ComPtr<ID2D1GeometrySink> sk;
            arc->Open(sk.Put());
            float a0 = -3.14159265f / 2, a1 = a0 + frac * 2 * 3.14159265f;
            sk->BeginFigure(D2D1::Point2F(c.x + r * std::cos(a0), c.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + r * std::cos(a1), c.y + r * std::sin(a1)), D2D1::SizeF(r, r), 0,
                                        D2D1_SWEEP_DIRECTION_CLOCKWISE, frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            ComPtr<ID2D1StrokeStyle> round;
            g_gfx.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, round.Put());
            brush_->SetColor(Mix(blob_[0], th.fg, 0.2f));
            rt_->DrawGeometry(arc.Get(), brush_.Get(), 5, round.Get());
        }
        DrawText(std::to_wstring((int)std::lround(frac * 100)) + L"%", fRing_.Get(), D2D1::RectF(c.x - r, c.y - 14, c.x + r, c.y + 6), th.fg);
        DrawText(night ? TR(L"av natta", L"of the night") : TR(L"av dagen", L"of the day"), fRingSub_.Get(), D2D1::RectF(c.x - r, c.y + 4, c.x + r, c.y + 18), th.fgDim);
        // and a line about the day (or the night) next to it
        DrawText(night ? TR(L"Natta så langt", L"The night so far") : TR(L"Dagen så langt", L"The day so far"), fHead_.Get(),
                 D2D1::RectF(box.left, c.y - 22, c.x - r - 12, c.y - 2), th.fg);
        int mins = night ? 480 - nightGone : 1440 - nowMin;
        wchar_t b[64];
        if (night) swprintf(b, 64, TR(L"%d t %d min til morgen", L"%d h %d min until morning"), mins / 60, mins % 60);
        else swprintf(b, 64, TR(L"%d t %d min igjen", L"%d h %d min left"), mins / 60, mins % 60);
        DrawText(b, fSub_.Get(), D2D1::RectF(box.left, c.y, c.x - r - 12, c.y + 18), th.fgDim);
    }

    void PaintHome(float W, float y, float bottom) {
        y = PaintTiles(W, y, 4, true);
        PaintRecent(W, y, bottom);
    }

    // "Now | Pinned": two small tabs where the section title was; the tiles below show the chosen one
    float PaintTiles(float W, float y, int cols, bool allPill) {
        const Theme& th = g_app.theme;
        const std::vector<AppEntry>& tiles = Tiles();
        float tx = 18;
        for (int t = 0; t < 2; ++t) {
            const wchar_t* label = t == 1 ? TR(L"Festet", L"Pinned") : learned_ ? TR(L"Nå", L"Now") : TR(L"Favoritter", L"Favourites");
            float tw = TextWidth(label, fHead_.Get()) + 22;
            D2D1_RECT_F r = D2D1::RectF(tx, y - 1, tx + tw, y + 23);
            int me = (int)hits_.size();
            hits_.push_back({r, KTab, t});
            if (t == tab_) {
                brush_->SetColor(Alpha(Mix(blob_[0], th.fg, 0.2f), 0.30f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 11, 11), brush_.Get());
            } else if (me == hover_) {
                brush_->SetColor(th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 11, 11), brush_.Get());
            }
            DrawText(label, fHead_.Get(), D2D1::RectF(r.left + 11, y, r.right, y + 22), t == tab_ ? th.fg : th.fgDim);
            tx = r.right + 4;
        }
        float pl = allPill ? AllAppsPill(W, y, TR(L"Alle apper  ›", L"All apps  ›"), KAllBtn) : W - 22;
        const wchar_t* sub = tab_ == 1 ? TR(L"også i SkyMenu", L"shared with SkyMenu")
                           : learned_ ? TR(L"det du pleier å bruke nå", L"what you usually use now") : TR(L"lærer etter hvert hva du bruker", L"learns what you use as you go");
        DrawText(sub, fTagR_.Get(), D2D1::RectF(tx + 10, y, pl - 10, y + 22), th.fgDim);
        y += 28;
        int px = Px(kIcon);
        double base = std::max(animStart_, tabAt_);
        float cellW = (W - 36) / cols, cellH = 108;
        int shown = std::min((int)tiles.size(), 8);
        for (int i = 0; i < shown; ++i) {
            float e = Enter(i, base);
            float x = 18 + (i % cols) * cellW, cy = y + (i / cols) * (cellH + 8) + (1 - e) * 16;
            D2D1_RECT_F cell = D2D1::RectF(x + 4, cy, x + cellW - 4, cy + cellH);
            int me = (int)hits_.size();
            hits_.push_back({cell, KNow, i});
            bool hot = Hot(me);
            if (hot) cell = D2D1::RectF(cell.left - 2, cell.top - 3, cell.right + 2, cell.bottom - 1);   // lifts a little
            D2D1_COLOR_F col = TileColour(tiles[i]);
            // tile: the icon's colour, brighter at the top
            D2D1_GRADIENT_STOP st[2] = {{0, Alpha(col, (hot ? 0.42f : 0.28f) * e)}, {1, Alpha(col, (hot ? 0.20f : 0.10f) * e)}};
            ComPtr<ID2D1GradientStopCollection> sc;
            ComPtr<ID2D1LinearGradientBrush> lb;
            if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, sc.Put())) &&
                SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, cell.top), D2D1::Point2F(0, cell.bottom)),
                                                         sc.Get(), lb.Put())))
                rt_->FillRoundedRectangle(D2D1::RoundedRect(cell, 18, 18), lb.Get());
            brush_->SetColor(Alpha(hot ? Mix(col, th.fg, 0.4f) : th.fg, (hot ? 0.55f : 0.08f) * e));
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cell.left + 0.5f, cell.top + 0.5f, cell.right - 0.5f, cell.bottom - 0.5f), 18, 18),
                                      brush_.Get(), hot ? 1.5f : 1.0f);
            float cx = (cell.left + cell.right) / 2;
            float ic = hot ? kIcon + 4 : kIcon;
            D2D1_RECT_F ir = D2D1::RectF(cx - ic / 2, cell.top + 14, cx + ic / 2, cell.top + 14 + ic);
            if (ID2D1Bitmap* b = Icon(tiles[i], px)) DrawImageHQ(rt_.Get(), b, ir, e);
            DrawText(tiles[i].name, fLabel_.Get(), D2D1::RectF(cell.left + 8, cell.bottom - 32, cell.right - 8, cell.bottom - 10), Alpha(th.fg, e));
            // its number: press it to start
            D2D1_RECT_F nb = D2D1::RectF(cell.left + 10, cell.top + 9, cell.left + 28, cell.top + 27);
            brush_->SetColor(Alpha(th.fg, 0.14f * e));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(nb, 6, 6), brush_.Get());
            wchar_t num[2] = {(wchar_t)(L'1' + i), 0};
            DrawText(num, fBadge_.Get(), nb, Alpha(th.fg, 0.9f * e));
        }
        if (tiles.empty())
            DrawText(tab_ == 1 ? TR(L"Høyreklikk en app og velg «Fest» for å legge den her. SkyMenu får den også.",
                                    L"Right-click an app and choose \u201cPin\u201d to put it here. SkyMenu gets it too.")
                               : TR(L"Åpne noen apper, så dukker de du bruker mest opp her.", L"Open a few apps and the ones you use most show up here."),
                     fHint_.Get(), D2D1::RectF(26, y, W - 26, y + 30), th.fgDim);
        int rows = (shown + cols - 1) / cols;
        y += shown == 0 ? 34 : rows * cellH + (rows - 1) * 8;
        // the card keeps its height whichever tab is showing, so nothing below jumps
        if (cols == 4 && shown > 0 && shown <= 4 && std::max(now_.size(), pins_.size()) > 4) y += cellH + 8;
        return y + 16;
    }

    void PaintRecent(float W, float y, float bottom) {
        const Theme& th = g_app.theme;
        // ---- Pick up where you left off: file, folder it lives in, and when
        Section(TR(L"Fortsett der du slapp", L"Pick up where you left off"), y, W);
        if (!recent_.empty()) AllAppsPill(W, y, TR(L"Vis flere  ›", L"Show more  ›"), KFilesBtn);
        y += 28;
        int spx = Px(28);
        // the last files you opened; more than fit scroll with the mouse wheel ("Show more" has up to 40)
        float top = y;
        recentCol_ = D2D1::RectF(0, top - 4, W, bottom);
        recentMax_ = std::max(0.0f, recent_.size() * kRow - (bottom - top) + 4);
        if (recentScroll_ > recentMax_) recentScroll_ = recentMax_;
        rt_->PushAxisAlignedClip(D2D1::RectF(0, top - 4, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        y = top - recentScroll_;
        for (int i = 0; i < (int)recent_.size(); ++i, y += kRow) {
            if (y + kRow <= top - 4 || y >= bottom) continue;   // only rows you can see are drawn and clickable
            float e = Enter(8 + std::min(i, 6), animStart_);
            D2D1_RECT_F r = D2D1::RectF(14, y + (1 - e) * 10, W - 14, y + kRow - 4 + (1 - e) * 10);
            int me = (int)hits_.size();
            hits_.push_back({D2D1::RectF(r.left, std::max(r.top, top - 4), r.right, std::min(r.bottom, bottom)), KRecent, i});
            if (Hot(me)) {
                brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.24f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
            }
            const AppEntry& a = recent_[i].a;
            float cy = (r.top + r.bottom) / 2;
            if (ID2D1Bitmap* b = Icon(a, spx)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(r.left + 12, cy - 14, r.left + 40, cy + 14), e);
            DrawText(a.name, fText_.Get(), D2D1::RectF(r.left + 52, r.top + 2, r.right - 120, cy + 4), Alpha(th.fg, e));
            std::wstring dir = a.target.substr(0, a.target.find_last_of(L'\\'));
            DrawText(FileNameOf(dir), fSub_.Get(), D2D1::RectF(r.left + 52, cy + 2, r.right - 120, r.bottom - 2), Alpha(th.fgDim, e));
            DrawText(Ago(recent_[i].when), fTagR_.Get(), D2D1::RectF(r.right - 140, r.top, r.right - 14, r.bottom), Alpha(th.fgDim, e));
        }
        rt_->PopAxisAlignedClip();
        y = top;
        if (recent_.empty() && trackOff_) {
            // Windows is set not to remember files: say so, and one click opens the right setting
            D2D1_RECT_F r = D2D1::RectF(14, y, W - 14, y + kRow - 4);
            int me = (int)hits_.size();
            hits_.push_back({r, KTrackDocs, 0});
            brush_->SetColor(Hot(me) ? (me == SelHit() ? Alpha(th.accent, 0.24f) : th.hover) : Alpha(th.fg, 0.05f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
            float cy = (r.top + r.bottom) / 2;
            DrawText(TR(L"Windows husker ikke filene du åpner.", L"Windows isn't remembering the files you open."), fText_.Get(),
                     D2D1::RectF(r.left + 16, r.top + 2, r.right - 16, cy + 4), th.fg);
            DrawText(TR(L"Klikk for å slå på «Vis nylig åpnede elementer».", L"Click to turn on \u201cShow recently opened items\u201d."), fSub_.Get(),
                     D2D1::RectF(r.left + 16, cy + 2, r.right - 16, r.bottom - 2), th.fgDim);
        } else if (recent_.empty())
            DrawText(TR(L"Filene du åpner, havner her.", L"The files you open end up here."), fHint_.Get(), D2D1::RectF(26, y, W - 26, y + 30), th.fgDim);
    }

    // The small pill at the right of a section title that switches to / from "All apps"; returns its left edge
    float AllAppsPill(float W, float y, const wchar_t* text, Kind kind) {
        const Theme& th = g_app.theme;
        float tw = TextWidth(text, fPill_.Get()) + 22;
        D2D1_RECT_F p = D2D1::RectF(W - 22 - tw, y, W - 22, y + 22);
        int me = (int)hits_.size();
        hits_.push_back({p, kind, 0});
        brush_->SetColor(Alpha(Mix(blob_[0], th.fg, 0.2f), me == hover_ ? 0.45f : 0.26f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(p, 11, 11), brush_.Get());
        DrawText(text, fPill_.Get(), p, th.fg);
        return p.left;
    }

    void PaintAllApps(float W, float y, float bottom, bool back = true) {
        const Theme& th = g_app.theme;
        Section(TR(L"Alle apper", L"All apps"), y, W);
        if (back) AllAppsPill(W, y, TR(L"‹  Tilbake", L"‹  Back"), KAllBtn);
        y += 30;
        const float rowH = 40;
        int px = Px(26);
        float top = y;
        // one row per app, a letter above each new starting letter
        float cy = top - allScroll_;
        wchar_t letter = 0;
        rt_->PushAxisAlignedClip(D2D1::RectF(0, top - 4, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);   // the list scrolls under the title
        for (int i = 0; i < (int)all_.size(); ++i) {
            wchar_t c = (wchar_t)towupper(all_[i].name[0]);
            if (!iswalpha(c)) c = L'#';
            if (c != letter) {
                letter = c;
                if (cy + 24 > top - 30 && cy < bottom) {
                    wchar_t s[2] = {c, 0};
                    DrawText(s, fHead_.Get(), D2D1::RectF(28, cy + 2, W - 26, cy + 22), Alpha(Mix(blob_[0], th.fg, 0.35f), 1));
                }
                cy += 24;
            }
            if (cy + rowH > top - 4 && cy < bottom) {   // only rows you can see are drawn and clickable
                float e = (float)Ease((float)((NowMs() - resultsAt_ - std::min(i, 12) * 18) / 180));
                D2D1_RECT_F r = D2D1::RectF(14, cy, W - 14, cy + rowH - 4);
                int me = (int)hits_.size();
                hits_.push_back({D2D1::RectF(r.left, std::max(r.top, top - 4), r.right, std::min(r.bottom, bottom)), KAll, i});
                if (Hot(me)) {
                    brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.26f) : th.hover);
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
                }
                float my = (r.top + r.bottom) / 2;
                if (ID2D1Bitmap* b = Icon(all_[i], px)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(r.left + 12, my - 13, r.left + 38, my + 13), e);
                DrawText(all_[i].name, fText_.Get(), D2D1::RectF(r.left + 50, r.top, r.right - 14, r.bottom), Alpha(th.fg, e));
            }
            cy += rowH;
        }
        rt_->PopAxisAlignedClip();
        allMax_ = std::max(0.0f, (cy + allScroll_) - top - (bottom - top) + 10);
        if (allScroll_ > allMax_) allScroll_ = allMax_;
    }

    void PaintMoreFiles(float W, float y, float bottom) {
        const Theme& th = g_app.theme;
        Section(TR(L"Nylige filer", L"Recent files"), y, W);
        AllAppsPill(W, y, TR(L"‹  Tilbake", L"‹  Back"), KFilesBtn);
        y += 30;
        float top = y, cy = top - scroll_;
        int spx = Px(28);
        rt_->PushAxisAlignedClip(D2D1::RectF(0, top - 4, W, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < (int)moreFiles_.size(); ++i, cy += kRow) {
            if (cy + kRow <= top - 4 || cy >= bottom) continue;   // only rows you can see are drawn and clickable
            float e = (float)Ease((float)((NowMs() - resultsAt_ - std::min(i, 12) * 18) / 180));
            D2D1_RECT_F r = D2D1::RectF(14, cy, W - 14, cy + kRow - 4);
            int me = (int)hits_.size();
            hits_.push_back({D2D1::RectF(r.left, std::max(r.top, top - 4), r.right, std::min(r.bottom, bottom)), KMoreFile, i});
            if (Hot(me)) {
                brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.26f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
            }
            const AppEntry& a = moreFiles_[i].a;
            float my = (r.top + r.bottom) / 2;
            if (ID2D1Bitmap* b = Icon(a, spx)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(r.left + 12, my - 14, r.left + 40, my + 14), e);
            DrawText(a.name, fText_.Get(), D2D1::RectF(r.left + 52, r.top + 2, r.right - 120, my + 4), Alpha(th.fg, e));
            std::wstring dir = a.target.substr(0, a.target.find_last_of(L'\\'));
            DrawText(FileNameOf(dir), fSub_.Get(), D2D1::RectF(r.left + 52, my + 2, r.right - 120, r.bottom - 2), Alpha(th.fgDim, e));
            DrawText(Ago(moreFiles_[i].when), fTagR_.Get(), D2D1::RectF(r.right - 140, r.top, r.right - 14, r.bottom), Alpha(th.fgDim, e));
        }
        rt_->PopAxisAlignedClip();
        if (moreFiles_.empty())
            DrawText(TR(L"Ingen nylige filer.", L"No recent files."), fHint_.Get(), D2D1::RectF(26, top, W - 26, top + 30), th.fgDim);
        maxScroll_ = std::max(0.0f, moreFiles_.size() * kRow - (bottom - top) + 10);
        if (scroll_ > maxScroll_) scroll_ = maxScroll_;
    }

    void PaintResults(float W, float y) {
        const Theme& th = g_app.theme;
        D2D1_COLOR_F hi = Mix(blob_[0], th.fg, th.dark ? 0.35f : 0.0f);
        int px = Px(30);
        int n = 0;
        auto row = [&](const AppEntry& a, Kind k, int idx) {
            float e = (float)Ease((float)((NowMs() - resultsAt_ - n * 25) / 180));
            D2D1_RECT_F r = D2D1::RectF(14, y + (1 - e) * 8, W - 14, y + kRow - 4 + (1 - e) * 8);
            int me = (int)hits_.size();
            hits_.push_back({r, k, idx});
            if (Hot(me)) {
                brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.26f) : th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
            }
            float cy = (r.top + r.bottom) / 2;
            if (ID2D1Bitmap* b = Icon(a, px)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(r.left + 12, cy - 15, r.left + 42, cy + 15), e);
            std::wstring sub = a.file ? FileNameOf(a.target.substr(0, a.target.find_last_of(L'\\'))) : std::wstring();
            if (sub.empty()) DrawMatch(a.name, fText_.Get(), D2D1::RectF(r.left + 54, cy - 10, r.right - 110, cy + 12), Alpha(th.fg, e), hi);
            else {
                DrawMatch(a.name, fText_.Get(), D2D1::RectF(r.left + 54, r.top + 4, r.right - 110, cy + 4), Alpha(th.fg, e), hi);
                DrawText(sub, fSub_.Get(), D2D1::RectF(r.left + 54, cy + 2, r.right - 110, r.bottom - 2), Alpha(th.fgDim, e));
            }
            // a small coloured pill says what it is
            const wchar_t* tag = a.setting ? TR(L"Innstilling", L"Setting") : a.folder ? TR(L"Mappe", L"Folder") : a.file ? TR(L"Fil", L"File") : L"App";
            D2D1_COLOR_F pc = a.setting ? blob_[2] : a.file ? blob_[1] : blob_[0];
            float tw = TextWidth(tag, fPill_.Get()) + 18;
            D2D1_RECT_F pill = D2D1::RectF(r.right - 14 - tw, cy - 10, r.right - 14, cy + 10);
            brush_->SetColor(Alpha(pc, 0.30f * e));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(pill, 10, 10), brush_.Get());
            DrawText(tag, fPill_.Get(), pill, Alpha(th.fg, e));
            y += kRow;
            ++n;
        };
        if (!apps_.empty()) {
            Section(TR(L"Treff", L"Results"), y, W);
            y += 26;
            for (int i = 0; i < (int)apps_.size(); ++i) row(apps_[i], KApp, i);
            y += 6;
        }
        if (!files_.empty()) {
            Section(TR(L"Filer og mapper", L"Files and folders"), y, W);
            y += 26;
            for (int i = 0; i < (int)files_.size(); ++i) row(files_[i], KFile, i);
            y += 6;
        }
        // hand over to Windows
        D2D1_RECT_F r = D2D1::RectF(14, y, W - 14, y + kRow - 4);
        int me = (int)hits_.size();
        hits_.push_back({r, KWindows, 0});
        if (Hot(me)) {
            brush_->SetColor(me == SelHit() ? Alpha(th.accent, 0.26f) : th.hover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), brush_.Get());
        }
        DrawGlyph(0xE8B7, fGlyph_.Get(), D2D1::RectF(r.left + 12, r.top, r.left + 42, r.bottom), th.fgDim);
        DrawText(std::wstring(TR(L"Søk i Windows etter «", L"Search Windows for “")) + query_ + TR(L"»", L"”"), fText_.Get(),
                 D2D1::RectF(r.left + 54, r.top, r.right - 14, r.bottom), th.fg);
    }

    void PaintCalc(float W, float y) {
        const Theme& th = g_app.theme;
        Section(TR(L"Svar", L"Answer"), y, W);
        y += 30;
        D2D1_RECT_F r = D2D1::RectF(14, y, W - 14, y + 110);
        int me = (int)hits_.size();
        hits_.push_back({r, KCalc, 0});
        // a card in the colours of the hour
        D2D1_GRADIENT_STOP st[2] = {{0, Alpha(blob_[0], Hot(me) ? 0.45f : 0.32f)}, {1, Alpha(blob_[1], Hot(me) ? 0.40f : 0.26f)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lb;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, sc.Put())) &&
            SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
                                                     sc.Get(), lb.Put())))
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, 20, 20), lb.Get());
        std::wstring expr = query_.substr(1);
        DrawText(expr + L" =", fHint_.Get(), D2D1::RectF(r.left + 24, r.top + 12, r.right - 24, r.top + 34), th.fgDim);
        DrawText(calc_, fCalc_.Get(), D2D1::RectF(r.left + 22, r.top + 36, r.right - 24, r.bottom - 10), th.fg);
        bool copied = NowMs() - copiedAt_ < 1600;
        DrawText(copied ? TR(L"Kopiert ✓", L"Copied ✓") : TR(L"Enter kopierer", L"Enter copies"), fTagR_.Get(),
                 D2D1::RectF(r.left, r.top + 12, r.right - 22, r.top + 34), copied ? th.fg : th.fgDim);
    }

    void PaintBottom(float W, float H, float bottom) {
        const Theme& th = g_app.theme;
        brush_->SetColor(th.hairline);
        rt_->FillRectangle(D2D1::RectF(18, bottom, W - 18, bottom + 1), brush_.Get());
        // you: picture and name (opens your account settings)
        float cy = (bottom + H) / 2;
        D2D1_RECT_F ur = D2D1::RectF(14, cy - 20, 220, cy + 20);
        int me = (int)hits_.size();
        hits_.push_back({ur, KUser, 0});
        if (me == hover_) { brush_->SetColor(th.hover); rt_->FillRoundedRectangle(D2D1::RoundedRect(ur, 12, 12), brush_.Get()); }
        D2D1_ELLIPSE av = D2D1::Ellipse(D2D1::Point2F(ur.left + 22, cy), 15, 15);
        if (!avatar_ && avatarSrc_) rt_->CreateBitmapFromWicBitmap(avatarSrc_.Get(), nullptr, avatar_.Put());
        if (avatar_) {
            ComPtr<ID2D1BitmapBrush> bb;
            rt_->CreateBitmapBrush(avatar_.Get(), bb.Put());
            if (bb) {
                D2D1_SIZE_F bs = avatar_->GetSize();
                bb->SetTransform(D2D1::Matrix3x2F::Scale(30 / bs.width, 30 / bs.height) * D2D1::Matrix3x2F::Translation(av.point.x - 15, av.point.y - 15));
                rt_->FillEllipse(av, bb.Get());
            }
        } else {
            brush_->SetColor(Alpha(blob_[0], 0.6f));
            rt_->FillEllipse(av, brush_.Get());
            DrawGlyph(0xE77B, fGlyph_.Get(), D2D1::RectF(av.point.x - 15, cy - 15, av.point.x + 15, cy + 15), th.fg);
        }
        if (armed_ >= 0)
            DrawText(ArmedText(armed_), fName_.Get(), D2D1::RectF(ur.left + 46, ur.top, W - 220, ur.bottom), th.red);
        else if (hover_ >= 0 && hover_ < (int)hitsPrev_.size() && (hitsPrev_[hover_].kind == KLock || hitsPrev_[hover_].kind == KSignOut ||
                                                                   hitsPrev_[hover_].kind == KRestart || hitsPrev_[hover_].kind == KShutdown)) {
            Kind k = hitsPrev_[hover_].kind;
            const wchar_t* what = k == KLock ? TR(L"Lås", L"Lock") : k == KSignOut ? TR(L"Logg av", L"Sign out")
                                : k == KRestart ? TR(L"Start på nytt", L"Restart") : TR(L"Slå av", L"Shut down");
            DrawText(what, fName_.Get(), D2D1::RectF(ur.left + 46, ur.top, W - 220, ur.bottom), th.fgDim);
        } else
            DrawText(UserDisplayNameCached(), fName_.Get(), D2D1::RectF(ur.left + 46, ur.top, ur.right - 8, ur.bottom), th.fg);
        // lock, sign out, restart, shut down (each needs a second click)
        struct Btn { Kind k; wchar_t g; int arm; } btns[] = {{KLock, 0xE72E, 2}, {KSignOut, 0xF3B1, 3}, {KRestart, 0xE72C, 0}, {KShutdown, 0xE7E8, 1}};
        for (int i = 0; i < 4; ++i) {
            float s = 36, x = W - 20 - (4 - i) * (s + 8) + 8, top = cy - s / 2;
            D2D1_RECT_F r = D2D1::RectF(x, top, x + s, top + s);
            int m = (int)hits_.size();
            hits_.push_back({r, btns[i].k, 0});
            bool armed = armed_ == btns[i].arm;
            D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(x + s / 2, top + s / 2), s / 2, s / 2);
            brush_->SetColor(armed ? Alpha(th.red, 0.9f) : Alpha(th.fg, m == hover_ ? 0.18f : 0.08f));
            rt_->FillEllipse(e, brush_.Get());
            DrawGlyph(btns[i].g, fGlyph_.Get(), r, armed ? D2D1::ColorF(1, 1, 1) : th.fg);
        }
    }

    const std::wstring& UserDisplayNameCached() {
        if (fullName_.empty()) fullName_ = UserDisplayName();
        return fullName_;
    }

    // Mouse position in the card's own coordinates
    D2D1_POINT_2F ToCard(float x, float y) const { return {(x - ox_) / s_, (y - oy_) / s_ - 14 * (1 - anim_)}; }

    int HitAt(float x, float y) const {
        D2D1_POINT_2F c = ToCard(x, y);
        for (int i = (int)hitsPrev_.size() - 1; i >= 0; --i)
            if (In(hitsPrev_[i].r, c.x, c.y)) return i;
        return -1;
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
        switch (msg) {
        case WM_TIMER:
            if (wp == T_FILES) {
                KillTimer(hwnd_, T_FILES);
                if (!query_.empty()) FileSearch::Query(hwnd_, ticket_, query_, 4);
            } else if (wp == T_POWER) {
                KillTimer(hwnd_, T_POWER);
                armed_ = -1;
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wp == T_AURORA) {
                if (!closing_) InvalidateRect(hwnd_, nullptr, FALSE);   // let the colours drift
            }
            return 0;
        case FileSearch::WM_SKY_FILES: {
            std::unique_ptr<std::vector<AppEntry>> found(reinterpret_cast<std::vector<AppEntry>*>(lp));
            if ((unsigned)wp != ticket_ || query_.size() < 2) return 0;
            files_ = std::move(*found);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            int h = HitAt(x, y);
            if (h != hover_) { hover_ = h; InvalidateRect(hwnd_, nullptr, FALSE); }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            if (query_.empty() && calc_.empty()) {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd_, &p);
                D2D1_POINT_2F c = ToCard(p.x * 96.0f / dpi_, p.y * 96.0f / dpi_);
                float d = GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 120.0f;
                bool all = fullscreen_ ? In(allCol_, c.x, c.y) : allMode_;
                bool files = fullscreen_ ? filesMode_ && In(leftCol_, c.x, c.y) : filesMode_;
                bool recent = !all && !files && !allMode_ && !filesMode_ && In(recentCol_, c.x, c.y);
                if (all) allScroll_ = std::clamp(allScroll_ - d * 0.6f, 0.0f, allMax_);
                else if (files) scroll_ = std::clamp(scroll_ - d * 0.6f, 0.0f, maxScroll_);
                else if (recent) recentScroll_ = std::clamp(recentScroll_ - d * 0.4f, 0.0f, recentMax_);
                if (all || files || recent) { hover_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
            }
            return 0;
        case WM_LBUTTONUP: {
            int h = HitAt(x, y);
            if (h >= 0) Activate(hitsPrev_[h]);
            else if (armed_ >= 0) { armed_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); }
            else if (fullscreen_) Close();   // a click on empty space closes it, like SkyLauncher
            return 0;
        }
        case WM_RBUTTONUP: {
            int h = HitAt(x, y);
            if (h < 0) return 0;
            const AppEntry* e = EntryOf(hitsPrev_[h]);
            if (!e) return 0;
            AppEntry a = *e;
            POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &s);
            std::function<void(bool)> pin;
            if (!a.file) pin = [this, a](bool p) { SetMenuPinned(a, p); LoadPins(); };
            // "Now" and "Pick up …" items can be removed (they may come back later, see VibeUsage)
            std::function<void()> remove;
            Kind kind = hitsPrev_[h].kind;
            if (kind == KNow && tab_ == 0) remove = [this, a] { VibeUsage::HideApp(a); LoadNow(); };
            if (kind == KRecent) {
                ULONGLONG when = recent_[hitsPrev_[h].idx].when;
                remove = [this, a, when] { VibeUsage::HideFile(a.target, when); recent_ = RecentFiles(RecentCount()); };
            }
            if (kind == KMoreFile) {
                ULONGLONG when = moreFiles_[hitsPrev_[h].idx].when;
                remove = [this, a, when] {
                    VibeUsage::HideFile(a.target, when);
                    recent_ = RecentFiles(RecentCount());
                    moreFiles_ = RecentFiles(40);
                };
            }
            inMenu_ = true;
            bool launched = AppContextMenu(hwnd_, a, s, !a.file && IsMenuPinned(a), pin, remove, {}, true);
            inMenu_ = false;
            if (!hwnd_) return 0;
            if (launched || GetForegroundWindow() != hwnd_) Close();
            else InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // 1–8 start a suggestion, while nothing has been typed
            if (query_.empty() && !ctrl && wp >= '1' && wp <= '8' && (int)(wp - '1') < (int)Tiles().size()) {
                Launch(Tiles()[wp - '1']);   // closing drops the character that follows
                return 0;
            }
            int n = SelectableCount();
            switch (wp) {
            case VK_ESCAPE:
                if (armed_ >= 0) { armed_ = -1; KillTimer(hwnd_, T_POWER); }
                else if (!query_.empty()) { query_.clear(); OnQuery(); }
                else if (allMode_) ShowAllApps(false);
                else if (filesMode_) ShowMoreFiles(false);
                else Close();
                break;
            case VK_RETURN: {
                int h = SelHit();
                if (h < 0) for (int i = 0; i < (int)hitsPrev_.size() && h < 0; ++i) if (Selectable(hitsPrev_[i].kind)) h = i;
                if (h >= 0) Activate(hitsPrev_[h]);
                break;
            }
            case VK_DOWN: case VK_RIGHT: if (n) sel_ = sel_ < 0 ? 0 : (sel_ + 1) % n; break;
            case VK_UP: case VK_LEFT: if (n) sel_ = sel_ < 0 ? n - 1 : (sel_ + n - 1) % n; break;
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

private:
    std::vector<AppEntry> now_, apps_, files_;
    std::vector<RecentFile> recent_;
    std::vector<Hit> hits_, hitsPrev_;
    std::map<std::wstring, D2D1_COLOR_F> tileColour_;
    std::wstring calc_, first_, fullName_;
    D2D1_COLOR_F blob_[3]{};
    bool learned_ = false, tracking_ = false, fullscreen_ = false;
    float s_ = 1, ox_ = 0, oy_ = 0;   // full screen: card scale and position
    float iconScale_ = 1;             // the same scale, known before the first paint
    bool trackOff_ = false;           // Windows doesn't keep a list of recent files
    bool allMode_ = false;            // showing "All apps"
    bool filesMode_ = false;          // showing "Show more" recent files
    std::vector<RecentFile> moreFiles_;
    std::vector<AppEntry> all_, pins_;
    float scroll_ = 0, maxScroll_ = 0;      // "Show more" files
    float allScroll_ = 0, allMax_ = 0;      // all apps
    int tab_ = 0;                           // 0 Now, 1 Pinned
    double tabAt_ = 0;
    D2D1_RECT_F allCol_{}, leftCol_{};      // full screen: where the two columns are (for the mouse wheel)
    D2D1_RECT_F recentCol_{};               // the recent files list (card coordinates, for the mouse wheel)
    float recentScroll_ = 0, recentMax_ = 0;
    int Px(float dip) const { return (int)std::lround(dip * iconScale_ * dpi_ / 96.0f); }
    int sel_ = -1, hover_ = -1, armed_ = -1;
    unsigned ticket_ = 0;
    double resultsAt_ = 0, copiedAt_ = -1e9;
    UINT artGen_ = (UINT)-1;
    ComPtr<ID2D1Bitmap> art_, avatar_;
    ComPtr<IWICBitmapSource> avatarSrc_;
    int avatarPx_ = 0;
    ComPtr<IDWriteTextFormat> fClock_, fHello_, fDate_, fSearch_, fGlyph_, fHead_, fText_, fSub_, fTagR_, fPill_, fLabel_, fBadge_, fCalc_,
        fHint_, fRing_, fRingSub_, fMediaT_, fName_, fClockBig_, fClockMid_, fHelloBig_, fDateC_;
};


// ===========================================================================
// Windows key
// ===========================================================================
// The hook lives on its own little thread: the UI thread can be busy (reading the
// app list, loading icons) and Windows drops low-level hooks that answer slowly.
std::atomic<bool> g_hookWanted{false};
std::atomic<bool> g_winTabWanted{false};   // Mission Control takes Win+Tab
bool g_tabSwallowed = false;                // we took this Tab press: its repeats and its key-up are ours too
HHOOK g_kbHook = nullptr;
DWORD g_hookThreadId = 0;
HANDLE g_hookThread = nullptr;
DWORD g_lastVk = 0;
bool g_lastDown = false;
bool g_winCombo = false;
Launcher* g_launcher = nullptr;
Menu* g_menu = nullptr;
Spotlight* g_spotlight = nullptr;
Vibe* g_vibe = nullptr;
bool g_spotlightKey = false;   // our hotkey is registered
DWORD g_lastUsed = 0;
constexpr WORD kMaskKey = 0xE8;   // unassigned virtual key

// Is the window in front running as administrator while we aren't? Then Windows drops
// the keys we inject into it, and swallowing the real Win-up would leave Win "held down".
bool TokenElevated(HANDLE process) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD len = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

bool ForegroundOutranksUs() {
    static const bool selfElevated = TokenElevated(GetCurrentProcess());
    if (selfElevated) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return true;   // can't even look: assume it's above us, play safe
    bool elevated = TokenElevated(p);
    CloseHandle(p);
    return elevated;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        bool injected = (k->flags & LLKHF_INJECTED) != 0;
        if (win && !injected) {
            if (down) {
                // A fresh press (not auto-repeat): forget any combination from before
                if (!(g_lastDown && g_lastVk == k->vkCode)) g_winCombo = false;
            } else {
                bool solo = g_lastDown && g_lastVk == k->vkCode && !g_winCombo;
                if (solo && g_hookWanted.load() && !ForegroundOutranksUs()) {
                    // Swallow this Win-up and send our own: an unassigned key in between
                    // tells Windows it was a combination, so its Start menu stays shut.
                    INPUT in[3] = {};
                    in[0].type = in[1].type = in[2].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = kMaskKey;
                    in[1].ki.wVk = kMaskKey;
                    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[2].ki.wVk = (WORD)k->vkCode;
                    in[2].ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    if (SendInput(3, in, sizeof(INPUT)) == 3) {   // only swallow if ours got through
                        g_lastVk = k->vkCode;
                        g_lastDown = false;
                        PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 20, 0);
                        return 1;
                    }
                }
            }
            g_lastVk = k->vkCode;
            g_lastDown = down;
        } else if (!win && k->vkCode == VK_TAB && !injected && g_tabSwallowed) {
            if (!down) g_tabSwallowed = false;   // auto-repeat and the release of a Win+Tab we already handled
            return 1;
        } else if (!win && down && k->vkCode == VK_TAB && !injected && g_winTabWanted.load() &&
                   ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) && !ForegroundOutranksUs()) {
            // Win+Tab: ours. Swallow the Tab, and send an unassigned key so letting go of Win doesn't open Start.
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_KEYBOARD;
            in[0].ki.wVk = kMaskKey;
            in[1].ki.wVk = kMaskKey;
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            g_winCombo = true;
            g_tabSwallowed = true;
            g_lastVk = k->vkCode;
            g_lastDown = true;
            PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 23, 0);
            return 1;
        } else if (!win && down && k->vkCode != kMaskKey) {
            g_winCombo = true;   // any other key while Win is held (remappers included)
            g_lastVk = k->vkCode;
            g_lastDown = true;
        }
    }
    return CallNextHookEx(g_kbHook, code, wp, lp);
}

DWORD WINAPI HookThread(LPVOID) {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_app.hinst, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    g_kbHook = nullptr;
    return 0;
}

} // namespace

// ===========================================================================
namespace StartMenus {

bool Active() { return g_app.settings.startMenu != 0; }

void ApplySettings() {
    bool want = Active() && g_app.settings.startWinKey;
    g_hookWanted = want;
    g_winTabWanted = g_app.settings.missionEnabled && g_app.settings.missionWinTab;
    if ((want || g_winTabWanted) && !g_hookThread) g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &g_hookThreadId);
    if (g_menu) g_menu->Close(false);
    if (g_launcher) g_launcher->Close(false);
    if (g_vibe) g_vibe->Close(false);
    ApplySpotlightHotkey();
    // A few seconds after start: read the app list and load the icons in the background,
    // so the first open is already complete.
    if (Active())
        SetTimer(nullptr, 0, 3000, [](HWND, UINT, UINT_PTR id, DWORD) {
            KillTimer(nullptr, id);
            AllApps(0xFFFFFFFF);   // only reads it if we don't have it yet
            HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            if (g_app.settings.startMenu == 2) PrewarmIcons(LauncherIconPx(primary));
            else if (g_app.settings.startMenu == 1) PrewarmIcons(MulDiv(24, MonitorDpi(primary), 96));
            else if (g_app.settings.startMenu == 3) PrewarmIcons(MulDiv(48, MonitorDpi(primary), 96));
            g_lastUsed = GetTickCount();
        });
}

void Toggle(Kind k, HMONITOR mon) {
    if (k == Configured) {
        if (g_app.settings.startMenu == 0) { SendHotkey({VK_LWIN}); return; }
        int m = g_app.settings.startMenu;
        k = m == 2 ? Launcher : m == 3 ? Vibe : Menu;
    }
    if (!g_menu) g_menu = new ::Menu();
    if (!g_launcher) g_launcher = new ::Launcher();
    if (!g_vibe) g_vibe = new ::Vibe();
    g_lastUsed = GetTickCount();
    if (k == Vibe) {
        if (g_vibe->Open()) { g_vibe->Close(); return; }
        g_menu->Close(false);
        g_launcher->Close(false);
        g_vibe->Show(mon);
        return;
    }
    g_vibe->Close(false);
    if (k == Launcher) {
        if (g_launcher->Open()) { g_launcher->Close(); return; }
        g_menu->Close(false);
        g_launcher->Show(mon);
    } else {
        if (g_menu->Open()) { g_menu->Close(); return; }
        g_launcher->Close(false);
        g_menu->Show(mon);
    }
}

// ---- Spotlight ----
bool ApplySpotlightHotkey() {
    if (g_spotlightKey) { UnregisterHotKey(g_app.ctlWnd, kSpotlightHotkey); g_spotlightKey = false; }
    if (!g_app.settings.spotlightEnabled) {
        if (g_spotlight) g_spotlight->Close(false);
        return true;
    }
    g_spotlightKey = RegisterHotKey(g_app.ctlWnd, kSpotlightHotkey, g_app.settings.spotlightMods | MOD_NOREPEAT, g_app.settings.spotlightVk) != FALSE;
    return g_spotlightKey;
}

void SuspendSpotlightHotkey() {
    if (g_spotlightKey) { UnregisterHotKey(g_app.ctlWnd, kSpotlightHotkey); g_spotlightKey = false; }
}

void ToggleSpotlight() {
    if (!g_app.settings.spotlightEnabled) return;
    if (!g_spotlight) g_spotlight = new ::Spotlight();
    if (g_spotlight->Open()) { g_spotlight->Close(); return; }
    if (g_menu) g_menu->Close(false);
    if (g_launcher) g_launcher->Close(false);
    if (g_vibe) g_vibe->Close(false);
    g_lastUsed = GetTickCount();
    g_spotlight->Show();
}

void NoteForeground(HWND h, const std::wstring& exePath) {
    if (g_app.settings.startMenu == 3) VibeUsage::Note(h, exePath);   // only while VibeMenu is the chosen menu
}

void ForgetVibeUsage() { VibeUsage::Forget(); }
void SaveVibeUsage() { VibeUsage::Save(); }
void UnloadVibeUsage() { VibeUsage::Unload(); }

std::wstring HotkeyName(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_WIN) s += L"Win + ";
    if (vk == VK_SPACE) return s + TR(L"Mellomrom", L"Space");
    wchar_t name[64] = {};
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {   // keys that live on the extended part of the keyboard
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: lp |= 1 << 24; break;
    }
    if (GetKeyNameTextW(lp, name, 64) > 0) return s + name;
    return s + L"#" + std::to_wstring(vk);
}

void Purge() {
    bool open = (g_menu && g_menu->Open()) || (g_launcher && g_launcher->Open()) || (g_spotlight && g_spotlight->Open()) ||
                (g_vibe && g_vibe->Open());
    if (!open && g_lastUsed && GetTickCount() - g_lastUsed > 30 * 60000) {
        PurgeAppIcons();   // a few MB of icons: give them back when the menus aren't used
        g_lastUsed = 0;
    }
}

void IconsArrived() {
    if (g_spotlight) g_spotlight->IconsArrived();
    if (g_vibe) g_vibe->IconsArrived();
    if (g_menu) g_menu->IconsArrived();
    if (g_launcher) g_launcher->IconsArrived();
}

void Shutdown() {
    StopIconLoader();
    g_hookWanted = false;
    if (g_hookThread) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 1000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
    SuspendSpotlightHotkey();
    FileSearch::Shutdown();
    delete g_spotlight;
    g_spotlight = nullptr;
    VibeUsage::Save();
    delete g_vibe;
    g_vibe = nullptr;
    delete g_menu;
    g_menu = nullptr;
    delete g_launcher;
    g_launcher = nullptr;
}

} // namespace StartMenus
