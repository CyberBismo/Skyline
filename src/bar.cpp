// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include "bar.h"
#include "app.h"
#include "menus.h"
#include "dock.h"
#include "tiling.h"
#include "enhancedtiling.h"
#include "settings.h"
#include "icons.h"
#include "nowplaying.h"
#include "flyouts.h"
#include "garden.h"
#include "update.h"
#include <shellscalingapi.h>

namespace {

constexpr wchar_t kBarClass[] = L"SkylineBar";
constexpr wchar_t kTriggerClass[] = L"SkylineBarEdge";
constexpr wchar_t kPanelClass[] = L"SkylineBarPanel";
constexpr UINT_PTR T_SLIDE = 0x5101, T_AUTOHIDE = 0x5102, T_WORKAREA = 0x5103;
constexpr UINT WM_SKY_SHAPE = WM_APP + 20;

// Undocumented but stable since Windows 10: blur/acrylic behind a window.
struct ACCENTPOLICY { int AccentState; int AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };
using SetWCA = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);

void SetAccent(HWND h, int state, DWORD abgr) {
    static SetWCA fn = (SetWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    ACCENTPOLICY p{state, 2, abgr, 0};
    WINCOMPATTRDATA d{19 /*WCA_ACCENT_POLICY*/, &p, sizeof p};
    fn(h, &d);
}

std::wstring FormatClock() {
    const Settings& s = g_app.settings;
    wchar_t date[128] = {}, time[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, s.dateFormat.c_str(), date, 128, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, s.showSeconds ? 0 : TIME_NOSECONDS, nullptr, nullptr, time, 64);
    std::wstring out = date;
    if (!out.empty()) out += L"   ";
    out += time;
    return out;
}

// Segoe Fluent Icons on Windows 11, MDL2 Assets on Windows 10.
const wchar_t* IconFontName() {
    static std::wstring name;
    if (!name.empty()) return name.c_str();
    name = L"Segoe MDL2 Assets";
    ComPtr<IDWriteFontCollection> fc;
    if (SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE))) {
        UINT32 idx; BOOL exists = FALSE;
        if (SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) && exists) name = L"Segoe Fluent Icons";
    }
    return name.c_str();
}

const wchar_t* TextFontName() {
    static std::wstring name;
    if (!name.empty()) return name.c_str();
    name = L"Segoe UI";
    ComPtr<IDWriteFontCollection> fc;
    if (SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE))) {
        UINT32 idx; BOOL exists = FALSE;
        if (SUCCEEDED(fc->FindFamilyName(L"Segoe UI Variable Text", &idx, &exists)) && exists) name = L"Segoe UI Variable Text";
    }
    return name.c_str();
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

} // namespace

// ---------------------------------------------------------------------------
Bar::~Bar() {
    if (pwr1_) UnregisterPowerSettingNotification(pwr1_);
    if (pwr2_) UnregisterPowerSettingNotification(pwr2_);
    UnregisterAppBar();
    ReleaseWorkArea();
    for (HWND p : panels_) DestroyWindow(p);
    panels_.clear();
    if (tip_) DestroyWindow(tip_);
    if (trigger_) { SetWindowLongPtrW(trigger_, GWLP_USERDATA, 0); DestroyWindow(trigger_); }
    if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); }
}

// Corner radius of the panels (DIPs): Separated is rounder, almost a pill.
static float PanelRadius(float H) {
    (void)H;
    return 8.0f;   // same as Windows' own rounded corners on the glass panels
}

int Bar::MarginPx() const {
    return g_app.settings.barStyle == 0 ? 0 : MulDiv(6, dpi_, 96);
}

bool Bar::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBarClass;
        if (!RegisterClassExW(&wc)) return false;
        wc.lpfnWndProc = TriggerProc;
        wc.lpszClassName = kTriggerClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon_, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon_, MDT_EFFECTIVE_DPI, &dx, &dy);
    dpi_ = dx;
    int h = MulDiv(g_app.settings.heightDip, dpi_, 96);

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBarClass, L"Skyline",
                            WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top,
                            mi.rcMonitor.right - mi.rcMonitor.left, h,
                            nullptr, nullptr, g_app.hinst, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);

    BOOL exclude = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
    DWORD corner = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof corner);
    COLORREF none = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof none);
    ApplyBackdrop();

    tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                           hwnd_, nullptr, g_app.hinst, nullptr);
    if (tip_) {
        TOOLINFOW ti{sizeof ti};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        ti.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(tip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 480);
        SendMessageW(tip_, TTM_ACTIVATE, FALSE, 0);
    }

    pwr1_ = RegisterPowerSettingNotification(hwnd_, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
    pwr2_ = RegisterPowerSettingNotification(hwnd_, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (g_app.settings.barAutohide) {
        Reposition();
        SetTimer(hwnd_, T_AUTOHIDE, 1500, nullptr);   // show briefly at start, then tuck away
    } else {
        RegisterAppBar();
    }
    ApplyShape();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

// The glass itself. In the island styles the bar window is fully see-through and
// every island gets its own window with the glass, so nothing shows between them.
static void ApplyGlass(HWND h, bool clear) {
    const Theme& t = g_app.theme;
    BOOL dark = t.dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);

    MARGINS m{-1, -1, -1, -1};
    DWORD backdropNone = 1;
    if (clear) {
        SetAccent(h, 0, 0);
        DwmExtendFrameIntoClientArea(h, &m);
        DwmSetWindowAttribute(h, 38, &backdropNone, sizeof backdropNone);
        return;
    }
    ApplyGlassEffect(h, Part::Bar);   // see-through: the tint is painted in Paint()
}

void Bar::ApplyBackdrop() {
    ApplyGlass(hwnd_, g_app.settings.barStyle >= 2);
    for (HWND p : panels_) { ApplyGlass(p, false); InvalidateRect(p, nullptr, TRUE); }
}

LRESULT CALLBACK Bar::PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;   // the bar on top handles the mouse
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        // Black, which the extended frame turns into "show the glass".
        HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &ps.rcPaint, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Bar::SyncPanels() {
    bool want = g_app.settings.barStyle >= 2 && hwnd_ && IsWindowVisible(hwnd_);
    size_t n = g_app.settings.barStyle >= 2 ? islands_.size() : 0;
    while (panels_.size() > n) { DestroyWindow(panels_.back()); panels_.pop_back(); }
    if (n == 0) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    for (size_t i = 0; i < n; ++i) {
        if (i >= panels_.size()) {
            HWND p = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kPanelClass, L"", WS_POPUP,
                                     0, 0, 10, 10, nullptr, nullptr, g_app.hinst, nullptr);
            if (!p) return;
            BOOL exclude = TRUE;
            DwmSetWindowAttribute(p, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
            DWORD corner = 2;   // DWMWCP_ROUND: smooth, antialiased corners drawn by Windows
            DwmSetWindowAttribute(p, 33, &corner, sizeof corner);
            ApplyGlass(p, false);
            panels_.push_back(p);
        }
        RECT r = ToPixels(islands_[i]);
        SetWindowPos(panels_[i], hwnd_, wr.left + r.left, wr.top + r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | (want ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
}

void Bar::ThemeChanged() {
    ApplyBackdrop();
    DiscardResources();
    Invalidate();
}

// ---------------------------------------------------------------------------
// App bar: reserves the strip at the top so maximized windows stay below it.
void Bar::RegisterAppBar() {
    if (g_app.settings.barAutohide) { Reposition(); return; }   // auto-hide reserves no space
    APPBARDATA abd{sizeof abd};
    abd.hWnd = hwnd_;
    abd.uCallbackMessage = WM_SKY_APPBAR;
    appbar_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
    Reposition();
}

// The bar asks Windows to keep its strip free (an "app bar"), so maximized and tiled windows
// stay below it. Sometimes Windows doesn't (seen with the classic style while the taskbar is
// hidden): then title bars ended up under the bar. If the work area still includes our strip,
// we set it ourselves. Checked when the bar moves and when Windows changes the work area.
void Bar::FlushPending() {
    if (GameMode() || fullscreenHidden_) return;
    if (pendingRepos_) { pendingRepos_ = false; Reposition(); }
    EnsureWorkArea();
}

std::wstring Bar::DebugLine() const {
    wchar_t b[400];
    RECT wr{};
    if (hwnd_) GetWindowRect(hwnd_, &wr);
    swprintf(b, 400, L"  bar: hwnd=%p visible=%d rect=(%ld,%ld,%ld,%ld) home=(%ld,%ld,%ld,%ld) reserved=%d autohide=%d fullscreenHidden=%d forcedWork=%d primary=%d\r\n",
             (void*)hwnd_, hwnd_ ? IsWindowVisible(hwnd_) : 0, wr.left, wr.top, wr.right, wr.bottom, home_.left, home_.top, home_.right, home_.bottom,
             ReservedBottom(), (int)g_app.settings.barAutohide, (int)fullscreenHidden_, (int)forcedWork_, (int)primary_);
    return b;
}

void Bar::EnsureWorkArea() {
    if (hwnd_ && g_app.settings.barAutohide && primary_ && !staleChecked_ && !fullscreenHidden_ && !GameMode()) {
        // Auto-hide reserves nothing. A reservation left over from before (e.g. Skyline was ended without giving the strip back)
        // would keep maximized windows and the tiling away from the top, so give the top back once.
        staleChecked_ = true;
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(mon_, &mi) && mi.rcWork.top > mi.rcMonitor.top) {
            RECT w = mi.rcWork;
            w.top = mi.rcMonitor.top;
            SystemParametersInfoW(SPI_SETWORKAREA, 0, &w, SPIF_SENDCHANGE);
        }
    }
    if (!hwnd_ || g_app.settings.barAutohide) return;
    // Never during a game: setting the work area tells every window (the game too) that the screen changed
    if (fullscreenHidden_ || GameMode()) return;
    int need = ReservedBottom();
    MONITORINFO mi{sizeof mi};
    if (!need || !GetMonitorInfoW(mon_, &mi) || mi.rcWork.top >= need) return;
    RECT w = mi.rcWork;
    w.top = need;
    if (w.bottom > w.top && SystemParametersInfoW(SPI_SETWORKAREA, 0, &w, SPIF_SENDCHANGE)) forcedWork_ = true;
}

void Bar::ReleaseWorkArea() {
    if (!forcedWork_) return;
    forcedWork_ = false;
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(mon_, &mi) || mi.rcWork.top <= mi.rcMonitor.top) return;
    RECT w = mi.rcWork;
    w.top = mi.rcMonitor.top;   // give the strip back (the rest of the work area stays as Windows has it)
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &w, SPIF_SENDCHANGE);
}

void Bar::UnregisterAppBar() {
    if (!appbar_ || !hwnd_) return;
    APPBARDATA abd{sizeof abd};
    abd.hWnd = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    appbar_ = false;
}

void Bar::Reposition() {
    // SHAppBarMessage can make Explorer notify us again while we're still in here.
    // (Per bar: with two monitors, one bar's notification must not block the other.)
    if (repositioning_) return;
    repositioning_ = true;
    struct Reset { bool& b; ~Reset() { b = false; } } reset{repositioning_};

    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(mon_, &mi)) return;
    int h = MulDiv(g_app.settings.heightDip, dpi_, 96);
    int m = MarginPx();
    bool autohide = g_app.settings.barAutohide;

    // The strip we reserve at the top of the screen (none when auto-hiding)
    APPBARDATA abd{sizeof abd};
    abd.hWnd = hwnd_;
    abd.uEdge = ABE_TOP;
    abd.rc = {mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.top + h + 2 * m};
    if (autohide) {
        UnregisterAppBar();
    } else if (appbar_) {
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + h + 2 * m;
        SHAppBarMessage(ABM_SETPOS, &abd);
    }
    // Floating styles sit inside that strip with some air around them
    home_ = {abd.rc.left + m, abd.rc.top + m, abd.rc.right - m, abd.rc.top + m + h};
    int y = home_.top - (int)std::lround(slide_ * (h + m + 2));
    SetWindowPos(hwnd_, HWND_TOPMOST, home_.left, y, home_.right - home_.left, h, SWP_NOACTIVATE);
    if (primary_ && g_app.tray) g_app.tray->SetHostRect(home_);
    if (autohide) ReleaseWorkArea();
    else SetTimer(hwnd_, T_WORKAREA, 300, nullptr);   // once Windows has settled, check the strip really is kept free

    // Thin strip at the very top edge that brings an auto-hidden bar back
    if (autohide) {
        if (!trigger_) {
            trigger_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, kTriggerClass, L"",
                                       WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_app.hinst, this);
            if (trigger_) SetLayeredWindowAttributes(trigger_, 0, 1, LWA_ALPHA);
        }
        if (trigger_)
            // leave the very corners free for hot corners
            SetWindowPos(trigger_, HWND_TOPMOST, mi.rcMonitor.left + 4, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left - 8,
                         2, SWP_NOACTIVATE | (shown_ || fullscreenHidden_ ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
    } else if (trigger_) {
        SetWindowLongPtrW(trigger_, GWLP_USERDATA, 0);
        DestroyWindow(trigger_);
        trigger_ = nullptr;
    }
    Invalidate();
}

// Window corners and, for the "islands" style, the cut-out shape.
void Bar::ApplyShape() {
    int style = g_app.settings.barStyle;
    DWORD corner = style == 1 ? 2 /*ROUND*/ : 1 /*DONOTROUND*/;
    DwmSetWindowAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof corner);
    COLORREF border = style == 1 ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/ : 0xFFFFFFFE /*DWMWA_COLOR_NONE*/;
    DwmSetWindowAttribute(hwnd_, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof border);

    if (style < 2 || islands_.empty()) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        appliedIslands_.clear();
        SyncPanels();
        return;
    }
    int d = (int)std::lround(2 * PanelRadius((float)g_app.settings.heightDip) * dpi_ / 96.0f);   // corner diameter
    HRGN all = CreateRectRgn(0, 0, 0, 0);
    for (const auto& r : islands_) {
        RECT px = ToPixels(r);
        HRGN one = CreateRoundRectRgn(px.left, px.top, px.right + 1, px.bottom + 1, d, d);
        CombineRgn(all, all, one, RGN_OR);
        DeleteObject(one);
    }
    SetWindowRgn(hwnd_, all, TRUE);   // the window owns the region now (it decides where clicks land)
    appliedIslands_ = islands_;
    SyncPanels();
}

