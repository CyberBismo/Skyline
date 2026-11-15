// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Dock hover previews — see preview.h
#include "preview.h"
#include "app.h"
#include "icons.h"

namespace {
constexpr wchar_t kClass[] = L"SkylineDockPreview";
constexpr float kMaxW = 232, kMaxH = 146, kPad = 10, kTitle = 22, kGap = 8;
constexpr int kMaxWindows = 6;

struct ACCENTPOLICY { int AccentState; int AccentFlags; DWORD GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attribute; PVOID Data; SIZE_T SizeOfData; };
using SetWCA = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);

void Glass(HWND h) {
    static SetWCA fn = (SetWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    const Theme& t = g_app.theme;
    BOOL dark = t.dark;
    DwmSetWindowAttribute(h, 20, &dark, sizeof dark);
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(h, &m);
    DWORD corner = 2;   // round
    DwmSetWindowAttribute(h, 33, &corner, sizeof corner);
    (void)fn;
    ApplyGlassEffect(h, Part::Card);
}

// Size of the window's visible frame (works for minimized windows too).
SIZE SourceSize(HWND w) {
    RECT r{};
    if (IsIconic(w)) {
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(w, &wp);
        r = wp.rcNormalPosition;
    } else if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) {
        GetWindowRect(w, &r);
    }
    SIZE s{std::max<LONG>(1, r.right - r.left), std::max<LONG>(1, r.bottom - r.top)};
    return s;
}
} // namespace

DockPreview::~DockPreview() {
    Clear();
    if (hwnd_) { SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0); DestroyWindow(hwnd_); }
}

bool DockPreview::Create() {
    if (hwnd_) return true;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 10, 10,
                            nullptr, nullptr, g_app.hinst, this);
    if (!hwnd_) return false;
    BOOL exclude = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
    Glass(hwnd_);
    return true;
}

void DockPreview::Clear() {
    for (auto& it : items_) if (it.th) DwmUnregisterThumbnail(it.th);
    items_.clear();
    hover_ = -1;
    hoverClose_ = false;
}

void DockPreview::Restyle() {
    if (!hwnd_) return;
    Glass(hwnd_);
    rt_.Reset(); brush_.Reset(); font_.Reset(); icon_.Reset();
}

