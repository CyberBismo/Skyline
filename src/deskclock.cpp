// Skyline - Copyright (c) 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Desktop clock: one layered window above the wallpaper, redrawn once a minute (no polling).
#include "deskclock.h"
#include "app.h"
#include <dwmapi.h>
#include <algorithm>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"SkylineDeskClock";
constexpr UINT_PTR kTick = 1;
HWND g_wnd = nullptr;

std::vector<HMONITOR> Monitors() {
    std::vector<HMONITOR> v;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL { ((std::vector<HMONITOR>*)lp)->push_back(m); return TRUE; }, (LPARAM)&v);
    return v;
}

// Just above the desktop (the window that holds the icons), so every app window covers it.
void SitOnDesktop(HWND w) {
    HWND def = nullptr, host = FindWindowW(L"Progman", nullptr);
    if (host && FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr)) def = host;
    for (HWND ww = nullptr; !def && (ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr)) != nullptr;)
        if (FindWindowExW(ww, nullptr, L"SHELLDLL_DefView", nullptr)) def = ww;
    if (!def) def = host;
    if (!def) { SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); return; }
    HWND above = GetWindow(def, GW_HWNDPREV);
    SetWindowPos(w, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ArmTick() {   // next time the minute changes
    SYSTEMTIME st;
    GetLocalTime(&st);
    SetTimer(g_wnd, kTick, (UINT)(60000 - (st.wSecond * 1000 + st.wMilliseconds)) + 50, nullptr);
}

void Draw() {
    if (!g_wnd || !g_gfx.d2d || !g_gfx.dwrite) return;
    const std::wstring ini = g_app.settings.path;
    int size = std::clamp((int)GetPrivateProfileIntW(L"DeskClock", L"Size", 1, ini.c_str()), 0, 2);
    int pos = std::clamp((int)GetPrivateProfileIntW(L"DeskClock", L"Position", 0, ini.c_str()), 0, 4);
    float scale = GetDpiForSystem() / 96.0f;
    float timePx = (size == 0 ? 64.0f : size == 1 ? 104.0f : 150.0f) * scale, datePx = timePx * 0.28f;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[32] = {}, d[96] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_FORCE24HOURFORMAT, &st, nullptr, t, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd d. MMMM", d, 96, nullptr);

    ComPtr<IDWriteTextFormat> ft, fd;
    g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, timePx, L"", ft.Put());
    g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, datePx, L"", fd.Put());
    if (!ft || !fd) return;
    ft->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fd->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> lt, ld;
    g_gfx.dwrite->CreateTextLayout(t, (UINT32)wcslen(t), ft.Get(), 4000, 4000, lt.Put());
    g_gfx.dwrite->CreateTextLayout(d, (UINT32)wcslen(d), fd.Get(), 4000, 4000, ld.Put());
    if (!lt || !ld) return;
    DWRITE_TEXT_METRICS mt{}, md{};
    lt->GetMetrics(&mt);
    ld->GetMetrics(&md);
    float pad = 16 * scale;
    int W = (int)std::ceil(std::max(mt.widthIncludingTrailingWhitespace, md.widthIncludingTrailingWhitespace) + 2 * pad);
    int H = (int)std::ceil(mt.height + md.height + 2 * pad);
    lt->SetMaxWidth((float)W);
    ld->SetMaxWidth((float)W);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);   // the main screen, unless another one is chosen
    if (int mi = (int)GetPrivateProfileIntW(L"DeskClock", L"Monitor", 0, ini.c_str()); mi > 0) {
        auto mons = Monitors();
        if (mi <= (int)mons.size()) {
            MONITORINFO info{sizeof info};
            if (GetMonitorInfoW(mons[mi - 1], &info)) wa = info.rcWork;
        }
    }
    int m = (int)(48 * scale), x, y;
    switch (pos) {
    case 1: x = wa.left + m; y = wa.top + m; break;
    case 2: x = wa.right - W - m; y = wa.top + m; break;
    case 3: x = wa.left + m; y = wa.bottom - H - m; break;
    case 4: x = wa.right - W - m; y = wa.bottom - H - m; break;
    default: x = (wa.left + wa.right - W) / 2; y = (wa.top + wa.bottom - H) / 2; break;
    }

    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        ComPtr<ID2D1DCRenderTarget> rt;
        RECT rc{0, 0, W, H};
        if (SUCCEEDED(g_gfx.d2d->CreateDCRenderTarget(&props, rt.Put())) && SUCCEEDED(rt->BindDC(mdc, &rc))) {
            ComPtr<ID2D1SolidColorBrush> br;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.Put());
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            if (br) {
                float dy = 2 * scale;
                for (int pass = 0; pass < 2; ++pass) {   // soft shadow first, then the text
                    float o = pass == 0 ? dy : 0;
                    br->SetColor(pass == 0 ? D2D1::ColorF(0, 0, 0, 0.35f) : D2D1::ColorF(1, 1, 1, 0.92f));
                    rt->DrawTextLayout(D2D1::Point2F(0 + o, pad + o), lt.Get(), br.Get());
                    br->SetColor(pass == 0 ? D2D1::ColorF(0, 0, 0, 0.30f) : D2D1::ColorF(1, 1, 1, 0.80f));
                    rt->DrawTextLayout(D2D1::Point2F(0 + o, pad + mt.height + o), ld.Get(), br.Get());
                }
            }
            rt->EndDraw();
            POINT dst{x, y}, src{0, 0};
            SIZE sz{W, H};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(g_wnd, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        }
        SelectObject(mdc, old);
        DeleteObject(bmp);
    }
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER:
        if (wp == kTick) { Draw(); ArmTick(); }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_TIMECHANGE:
        Draw();
        SitOnDesktop(h);
        ArmTick();
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    }
    return DefWindowProcW(h, m, wp, lp);
}

} // namespace

int DeskClockMonitorCount() { return (int)Monitors().size(); }

void DeskClockApply() {
    bool want = GetPrivateProfileIntW(L"DeskClock", L"Enabled", 0, g_app.settings.path.c_str()) != 0;
    if (!want) {
        if (g_wnd) { KillTimer(g_wnd, kTick); DestroyWindow(g_wnd); g_wnd = nullptr; }
        return;
    }
    if (!g_wnd) {
        static bool reg = false;
        if (!reg) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
            reg = true;
        }
        g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP,
                                0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_wnd) return;
        ShowWindow(g_wnd, SW_SHOWNA);
    }
    Draw();
    SitOnDesktop(g_wnd);
    ArmTick();
}
