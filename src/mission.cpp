// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Mission Control — see mission.h
#include "mission.h"
#include "app.h"
#include "icons.h"
#include "enhancedtiling.h"
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

void WindowThumbs::Remove(HWND h) {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i].h == h) {
            if (items_[i].thumb) DwmUnregisterThumbnail(items_[i].thumb);
            items_.erase(items_.begin() + i);
            return;
        }
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

constexpr UINT_PTR T_FRAME = 1, T_OUTSIDE = 2, T_RECOLLECT = 3, T_SNAP = 4;

// A workspace picked in the strip: the switch happens when Mission Control has closed (see Finish).
HMONITOR g_pendMon = nullptr;
int g_pendSpace = 0;

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
void OverlayHint(HWND src, bool clear, HWND w = nullptr, const std::wstring& title = std::wstring(), const std::wstring& exe = std::wstring());   // a window is dragged over the other screens: they light up what it would land on
bool DropOnOther(HWND src, POINT scr, int* n, HMONITOR* tm, bool* plus);
void RefreshAllOverlays();           // the workspace list changed: every screen's strip is made again at once
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
        // Enhanced Tiling: a strip with the four workspaces along the top
        spaces_.clear();
        snapBmps_.clear();
        cardHover_ = -1;
        int top = MulDiv(g_app.settings.heightDip + 34, dpi_, 96), bottom = MulDiv(96, dpi_, 96), side = MulDiv(40, dpi_, 96);
        if (EnhancedSpacesAvailable()) RefreshSpaces();
        if (!spaces_.empty()) {   // new pictures of what is on show, and look again a little later when they are ready
            EnhancedSnapshotRefresh(mon_);
            snapTick_ = 0;
            SetTimer(hwnd_, T_SNAP, 450, nullptr);
        }
        if (!spaces_.empty()) { LayoutCards(); top = stripBottom_; }
        RECT area{side, top, (r.right - r.left) - side, (r.bottom - r.top) - bottom};
        area_ = area;
        pressed_ = -1;
        dragging_ = false;
        dragWin_ = nullptr;
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

    void RefreshNow() {   // (from another screen's overlay) new list of workspaces and their cards
        if (!hwnd_ || closing_ || spaces_.empty()) return;
        RefreshSpaces();
        LayoutCards();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    HMONITOR Mon() const { return mon_; }

    // The workspace cards in a row, then a "+" to add one more. Height stays the same however many there are.
    void LayoutCards() {
        RECT cr;
        GetClientRect(hwnd_, &cr);
        float k = dpi_ / 96.0f, W = (float)(cr.right - cr.left), side = 40 * k, gap = 14 * k, plusW = 52 * k;
        int n = (int)spaces_.size();
        float ratio = (float)std::max<LONG>(1, waSz_.cx) / (float)std::max<LONG>(1, waSz_.cy);
        // height is fixed by the four-card layout
        bool multi = !spaces_.empty() && spaces_[0].mons.size() > 1;   // all screens in one card: wider cards so each little screen is easy to see and hit
        float cw4 = std::min((multi ? 340.0f : 210.0f) * k, (W - 2 * side - plusW - 4 * gap) / 4.0f);
        float bodyH = std::min((cw4 - 16 * k) / ratio, 130.0f * k);
        if (multi) bodyH = std::max(bodyH, 60.0f * k);
        cardH_ = bodyH + 32 * k;
        float cw = std::min(cw4 + 0.0f, (W - 2 * side - plusW - n * gap) / std::max(1, n));
        cw = std::max(cw, 46 * k);
        float total = n * cw + n * gap + plusW, x0 = (W - total) / 2, y0 = (float)MulDiv(g_app.settings.heightDip + 14, dpi_, 96);
        cards_.assign(n, {});
        for (int i = 0; i < n; ++i) cards_[i] = D2D1::RectF(x0 + i * (cw + gap), y0, x0 + i * (cw + gap) + cw, y0 + cardH_);
        float px = x0 + n * (cw + gap);
        plusRc_ = D2D1::RectF(px, y0, px + plusW, y0 + cardH_);
        stripBottom_ = (int)(y0 + cardH_ + 18 * k);
        canRemove_ = n > 2 && EnhancedCanRemoveSpace();
    }
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
        snapBmps_.clear();
        dragBmp_.Reset();
        font_.Reset();   // the next screen may have another scale
        rt_.Reset();
        brush_.Reset();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
        if (g_pendSpace > 0 && g_pendMon == mon_) {   // a workspace was picked: go there now that everything is closed
            int n = g_pendSpace;
            g_pendSpace = 0;
            EnhancedSwitchSpace(mon_, n);
        }
    }

    bool Resources() {
        if (rt_) return true;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY), rt_.Put())))
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

    // The workspace strip: four little screens with the windows in them (their last picture), the current one framed.
    void PaintSpaces(float k) {
        const Theme& th = g_app.theme;
        float fade = std::clamp(anim_ * 1.4f, 0.0f, 1.0f);
        for (int i = 0; i < (int)spaces_.size() && i < (int)cards_.size(); ++i) {
            const auto& sp = spaces_[i];
            const D2D1_RECT_F& c = cards_[i];
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (i == cardHover_ ? 0.20f : 0.11f) * fade));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(c, 12 * k, 12 * k), brush_.Get());
            if (sp.current) {
                brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, fade));
                rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + k, c.top + k, c.right - k, c.bottom - k), 12 * k, 12 * k), brush_.Get(), 2.5f * k);
            }
            std::wstring label = std::to_wstring(sp.n) + L"   " + (sp.wins.empty() ? std::wstring(TR(L"tom", L"empty"))
                                 : std::to_wstring(sp.wins.size()) + L" " + (sp.wins.size() == 1 ? TR(L"vindu", L"window") : TR(L"vinduer", L"windows")));
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.9f * fade));
            rt_->DrawTextW(label.c_str(), (UINT32)label.size(), font_.Get(), D2D1::RectF(c.left, c.top + 3 * k, c.right, c.top + 26 * k), brush_.Get());
            D2D1_RECT_F body = D2D1::RectF(c.left + 8 * k, c.top + 28 * k, c.right - 8 * k, c.bottom - 8 * k);
            {   // keep the screen's proportions, centred in what is left of the card
                float bw0 = body.right - body.left, bh0 = body.bottom - body.top, want = bw0 * (float)waSz_.cy / (float)std::max<LONG>(1, waSz_.cx);
                if (want < bh0) { float m = (bh0 - want) / 2; body.top += m; body.bottom -= m; }
            }
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.38f * fade));
            float bw = body.right - body.left, bh = body.bottom - body.top;
            if (sp.mons.empty()) rt_->FillRoundedRectangle(D2D1::RoundedRect(body, 5 * k, 5 * k), brush_.Get());
            else for (const auto& m : sp.mons) {   // all screens: one little screen each, with a clear frame
                D2D1_RECT_F mr = D2D1::RectF(body.left + m.l * bw + 1 * k, body.top + m.t * bh + 1 * k, body.left + m.r * bw - 1 * k, body.top + m.b * bh - 1 * k);
                brush_->SetColor(D2D1::ColorF(0, 0, 0, 0.42f * fade));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(mr, 3 * k, 3 * k), brush_.Get());
                brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.35f * fade));
                rt_->DrawRoundedRectangle(D2D1::RoundedRect(mr, 3 * k, 3 * k), brush_.Get(), 1.5f * k);
            }
            for (const auto& w : sp.wins) {
                D2D1_RECT_F r = D2D1::RectF(body.left + w.l * bw, body.top + w.t * bh, body.left + w.r * bw, body.top + w.b * bh);
                ID2D1Bitmap* bmp = nullptr;
                auto it = snapBmps_.find(w.h);
                if (it == snapBmps_.end()) {   // (no picture yet: don't remember that, it may arrive any moment)
                    ComPtr<ID2D1Bitmap> b;
                    if (auto src = EnhancedSnapshot(w.h)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
                    if (b) it = snapBmps_.emplace(w.h, b).first;
                }
                bmp = it != snapBmps_.end() ? it->second.Get() : nullptr;
                if (bmp) rt_->DrawBitmap(bmp, r, fade, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                else {   // no picture (yet): the app's icon in the middle instead of an empty grey box
                    brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * fade));
                    rt_->FillRectangle(r, brush_.Get());
                    float side = std::min({r.right - r.left, r.bottom - r.top}) * 0.55f;
                    if (side > 10 * k) {
                        side = std::min(side, 40 * k);
                        int px = (int)std::lround(side);
                        if (ID2D1Bitmap* ib = IconFor(ExeOf(w.h), px)) {
                            float cx0 = (r.left + r.right) / 2, cy0 = (r.top + r.bottom) / 2;
                            DrawImageHQ(rt_.Get(), ib, D2D1::RectF(cx0 - side / 2, cy0 - side / 2, cx0 + side / 2, cy0 + side / 2), 0.9f * fade);
                        }
                    }
                }
                brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.35f * fade));
                rt_->DrawRectangle(r, brush_.Get(), 1.0f);
            }
        }
        if (canRemove_ && !cards_.empty()) {   // "x" on the last card
            D2D1_RECT_F xr = XRect();
            float cxp = (xr.left + xr.right) / 2, cyp = (xr.top + xr.bottom) / 2, arm = 3.5f * k;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (xHover_ ? 0.30f : 0.14f) * fade));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cxp, cyp), 9 * k, 9 * k), brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (xHover_ ? 1.0f : 0.75f) * fade));
            rt_->DrawLine(D2D1::Point2F(cxp - arm, cyp - arm), D2D1::Point2F(cxp + arm, cyp + arm), brush_.Get(), 1.6f * k, nullptr);
            rt_->DrawLine(D2D1::Point2F(cxp - arm, cyp + arm), D2D1::Point2F(cxp + arm, cyp - arm), brush_.Get(), 1.6f * k, nullptr);
        }
        if (spaces_.size() < 8) {   // the "+" card
            float cxp = (plusRc_.left + plusRc_.right) / 2, cyp = (plusRc_.top + plusRc_.bottom) / 2, arm = 9 * k;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (plusHover_ ? 0.20f : 0.08f) * fade));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(plusRc_, 12 * k, 12 * k), brush_.Get());
            brush_->SetColor(D2D1::ColorF(1, 1, 1, (plusHover_ ? 0.95f : 0.6f) * fade));
            rt_->DrawLine(D2D1::Point2F(cxp - arm, cyp), D2D1::Point2F(cxp + arm, cyp), brush_.Get(), 2.0f * k, nullptr);
            rt_->DrawLine(D2D1::Point2F(cxp, cyp - arm), D2D1::Point2F(cxp, cyp + arm), brush_.Get(), 2.0f * k, nullptr);
            }
    }

    // The little "x" in the corner of the last card (when it can go)
    D2D1_RECT_F XRect() const {
        float k = dpi_ / 96.0f;
        const D2D1_RECT_F& c = cards_.back();
        return D2D1::RectF(c.right - 24 * k, c.top + 4 * k, c.right - 4 * k, c.top + 24 * k);
    }
    bool OnX(POINT p) const {
        if (!canRemove_ || cards_.empty()) return false;
        D2D1_RECT_F r = XRect();
        return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
    }

    // The workspace list: with "switch on all screens" every card shows all the screens side by side (like the Spaces bar on a Mac)
    const std::wstring& ExeOf(HWND h) {   // the program of a window (found once)
        auto it = exeOf_.find(h);
        if (it == exeOf_.end()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            it = exeOf_.emplace(h, ProcessImagePath(pid)).first;
        }
        return it->second;
    }
    // Which screen's little picture inside card `ci` is under the point (or nearest to it)
    HMONITOR MonAtCard(POINT p, int ci) const {
        if (ci < 0 || ci >= (int)spaces_.size() || ci >= (int)cards_.size()) return mon_;
        const auto& ms = spaces_[ci].mons;
        if (ms.empty()) return mon_;
        D2D1_RECT_F body = CardBody(ci);
        float bw = body.right - body.left, bh = body.bottom - body.top, bd = 1e30f;
        HMONITOR best = nullptr;
        for (const auto& m : ms) {
            float l = body.left + m.l * bw, t = body.top + m.t * bh, r = body.left + m.r * bw, b = body.top + m.b * bh;
            if (p.x >= l && p.x < r && p.y >= t && p.y < b) return (HMONITOR)m.h;
            float dx = p.x - (l + r) / 2, dy = p.y - (t + b) / 2, d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = (HMONITOR)m.h; }
        }
        return best ? best : mon_;
    }