void Bar::SetShown(bool s) {
    if (!g_app.settings.barAutohide) s = true;
    if (s == shown_) return;
    shown_ = s;
    slideTarget_ = s ? 0.0f : 1.0f;
    if (s) {
        if (trigger_) ShowWindow(trigger_, SW_HIDE);
        if (!fullscreenHidden_) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // in front of the windows, not behind
            KeepOnTop();
        }
        SetTimer(hwnd_, T_AUTOHIDE, 1200, nullptr);   // in case the mouse never comes in
    } else {
        POINT none{-1, -1};
        SetHover(-1, none);
    }
    if (!sliding_) { sliding_ = true; SetTimer(hwnd_, T_SLIDE, 15, nullptr); }
}

void Bar::SlideStep() {
    float d = slideTarget_ - slide_;
    bool done = std::fabs(d) < 0.01f;
    slide_ = done ? slideTarget_ : slide_ + d * 0.32f;
    int h = home_.bottom - home_.top, m = MarginPx();
    int y = home_.top - (int)std::lround(slide_ * (h + m + 2));
    SetWindowPos(hwnd_, nullptr, home_.left, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    if (done) {
        KillTimer(hwnd_, T_SLIDE);
        sliding_ = false;
        if (!shown_) {
            ShowWindow(hwnd_, SW_HIDE);    // fully out of the way (and off a monitor above)
            if (trigger_ && !fullscreenHidden_) ShowWindow(trigger_, SW_SHOWNOACTIVATE);
        }
    }
}

LRESULT CALLBACK Bar::TriggerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) static_cast<Bar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    } else if (msg == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    } else if (msg == WM_MOUSEMOVE) {
        if (auto* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA)))
            if (!self->fullscreenHidden_) self->SetShown(true);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Bar::OnAppBarNotify(WPARAM wp, LPARAM lp) {
    switch (wp) {
    case ABN_POSCHANGED:
        if (fullscreenHidden_ || GameMode()) { pendingRepos_ = true; break; }   // not in the middle of a game
        Reposition();
        break;
    case ABN_FULLSCREENAPP:
        // Explorer's signal is unreliable with its taskbar hidden; use our own check.
        PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 5, 0);
        break;
    }
}

void Bar::SetFullscreen(bool fs) {
    if (fs == fullscreenHidden_) return;
    fullscreenHidden_ = fs;
    if (fs) {
        ShowWindow(hwnd_, SW_HIDE);
        for (HWND p : panels_) ShowWindow(p, SW_HIDE);   // the islands' glass goes too
        if (trigger_) ShowWindow(trigger_, SW_HIDE);   // never pop up over a game
    } else if (shown_) {
        if (pendingRepos_) { pendingRepos_ = false; Reposition(); }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SyncPanels();
        KeepOnTop();
    } else if (trigger_) {
        ShowWindow(trigger_, SW_SHOWNOACTIVATE);
    }
}

