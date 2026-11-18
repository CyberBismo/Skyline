// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Mission Control — see mission.h
#include "mission.h"
#include "app.h"
#include "icons.h"
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>
#include <map>
#include <memory>

namespace {

bool AppWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;   // minimised windows count too
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

RECT FrameOf(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    return r;
}

float EaseOut(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }

} // namespace

// ---------------------------------------------------------------------------
void WindowThumbs::Collect(HMONITOR mon, HWND dest) {
    Release();
    dest_ = dest;
    RECT dr{};
    GetWindowRect(dest, &dr);
    origin_ = {dr.left, dr.top};
    struct Ctx { HMONITOR mon; std::vector<HWND> wins; } ctx{mon, {}};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (AppWindow(h) && MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) == c->mon) c->wins.push_back(h);
        return TRUE;
    }, (LPARAM)&ctx);
    for (HWND h : ctx.wins) {
        Item it;
        it.h = h;
        if (FAILED(DwmRegisterThumbnail(dest, h, &it.thumb)) || !it.thumb) continue;
        if (IsIconic(h)) {
            // DWM keeps the last picture of a minimised window: show all of it, at its restored size,
            // rising out of the dock at the bottom of the screen
            it.minimized = true;
            WINDOWPLACEMENT wp{sizeof wp};
            GetWindowPlacement(h, &wp);
            SIZE s{wp.rcNormalPosition.right - wp.rcNormalPosition.left, wp.rcNormalPosition.bottom - wp.rcNormalPosition.top};
            SIZE q{};
            if (SUCCEEDED(DwmQueryThumbnailSourceSize(it.thumb, &q)) && q.cx > 40 && q.cy > 40) s = q;
            if (s.cx < 80 || s.cy < 60) s = {800, 600};
            it.full = s;
            LONG cx = (dr.right - dr.left) / 2, by = dr.bottom - dr.top;
            LONG w = std::max<LONG>(8, s.cx / 10), hh = std::max<LONG>(6, s.cy / 10);
            it.from = it.cur = it.to = {cx - w / 2, by - hh, cx + w / 2, by};
        } else {
            RECT f = FrameOf(h), wr{};
            GetWindowRect(h, &wr);
            it.source = {f.left - wr.left, f.top - wr.top, f.right - wr.left, f.bottom - wr.top};
            OffsetRect(&f, -origin_.x, -origin_.y);
            it.from = it.cur = it.to = f;
            it.full = {f.right - f.left, f.bottom - f.top};
        }
        wchar_t t[256] = {};
        GetWindowTextW(h, t, 256);
        it.title = t;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        it.exe = ProcessImagePath(pid);
        items_.push_back(std::move(it));
    }
}

void WindowThumbs::Layout(RECT area, float scale) {
    int n = (int)items_.size();
    if (!n) return;
    float aw = (float)(area.right - area.left), ah = (float)(area.bottom - area.top);
    const float gap = std::max(16.0f * scale, aw * 0.018f), label = 34 * scale;
    // Try every column count; keep the one where the windows end up biggest overall
    int bestCols = 1;
    float bestScore = -1;
    for (int cols = 1; cols <= n; ++cols) {
        int rows = (n + cols - 1) / cols;
        float cw = (aw - gap * (cols + 1)) / cols, ch = (ah - gap * (rows + 1)) / rows - label;
        if (cw <= 20 || ch <= 20) continue;
        float score = 0;
        for (auto& it : items_) {
            float w = (float)it.full.cx, h = (float)it.full.cy;
            float s = std::min({1.0f, cw / std::max(1.0f, w), ch / std::max(1.0f, h)});
            score += s * s * w * h;
        }
        if (score > bestScore) { bestScore = score; bestCols = cols; }
    }
    int cols = bestCols, rows = (n + cols - 1) / cols;
    float cw = (aw - gap * (cols + 1)) / cols, ch = (ah - gap * (rows + 1)) / rows - label;
    // Keep the grid in roughly the same order as the windows are on screen (fewer windows crossing paths)
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    float band = ah / rows;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const RECT& A = items_[a].from; const RECT& B = items_[b].from;
        int ra = (int)(((A.top + A.bottom) / 2.0f - area.top) / band), rb = (int)(((B.top + B.bottom) / 2.0f - area.top) / band);
        if (ra != rb) return ra < rb;
        return (A.left + A.right) < (B.left + B.right);
    });
    for (int k = 0; k < n; ++k) {
        Item& it = items_[order[k]];
        int r = k / cols, c = k % cols;
        int inRow = std::min(cols, n - r * cols);   // the last row is centred
        float rowW = inRow * cw + (inRow - 1) * gap;
        float x0 = area.left + (aw - rowW) / 2 + c * (cw + gap);
        float y0 = area.top + gap + r * (ch + label + gap);
        float w = (float)it.full.cx, h = (float)it.full.cy;
        float s = std::min({1.0f, cw / std::max(1.0f, w), ch / std::max(1.0f, h)}) * 0.94f;
        float tw = w * s, th = h * s;
        float x = x0 + (cw - tw) / 2, y = y0 + (ch - th) / 2;
        it.to = {(LONG)std::lround(x), (LONG)std::lround(y), (LONG)std::lround(x + tw), (LONG)std::lround(y + th)};
    }
}