public:
    // A window is dropped on this screen's Mission Control from another screen: which workspace and which screen it lands on
    // (ci: -2 = the "+" button). Not on a card: the workspace on show on this screen.
    bool DropAt(POINT scr, int* n, HMONITOR* tm, bool* plus) {
        POINT p = scr;
        ScreenToClient(hwnd_, &p);
        int ci = CardAt(p);
        *plus = ci == -2;
        if (ci >= 0 && ci < (int)spaces_.size()) { *n = spaces_[ci].n; *tm = MonAtCard(p, ci); return true; }
        if (ci == -2) { *n = 0; *tm = mon_; return true; }
        *n = EnhancedCurrentSpace(mon_); *tm = mon_;
        return *n > 0;
    }
    void SetHint(int card, HMONITOR m) { if (hintCard_ != card || hintMon_ != m) { hintCard_ = card; hintMon_ = m; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); } }
    void HintAt(POINT scr, HWND w, const std::wstring& title, const std::wstring& exe) {   // the dragged window is over this screen: show it here too
        POINT p = scr;
        ScreenToClient(hwnd_, &p);
        int ci = CardAt(p);
        SetHint(ci, ci >= 0 ? MonAtCard(p, ci) : nullptr);
        if (w && rt_) {
            if (ghostWin_ != w) {
                ghostWin_ = w; ghostTitle_ = title; ghostExe_ = exe; ghostBmp_.Reset();
                if (auto src = EnhancedSnapshot(w)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, ghostBmp_.Put());
            }
            if (abs(p.x - ghostPt_.x) + abs(p.y - ghostPt_.y) >= 2) { ghostPt_ = p; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
        }
    }
    void ClearGhost() { if (ghostWin_) { ghostWin_ = nullptr; ghostBmp_.Reset(); if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); } }
    void RefreshAfterDrop() {
        RefreshSpaces(); LayoutCards(); RefreshNow();
    }