void Bar::KeepOnTop() {
    // The hover strip of an auto-hidden bar must stay above a maximized window too (the bar itself is hidden then)
    if (trigger_ && !fullscreenHidden_ && !shown_ && IsWindowVisible(trigger_))
        SetWindowPos(trigger_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!hwnd_ || fullscreenHidden_ || inMenu_ || !IsWindowVisible(hwnd_)) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    for (HWND p : panels_)   // the glass islands sit right under the bar
        SetWindowPos(p, hwnd_, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void Bar::StartMenuChanged(bool open) {
    if (open) {
        if (g_app.settings.barAutohide) SetShown(true);
        KeepOnTop();
    } else if (g_app.settings.barAutohide) {
        SetTimer(hwnd_, T_AUTOHIDE, 450, nullptr);
    }
}

// ---------------------------------------------------------------------------
bool Bar::CreateResources() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi_, (float)dpi_);
    auto hprops = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom));
    if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, hprops, rt_.Put()))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);   // needed on transparent surfaces
    rt_->CreateSolidColorBrush(g_app.theme.fg, brush_.Put());

    auto mk = [](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, DWRITE_FONT_WEIGHT w, float size, bool center) {
        g_gfx.dwrite->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                       size, L"", f.Put());
        if (!f) return;
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(fBold_, TextFontName(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, false);
    mk(fText_, TextFontName(), DWRITE_FONT_WEIGHT_NORMAL, 12.5f, false);
    mk(fIcon_, IconFontName(), DWRITE_FONT_WEIGHT_NORMAL, 15.0f, true);
    mk(fNum_, TextFontName(), DWRITE_FONT_WEIGHT_SEMI_BOLD, 11.0f, true);

    // Ellipsis for long window names
    if (fBold_) {
        ComPtr<IDWriteInlineObject> ell;
        g_gfx.dwrite->CreateEllipsisTrimmingSign(fBold_.Get(), ell.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fBold_->SetTrimming(&trim, ell.Get());
    }
    return true;
}

void Bar::DiscardResources() {
    appLayout_.Reset();
    clockLayout_.Reset();
    appBmp_.Reset();
    appBmpGen_ = 0;
    mediaBmp_.Reset();   // belongs to the old render target: drawing it on a new one fails the whole frame
    mediaGen_ = 0;
    trayBmps_.clear();
    fBold_.Reset(); fText_.Reset(); fIcon_.Reset();
    brush_.Reset();
    rt_.Reset();
}

// ---------------------------------------------------------------------------
// Layout, all in DIPs.  [app]                 [tray…] [status] [clock]
// "Title — Artist", at most 240 DIPs wide (ellipsis after that)
float Bar::MediaWidth(float H) {
    const auto& np = NowPlaying::Current();
    std::wstring t = np.title;
    if (!np.artist.empty()) t += L"  \u2014  " + np.artist;
    mediaLayout_.Reset();
    g_gfx.dwrite->CreateTextLayout(t.c_str(), (UINT32)t.size(), fText_.Get(), 240, H, mediaLayout_.Put());
    if (!mediaLayout_) return 0;
    ComPtr<IDWriteInlineObject> ell;
    g_gfx.dwrite->CreateEllipsisTrimmingSign(fText_.Get(), ell.Put());
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    mediaLayout_->SetTrimming(&trim, ell.Get());
    mediaLayout_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    mediaLayout_->GetMetrics(&m);
    return 8 + 18 + 7 + std::min(m.widthIncludingTrailingWhitespace, 240.0f) + 10;
}

void Bar::Layout(float W, float H) {
    items_.clear();
    const float padV = 3;

    // Right side, built from the right edge inwards
    float x = W - 6;

    std::wstring clock = FormatClock();
    clockLayout_.Reset();
    g_gfx.dwrite->CreateTextLayout(clock.c_str(), (UINT32)clock.size(), fText_.Get(), 400, H, clockLayout_.Put());
    DWRITE_TEXT_METRICS m{};
    if (clockLayout_) clockLayout_->GetMetrics(&m);
    float cw = m.widthIncludingTrailingWhitespace + 20;
    float centerL = 1e9f;   // left edge of the centre panel (Separated style)
    float rightLimit = 0;   // the right-hand group may not grow past this (Separated: the centre panel)
    // Now Playing (primary screen only), next to the clock or next to the tray
    bool media = primary_ && NowPlaying::InBar();
    float mw = media ? MediaWidth(H) : 0;
    bool mediaAtClock = media && NowPlaying::BarAtClock();
    bool mediaCenter = media && NowPlaying::BarCentered();
    bool mediaLeft = media && NowPlaying::BarAfterMenus();
    if (g_app.settings.barStyle == 3 && mediaCenter) { mediaAtClock = true; mediaCenter = false; }   // Separated: the middle is the clock's
    if (mediaCenter) {   // on its own in the middle; both sides stop short of it
        float cx = std::round(W / 2 - mw / 2);
        items_.push_back({Kind::Media, D2D1::RectF(cx, padV, cx + mw, H - padV)});
        centerL = cx - 10;
        rightLimit = cx + mw + 10;
    }
    if (g_app.settings.barStyle == 3) {
        float total = cw + (mediaAtClock ? mw + 4 : 0);
        float cx = std::round(W / 2 - total / 2);
        if (mediaAtClock) {
            items_.push_back({Kind::Media, D2D1::RectF(cx, padV, cx + mw, H - padV)});
            cx += mw + 4;
        }
        items_.push_back({Kind::Clock, D2D1::RectF(cx, padV, cx + cw, H - padV)});
        centerL = std::round(W / 2 - total / 2) - 10;
        rightLimit = cx + cw + 14;
    } else {
        items_.push_back({Kind::Clock, D2D1::RectF(x - cw, padV, x, H - padV)});
        x -= cw + 2;
        if (mediaAtClock) {
            items_.push_back({Kind::Media, D2D1::RectF(x - mw, padV, x, H - padV)});
            x -= mw + 6;
        }
    }

    const StatusState& st = g_app.status->State();
    float sw = 12 + 24 /*net*/ + 24 /*vol*/ + (st.hasBattery ? 32 : 0);
    items_.push_back({Kind::Status, D2D1::RectF(x - sw, padV, x, H - padV)});
    x -= sw + 6;

    if (g_app.settings.tilingButton) {
        items_.push_back({Kind::Tiling, D2D1::RectF(x - 28, padV, x, H - padV)});
        x -= 28 + 4;
    }
    if (EnhancedSpacesAvailable()) {
        spacesIcon_ = GetPrivateProfileIntW(L"Tiling", L"SpacesIcon", 1, g_app.settings.path.c_str()) == 2 ? 2 : 1;
        // Enhanced Tiling: the four workspaces as dots (the lit one is where you are)
        items_.push_back({Kind::Spaces, D2D1::RectF(x - 36, padV, x, H - padV)});
        x -= 36 + 4;
    }

    if (primary_ && g_app.tray && g_app.settings.trayEnabled) {
        auto& icons = g_app.tray->Icons();
        std::vector<TrayIcon*> vis;
        for (auto& t : icons) if (g_app.tray->Visible(t)) vis.push_back(&t);
        // User order first (drag & drop), unknown icons keep arrival order after them
        const auto& order = g_app.settings.trayOrder;
        auto rank = [&](const TrayIcon* t) {
            auto it = std::find(order.begin(), order.end(), t->exe);
            return it == order.end() ? (int)order.size() : (int)(it - order.begin());
        };
        std::stable_sort(vis.begin(), vis.end(), [&](const TrayIcon* a, const TrayIcon* b) { return rank(a) < rank(b); });
        // The icon being dragged is drawn under the cursor, so leave it out of the row
        if (trayDragging_ && trayDropHide_)
            vis.erase(std::remove_if(vis.begin(), vis.end(), [&](const TrayIcon* t) { return t->exe == dragKey_; }), vis.end());
        const float slot = 26;
        // Not enough room before the centre panel? Show the icons closest to the status area.
        size_t room = (size_t)std::max(0.0f, (x - rightLimit) / slot);
        if (vis.size() > room) vis.erase(vis.begin(), vis.begin() + (vis.size() - room));
        float left = x - slot * vis.size();
        for (size_t i = 0; i < vis.size(); ++i) {
            Item it{Kind::Tray, D2D1::RectF(left + slot * i, padV, left + slot * (i + 1), H - padV)};
            it.trayHwnd = vis[i]->hwnd;
            it.trayUid = vis[i]->uid;
            RECT px = ToPixels(it.rc);
            POINT o{0, 0};
            ClientToScreen(hwnd_, &o);
            vis[i]->screenRect = {px.left + o.x, px.top + o.y, px.right + o.x, px.bottom + o.y};
            items_.push_back(it);
        }
        // Tell the tray host where the icons actually are.
        POINT o{0, 0};
        ClientToScreen(hwnd_, &o);
        RECT area = ToPixels(D2D1::RectF(left, padV, x, H - padV));
        OffsetRect(&area, o.x, o.y);
        g_app.tray->SetTrayArea(area);
        x = left - 6;
    }
    if (media && !mediaAtClock && !mediaCenter && !mediaLeft && x - mw >= rightLimit) {
        items_.push_back({Kind::Media, D2D1::RectF(x - mw, padV, x, H - padV)});
        x -= mw + 6;
    }

    // Left: active app
    const ForegroundApp& fg = g_app.fg;
    if (!fg.name.empty()) {
        float maxText = std::max(60.0f, std::min(x, centerL) - 8 - 40 - (mediaLeft ? mw + 10 : 0));
        appLayout_.Reset();
        g_gfx.dwrite->CreateTextLayout(fg.name.c_str(), (UINT32)fg.name.size(), fBold_.Get(),
                                       std::min(maxText, 420.0f), H, appLayout_.Put());
        DWRITE_TEXT_METRICS am{};
        if (appLayout_) appLayout_->GetMetrics(&am);
        float w = 8 + 16 + 7 + std::min(am.width, std::min(maxText, 420.0f)) + 10;
        items_.insert(items_.begin(), Item{Kind::App, D2D1::RectF(6, padV, 6 + w, H - padV)});

        // App menus, like the macOS menu bar
        float mx = 6 + w + (g_app.settings.barStyle == 2 ? 12.0f : 2.0f);
        float limit = std::min(x, centerL) - 12 - (mediaLeft ? mw + 10 : 0);
        menuLayouts_.clear();
        const auto& menus = fg.menus.menus;
        for (int i = 0; i < (int)menus.size(); ++i) {
            ComPtr<IDWriteTextLayout> tl;
            g_gfx.dwrite->CreateTextLayout(menus[i].title.c_str(), (UINT32)menus[i].title.size(), fText_.Get(),
                                           300, H, tl.Put());
            if (!tl) break;
            DWRITE_TEXT_METRICS mm{};
            tl->GetMetrics(&mm);
            float mw = mm.widthIncludingTrailingWhitespace + 18;
            if (mx + mw > limit) break;
            Item it{Kind::Menu, D2D1::RectF(mx, padV, mx + mw, H - padV)};
            it.menu = i;
            items_.push_back(it);
            menuLayouts_.push_back(tl);
            mx += mw;
        }
        if (mediaLeft) {
            if (mx + 8 + mw <= std::min(x, centerL) - 12) items_.push_back({Kind::Media, D2D1::RectF(mx + 8, padV, mx + 8 + mw, H - padV)});
            else if (x - mw >= rightLimit) items_.push_back({Kind::Media, D2D1::RectF(x - mw, padV, x, H - padV)});   // no room: by the tray
        }
    } else if (mediaLeft && 6 + mw <= std::min(x, centerL) - 12) {   // no app in front: the player still sits at the far left
        items_.push_back({Kind::Media, D2D1::RectF(6, padV, 6 + mw, H - padV)});
    }

    // Islands style: [app] [menus] ........ [tray · status · clock]
    islands_.clear();
    if (g_app.settings.barStyle == 2) {
        float appR = -1, menuL = 1e9f, menuR = -1, rightL = 1e9f;
        D2D1_RECT_F center{};
        for (const auto& it : items_) {
            switch (it.kind) {
            case Kind::App: appR = it.rc.right; break;
            case Kind::Menu: menuL = std::min(menuL, it.rc.left); menuR = std::max(menuR, it.rc.right); break;
            case Kind::Media:
                if (NowPlaying::BarCentered()) { center = it.rc; break; }                                              // its own island
                if (NowPlaying::BarAfterMenus() && it.rc.left < W / 2) { menuL = std::min(menuL, it.rc.left); menuR = std::max(menuR, it.rc.right); break; }
                rightL = std::min(rightL, it.rc.left);
                break;
            default: rightL = std::min(rightL, it.rc.left); break;
            }
        }
        if (center.right > center.left) islands_.push_back(D2D1::RectF(center.left - 4, 0, center.right + 4, H));
        if (appR > 0) islands_.push_back(D2D1::RectF(0, 0, appR + 4, H));
        if (menuR > 0) islands_.push_back(D2D1::RectF(menuL - 4, 0, menuR + 4, H));
        if (rightL < 1e8f) islands_.push_back(D2D1::RectF(rightL - 6, 0, W, H));
    } else if (g_app.settings.barStyle == 3) {
        // Separated: [app + menus]      [clock]      [tray · status]
        float leftR = -1, rightL = 1e9f;
        D2D1_RECT_F clock{};
        for (const auto& it : items_) {
            switch (it.kind) {
            case Kind::App: case Kind::Menu: leftR = std::max(leftR, it.rc.right); break;
            case Kind::Clock: clock.right = it.rc.right; if (clock.left == 0 || it.rc.left < clock.left) clock.left = it.rc.left; break;
            case Kind::Media:
                if (NowPlaying::BarAtClock() || NowPlaying::BarCentered()) { if (clock.left == 0 || it.rc.left < clock.left) clock.left = it.rc.left; }
                else if (NowPlaying::BarAfterMenus() && it.rc.left < W / 2) leftR = std::max(leftR, it.rc.right);
                else rightL = std::min(rightL, it.rc.left);
                break;
            default: rightL = std::min(rightL, it.rc.left); break;
            }
        }
        float pad = PanelRadius(H) * 0.6f;
        if (leftR > 0) islands_.push_back(D2D1::RectF(0, 0, leftR + pad, H));
        islands_.push_back(D2D1::RectF(clock.left - pad, 0, clock.right + pad, H));
        if (rightL < 1e8f) islands_.push_back(D2D1::RectF(rightL - pad, 0, W, H));
    }
}

RECT Bar::ToPixels(const D2D1_RECT_F& r) const {
    return {(LONG)(r.left * dpi_ / 96.0f), (LONG)(r.top * dpi_ / 96.0f),
            (LONG)(r.right * dpi_ / 96.0f), (LONG)(r.bottom * dpi_ / 96.0f)};
}

// ---------------------------------------------------------------------------
void Bar::Paint() {
    if (!CreateResources()) return;
    const Theme& th = g_app.theme;
    D2D1_SIZE_F sz = rt_->GetSize();

    Layout(sz.width, sz.height);

    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));   // island styles: the glass is in the panel windows

    int style = g_app.settings.barStyle;
    if (D2D1_COLOR_F tint = GlassTint(Part::Bar); tint.a > 0) {
        // see-through: the tint, painted over the glass: the whole bar, or each island (its glass is the panel window below)
        brush_->SetColor(tint);
        if (style < 2) rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), brush_.Get());
        else for (const auto& r : islands_) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, PanelRadius(sz.height), PanelRadius(sz.height)), brush_.Get());
    }
    PaintVibe(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), Part::Bar);   // Vibe: the colours of the hour (the window's shape cuts out the islands)
    if (style < 2) PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, sz.width, sz.height), style == 1 ? PanelRadius(sz.height) : 0.0f, AuroraOn() ? 1 : 0);
    else for (const auto& r : islands_) PaintLiquid(rt_.Get(), r, PanelRadius(sz.height), AuroraOn() ? 1 : 0);
    if (style == 0) {
        // hairline along the bottom edge
        brush_->SetColor(th.hairline);
        rt_->FillRectangle(D2D1::RectF(0, sz.height - 1.0f * 96 / dpi_, sz.width, sz.height), brush_.Get());
    }
    if (style >= 2) {
        auto same = [](const std::vector<D2D1_RECT_F>& a, const std::vector<D2D1_RECT_F>& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::fabs(a[i].left - b[i].left) > 0.5f || std::fabs(a[i].right - b[i].right) > 0.5f) return false;
            return true;
        };
        if (!same(islands_, appliedIslands_)) PostMessageW(hwnd_, WM_SKY_SHAPE, 0, 0);
    }

    for (int i = 0; i < (int)items_.size(); ++i) {
        const Item& it = items_[i];
        if (i == hover_ || i == pressed_ || i == menuOpen_) {
            brush_->SetColor(i == menuOpen_ ? th.hover : i == pressed_ ? th.press : th.hover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(it.rc, 7, 7), brush_.Get());
        }
        float cy = (it.rc.top + it.rc.bottom) / 2;

        switch (it.kind) {
        case Kind::App: {
            const ForegroundApp& fg = g_app.fg;
            int px = (int)std::lround(16.0f * dpi_ / 96.0f);
            if (!appBmp_ || appBmpGen_ != fg.iconGen || appBmpPath_ != fg.path) {
                appBmp_.Reset();
                // Sharp icon from the exe itself, at exactly the size we draw it
                if (auto img = CachedAppImage(fg.path, px)) rt_->CreateBitmapFromWicBitmap(img.Get(), nullptr, appBmp_.Put());
                if (!appBmp_ && fg.icon)
                    if (auto img = ImageFromIcon(fg.icon, px)) rt_->CreateBitmapFromWicBitmap(img.Get(), nullptr, appBmp_.Put());
                appBmpGen_ = fg.iconGen;
                appBmpPath_ = fg.path;
            }
            float ix = it.rc.left + 8;
            if (appBmp_) DrawImageHQ(rt_.Get(), appBmp_.Get(), D2D1::RectF(ix, cy - 8, ix + 16, cy + 8));
            if (appLayout_) {
                brush_->SetColor(th.fg);
                rt_->DrawTextLayout(D2D1::Point2F(ix + 16 + 7, 0), appLayout_.Get(), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            break;
        }
        case Kind::Menu:
            if (it.menu >= 0 && it.menu < (int)menuLayouts_.size() && menuLayouts_[it.menu]) {
                brush_->SetColor(th.fg);
                rt_->DrawTextLayout(D2D1::Point2F(it.rc.left + 9, 0), menuLayouts_[it.menu].Get(), brush_.Get());
            }
            break;
        case Kind::Tray: {
            TrayIcon* t = TrayFor(it);
            if (!t) break;
            TrayBmp* cached = nullptr;
            for (auto& b : trayBmps_) if (b.hwnd == t->hwnd && b.uid == t->uid) { cached = &b; break; }
            if (!cached) { trayBmps_.push_back({t->hwnd, t->uid, 0, {}}); cached = &trayBmps_.back(); }
            if (!cached->bmp || cached->gen != t->iconGen) {
                cached->bmp.Reset();
                int px = (int)std::lround(16.0f * dpi_ / 96.0f);
                if (t->icon) {
                    if (auto img = ImageFromIcon(t->icon, px)) rt_->CreateBitmapFromWicBitmap(img.Get(), nullptr, cached->bmp.Put());
                    if (!cached->bmp) cached->bmp = BitmapFromIcon(rt_.Get(), t->icon);
                }
                if (!cached->bmp && t->pid) {   // nothing usable from the app: its own exe icon, so it never goes invisible
                    std::wstring exe = ProcessImagePath(t->pid);
                    if (!exe.empty())
                        if (auto img = CachedAppImage(exe, px)) rt_->CreateBitmapFromWicBitmap(img.Get(), nullptr, cached->bmp.Put());
                }
                cached->gen = t->iconGen;
            }
            float cx = (it.rc.left + it.rc.right) / 2;
            bool dragged = trayDragging_ && t->exe == dragKey_;
            if (dragged) {
                brush_->SetColor(th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(it.rc, 5, 5), brush_.Get());
            }
            if (cached->bmp)
                DrawImageHQ(rt_.Get(), cached->bmp.Get(), D2D1::RectF(cx - 8, cy - 8, cx + 8, cy + 8), dragged ? 0.55f : 1.0f);
            break;
        }
        case Kind::Status:
            DrawStatus(it.rc);
            break;
        case Kind::Tiling: {
            bool on = TilingRunning();
            float cx = (it.rc.left + it.rc.right) / 2, cyy = (it.rc.top + it.rc.bottom) / 2;
            // Four tiles: filled in accent when tiling is on, outlined when off
            float s = 5.2f, g = 1.6f;
            brush_->SetColor(on ? th.accent : th.fgDim);
            for (int yy = 0; yy < 2; ++yy)
                for (int xx = 0; xx < 2; ++xx) {
                    float l = cx - s - g / 2 + xx * (s + g), t = cyy - s - g / 2 + yy * (s + g);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(l, t, l + s, t + s), 1.2f, 1.2f);
                    if (on) rt_->FillRoundedRectangle(rr, brush_.Get());
                    else rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l + 0.5f, t + 0.5f, l + s - 0.5f, t + s - 0.5f), 1.2f, 1.2f), brush_.Get(), 1.0f);
                }
            break;
        }
        case Kind::Spaces: {
            float cx = std::floor((it.rc.left + it.rc.right) / 2), cyy = std::floor((it.rc.top + it.rc.bottom) / 2);
            if (spacesIcon_ == 2) {   // a little screen with the number of the workspace you are on
                D2D1_RECT_F r = D2D1::RectF(cx - 10.5f, cyy - 8, cx + 10.5f, cyy + 4.5f);
                brush_->SetColor(th.fgDim);
                rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, 2.5f, 2.5f), brush_.Get(), 1.4f);
                rt_->DrawLine(D2D1::Point2F(cx - 4, cyy + 8), D2D1::Point2F(cx + 4, cyy + 8), brush_.Get(), 1.4f);
                if (fNum_) {
                    std::wstring n = std::to_wstring(EnhancedCurrentSpace(mon_));
                    brush_->SetColor(th.accent);
                    rt_->DrawTextW(n.c_str(), (UINT32)n.size(), fNum_.Get(), D2D1::RectF(r.left, r.top, r.right, r.bottom + 1), brush_.Get());
                }
                break;
            }
            // three screens stacked behind each other; the front one carries the accent colour.
            // The two behind are drawn as open "L" shapes so the bar shows through (no filled boxes).
            const float w = 15, h = 10, st = 3.5f, rad = 2.5f;
            float x0 = cx - (w + 2 * st) / 2, y0 = cyy - (h + 2 * st) / 2;
            for (int i = 0; i < 3; ++i) {
                float l = std::floor(x0 + (2 - i) * st) + 0.5f, t = std::floor(y0 + i * st) + 0.5f, r = l + w, b = t + h;
                if (i == 2) {
                    brush_->SetColor(th.accent);
                    rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), brush_.Get(), 1.4f);
                    continue;
                }
                ComPtr<ID2D1PathGeometry> pg;
                ComPtr<ID2D1GeometrySink> sk;
                if (FAILED(g_gfx.d2d->CreatePathGeometry(pg.Put())) || FAILED(pg->Open(sk.Put()))) continue;
                auto arc = [&](float x, float y) { sk->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(rad, rad), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL)); };
                sk->BeginFigure(D2D1::Point2F(l, t + st), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddLine(D2D1::Point2F(l, t + rad));
                arc(l + rad, t);
                sk->AddLine(D2D1::Point2F(r - rad, t));
                arc(r, t + rad);
                sk->AddLine(D2D1::Point2F(r, b - rad));
                arc(r - rad, b);
                sk->AddLine(D2D1::Point2F(r - st, b));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                D2D1_COLOR_F c = th.fg;
                c.a = i == 0 ? 0.38f : 0.6f;
                brush_->SetColor(c);
                rt_->DrawGeometry(pg.Get(), brush_.Get(), 1.4f);
            }
            break;
        }
        case Kind::Clock:
            if (clockLayout_) {
                brush_->SetColor(th.fg);
                rt_->DrawTextLayout(D2D1::Point2F(it.rc.left + 10, 0), clockLayout_.Get(), brush_.Get());
            }
            break;
        case Kind::Media: {
            const auto& np = NowPlaying::Current();
            float cyy = (it.rc.top + it.rc.bottom) / 2;
            D2D1_RECT_F ar = D2D1::RectF(it.rc.left + 8, cyy - 9, it.rc.left + 26, cyy + 9);
            if (mediaGen_ != np.gen) { mediaBmp_.Reset(); mediaGen_ = np.gen; }
            if (!mediaBmp_ && np.art) rt_->CreateBitmapFromWicBitmap(np.art.Get(), nullptr, mediaBmp_.Put());
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            g_gfx.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ar, 4, 4), geo.Put());
            if (mediaBmp_ && geo) {
                ComPtr<ID2D1Layer> layer;
                rt_->CreateLayer(layer.Put());
                rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get()), layer.Get());
                rt_->DrawBitmap(mediaBmp_.Get(), ar, np.playing ? 1.0f : 0.55f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                rt_->PopLayer();
            } else {
                brush_->SetColor(th.hover);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(ar, 4, 4), brush_.Get());
            }
            if (mediaLayout_) {
                brush_->SetColor(np.playing ? th.fg : th.fgDim);
                rt_->PushAxisAlignedClip(D2D1::RectF(ar.right + 7, it.rc.top, it.rc.right - 8, it.rc.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
                rt_->DrawTextLayout(D2D1::Point2F(ar.right + 7, 0), mediaLayout_.Get(), brush_.Get());
                rt_->PopAxisAlignedClip();
            }
            break;
        }
        }
    }

    HRESULT hr = rt_->EndDraw();
    static int fails = 0;   // any failed frame: start fresh (not only device loss), but don't spin on a broken device
    if (FAILED(hr)) { DiscardResources(); if (++fails < 3) Invalidate(); } else fails = 0;

    // forget bitmaps of icons that are gone
    if (g_app.tray) {
        auto& icons = g_app.tray->Icons();
        trayBmps_.erase(std::remove_if(trayBmps_.begin(), trayBmps_.end(), [&](const TrayBmp& b) {
            for (auto& t : icons)
                if (t.hwnd == b.hwnd && t.uid == b.uid) return false;
            return true;
        }), trayBmps_.end());
    }
}

