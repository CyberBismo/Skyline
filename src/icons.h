// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Sharp icons for apps.
//   Win32 exes: the largest icon the exe actually contains (up to 256 px),
//   not the 32/48 px one Windows hands out by default.
//   Store apps / shortcuts: the shell's 256 px image.
//   Icons that are only a small picture padded inside a big square are cropped.
// The result is scaled once to `px` pixels with a high-quality cubic filter.
ComPtr<IWICBitmapSource> LoadAppImage(const std::wstring& target, int px);

// Same filter for an HICON (tray icons, window icons).
ComPtr<IWICBitmapSource> ImageFromIcon(HICON icon, int px);

// Draw with high-quality interpolation, snapped to the pixel grid.
void DrawImageHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, D2D1_RECT_F dest, float opacity = 1.0f);

// LoadAppImage with a small cache (top bar: the same apps come back all the time).
ComPtr<IWICBitmapSource> CachedAppImage(const std::wstring& target, int px);

// For a background thread: use this WIC factory for icons loaded on the calling thread.
void SetThreadWicFactory(IWICImagingFactory* f);