void WindowThumbs::Step(float t) {
    float e = EaseOut(t);
    for (auto& it : items_) {
        it.cur.left = (LONG)std::lround(it.from.left + (it.to.left - it.from.left) * e);
        it.cur.top = (LONG)std::lround(it.from.top + (it.to.top - it.from.top) * e);
        it.cur.right = (LONG)std::lround(it.from.right + (it.to.right - it.from.right) * e);
        it.cur.bottom = (LONG)std::lround(it.from.bottom + (it.to.bottom - it.from.bottom) * e);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        if (!it.minimized) p.dwFlags |= DWM_TNP_RECTSOURCE;
        p.rcDestination = it.cur;
        p.rcSource = it.source;   // only the visible window, measured once
        p.fVisible = TRUE;
        p.opacity = it.minimized ? (BYTE)std::lround(225 * e) : 255;   // minimised ones fade in, a touch see-through
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(it.thumb, &p);
    }
}

void WindowThumbs::SetVisible(bool on) {
    for (auto& it : items_) {
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_VISIBLE;
        p.fVisible = on ? TRUE : FALSE;
        DwmUpdateThumbnailProperties(it.thumb, &p);
    }
}

void WindowThumbs::Release() {
    for (auto& it : items_) if (it.thumb) DwmUnregisterThumbnail(it.thumb);
    items_.clear();
}