void Bar::DrawGlyph(wchar_t glyph, const D2D1_RECT_F& rc, const D2D1_COLOR_F& color) {
    brush_->SetColor(color);
    rt_->DrawTextW(&glyph, 1, fIcon_.Get(), rc, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Bar::DrawWifi(const D2D1_RECT_F& rc) {
    const Theme& th = g_app.theme;
    const StatusState& st = g_app.status->State();
    constexpr wchar_t kWifi = 0xE701;
    if (st.net == StatusState::Ethernet) { DrawGlyph(0xE839, rc, th.fg); return; }
    if (st.net == StatusState::Offline) { DrawGlyph(kWifi, rc, th.fgDim); return; }

    // Full arcs dimmed, then the signal strength lit from the bottom centre,
    // exactly like the Windows 11 quick settings icon.
    DrawGlyph(kWifi, rc, WithAlpha(th.fg, 0.35f));
    int q = st.wifiQuality;
    float frac = q >= 75 ? 1.0f : q >= 50 ? 0.72f : q >= 25 ? 0.48f : 0.26f;
    if (frac >= 1.0f) { DrawGlyph(kWifi, rc, th.fg); return; }

    float cx = (rc.left + rc.right) / 2;
    float cy = (rc.top + rc.bottom) / 2 + 6.0f;   // arcs share a centre just below the glyph
    ComPtr<ID2D1EllipseGeometry> g;
    g_gfx.d2d->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), 13.0f * frac, 13.0f * frac), g.Put());
    if (!g) return;
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get()), nullptr);
    DrawGlyph(kWifi, rc, th.fg);
    rt_->PopLayer();
}

