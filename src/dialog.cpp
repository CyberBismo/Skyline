// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline dialog — a WinUI ContentDialog look-alike (Mica, heading, text,
// accent + standard button in a footer) drawn with Direct2D, like Settings.
#include "dialog.h"
#include "app.h"
#include <shellscalingapi.h>

namespace {

constexpr wchar_t kClass[] = L"SkylineDialog";
constexpr float kPad = 24, kFooterH = 80, kBtnH = 32, kBtnGap = 8;

// Same WinUI colours as Settings
struct Pal {
    D2D1_COLOR_F text, text2, layer, stroke, ctl, ctlHover, ctlPress, ctlStroke, accent, onAccent;
};
Pal P() {
    const Theme& t = g_app.theme;
    if (t.dark)
        return {{1, 1, 1, 1}, {1, 1, 1, 0.786f}, {1, 1, 1, 0.054f}, {0, 0, 0, 0.2f}, {1, 1, 1, 0.061f},
                {1, 1, 1, 0.084f}, {1, 1, 1, 0.033f}, {1, 1, 1, 0.07f}, t.accent, {0, 0, 0, 1}};
    return {{0, 0, 0, 0.896f}, {0, 0, 0, 0.606f}, {1, 1, 1, 0.7f}, {0, 0, 0, 0.058f}, {1, 1, 1, 0.7f},
            {0.976f, 0.976f, 0.976f, 0.5f}, {0.976f, 0.976f, 0.976f, 0.3f}, {0, 0, 0, 0.1f}, {0, 0.37f, 0.72f, 1},
            {1, 1, 1, 1}};
}

class DialogWindow {
public:
    void Show(const Dialog::Spec& s, std::function<void(bool)> cb);
    void Finish(bool primary);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    bool Resources();
    void Discard();
    void Fonts();
    void Layout();
    void Paint();
    D2D1_RECT_F Button(int i) const;
    bool HasButton(int i) const { return i == 0 ? !spec.primary.empty() : !spec.secondary.empty(); }
    float FooterTop() const { return viewH - kFooterH; }
    float ViewH() const;
    int HitButton(LPARAM lp) const;
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }
    void SetFrame();

    HWND hwnd = nullptr;
    UINT dpi = 96;
    Dialog::Spec spec;
    std::function<void(bool)> onClose;
    int hover = -1, pressed = -1, focus = 0;
    bool keyboard = false, tracking = false;
    float headingY = 0, bodyY = 0, noteY = 0, footerY = 0, height = 0;
    float viewH = 0, scroll = 0;   // the text scrolls when the window can't be tall enough

    ComPtr<IDWriteTextFormat> fHeading, fBody, fNote, fButton;
    ComPtr<IDWriteTextLayout> lHeading, lBody, lNote;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
};

DialogWindow g_dlg;

void DialogWindow::Fonts() {
    ComPtr<IDWriteFontCollection> fc;
    g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i; BOOL e = FALSE; return fc && SUCCEEDED(fc->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* disp = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    auto font = [](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT w, float size) {
        f.Reset();
        g_gfx.dwrite->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    };
    font(fHeading, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20);
    font(fBody, text, DWRITE_FONT_WEIGHT_NORMAL, 14);
    font(fNote, text, DWRITE_FONT_WEIGHT_NORMAL, 12);
    font(fButton, text, DWRITE_FONT_WEIGHT_NORMAL, 14);
    if (fButton) {
        fButton->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        fButton->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fButton->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

// Wrapped text decides the height; everything is in DIPs.
void DialogWindow::Layout() {
    float w = spec.width;
    auto make = [w](ComPtr<IDWriteTextLayout>& l, const std::wstring& s, IDWriteTextFormat* f) -> float {
        l.Reset();
        if (s.empty() || !f) return 0;
        g_gfx.dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w - 2 * kPad, 4000, l.Put());
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return m.height;
    };
    float y = kPad;
    headingY = y;
    y += make(lHeading, spec.heading, fHeading.Get());
    bodyY = y + 12;
    float hb = make(lBody, spec.body, fBody.Get());
    if (hb > 0) y = bodyY + hb;
    noteY = y + 12;
    float hn = make(lNote, spec.note, fNote.Get());
    if (hn > 0) y = noteY + hn;
    footerY = y + kPad;
    height = footerY + kFooterH;
}

// Two equal buttons side by side; a lone button takes the right one's place (as in WinUI).
D2D1_RECT_F DialogWindow::Button(int i) const {
    float w = (spec.width - 2 * kPad - kBtnGap) / 2, x = kPad + i * (w + kBtnGap), y = FooterTop() + (kFooterH - kBtnH) / 2;
    return D2D1::RectF(x, y, x + w, y + kBtnH);
}

float DialogWindow::ViewH() const {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return rc.bottom * 96.0f / dpi;
}

int DialogWindow::HitButton(LPARAM lp) const {
    float x = GET_X_LPARAM(lp) * 96.0f / dpi, y = GET_Y_LPARAM(lp) * 96.0f / dpi;
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F r = Button(i);
        if (HasButton(i) && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
    }
    return -1;
}

bool DialogWindow::Resources() {
    if (rt) return true;
    RECT rc;
    GetClientRect(hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              (float)dpi, (float)dpi);
    if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                                                 rt.Put())))
        return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), br.Put());
    return true;
}