int WindowThumbs::HitTest(POINT p) const {
    for (int i = 0; i < (int)items_.size(); ++i) {
        RECT r = items_[i].cur;
        InflateRect(&r, 6, 6);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

// ===========================================================================
namespace {

constexpr UINT_PTR T_FRAME = 1, T_OUTSIDE = 2;

// Windows only lets the app that got the last input take the foreground: a harmless empty mouse input
// makes that us; if that's still refused, borrow the foreground thread's input state for a moment.
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

void CloseAll(HWND chosen);          // all screens close together
bool IsOverlayWindow(HWND h);
std::vector<HWND> AllOverlayWindows();

class Overlay {
public:
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

    void Show(HMONITOR mon, bool focus) {
        mon_ = mon;
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon_, &mi);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(mon_, MDT_EFFECTIVE_DPI, &dx, &dy);
        dpi_ = dx;
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"SkylineMissionControl";
            RegisterClassExW(&wc);
            reg = true;
        }
        RECT r = mi.rcMonitor;
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SkylineMissionControl", L"Mission Control", WS_POPUP, r.left, r.top,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, g_app.hinst, this);
        if (!hwnd_) return;
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
        ApplyGlassEffect(hwnd_, Part::Start);
        thumbs_.Collect(mon_, hwnd_);
        // the grid: leave room for the top bar and the dock
        int top = MulDiv(g_app.settings.heightDip + 34, dpi_, 96), bottom = MulDiv(96, dpi_, 96), side = MulDiv(40, dpi_, 96);
        RECT area{side, top, (r.right - r.left) - side, (r.bottom - r.top) - bottom};
        thumbs_.Layout(area, dpi_ / 96.0f);
        thumbs_.Step(0);
        anim_ = 0;
        closing_ = false;
        chosen_ = nullptr;
        hover_ = sel_ = -1;
        start_ = GetTickCount64();
        if (focus) {
            ShowWindow(hwnd_, SW_SHOW);
            TakeForeground(hwnd_);
            // still refused (it happens from a hot corner): watch for clicks elsewhere ourselves
            if (GetForegroundWindow() != hwnd_) SetTimer(hwnd_, T_OUTSIDE, 50, nullptr);
        } else {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }
        SetTimer(hwnd_, T_FRAME, 10, nullptr);
    }

    HWND Hwnd() const { return hwnd_; }

    void Close(HWND chosen = nullptr) {
        if (!hwnd_ || closing_) return;
        closing_ = true;
        chosen_ = chosen;
        from_ = anim_;
        start_ = GetTickCount64();
        SetTimer(hwnd_, T_FRAME, 10, nullptr);
    }

private:
    void Frame() {
        float t = (GetTickCount64() - start_) / (closing_ ? 200.0f : 260.0f);
        anim_ = closing_ ? from_ * (1 - std::clamp(t, 0.0f, 1.0f)) : std::clamp(t, 0.0f, 1.0f);
        thumbs_.Step(anim_);   // out into the grid, and back to where they were
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (t >= 1) {
            KillTimer(hwnd_, T_FRAME);
            if (closing_) Finish();
        }
    }

    void Finish() {
        HWND chosen = chosen_;
        thumbs_.Release();
        HWND h = hwnd_;
        // hand the foreground to the chosen window while we still have it (Windows may refuse once we're hidden), then go
        if (chosen && IsWindow(chosen)) {
            if (IsIconic(chosen)) ShowWindow(chosen, SW_RESTORE);
            SetForegroundWindow(chosen);
        }
        ShowWindow(h, SW_HIDE);
        hwnd_ = nullptr;
        bmps_.clear();
        font_.Reset();   // the next screen may have another scale
        rt_.Reset();
        brush_.Reset();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }

    bool Resources() {
        if (rt_) return true;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), rt_.Put())))
            return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
        if (!font_) {
            g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           13.0f * dpi_ / 96.0f, L"", font_.Put());
            if (font_) {
                font_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                font_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                ComPtr<IDWriteInlineObject> ell;
                g_gfx.dwrite->CreateEllipsisTrimmingSign(font_.Get(), ell.Put());
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                font_->SetTrimming(&trim, ell.Get());
            }
        }
        return true;
    }

    ID2D1Bitmap* IconFor(const std::wstring& exe, int px) {
        auto it = bmps_.find(exe);
        if (it != bmps_.end()) return it->second.Get();
        ComPtr<ID2D1Bitmap> b;
        if (auto src = LoadAppImage(exe, px)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
        bmps_[exe] = b;
        return b.Get();
    }

    // In pixels (the render target is set to 96 DPI so it maps 1:1 to the thumbnails' coordinates)
    void Paint() {
        if (!Resources()) return;
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        D2D1_COLOR_F tint = GlassTint(Part::Start);
        brush_->SetColor(D2D1::ColorF(0, 0, 0, (th.dark ? 0.30f : 0.18f) * anim_ + tint.a * 0.3f * anim_));
        rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), brush_.Get());
        float k = dpi_ / 96.0f;
        const auto& items = thumbs_.Items();
        bool settled = anim_ > 0.97f && !closing_;
        for (int i = 0; i < (int)items.size(); ++i) {
            const RECT& r = items[i].cur;
            D2D1_RECT_F rf = D2D1::RectF((float)r.left, (float)r.top, (float)r.right, (float)r.bottom);
            bool hot = i == hover_ || i == sel_;
            if (hot && settled) {   // a bright frame round the window you point at
                brush_->SetColor(th.accent);
                rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rf.left - 4 * k, rf.top - 4 * k, rf.right + 4 * k, rf.bottom + 4 * k), 10 * k, 10 * k),
                                          brush_.Get(), 3 * k);
            }
            if (settled) {   // the app's icon and the window's name underneath
                int px = (int)std::lround(22 * k);
                float cx = (rf.left + rf.right) / 2, y = rf.bottom + 8 * k;
                std::wstring t = items[i].title;
                // icon and title together, centred under the window
                float maxW = std::min(rf.right - rf.left + 60 * k, 420 * k) - 26 * k;
                float textW = maxW;
                ComPtr<IDWriteTextLayout> tl;
                g_gfx.dwrite->CreateTextLayout(t.c_str(), (UINT32)t.size(), font_.Get(), maxW, 22 * k, tl.Put());
                if (tl) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); textW = std::min(maxW, m.width); }
                float left = cx - (textW + 26 * k) / 2;
                if (ID2D1Bitmap* b = IconFor(items[i].exe, px)) DrawImageHQ(rt_.Get(), b, D2D1::RectF(left, y, left + 22 * k, y + 22 * k), hot ? 1.0f : 0.85f);
                brush_->SetColor(hot ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(1, 1, 1, 0.78f));
                rt_->DrawTextW(t.c_str(), (UINT32)t.size(), font_.Get(), D2D1::RectF(left + 26 * k - (maxW - textW) / 2, y, left + 26 * k + textW + (maxW - textW) / 2, y + 22 * k),
                               brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (items.empty() && settled) {
            std::wstring t = TR(L"Ingen åpne vinduer på denne skjermen", L"No open windows on this screen");
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
            rt_->DrawTextW(t.c_str(), (UINT32)t.size(), font_.Get(), D2D1::RectF(0, sz.height / 2 - 20, sz.width, sz.height / 2 + 20), brush_.Get());
        }
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); }
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        if (!self->hwnd_) self->hwnd_ = h;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            self->Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (wp == T_FRAME) self->Frame();
            else if (wp == T_OUTSIDE) {   // no foreground: a click outside our screen closes us
                if (GetForegroundWindow() == h) { KillTimer(h, T_OUTSIDE); return 0; }
                if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) {
                    POINT c;
                    GetCursorPos(&c);
                    RECT r;
                    GetWindowRect(h, &r);
                    bool inAny = PtInRect(&r, c) != 0;
                    for (HWND o : AllOverlayWindows()) { RECT ro; if (GetWindowRect(o, &ro) && PtInRect(&ro, c)) inAny = true; }
                    if (!inAny) { KillTimer(h, T_OUTSIDE); CloseAll(nullptr); }
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            int hv = self->thumbs_.HitTest(p);
            if (hv != self->hover_) { self->hover_ = hv; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->closing_) return 0;
            int hv = self->thumbs_.HitTest(p);
            CloseAll(hv >= 0 ? self->thumbs_.Items()[hv].h : nullptr);
            return 0;
        }
        case WM_RBUTTONUP: if (!self->closing_) CloseAll(nullptr); return 0;
        case WM_KEYDOWN: {
            if (self->closing_) return 0;
            int n = (int)self->thumbs_.Items().size();
            if (wp == VK_ESCAPE) CloseAll(nullptr);
            else if (wp == VK_RETURN) CloseAll(self->sel_ >= 0 && self->sel_ < n ? self->thumbs_.Items()[self->sel_].h : nullptr);
            else if (n && (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_TAB)) { self->sel_ = (self->sel_ + 1) % n; InvalidateRect(h, nullptr, FALSE); }
            else if (n && (wp == VK_LEFT || wp == VK_UP)) { self->sel_ = (self->sel_ + n - 1) % n; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && !self->closing_ && !IsOverlayWindow((HWND)lp)) CloseAll(nullptr);   // to another screen of ours: stay
            break;
        case WM_CLOSE: CloseAll(nullptr); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HMONITOR mon_ = nullptr;
    UINT dpi_ = 96;
    WindowThumbs thumbs_;
    float anim_ = 0, from_ = 0;
    bool closing_ = false;
    HWND chosen_ = nullptr;
    int hover_ = -1, sel_ = -1;
    ULONGLONG start_ = 0;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> font_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bmps_;
};