void DockPreview::Hide() {
    Clear();
    inside_ = false;
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void DockPreview::Show(HWND owner, const std::vector<HWND>& wins, IWICBitmapSource* icon, int anchorX, int bottomY, UINT dpi) {
    if (!Create()) return;
    owner_ = owner;
    anchorX_ = anchorX;
    bottomY_ = bottomY;
    if (dpi != dpi_) { dpi_ = dpi; rt_.Reset(); brush_.Reset(); font_.Reset(); icon_.Reset(); }
    Clear();
    if (iconSrc_.Get() != icon) { iconSrc_.Reset(); if (icon) icon->QueryInterface(IID_PPV_ARGS(iconSrc_.Put())); icon_.Reset(); }

    // Lay out the cells (DIPs)
    float x = kPad, maxH = 0;
    for (HWND w : wins) {
        if ((int)items_.size() >= kMaxWindows || !IsWindow(w)) continue;
        SIZE s = SourceSize(w);
        // s is in pixels; convert to DIPs before fitting
        float sw = s.cx * 96.0f / dpi_, sh = s.cy * 96.0f / dpi_;
        float sc = std::min(kMaxW / sw, kMaxH / sh);
        float tw = std::max(96.0f, sw * sc), th = sh * sc;
        Item it;
        it.win = w;
        it.iconic = IsIconic(w) != FALSE;
        wchar_t title[256] = {};
        GetWindowTextW(w, title, 256);
        it.title = title;
        it.slot = D2D1::RectF(x - 4, kPad - 4, x + tw + 4, 0);   // bottom fixed below
        it.thumb = D2D1::RectF(x, kPad + kTitle, x + tw, kPad + kTitle + th);
        items_.push_back(it);
        x += tw + kGap;
        maxH = std::max(maxH, th);
    }
    if (items_.empty()) { Hide(); return; }
    float W = x - kGap + kPad, H = kPad + kTitle + maxH + kPad;
    for (auto& it : items_) {
        // centre each thumbnail vertically in the row
        float dy = (maxH - (it.thumb.bottom - it.thumb.top)) / 2;
        it.thumb.top += dy;
        it.thumb.bottom += dy;
        it.slot.bottom = H - kPad + 4;
    }

    int w = (int)std::lround(W * dpi_ / 96.0f), h = (int)std::lround(H * dpi_ / 96.0f);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint({anchorX, bottomY}, MONITOR_DEFAULTTONEAREST), &mi);
    int lo = (int)mi.rcWork.left + 8, hi = std::max(lo, (int)mi.rcWork.right - w - 8);
    int left = std::clamp(anchorX - w / 2, lo, hi);
    int top = bottomY - h;
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Live thumbnails, drawn by DWM on top of our window
    for (auto& it : items_) {
        if (FAILED(DwmRegisterThumbnail(hwnd_, it.win, &it.th))) { it.th = nullptr; continue; }
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
        p.rcDestination = {(LONG)std::lround(it.thumb.left * dpi_ / 96.0f), (LONG)std::lround(it.thumb.top * dpi_ / 96.0f),
                           (LONG)std::lround(it.thumb.right * dpi_ / 96.0f), (LONG)std::lround(it.thumb.bottom * dpi_ / 96.0f)};
        p.fVisible = TRUE;   // minimized too: DWM keeps the last picture of the window
        if (it.iconic) {
            // If all DWM has is the tiny minimized caption, keep the app icon instead
            SIZE src{};
            if (FAILED(DwmQueryThumbnailSourceSize(it.th, &src)) || src.cx < 200 || src.cy < 120) p.fVisible = FALSE;
        }
        p.fSourceClientAreaOnly = FALSE;
        p.opacity = 255;
        DwmUpdateThumbnailProperties(it.th, &p);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int DockPreview::HitTest(POINT c) const {
    float x = c.x * 96.0f / dpi_, y = c.y * 96.0f / dpi_;
    for (int i = 0; i < (int)items_.size(); ++i) {
        const auto& s = items_[i].slot;
        if (x >= s.left && x < s.right && y >= s.top && y < s.bottom) return i;
    }
    return -1;
}

D2D1_RECT_F DockPreview::CloseRect(int i) const {
    const Item& it = items_[i];
    float s = 20, top = it.slot.top + 3;
    return D2D1::RectF(it.slot.right - 4 - s, top, it.slot.right - 4, top + s);
}

// Close one window, keep the preview open with the rest (like the taskbar does)
void DockPreview::CloseAt(int i) {
    if (i < 0 || i >= (int)items_.size()) return;
    PostMessageW(items_[i].win, WM_SYSCOMMAND, SC_CLOSE, 0);
    std::vector<HWND> rest;
    for (int k = 0; k < (int)items_.size(); ++k)
        if (k != i && IsWindow(items_[k].win)) rest.push_back(items_[k].win);
    if (rest.empty()) { Hide(); return; }
    ComPtr<IWICBitmapSource> icon = iconSrc_;
    Show(owner_, rest, icon.Get(), anchorX_, bottomY_, dpi_);
    hover_ = -1;
    hoverClose_ = false;
}

void DockPreview::Paint() {
    if (!rt_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  (float)dpi_, (float)dpi_);
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                                     rt_.Put())))
            return;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
        g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 12, L"", font_.Put());
        if (font_) {
            font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ell;
            g_gfx.dwrite->CreateEllipsisTrimmingSign(font_.Get(), ell.Put());
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            font_->SetTrimming(&trim, ell.Get());
        }
    }
    const Theme& th = g_app.theme;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (D2D1_COLOR_F tint = GlassTint(Part::Card); tint.a > 0) {   // see-through: the tint painted over the glass
        brush_->SetColor(tint);
        D2D1_SIZE_F full = rt_->GetSize();
        rt_->FillRectangle(D2D1::RectF(0, 0, full.width, full.height), brush_.Get());
    }
    { D2D1_SIZE_F full = rt_->GetSize(); PaintVibe(rt_.Get(), D2D1::RectF(0, 0, full.width, full.height), Part::Card); PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, full.width, full.height), 8); }
    if (!icon_ && iconSrc_) rt_->CreateBitmapFromWicBitmap(iconSrc_.Get(), nullptr, icon_.Put());

    for (int i = 0; i < (int)items_.size(); ++i) {
        const Item& it = items_[i];
        if (i == hover_) {
            brush_->SetColor(th.hover);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(it.slot, 7, 7), brush_.Get());
        }
        // × on the hovered cell, in the title row so the live thumbnail never covers it
        float titleRight = it.thumb.right;
        if (i == hover_) {
            D2D1_RECT_F x = CloseRect(i);
            titleRight = std::min(titleRight, x.left - 4);
            if (hoverClose_) {
                brush_->SetColor(D2D1::ColorF(0xC42B1C));   // Windows' close red
                rt_->FillRoundedRectangle(D2D1::RoundedRect(x, 4, 4), brush_.Get());
            }
            brush_->SetColor(hoverClose_ ? D2D1::ColorF(1, 1, 1) : th.fg);
            float cx = (x.left + x.right) / 2, cy = (x.top + x.bottom) / 2, r = 4.5f;
            rt_->DrawLine({cx - r, cy - r}, {cx + r, cy + r}, brush_.Get(), 1.2f);
            rt_->DrawLine({cx - r, cy + r}, {cx + r, cy - r}, brush_.Get(), 1.2f);
        }
        brush_->SetColor(th.fg);
        if (font_)
            rt_->DrawTextW(it.title.c_str(), (UINT32)it.title.size(), font_.Get(),
                           D2D1::RectF(it.thumb.left, it.slot.top + 4, titleRight, it.thumb.top - 2), brush_.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        // Behind the live thumbnail: a soft card with the app icon (what you see for minimized windows)
        brush_->SetColor(D2D1::ColorF(th.fg.r, th.fg.g, th.fg.b, 0.06f));
        rt_->FillRoundedRectangle(D2D1::RoundedRect(it.thumb, 4, 4), brush_.Get());
        if (it.iconic && icon_) {
            float cx = (it.thumb.left + it.thumb.right) / 2, cy = (it.thumb.top + it.thumb.bottom) / 2, s = 40;
            DrawImageHQ(rt_.Get(), icon_.Get(), D2D1::RectF(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2), 0.9f);
        }
    }
    static int fails = 0;
    if (FAILED(rt_->EndDraw())) { rt_.Reset(); brush_.Reset(); font_.Reset(); icon_.Reset(); if (++fails < 3) InvalidateRect(hwnd_, nullptr, FALSE); } else fails = 0;
}

LRESULT CALLBACK DockPreview::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<DockPreview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    auto* self = reinterpret_cast<DockPreview*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT DockPreview::Handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            tracking_ = true;
        }
        inside_ = true;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = HitTest(p);
        bool onX = false;
        if (i >= 0) {
            D2D1_RECT_F x = CloseRect(i);
            float px = p.x * 96.0f / dpi_, py = p.y * 96.0f / dpi_;
            onX = px >= x.left && px < x.right && py >= x.top && py < x.bottom;
        }
        if (i != hover_ || onX != hoverClose_) { hover_ = i; hoverClose_ = onX; InvalidateRect(hwnd_, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        inside_ = false;
        hover_ = -1;
        hoverClose_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (owner_) PostMessageW(owner_, WM_PREVIEW_LEFT, 0, 0);
        return 0;
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = HitTest(p);
        if (i >= 0 && hoverClose_ && i == hover_) { CloseAt(i); return 0; }
        if (i >= 0) {
            HWND w = items_[i].win;
            Hide();
            if (IsWindow(w)) {
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
            }
        }
        return 0;
    }
    case WM_MBUTTONUP: {   // middle click: close that window
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int i = HitTest(p);
        if (i >= 0) CloseAt(i);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