void Bar::DrawBattery(const D2D1_RECT_F& rc) {
    const Theme& th = g_app.theme;
    const StatusState& st = g_app.status->State();
    float cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
    D2D1_RECT_F body = D2D1::RectF(cx - 10, cy - 5, cx + 8, cy + 5);

    brush_->SetColor(WithAlpha(th.fg, 0.8f));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.5f, 2.5f), brush_.Get(), 1.0f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx + 9, cy - 2, cx + 10.5f, cy + 2), 0.7f, 0.7f), brush_.Get());

    float inner = (body.right - body.left - 3) * std::clamp(st.batteryPct, 0, 100) / 100.0f;
    D2D1_COLOR_F fill = st.charging ? th.green : (st.batteryPct <= 20 ? th.red : th.fg);
    brush_->SetColor(fill);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + 1.5f, body.top + 1.5f,
                                                            body.left + 1.5f + std::max(inner, 1.5f), body.bottom - 1.5f),
                                                1.2f, 1.2f), brush_.Get());
    if (st.charging) {
        // tiny lightning bolt
        ComPtr<ID2D1PathGeometry> bolt;
        g_gfx.d2d->CreatePathGeometry(bolt.Put());
        ComPtr<ID2D1GeometrySink> s;
        if (bolt && SUCCEEDED(bolt->Open(s.Put()))) {
            float bx = cx - 1, by = cy;
            s->BeginFigure(D2D1::Point2F(bx + 1.5f, by - 6), D2D1_FIGURE_BEGIN_FILLED);
            D2D1_POINT_2F pts[] = {{bx - 2.5f, by + 0.8f}, {bx - 0.2f, by + 0.8f}, {bx - 1.5f, by + 6},
                                   {bx + 2.5f, by - 0.8f}, {bx + 0.2f, by - 0.8f}};
            s->AddLines(pts, 5);
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            brush_->SetColor(th.dark ? D2D1::ColorF(0.08f, 0.08f, 0.08f, 1) : D2D1::ColorF(1, 1, 1, 1));
            rt_->FillGeometry(bolt.Get(), brush_.Get());
        }
    }
}

void Bar::DrawStatus(const D2D1_RECT_F& rc) {
    const Theme& th = g_app.theme;
    const StatusState& st = g_app.status->State();
    float x = rc.left + 6;
    DrawWifi(D2D1::RectF(x, rc.top, x + 24, rc.bottom));
    x += 24;

    wchar_t vol = 0xE74F; // mute
    if (st.audioPresent && !st.muted) {
        vol = st.volume <= 0.001f ? 0xE992 : st.volume < 0.34f ? 0xE993 : st.volume < 0.67f ? 0xE994 : 0xE995;
    }
    DrawGlyph(vol, D2D1::RectF(x, rc.top, x + 24, rc.bottom), st.audioPresent ? th.fg : th.fgDim);
    x += 24;

    if (st.hasBattery) DrawBattery(D2D1::RectF(x, rc.top, x + 32, rc.bottom));
}