std::map<HMONITOR, std::unique_ptr<Overlay>> g_overlays;   // one per screen

std::vector<HWND> AllOverlayWindows() {
    std::vector<HWND> v;
    for (auto& [m, o] : g_overlays) if (o->Hwnd()) v.push_back(o->Hwnd());
    return v;
}
bool IsOverlayWindow(HWND h) {
    if (!h) return false;
    for (auto& [m, o] : g_overlays) if (o->Hwnd() == h) return true;
    return false;
}
void CloseAll(HWND chosen) {
    for (auto& [m, o] : g_overlays) o->Close(chosen);
}
bool AnyVisible() {
    for (auto& [m, o] : g_overlays) if (o->Visible()) return true;
    return false;
}
BOOL CALLBACK EnumMon(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
    return TRUE;
}

} // namespace

namespace MissionControl {
bool Enabled() { return g_app.settings.missionEnabled; }
bool Open() { return AnyVisible(); }
void Toggle() {
    if (AnyVisible()) { CloseAll(nullptr); return; }
    POINT c;
    GetCursorPos(&c);
    HMONITOR cur = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, EnumMon, (LPARAM)&mons);
    // every screen at once (one less trip with the mouse); the one with the mouse is shown last and gets the keyboard
    for (HMONITOR m : mons) {
        if (m == cur) continue;
        auto& o = g_overlays[m];
        if (!o) o = std::make_unique<Overlay>();
        o->Show(m, false);
    }
    auto& o = g_overlays[cur];
    if (!o) o = std::make_unique<Overlay>();
    o->Show(cur, true);
}
} // namespace MissionControl
