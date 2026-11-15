// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Live window previews above a dock icon (hover). The thumbnails are drawn by
// DWM itself (DwmRegisterThumbnail): live, GPU-composited, no copying of pixels.
class DockPreview {
public:
    ~DockPreview();
    // Shows previews of `wins` centred above `anchorX`, with the bottom at `bottomY` (screen px).
    void Show(HWND owner, const std::vector<HWND>& wins, IWICBitmapSource* icon, int anchorX, int bottomY, UINT dpi);
    void Hide();
    void Restyle();   // theme/glass changed
    bool Visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    bool MouseInside() const { return inside_; }
    HWND Hwnd() const { return hwnd_; }

    static constexpr UINT WM_PREVIEW_LEFT = WM_APP + 31;   // sent to the owner when the mouse leaves

private:
    struct Item {
        HWND win = nullptr;
        std::wstring title;
        D2D1_RECT_F slot{};     // whole cell (DIPs)
        D2D1_RECT_F thumb{};    // thumbnail area (DIPs)
        HTHUMBNAIL th = nullptr;
        bool iconic = false;
    };
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    bool Create();
    void Clear();
    void Paint();
    int HitTest(POINT client) const;
    D2D1_RECT_F CloseRect(int i) const;   // the × on a cell (DIPs), above the thumbnail
    void CloseAt(int i);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    UINT dpi_ = 96;
    std::vector<Item> items_;
    int hover_ = -1;
    bool hoverClose_ = false;
    int anchorX_ = 0, bottomY_ = 0;
    bool inside_ = false;
    bool tracking_ = false;
    ComPtr<IWICBitmapSource> iconSrc_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> font_;
    ComPtr<ID2D1Bitmap> icon_;
};