// ---------------------------------------------------------------------------
int Bar::HitTest(POINT client) const {
    D2D1_POINT_2F p = ToDip(client);
    for (int i = 0; i < (int)items_.size(); ++i) {
        const auto& r = items_[i].rc;
        if (p.x >= r.left && p.x < r.right && p.y >= 0 && p.y < r.bottom + 4) return i;
    }
    return -1;
}

TrayIcon* Bar::TrayFor(const Item& it) {
    if (it.kind != Kind::Tray || !g_app.tray) return nullptr;
    for (auto& t : g_app.tray->Icons())
        if (t.hwnd == it.trayHwnd && t.uid == it.trayUid) return &t;
    return nullptr;
}

void Bar::SetHover(int idx, POINT client) {
    if (idx == hover_) return;
    POINT scr = client;
    ClientToScreen(hwnd_, &scr);
    if (hover_ >= 0 && hover_ < (int)items_.size()) {
        if (TrayIcon* t = TrayFor(items_[hover_])) g_app.tray->HoverLeave(*t, scr);
    }
    hover_ = idx;
    if (hover_ >= 0) {
        if (TrayIcon* t = TrayFor(items_[hover_])) g_app.tray->HoverEnter(*t, scr);
    }
    UpdateTooltip();
    Invalidate();
}

std::wstring Bar::TooltipFor(const Item& it) {
    const StatusState& st = g_app.status->State();
    switch (it.kind) {
    case Kind::App: {
        wchar_t title[512] = {};
        if (g_app.fg.hwnd) GetWindowTextW(g_app.fg.hwnd, title, 512);
        return title;
    }
    case Kind::Menu:
        return L"";
    case Kind::Tray: {
        TrayIcon* t = TrayFor(it);
        if (!t) return L"";
        // Version-4 icons without NIF_SHOWTIP draw their own popup
        if (t->version > 3 && !t->showTip) return L"";
        return t->tip;
    }
    case Kind::Spaces:
        return std::wstring(TR(L"Arbeidsområde ", L"Workspace ")) + std::to_wstring(EnhancedCurrentSpace(mon_)) + TR(L" av 4\nKlikk for oversikt · Ctrl+Alt+1–4 bytter", L" of 4\nClick for the overview · Ctrl+Alt+1–4 switches");
    case Kind::Tiling: {
        bool on = TilingRunning();
        return std::wstring(on ? TR(L"Tiling: på", L"Tiling: on") : TR(L"Tiling: av", L"Tiling: off")) + TR(L"\nKlikk for å slå av/på · Høyreklikk for innstillinger", L"\nClick to toggle · Right-click for settings");
    }
    case Kind::Status: {
        std::wstring s;
        if (st.net == StatusState::Wifi) s = L"Wi-Fi: " + st.ssid + L" (" + std::to_wstring(st.wifiQuality) + L" %)";
        else if (st.net == StatusState::Ethernet) s = TR(L"Ethernet: tilkoblet", L"Ethernet: connected");
        else s = TR(L"Ikke tilkoblet", L"Not connected");
        if (st.audioPresent) s += st.muted ? TR(L"\nLyd: dempet", L"\nSound: muted") : TR(L"\nLyd: ", L"\nSound: ") + std::to_wstring((int)(st.volume * 100 + 0.5f)) + L" %";
        if (st.hasBattery) s += TR(L"\nBatteri: ", L"\nBattery: ") + std::to_wstring(st.batteryPct) + L" %" + (st.charging ? TR(L" (lader)", L" (charging)") : L"");
        s += TR(L"\nRull for å endre volum", L"\nScroll to change volume");
        return s;
    }
    case Kind::Media: {
        const auto& np = NowPlaying::Current();
        std::wstring t = np.title;
        if (!np.artist.empty()) t += L"\n" + np.artist;
        if (!np.app.empty()) t += L"\n" + np.app;
        return t + TR(L"\nKlikk for kontroller · Midtklikk for spill/pause", L"\nClick for controls · Middle-click to play/pause");
    }
    case Kind::Clock: {
        wchar_t d[128] = {};
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, d, 128, nullptr);
        return d;
    }
    }
    return L"";
}

void Bar::UpdateTooltip() {
    if (!tip_) return;
    std::wstring text = (hover_ >= 0 && hover_ < (int)items_.size()) ? TooltipFor(items_[hover_]) : L"";
    TOOLINFOW ti{sizeof ti};
    ti.hwnd = hwnd_;
    ti.uId = 1;
    if (text.empty()) {
        SendMessageW(tip_, TTM_ACTIVATE, FALSE, 0);
        tipText_.clear();
        return;
    }
    ti.rect = ToPixels(items_[hover_].rc);
    SendMessageW(tip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    if (text != tipText_) {
        tipText_ = text;
        ti.lpszText = tipText_.data();
        SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    SendMessageW(tip_, TTM_ACTIVATE, TRUE, 0);
}

// ---------------------------------------------------------------------------
enum MenuCmd { CmdMin = 1, CmdMax, CmdClose, CmdAutostart, CmdSettings, CmdMenus, CmdReload, CmdQuit, CmdShowAllTray,
               CmdFloatWin, CmdNeverTile, CmdUpdate, CmdUnhide = 500 };

// ---- Starting with Windows ----
// The usual way is the Run key, which Windows starts late on purpose (after the desktop, one by one, with a delay).
// "Fast startup" uses a Task Scheduler task at sign-in instead: no delay, normal priority.
namespace {
constexpr wchar_t kStartupTask[] = L"Skyline Fast Startup";

bool RunKeyExists() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Skyline",
                        RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetRunKey(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"Skyline", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"Skyline");
    }
    RegCloseKey(k);
}

int IniInt(const wchar_t* key) { return GetPrivateProfileIntW(L"General", key, 0, g_app.settings.path.c_str()); }
void IniSet(const wchar_t* key, int v) { WritePrivateProfileStringW(L"General", key, std::to_wstring(v).c_str(), g_app.settings.path.c_str()); }

// schtasks.exe, hidden; true if it says it went well
bool Schtasks(const std::wstring& args) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

std::wstring XmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'&') o += L"&amp;";
        else if (c == L'<') o += L"&lt;";
        else if (c == L'>') o += L"&gt;";
        else if (c == L'"') o += L"&quot;";
        else o += c;
    }
    return o;
}

bool CreateStartupTask() {
    wchar_t exe[MAX_PATH], user[256];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ULONG un = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &un)) {   // DOMAIN\\user (or PC\\user)
        DWORD n = 256;
        if (!GetUserNameW(user, &n)) return false;
    }
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    // Run at sign-in for this user, as this user (no admin), at normal priority, never stopped after 72 hours or on battery
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Author>Skyline</Author><Description>Starts Skyline as soon as you sign in.</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEsc(user) + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + XmlEsc(user) + L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>false</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + XmlEsc(exe) + L"\"</Command><WorkingDirectory>" + XmlEsc(dir) +
        L"</WorkingDirectory></Exec></Actions>\r\n</Task>\r\n";
    wchar_t tmp[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"sky", 0, file);
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const wchar_t bom = 0xFEFF;
    WriteFile(f, &bom, sizeof bom, &w, nullptr);
    WriteFile(f, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
    bool ok = Schtasks(L"/Create /TN \"" + std::wstring(kStartupTask) + L"\" /XML \"" + file + L"\" /F");
    DeleteFileW(file);
    return ok;
}

void DeleteStartupTask() { Schtasks(L"/Delete /TN \"" + std::wstring(kStartupTask) + L"\" /F"); }
} // namespace

bool AutostartEnabled() { return RunKeyExists() || IniInt(L"StartupTask") != 0; }
bool FastStartupEnabled() { return GetPrivateProfileIntW(L"General", L"FastStartup", 1, g_app.settings.path.c_str()) != 0; }   // on unless turned off

// Starting with Windows the old, slow way while fast startup is wanted (an update, or a restored backup): switch over once.
// Runs on its own thread at start-up (schtasks takes a moment).
void MigrateStartup(std::wstring ini) {   // its own copy of the path: the settings may be reloaded meanwhile
    auto get = [&](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"General", k, d, ini.c_str()); };
    auto set = [&](const wchar_t* k, int v) { WritePrivateProfileStringW(L"General", k, std::to_wstring(v).c_str(), ini.c_str()); };
    if (!get(L"FastStartup", 1) || !RunKeyExists() || get(L"StartupTask", 0)) return;
    if (CreateStartupTask()) { set(L"StartupTask", 1); SetRunKey(false); }
    else set(L"FastStartup", 0);   // Windows won't allow it here: stay with the usual way, and don't try again
}

void SetAutostart(bool on) {
    if (on) {
        if (FastStartupEnabled() && CreateStartupTask()) { IniSet(L"StartupTask", 1); SetRunKey(false); }
        else { SetRunKey(true); if (IniInt(L"StartupTask")) { DeleteStartupTask(); IniSet(L"StartupTask", 0); } }
    } else {
        SetRunKey(false);
        if (IniInt(L"StartupTask")) { DeleteStartupTask(); IniSet(L"StartupTask", 0); }
    }
}

bool SetFastStartup(bool on) {
    bool autostart = AutostartEnabled();
    IniSet(L"FastStartup", on ? 1 : 0);
    if (!autostart) return true;   // takes effect once "Start with Windows" is on
    if (on) {
        if (!CreateStartupTask()) { IniSet(L"FastStartup", 0); return false; }   // keep the usual way
        IniSet(L"StartupTask", 1);
        SetRunKey(false);
    } else {
        if (IniInt(L"StartupTask")) { DeleteStartupTask(); IniSet(L"StartupTask", 0); }
        SetRunKey(true);
    }
    return true;
}