void DialogWindow::Discard() {
    br.Reset();
    rt.Reset();
}

void DialogWindow::Paint() {
    if (!Resources()) return;
    Pal p = P();
    D2D1_SIZE_F sz = rt->GetSize();
    viewH = sz.height;
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, footerY - FooterTop()));
    float top = FooterTop();
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));   // Mica shows through

    // Content on a lighter layer, the buttons on plain Mica below a hairline (as in WinUI)
    br->SetColor(p.layer);
    rt->FillRectangle(D2D1::RectF(0, 0, sz.width, top), br.Get());
    br->SetColor(p.stroke);
    rt->FillRectangle(D2D1::RectF(0, top, sz.width, top + 1), br.Get());

    rt->PushAxisAlignedClip(D2D1::RectF(0, 0, sz.width, top), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));

    auto text = [&](IDWriteTextLayout* l, float y, D2D1_COLOR_F c) {
        if (!l) return;
        br->SetColor(c);
        rt->DrawTextLayout(D2D1::Point2F(kPad, y), l, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    text(lHeading.Get(), headingY, p.text);
    text(lBody.Get(), bodyY, p.text);
    text(lNote.Get(), noteY, p.text2);
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->PopAxisAlignedClip();
    if (footerY > top) {   // scroll bar hint, like a WinUI ScrollViewer at rest
        float track = top - 8, thumb = std::max(24.0f, track * top / footerY), y = 4 + (track - thumb) * scroll / (footerY - top);
        br->SetColor(p.text2);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sz.width - 6, y, sz.width - 4, y + thumb), 1, 1), br.Get());
    }

    for (int i = 0; i < 2; ++i) {
        if (!HasButton(i)) continue;
        D2D1_RECT_F r = Button(i);
        const std::wstring& label = i == 0 ? spec.primary : spec.secondary;
        bool down = pressed == i && hover == i;
        if (i == 0) {
            D2D1_COLOR_F a = p.accent;
            if (down) a.a = 0.8f;
            else if (hover == i) a.a = 0.9f;
            br->SetColor(a);
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br.Get());
        } else {
            br->SetColor(down ? p.ctlPress : hover == i ? p.ctlHover : p.ctl);
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br.Get());
            br->SetColor(p.ctlStroke);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3.5f, 3.5f),
                                     br.Get(), 1.0f);
        }
        D2D1_COLOR_F c = i == 0 ? p.onAccent : p.text;
        if (down) c.a *= 0.786f;
        br->SetColor(c);
        rt->DrawTextW(label.c_str(), (UINT32)label.size(), fButton.Get(), r, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (keyboard && focus == i) {   // WinUI focus ring: 2px outside the button
            br->SetColor(p.text);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), 7, 7),
                                     br.Get(), 2.0f);
        }
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) Discard();
}

void DialogWindow::SetFrame() {
    BOOL dark = g_app.theme.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    DWORD mica = 2; // DWMSBT_MAINWINDOW, like Settings
    DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof mica);
    MARGINS mg{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &mg);
}