private:
    int hintCard_ = -1;
    HMONITOR hintMon_ = nullptr;
    HWND ghostWin_ = nullptr;   // a window dragged here from another screen
    POINT ghostPt_{};
    std::wstring ghostTitle_, ghostExe_;
    ComPtr<ID2D1Bitmap> ghostBmp_;
    void MakeDragBmp() {
        dragBmp_.Reset();
        if (!rt_ || !dragWin_) return;
        if (auto src = EnhancedSnapshot(dragWin_)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, dragBmp_.Put());
    }
    void RefreshSpaces() { spaces_ = EnhancedSpacesSync() ? EnhancedSpacesInfoAll(&waSz_) : EnhancedSpacesInfo(mon_, &waSz_); }

    // The little screen inside a workspace card (same maths as when it is painted)
    D2D1_RECT_F CardBody(int i) const {
        float k = dpi_ / 96.0f;
        const D2D1_RECT_F& c = cards_[i];
        D2D1_RECT_F body = D2D1::RectF(c.left + 8 * k, c.top + 28 * k, c.right - 8 * k, c.bottom - 8 * k);
        float bw0 = body.right - body.left, bh0 = body.bottom - body.top, want = bw0 * (float)waSz_.cy / (float)std::max<LONG>(1, waSz_.cx);
        if (want < bh0) { float m = (bh0 - want) / 2; body.top += m; body.bottom -= m; }
        return body;
    }
    // The window under the point inside any card (the one painted last wins), and which card it is in
    HWND WinInCard(POINT p, int* card) const {
        for (int i = 0; i < (int)spaces_.size() && i < (int)cards_.size(); ++i) {
            D2D1_RECT_F body = CardBody(i);
            float bw = body.right - body.left, bh = body.bottom - body.top;
            const auto& wins = spaces_[i].wins;
            for (int j = (int)wins.size() - 1; j >= 0; --j) {
                const auto& w = wins[j];
                float l = body.left + w.l * bw, t = body.top + w.t * bh, r = body.left + w.r * bw, b = body.top + w.b * bh;
                if (p.x >= l && p.x < r && p.y >= t && p.y < b) { if (card) *card = i; return w.h; }
            }
        }
        return nullptr;
    }

    int CardAt(POINT p) const {
        if (spaces_.size() < 8 && p.x >= plusRc_.left && p.x < plusRc_.right && p.y >= plusRc_.top && p.y < plusRc_.bottom) return -2;
        for (int i = 0; i < (int)spaces_.size() && i < (int)cards_.size(); ++i)
            if (p.x >= cards_[i].left && p.x < cards_[i].right && p.y >= cards_[i].top && p.y < cards_[i].bottom) return i;
        return -1;
    }

    void AddSpace() {
        if (!EnhancedAddSpace()) return;
        RefreshSpaces();
        LayoutCards();
        InvalidateRect(hwnd_, nullptr, FALSE);
        RefreshAllOverlays();
    }

    void RemoveSpace() {
        if (!EnhancedRemoveSpace()) return;
        xHover_ = false;
        RefreshSpaces();
        LayoutCards();
        InvalidateRect(hwnd_, nullptr, FALSE);
        RefreshAllOverlays();
    }

    void PickSpace(int i) {
        if (i < 0 || i >= (int)spaces_.size()) return;
        g_pendMon = mon_;
        g_pendSpace = spaces_[i].current ? 0 : spaces_[i].n;   // (the one you are on: just close)
        CloseAll(nullptr);
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
        if (!spaces_.empty()) PaintSpaces(k);
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
        if ((dragging_ && dragWin_) || hintCard_ != -1 || ghostWin_) {   // the window follows the mouse; the card it would land on lights up (also when it comes from another screen)
            int dropCard_ = dragging_ && dragWin_ ? this->dropCard_ : hintCard_;
            HMONITOR dropMon_ = dragging_ && dragWin_ ? this->dropMon_ : hintMon_;
            if (dropCard_ >= 0 && dropCard_ < (int)cards_.size() && dropMon_ && !spaces_[dropCard_].mons.empty()) {   // the little screen it would land on
                D2D1_RECT_F bd = CardBody(dropCard_);
                float bw = bd.right - bd.left, bh = bd.bottom - bd.top;
                for (const auto& m : spaces_[dropCard_].mons) {
                    if ((HMONITOR)m.h != dropMon_) continue;
                    D2D1_RECT_F mr = D2D1::RectF(bd.left + m.l * bw, bd.top + m.t * bh, bd.left + m.r * bw, bd.top + m.b * bh);
                    brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.45f));
                    rt_->FillRectangle(mr, brush_.Get());
                    brush_->SetColor(th.accent);
                    rt_->DrawRectangle(mr, brush_.Get(), 2 * k);
                }
            }
            if (dropCard_ == -2 || (dropCard_ >= 0 && dropCard_ < (int)cards_.size())) {
                const D2D1_RECT_F& c = dropCard_ == -2 ? plusRc_ : cards_[dropCard_];
                brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.30f));
                rt_->FillRoundedRectangle(D2D1::RoundedRect(c, 12 * k, 12 * k), brush_.Get());
                brush_->SetColor(th.accent);
                rt_->DrawRoundedRectangle(D2D1::RoundedRect(c, 12 * k, 12 * k), brush_.Get(), 3 * k);
            }
            bool localDrag = dragging_ && dragWin_;
            ID2D1Bitmap* gbmp = localDrag ? dragBmp_.Get() : ghostBmp_.Get();
            POINT gpt = localDrag ? dragPt_ : ghostPt_;
            const std::wstring& gtitle = localDrag ? dragTitle_ : ghostTitle_;
            const std::wstring& gexe = localDrag ? dragExe_ : ghostExe_;
            if (gbmp) {   // the window's own picture follows the mouse
                D2D1_SIZE_F bs = gbmp->GetSize();
                float sc = std::min(210 * k / std::max(1.0f, bs.width), 140 * k / std::max(1.0f, bs.height)), gw2 = bs.width * sc, gh2 = bs.height * sc;
                D2D1_RECT_F g2 = D2D1::RectF(gpt.x - gw2 / 2, gpt.y - gh2 / 2, gpt.x + gw2 / 2, gpt.y + gh2 / 2);
                rt_->DrawBitmap(gbmp, g2, 0.92f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                brush_->SetColor(th.accent);
                rt_->DrawRectangle(g2, brush_.Get(), 2 * k);
            } else if (localDrag || ghostWin_) {
            float gw = 150 * k, gh = 96 * k;
            D2D1_RECT_F g = D2D1::RectF(gpt.x - gw / 2, gpt.y - gh / 2, gpt.x + gw / 2, gpt.y + gh / 2);
            brush_->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.92f));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(g, 10 * k, 10 * k), brush_.Get());
            brush_->SetColor(th.accent);
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(g, 10 * k, 10 * k), brush_.Get(), 2 * k);
            int px = (int)std::lround(32 * k);
            if (ID2D1Bitmap* b = IconFor(gexe, px))
                DrawImageHQ(rt_.Get(), b, D2D1::RectF(gpt.x - 16 * k, g.top + 14 * k, gpt.x + 16 * k, g.top + 46 * k), 1.0f);
            const std::wstring& t = gtitle;
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
            rt_->DrawTextW(t.c_str(), (UINT32)t.size(), font_.Get(), D2D1::RectF(g.left + 8 * k, g.top + 56 * k, g.right - 8 * k, g.bottom - 6 * k),
                           brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (items.empty() && settled) {
            std::wstring t = TR(L"Ingen åpne vinduer på denne skjermen", L"No open windows on this screen");
            brush_->SetColor(D2D1::ColorF(1, 1, 1, 0.8f));
            rt_->DrawTextW(t.c_str(), (UINT32)t.size(), font_.Get(), D2D1::RectF(0, sz.height / 2 - 20, sz.width, sz.height / 2 + 20), brush_.Get());
        }
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); snapBmps_.clear(); dragBmp_.Reset(); }
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
            else if (wp == T_SNAP) {   // the background pictures are ready: show them (twice, for the slow ones)
                self->snapBmps_.clear();
                InvalidateRect(h, nullptr, FALSE);
                if (++self->snapTick_ >= 3) KillTimer(h, T_SNAP);
                else SetTimer(h, T_SNAP, 700, nullptr);
            }
            else if (wp == T_RECOLLECT) {   // a window came back to this workspace: put it in the grid too
                KillTimer(h, T_RECOLLECT);
                if (!self->closing_) {
                    self->thumbs_.Collect(self->mon_, h);
                    self->thumbs_.Layout(self->area_, self->dpi_ / 96.0f);
                    self->thumbs_.Step(1);
                    self->RefreshSpaces();
                    self->hover_ = self->sel_ = -1;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
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
        case WM_LBUTTONDOWN:
            if (!self->closing_ && !self->spaces_.empty()) {   // might become a drag of a window onto a workspace card
                int hv = self->thumbs_.HitTest(p);
                if (hv >= 0) {   // a window in the grid: it belongs to the workspace on show
                    const auto& it = self->thumbs_.Items()[hv];
                    self->dragWin_ = it.h; self->dragTitle_ = it.title; self->dragExe_ = it.exe;
                    self->dragFrom_ = 0;
                    self->dragSrcMon_ = self->mon_;
                    for (const auto& sp : self->spaces_) if (sp.current) self->dragFrom_ = sp.n;
                    if (!EnhancedIsTiled(it.h)) self->dragFrom_ = 0;   // minimized / floating: not in any workspace yet, so the one on show is a fine target too
                    self->pressed_ = 0; self->pressPt_ = p; self->dragging_ = false; SetCapture(h);
                } else {         // a little window inside a workspace card
                    int cc = -1;
                    HWND w = self->WinInCard(p, &cc);
                    if (w) {
                        wchar_t t[256] = {};
                        GetWindowTextW(w, t, 256);
                        DWORD pid = 0;
                        GetWindowThreadProcessId(w, &pid);
                        self->dragWin_ = w; self->dragTitle_ = t; self->dragExe_ = ProcessImagePath(pid);
                        self->dragFrom_ = self->spaces_[cc].n;
                        self->dragSrcMon_ = self->MonAtCard(p, cc);
                        if (self->spaces_[cc].current && !EnhancedIsTiled(w)) self->dragFrom_ = 0;
                        self->pressed_ = 0; self->pressPt_ = p; self->dragging_ = false; SetCapture(h);
                    }
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (self->dragging_ || self->pressed_ >= 0) { self->pressed_ = -1; self->dragging_ = false; self->dropCard_ = -1; self->dragWin_ = nullptr; self->dragBmp_.Reset(); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_SETCURSOR:
            if (self->dragging_) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
            break;
        case WM_MOUSEMOVE: {
            if (self->pressed_ >= 0) {
                float k = self->dpi_ / 96.0f;
                if (!self->dragging_ && (abs(p.x - self->pressPt_.x) > 8 * k || abs(p.y - self->pressPt_.y) > 8 * k)) { self->dragging_ = true; self->MakeDragBmp(); }
                if (self->dragging_) {
                    self->dragPt_ = p;
                    RECT cr0; GetClientRect(h, &cr0);
                    bool inside = PtInRect(&cr0, p) != 0;
                    int ci = inside ? self->CardAt(p) : -1;
                    HMONITOR tm = ci >= 0 ? self->MonAtCard(p, ci) : self->dragSrcMon_;
                    OverlayHint(h, false, self->dragWin_, self->dragTitle_, self->dragExe_);   // (other screens: light up what is under the mouse there)
                    bool ok = ci == -2 || (ci >= 0 && ci < (int)self->spaces_.size() && (self->spaces_[ci].n != self->dragFrom_ || tm != self->dragSrcMon_));   // another workspace, or another screen
                    int newCard = ok ? ci : -1;   // -2: the "+" (a new workspace)
                    HMONITOR newMon = ok && ci >= 0 ? tm : nullptr;
                    bool changed = newCard != self->dropCard_ || newMon != self->dropMon_;
                    self->dropCard_ = newCard;
                    self->dropMon_ = newMon;
                    if (inside || changed) InvalidateRect(h, nullptr, FALSE);   // (over another screen this one needs no new picture: the other one draws the window)
                    return 0;
                }
            }
            int hv = self->thumbs_.HitTest(p);
            int ch = self->CardAt(p);
            bool ph = ch == -2, xh = self->OnX(p);
            if (ph) ch = -1;
            if (hv != self->hover_ || ch != self->cardHover_ || ph != self->plusHover_ || xh != self->xHover_) { self->hover_ = hv; self->cardHover_ = ch; self->plusHover_ = ph; self->xHover_ = xh; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->pressed_ >= 0) {
                bool wasDrag = self->dragging_;
                HWND w = self->dragWin_;
                int from = self->dragFrom_;
                HMONITOR srcMon = self->dragSrcMon_;
                self->pressed_ = -1;
                self->dragging_ = false;
                self->dropCard_ = -1;
                self->dragWin_ = nullptr;
                self->dragBmp_.Reset();
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
                OverlayHint(h, true);
                if (wasDrag) {   // dropped: onto another workspace card = move the window there; anywhere else = nothing
                    RECT cr0; GetClientRect(h, &cr0);
                    if (!PtInRect(&cr0, p)) {   // let go over another screen's Mission Control
                        POINT scr{}; GetCursorPos(&scr);
                        int on = 0; HMONITOR otm = nullptr; bool plus = false;
                        if (w && !self->closing_ && DropOnOther(h, scr, &on, &otm, &plus)) {
                            if (plus && self->spaces_.size() < 8 && EnhancedAddSpace()) {
                                self->RefreshSpaces();
                                on = self->spaces_.empty() ? 0 : self->spaces_.back().n;
                            } else if (plus) on = 0;
                            if (on > 0 && EnhancedMoveToSpaceOn(w, on, otm)) {
                                self->thumbs_.Remove(w);
                                self->thumbs_.Layout(self->area_, self->dpi_ / 96.0f);
                                self->thumbs_.Step(1);
                                self->snapBmps_.erase(w);
                                self->hover_ = self->sel_ = -1;
                                RefreshAllOverlays();
                                SetTimer(h, T_RECOLLECT, 250, nullptr);
                            }
                        }
                        return 0;
                    }
                    int ci = self->CardAt(p);
                    HMONITOR tm = ci >= 0 ? self->MonAtCard(p, ci) : srcMon;
                    if (ci == -2 && w && !self->closing_ && self->spaces_.size() < 8 && EnhancedAddSpace()) {   // onto the "+": a new workspace, and the window goes there
                        self->RefreshSpaces();
                        self->LayoutCards();
                        RefreshAllOverlays();
                        ci = (int)self->spaces_.size() - 1;
                        tm = srcMon;
                    }
                    if (w && ci >= 0 && ci < (int)self->spaces_.size() && (self->spaces_[ci].n != from || tm != srcMon) && !self->closing_) {
                        if (EnhancedMoveToSpaceOn(w, self->spaces_[ci].n, tm)) {
                            bool toCurrent = self->spaces_[ci].current;
                            self->thumbs_.Remove(w);
                            self->thumbs_.Layout(self->area_, self->dpi_ / 96.0f);
                            self->thumbs_.Step(1);
                            self->RefreshSpaces();
                            self->snapBmps_.erase(w);
                            self->hover_ = self->sel_ = -1;
                            self->LayoutCards();
                            RefreshAllOverlays();   // the other screens' cards show this window too
                            if (toCurrent) SetTimer(h, T_RECOLLECT, 250, nullptr);   // it needs a moment to come back before the grid can show it
                        }
                    }
                    return 0;
                }
            }
            if (self->closing_) return 0;
            if (self->OnX(p)) { self->RemoveSpace(); return 0; }
            if (int ci = self->CardAt(p); ci == -2) { self->AddSpace(); return 0; }
            else if (ci >= 0) { self->PickSpace(ci); return 0; }
            int hv = self->thumbs_.HitTest(p);
            CloseAll(hv >= 0 ? self->thumbs_.Items()[hv].h : nullptr);
            return 0;
        }
        case WM_RBUTTONUP: if (!self->closing_) CloseAll(nullptr); return 0;
        case WM_KEYDOWN: {
            if (self->closing_) return 0;
            int n = (int)self->thumbs_.Items().size();
            if (wp == VK_ESCAPE) CloseAll(nullptr);
            else if (!self->spaces_.empty() && wp >= '1' && wp <= '8') self->PickSpace((int)(wp - '1'));
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
    std::vector<EnhancedSpaceInfo> spaces_;       // the workspace strip (empty: Enhanced Tiling is off)
    SIZE waSz_{};
    std::vector<D2D1_RECT_F> cards_;
    D2D1_RECT_F plusRc_{};
    float cardH_ = 0;
    int stripBottom_ = 0;
    bool plusHover_ = false, xHover_ = false, canRemove_ = false;
    int cardHover_ = -1;
    RECT area_{};                    // where the window grid sits
    int pressed_ = -1, dropCard_ = -1;   // mouse is down on a window (0) / the card it would land on
    bool dragging_ = false;
    int snapTick_ = 0;
    HMONITOR dragSrcMon_ = nullptr, dropMon_ = nullptr;   // the screen the dragged window is on / the screen it would land on
    std::map<HWND, std::wstring> exeOf_;
    ComPtr<ID2D1Bitmap> dragBmp_;      // the dragged window's picture
    HWND dragWin_ = nullptr;             // the window being dragged (from the grid or from a card)
    int dragFrom_ = 0;                   // the workspace number it comes from
    std::wstring dragTitle_, dragExe_;
    POINT pressPt_{}, dragPt_{};
    std::map<HWND, ComPtr<ID2D1Bitmap>> snapBmps_;
    ULONGLONG start_ = 0;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> font_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bmps_;
};

std::map<HMONITOR, std::unique_ptr<Overlay>> g_overlays;   // one per screen

void RefreshAllOverlays() { for (auto& [m, o] : g_overlays) o->RefreshNow(); }

void OverlayHint(HWND src, bool clear, HWND w, const std::wstring& title, const std::wstring& exe) {
    POINT scr{};
    GetCursorPos(&scr);
    for (auto& [m, o] : g_overlays) {
        HWND oh = o->Hwnd();
        if (!oh || oh == src) continue;
        RECT wr{};
        GetWindowRect(oh, &wr);
        if (!clear && PtInRect(&wr, scr)) o->HintAt(scr, w, title, exe); else { o->SetHint(-1, nullptr); o->ClearGhost(); }
    }
}

bool DropOnOther(HWND src, POINT scr, int* n, HMONITOR* tm, bool* plus) {
    for (auto& [m, o] : g_overlays) {
        HWND oh = o->Hwnd();
        if (!oh || oh == src) continue;
        RECT wr{};
        GetWindowRect(oh, &wr);
        if (PtInRect(&wr, scr)) return o->DropAt(scr, n, tm, plus);
    }
    return false;
}

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