void Bar::ShowAppMenu(const Item& it) {
    HWND target = g_app.fg.hwnd;
    bool valid = target && IsWindow(target);
    HMENU m = CreatePopupMenu();
    UINT dis = valid ? 0 : MF_GRAYED;
    AppendMenuW(m, MF_STRING | dis, CmdMin, TR(L"Minimer", L"Minimize"));
    AppendMenuW(m, MF_STRING | dis, CmdMax, valid && IsZoomed(target) ? TR(L"Gjenopprett", L"Restore") : TR(L"Maksimer", L"Maximize"));
    AppendMenuW(m, MF_STRING | dis, CmdClose, TR(L"Lukk vindu", L"Close Window"));
    bool tiling = TilingRunning();
    std::wstring exe = g_app.fg.exe;
    bool never = std::find(g_app.settings.tilingFloat.begin(), g_app.settings.tilingFloat.end(), exe) != g_app.settings.tilingFloat.end();
    if (tiling && valid) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (TilingIsFloating(target) ? MF_CHECKED : 0), CmdFloatWin, TR(L"Flytende vindu (ikke tile)", L"Floating window (don't tile)"));
        std::wstring label = TR(L"Aldri tile ", L"Never tile ") + g_app.fg.name;
        AppendMenuW(m, MF_STRING | (never ? MF_CHECKED : 0), CmdNeverTile, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (Update::GetState() == Update::State::Available) {
        std::wstring u = TR(L"Oppdater til Skyline ", L"Update to Skyline ") + Update::AvailableVersion() + L"…";
        AppendMenuW(m, MF_STRING, CmdUpdate, u.c_str());
    }
    AppendMenuW(m, MF_STRING, CmdSettings, TR(L"Innstillinger…", L"Settings…"));
    const auto hiddenList = g_app.settings.hiddenTray;
    if (!hiddenList.empty()) {
        HMENU sub = CreatePopupMenu();
        for (size_t i = 0; i < hiddenList.size() && i < 100; ++i)
            AppendMenuW(sub, MF_STRING, CmdUnhide + i, hiddenList[i].c_str());
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING, CmdShowAllTray, TR(L"Vis alle", L"Show all"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, TR(L"Skjulte tray-ikoner", L"Hidden tray icons"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CmdQuit, TR(L"Avslutt Skyline", L"Quit Skyline"));

    RECT px = ToPixels(it.rc);
    POINT pt{px.left, px.bottom + 2};
    ClientToScreen(hwnd_, &pt);
    SetForegroundWindow(hwnd_);
    inMenu_ = true;
    MenuGuard guard;
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, hwnd_, nullptr);
    inMenu_ = false;
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd >= CmdUnhide && cmd < CmdUnhide + (int)hiddenList.size()) {
        UnhideTray(hiddenList[cmd - CmdUnhide]);
        return;
    }
    switch (cmd) {
    case CmdMin: if (valid) PostMessageW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case CmdMax: if (valid) PostMessageW(target, WM_SYSCOMMAND, IsZoomed(target) ? SC_RESTORE : SC_MAXIMIZE, 0); break;
    case CmdClose: if (valid) PostMessageW(target, WM_SYSCOMMAND, SC_CLOSE, 0); break;
    case CmdAutostart: SetAutostart(!AutostartEnabled()); break;
    case CmdSettings: ShowSettings(SettingsPage::General); return;
    case CmdUpdate: Update::Install(); return;
    case CmdFloatWin: if (valid) TilingToggleFloat(target); break;
    case CmdNeverTile: {
        auto& f = g_app.settings.tilingFloat;
        if (never) f.erase(std::remove(f.begin(), f.end(), exe), f.end());
        else if (!exe.empty()) f.push_back(exe);
        IniWrite(L"Tiling", L"Float", JoinList(f, L", "));
        TilingSchedule(10);
        SettingsChanged();
        break;
    }
    case CmdMenus: {
        std::wstring p = ExeDir() + L"\\menus.txt";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case CmdReload: PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 2, 0); break;
    case CmdQuit: g_app.Quit(); return;
    case CmdShowAllTray:
        UnhideTray(L"");
        break;
    default: break;
    }
    // Hand focus back to the app the menu was about.
    if (cmd != CmdMin && cmd != CmdClose && valid && IsWindow(target)) SetForegroundWindow(target);
}

void Bar::OpenMenus(int itemIdx) {
    if (itemIdx < 0 || itemIdx >= (int)items_.size()) return;
    int menu = items_[itemIdx].menu;
    HWND target = g_app.fg.hwnd;
    AppMenus menus = g_app.fg.menus;   // copy: stays valid even if focus changes meanwhile
    auto hit = [this](POINT screen) {
        POINT c = screen;
        ScreenToClient(hwnd_, &c);
        int i = HitTest(c);
        return (i >= 0 && items_[i].kind == Kind::Menu) ? items_[i].menu : -1;
    };
    while (menu >= 0) {
        int idx = -1;
        for (int i = 0; i < (int)items_.size(); ++i)
            if (items_[i].kind == Kind::Menu && items_[i].menu == menu) { idx = i; break; }
        if (idx < 0) break;
        menuOpen_ = idx;
        Invalidate();
        UpdateWindow(hwnd_);
        RECT px = ToPixels(items_[idx].rc);
        POINT pt{px.left, px.bottom + 3};
        ClientToScreen(hwnd_, &pt);
        inMenu_ = true;
        { MenuGuard guard; menu = TrackTopMenu(menus, menu, pt, hwnd_, target, hit); }
        inMenu_ = false;
    }
    menuOpen_ = -1;
    pressed_ = -1;
    Invalidate();
}

bool Bar::TrayDragMove(POINT client) {
    if (trayDown_ < 0) return false;
    if (!trayDragging_) {
        if (abs(client.x - trayDownPt_.x) < 5 && abs(client.y - trayDownPt_.y) < 5) return true;
        trayDragging_ = true;
        SetCapture(hwnd_);
        POINT none{-1, -1};
        SetHover(-1, none);
    }
    D2D1_POINT_2F p = ToDip(client);
    dragX_ = p.x;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    // Dragged well below the bar: it will be hidden on drop
    trayDropHide_ = client.y > rc.bottom + MulDiv(24, dpi_, 96);

    if (!trayDropHide_) {
        // Current visual order of visible tray icons
        std::vector<std::wstring> row;
        std::vector<float> centers;
        for (auto& it : items_) {
            if (it.kind != Kind::Tray) continue;
            if (TrayIcon* t = TrayFor(it)) {
                if (std::find(row.begin(), row.end(), t->exe) == row.end()) {
                    row.push_back(t->exe);
                    centers.push_back((it.rc.left + it.rc.right) / 2);
                }
            }
        }
        auto from = std::find(row.begin(), row.end(), dragKey_);
        if (from != row.end() && row.size() > 1) {
            int fi = (int)(from - row.begin());
            int to = 0;
            for (int i = 0; i < (int)centers.size(); ++i) if (p.x > centers[i]) to = i;
            if (p.x < centers.front()) to = 0;
            if (to != fi) {
                row.erase(row.begin() + fi);
                row.insert(row.begin() + to, dragKey_);
                // Keep entries for apps that aren't running right now
                auto& order = g_app.settings.trayOrder;
                std::vector<std::wstring> merged = row;
                for (auto& o : order) if (std::find(merged.begin(), merged.end(), o) == merged.end()) merged.push_back(o);
                order = merged;
            }
        }
    }
    Invalidate();
    return true;
}

void Bar::TrayDragEnd(POINT client) {
    bool dragged = trayDragging_;
    int idx = trayDown_;
    UINT downMsg = trayDownMsg_;
    trayDown_ = -1;
    trayDragging_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();

    if (dragged) {
        if (trayDropHide_ && !dragKey_.empty()) {
            auto& h = g_app.settings.hiddenTray;
            if (std::find(h.begin(), h.end(), dragKey_) == h.end()) h.push_back(dragKey_);
        }
        trayDropHide_ = false;
        SaveTraySettings();
        SettingsChanged();
        Invalidate();
        return;
    }
    // A plain click: deliver it now (down was held back in case it became a drag)
    // (Matched by window + id, not position: icons may have come or gone meanwhile.)
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!PtInRect(&rc, client)) return;   // let go somewhere else: no click
    if (idx >= 0) {
        Item key{Kind::Tray, {}, trayDownHwnd_, trayDownUid_};
        if (TrayIcon* t = TrayFor(key)) {
            POINT scr = client;
            ClientToScreen(hwnd_, &scr);
            g_app.tray->MouseButton(*t, downMsg, scr);
            g_app.tray->MouseButton(*t, WM_LBUTTONUP, scr);
        }
    }
}