void DialogWindow::Show(const Dialog::Spec& s, std::function<void(bool)> cb) {
    if (hwnd) Finish(false);
    spec = s;
    onClose = std::move(cb);
    hover = pressed = -1;
    focus = HasButton(0) ? 0 : 1;
    scroll = 0;
    keyboard = false;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_app.hinst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    Fonts();
    Layout();

    // Centred on the owner (or the main display), sized for that display's DPI, never taller than it
    if (spec.owner && !IsWindow(spec.owner)) spec.owner = nullptr;
    RECT ownerRc{};
    if (spec.owner) GetWindowRect(spec.owner, &ownerRc);
    HMONITOR mon = spec.owner ? MonitorFromWindow(spec.owner, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = (spec.owner ? 0 : WS_EX_TOPMOST) | WS_EX_DLGMODALFRAME;
    RECT r{0, 0, MulDiv((int)spec.width, dx, 96), MulDiv((int)std::ceil(height), dx, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, dx);
    const RECT& area = mi.rcWork;
    int w = r.right - r.left, h = std::min<int>(r.bottom - r.top, (area.bottom - area.top) * 9 / 10);
    const RECT& c = spec.owner ? ownerRc : area;
    int x = std::clamp<int>(c.left + (c.right - c.left - w) / 2, area.left, std::max(area.left, area.right - w));
    int y = std::clamp<int>(c.top + (c.bottom - c.top - h) / 2, area.top, std::max(area.top, area.bottom - h));

    hwnd = CreateWindowExW(ex, kClass, spec.caption.c_str(), style, x, y, w, h, spec.owner, nullptr, g_app.hinst, this);
    if (!hwnd) {   // can't show it: same as saying no
        auto f = std::move(onClose);
        onClose = nullptr;
        if (f) f(false);
        return;
    }
    dpi = GetDpiForWindow(hwnd);
    viewH = ViewH();
    SetFrame();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

void DialogWindow::Finish(bool primary) {
    auto f = std::move(onClose);
    onClose = nullptr;
    if (hwnd) DestroyWindow(hwnd);
    if (f) f(primary);   // after the window is gone, so the callback may show another one
}

LRESULT CALLBACK DialogWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DialogWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = h;
    }
    auto* self = reinterpret_cast<DialogWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self || self->hwnd != h) return DefWindowProcW(h, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT DialogWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt) rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        viewH = HIWORD(lp) * 96.0f / dpi;
        Invalidate();
        return 0;
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        auto* r = reinterpret_cast<RECT*>(lp);
        Discard();
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet")) {
            BOOL dark = LoadTheme().dark;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
            Invalidate();
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            tracking = true;
        }
        int b = HitButton(lp);
        if (b != hover) { hover = b; Invalidate(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (hover != -1) { hover = -1; Invalidate(); }
        return 0;
    case WM_LBUTTONDOWN:
        pressed = HitButton(lp);
        if (pressed >= 0) { SetCapture(hwnd); keyboard = false; Invalidate(); }
        return 0;
    case WM_LBUTTONUP: {
        int b = HitButton(lp), was = pressed;
        pressed = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        Invalidate();
        if (was >= 0 && b == was) Finish(b == 0);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (footerY > FooterTop()) {
            scroll -= GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA * 48;
            Invalidate();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (pressed >= 0 && (HWND)lp != hwnd) { pressed = -1; Invalidate(); }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_RETURN: case VK_SPACE: Finish(focus == 0); return 0;
        case VK_ESCAPE: Finish(false); return 0;
        case VK_TAB: case VK_LEFT: case VK_RIGHT:
            if (HasButton(focus ^ 1)) focus ^= 1;
            keyboard = true;
            Invalidate();
            return 0;
        case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
            scroll += (wp == VK_UP ? -40.0f : wp == VK_DOWN ? 40.0f : wp == VK_PRIOR ? -(FooterTop() - 40) : FooterTop() - 40);
            Invalidate();
            return 0;
        }
        break;
    case WM_CLOSE:
        Finish(false);
        return 0;
    case WM_DESTROY:
        onClose = nullptr;   // the owner went away first: there's no answer
        Discard();
        lHeading.Reset(); lBody.Reset(); lNote.Reset();
        fHeading.Reset(); fBody.Reset(); fNote.Reset(); fButton.Reset();
        hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void Dialog::Show(const Spec& spec, std::function<void(bool)> onClose) { g_dlg.Show(spec, std::move(onClose)); }