void Bar::OnButton(UINT msg, POINT client) {
    int idx = HitTest(client);
    POINT scr = client;
    ClientToScreen(hwnd_, &scr);

    bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK;
    if (down) { pressed_ = idx; Invalidate(); }
    // Classic style: double-click an empty spot = maximize / restore the app in front (like a Mac title bar)
    if (idx < 0 && msg == WM_LBUTTONDBLCLK && g_app.settings.barDoubleClick && g_app.settings.barStyle == 0) {
        HWND f = g_app.fg.hwnd;
        if (f && IsWindow(f) && !g_app.IsOurWindow(f) && (GetWindowLongW(f, GWL_STYLE) & WS_MAXIMIZEBOX))
            PostMessageW(f, WM_SYSCOMMAND, IsZoomed(f) ? SC_RESTORE : SC_MAXIMIZE, 0);
    }
    if (idx < 0) { if (!down) { pressed_ = -1; Invalidate(); } return; }

    Item it = items_[idx];
    if (it.kind == Kind::Menu) {
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) OpenMenus(idx);
        pressed_ = -1;
        return;
    }
    if (it.kind == Kind::Tray) {
        TrayIcon* t = TrayFor(it);
        if (t && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
            trayDown_ = idx;
            trayDownHwnd_ = it.trayHwnd;
            trayDownUid_ = it.trayUid;
            trayDownMsg_ = msg;
            trayDownPt_ = client;
            SetCapture(hwnd_);   // so letting go outside the bar is seen too (no half-started drag left behind)
            dragKey_ = t->exe;
            pressed_ = -1;
            return;
        }
        if (t && msg != WM_LBUTTONUP) g_app.tray->MouseButton(*t, msg, scr);
    } else if (!down && pressed_ == idx) {
        switch (it.kind) {
        case Kind::App:
            if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) { pressed_ = -1; Invalidate(); ShowAppMenu(it); }
            break;
        case Kind::Status:
            if (msg == WM_LBUTTONUP && Flyouts::Enabled()) {
                // Native Flyouts: the network icon opens ours for network, the volume icon ours for sound;
                // the battery still opens Windows' Quick Settings
                D2D1_POINT_2F p = ToDip(client);
                float nx = it.rc.left + 6;
                int part = p.x < nx + 24 ? 0 : p.x < nx + 48 ? 1 : 2;
                if (part == 2) SendHotkey({VK_LWIN, 'A'});
                else {
                    D2D1_RECT_F ic = D2D1::RectF(nx + part * 24, it.rc.top, nx + part * 24 + 24, it.rc.bottom);
                    RECT r = ToPixels(ic);
                    RECT wr;
                    GetWindowRect(hwnd_, &wr);
                    OffsetRect(&r, wr.left, wr.top);
                    r.bottom = std::max(r.bottom, wr.top + (LONG)(it.rc.bottom * dpi_ / 96.0f));
                    Flyouts::Toggle(part == 0 ? Flyouts::Network : Flyouts::Sound, r, dpi_);
                }
            } else if (msg == WM_LBUTTONUP) SendHotkey({VK_LWIN, 'A'});   // quick settings
            else if (msg == WM_MBUTTONUP) { g_app.status->ToggleMute(); g_app.InvalidateAll(); }
            else if (msg == WM_RBUTTONUP) ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Kind::Spaces:
            if (msg == WM_LBUTTONUP) EnhancedSpacesToggle(mon_);
            else if (msg == WM_RBUTTONUP) ShowSettings(SettingsPage::Tiling);
            break;
        case Kind::Tiling:
            if (msg == WM_LBUTTONUP) {
                g_app.settings.tilingEnabled = !g_app.settings.tilingEnabled;
                IniWrite(L"Tiling", L"Enabled", g_app.settings.tilingEnabled ? 1 : 0);
                EnhancedApply();
                g_app.InvalidateAll();
                UpdateTooltip();
                SettingsChanged();
            } else if (msg == WM_RBUTTONUP) {
                ShowSettings(SettingsPage::Tiling);
            }
            break;
        case Kind::Media:
            if (msg == WM_LBUTTONUP) {
                RECT r = ToPixels(it.rc);
                int align = 0;
                // Islands / Separated: the card lines up with the box the player sits in, not beside it
                if (g_app.settings.barStyle >= 2) {
                    RECT cr;
                    GetClientRect(hwnd_, &cr);
                    float W = cr.right * 96.0f / dpi_;
                    for (const auto& isl : islands_) {
                        if (it.rc.left < isl.left - 1 || it.rc.right > isl.right + 1) continue;
                        r = ToPixels(isl);
                        float c = (isl.left + isl.right) / 2;
                        align = c < W / 3 ? -1 : c > W * 2 / 3 ? 1 : 0;
                        break;
                    }
                }
                POINT o{0, 0};
                ClientToScreen(hwnd_, &o);
                OffsetRect(&r, o.x, o.y);
                NowPlaying::ShowCard(r, false, dpi_, align);
            } else if (msg == WM_MBUTTONUP) {
                NowPlaying::TogglePlay();
            }
            break;
        case Kind::Clock:
            if (msg == WM_LBUTTONUP && g_app.settings.clockCalendar) {   // Skyline's own calendar
                RECT r = ToPixels(it.rc);
                RECT wr;
                GetWindowRect(hwnd_, &wr);
                OffsetRect(&r, wr.left, wr.top);
                Flyouts::Toggle(Flyouts::Calendar, r, dpi_);
            } else if (msg == WM_LBUTTONUP) SendHotkey({VK_LWIN, 'N'});          // calendar + notifications
            else if (msg == WM_RBUTTONUP) ShellExecuteW(nullptr, L"open", L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default: break;
        }
    }
    if (!down) { pressed_ = -1; Invalidate(); }
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK Bar::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Bar* self = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Bar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT Bar::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_app.msgTaskbarCreated && msg != 0) {
        // Explorer restarted: its app bar list is gone, register again.
        if (GetTickCount() > g_app.ignoreTaskbarCreatedUntil) {
            UnregisterAppBar();
            RegisterAppBar();
            if (primary_) for (auto& d : g_app.docks) d->ExplorerRestarted();
            if (primary_) Garden::ExplorerRestarted();
            // Explorer just asked every app to re-add its icons to *its* tray.
            // Once that settles, ask them again so they come to ours.
            if (primary_ && g_app.tray) g_app.tray->ScheduleReannounce(2500);
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
    case WM_SKY_SHAPE:
        ApplyShape();
        return 0;
    case WM_TIMER:
        if (wp == T_SLIDE) {
            SlideStep();
        } else if (wp == T_WORKAREA) {
            KillTimer(hwnd_, T_WORKAREA);
            EnsureWorkArea();
        } else if (wp == T_AUTOHIDE) {
            KillTimer(hwnd_, T_AUTOHIDE);
            if (!g_app.settings.barAutohide) return 0;
            POINT c;
            GetCursorPos(&c);
            RECT wr;
            GetWindowRect(hwnd_, &wr);
            HWND menu = FindWindowW(L"#32768", nullptr);   // any open menu (ours or a tray app's)
            // The bar plus the gap above it up to the screen edge counts as "on the bar"
            // (floating styles have a gap; hovering it used to make the bar bounce).
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(mon_, &mi);
            RECT zone = home_;
            zone.top = mi.rcMonitor.top;
            zone.left = mi.rcMonitor.left;
            zone.right = mi.rcMonitor.right;
            bool keep = inMenu_ || trayDragging_ || StartMenuOpen() || PtInRect(&wr, c) || PtInRect(&zone, c) || (menu && IsWindowVisible(menu));
            if (keep) SetTimer(hwnd_, T_AUTOHIDE, 400, nullptr);
            else SetShown(false);
        }
        return 0;
    case WM_DPICHANGED:
        dpi_ = HIWORD(wp);
        DiscardResources();
        appliedIslands_.clear();   // the islands' window shape is in pixels: make it again at the new scale
        Reposition();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (TrayDragMove(p)) return 0;
        SetHover(HitTest(p), p);
        if (hover_ >= 0 && hover_ < (int)items_.size()) {
            if (TrayIcon* t = TrayFor(items_[hover_])) {
                POINT s = p;
                ClientToScreen(hwnd_, &s);
                g_app.tray->MouseMove(*t, s);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        tracking_ = false;
        if (g_app.settings.barAutohide) SetTimer(hwnd_, T_AUTOHIDE, 450, nullptr);
        if (trayDragging_) return 0;
        POINT p{-1, -1};
        SetHover(-1, p);
        if (pressed_ != -1) { pressed_ = -1; Invalidate(); }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (msg == WM_LBUTTONUP && trayDown_ >= 0) { TrayDragEnd(p); return 0; }
        OnButton(msg, p);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (trayDragging_ && (HWND)lp != hwnd_) {
            trayDragging_ = false;
            trayDropHide_ = false;
            trayDown_ = -1;
            SaveTraySettings();
            Invalidate();
        } else if (trayDown_ >= 0 && (HWND)lp != hwnd_) {
            trayDown_ = -1;   // pressed, then something else took the mouse: forget the press
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &p);
        int idx = HitTest(p);
        if (idx >= 0 && items_[idx].kind == Kind::Status) {
            float steps = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
            g_app.status->ChangeVolume(steps * 0.02f);
            g_app.InvalidateAll();
            UpdateTooltip();
        }
        return 0;
    }
    case WM_SKY_APPBAR:
        OnAppBarNotify(wp, lp);
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (appbar_) {
            APPBARDATA abd{sizeof abd};
            abd.hWnd = hwnd_;
            SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
        }
        if (!panels_.empty() || g_app.settings.barStyle >= 2) SyncPanels();   // panels move, hide and stack with the bar
        break;
    case WM_DISPLAYCHANGE:
        PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 3, 0);   // rebuild bars
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0)
            PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 4, 0); // theme
        else if (wp == SPI_SETWORKAREA || (lp && lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"intl") == 0)) {
            if (wp == SPI_SETWORKAREA && !g_app.settings.barAutohide) SetTimer(hwnd_, T_WORKAREA, 300, nullptr);
            Invalidate();
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE || wp == PBT_APMPOWERSTATUSCHANGE) {
            g_app.status->RefreshBattery();
            g_app.InvalidateAll();
        }
        return TRUE;
    case WM_TIMECHANGE:
        Invalidate();
        return 0;
    case WM_CLOSE:
        return 0; // only Skyline decides when bars close
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
