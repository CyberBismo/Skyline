// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// High-quality app icons — see icons.h
#include "icons.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <d2d1_1.h>
#include <map>

// Icons can also be loaded on a background thread, which brings its own WIC factory.
static thread_local IWICImagingFactory* t_wic = nullptr;
void SetThreadWicFactory(IWICImagingFactory* f) { t_wic = f; }
static IWICImagingFactory* Wic() { return t_wic ? t_wic : g_gfx.wic.Get(); }

namespace {

struct GroupName { std::wstring str; WORD id = 0; bool found = false; };

BOOL CALLBACK FirstGroup(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR lp) {
    auto* g = reinterpret_cast<GroupName*>(lp);
    if (IS_INTRESOURCE(name)) g->id = (WORD)(ULONG_PTR)name;
    else g->str = name;
    g->found = true;
    return FALSE;   // the first group is the one Explorer shows
}

// The biggest icon image stored in the exe/dll (PNG-compressed 256 px included).
HICON LargestResourceIcon(const std::wstring& path) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return nullptr;
    HICON out = nullptr;
    GroupName g;
    EnumResourceNamesW(m, RT_GROUP_ICON, FirstGroup, (LONG_PTR)&g);
    if (g.found) {
        LPCWSTR name = g.str.empty() ? MAKEINTRESOURCEW(g.id) : g.str.c_str();
        HRSRC r = FindResourceW(m, name, RT_GROUP_ICON);
        HGLOBAL h = r ? LoadResource(m, r) : nullptr;
        auto* dir = h ? static_cast<BYTE*>(LockResource(h)) : nullptr;
        if (dir) {
            int id = LookupIconIdFromDirectoryEx(dir, TRUE, 256, 256, LR_DEFAULTCOLOR);
            HRSRC ri = id ? FindResourceW(m, MAKEINTRESOURCEW(id), RT_ICON) : nullptr;
            HGLOBAL hi = ri ? LoadResource(m, ri) : nullptr;
            auto* data = hi ? static_cast<BYTE*>(LockResource(hi)) : nullptr;
            DWORD size = ri ? SizeofResource(m, ri) : 0;
            // cx/cy = 0 without LR_DEFAULTSIZE: keep the image's real size
            if (data && size) out = CreateIconFromResourceEx(data, size, TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
        }
    }
    FreeLibrary(m);
    return out;
}

ComPtr<IWICBitmap> ToBitmap(IWICBitmapSource* src) {
    ComPtr<IWICBitmap> out;
    if (!src) return out;
    ComPtr<IWICFormatConverter> conv;
    Wic()->CreateFormatConverter(conv.Put());
    if (!conv || FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeMedianCut)))
        return out;
    Wic()->CreateBitmapFromSource(conv.Get(), WICBitmapCacheOnLoad, out.Put());
    return out;
}

// Some icons are a 48 px picture centred in a transparent 256 px square.
// Find the visible part and crop to it, so it fills the slot like other icons.
ComPtr<IWICBitmapSource> CropPadding(IWICBitmap* bmp) {
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    if (w < 64 || h < 64) return {};
    WICRect all{0, 0, (INT)w, (INT)h};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bmp->Lock(&all, WICBitmapLockRead, lock.Put()))) return {};
    UINT stride = 0, bytes = 0;
    BYTE* px = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&bytes, &px);
    int minx = (int)w, miny = (int)h, maxx = -1, maxy = -1;
    for (UINT y = 0; y < h; ++y) {
        const BYTE* row = px + y * stride;
        for (UINT x = 0; x < w; ++x)
            if (row[x * 4 + 3] > 12) {
                minx = std::min(minx, (int)x); maxx = std::max(maxx, (int)x);
                miny = std::min(miny, (int)y); maxy = std::max(maxy, (int)y);
            }
    }
    lock.Reset();
    if (maxx < 0) return {};
    int bw = maxx - minx + 1, bh = maxy - miny + 1;
    int side = std::max(bw, bh);
    if (side >= (int)(w * 0.62f)) return {};          // already fills the square
    side = std::min((int)w, (int)(side * 1.06f) + 2);  // a hair of breathing room
    int cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    WICRect r{std::clamp(cx - side / 2, 0, (int)w - side), std::clamp(cy - side / 2, 0, (int)h - side), side, side};
    ComPtr<IWICBitmapClipper> clip;
    Wic()->CreateBitmapClipper(clip.Put());
    if (!clip || FAILED(clip->Initialize(bmp, &r))) return {};
    ComPtr<IWICBitmapSource> out;
    clip->QueryInterface(IID_PPV_ARGS(out.Put()));
    return out;
}

ComPtr<IWICBitmapSource> Finish(IWICBitmapSource* src, int px) {
    ComPtr<IWICBitmapSource> result;
    ComPtr<IWICBitmap> bmp = ToBitmap(src);
    if (!bmp) return result;
    ComPtr<IWICBitmapSource> cropped = CropPadding(bmp.Get());
    IWICBitmapSource* use = cropped ? cropped.Get() : static_cast<IWICBitmapSource*>(bmp.Get());
    UINT w = 0, h = 0;
    use->GetSize(&w, &h);
    if ((int)w == px && (int)h == px) {
        use->QueryInterface(IID_PPV_ARGS(result.Put()));
        return result;
    }
    ComPtr<IWICBitmapScaler> sc;
    Wic()->CreateBitmapScaler(sc.Put());
    if (!sc) return result;
    if (FAILED(sc->Initialize(use, (UINT)px, (UINT)px, (WICBitmapInterpolationMode)4 /* HighQualityCubic (Win10+) */)) &&
        FAILED(sc->Initialize(use, (UINT)px, (UINT)px, WICBitmapInterpolationModeFant)))
        return result;
    // Cache the scaled pixels so drawing never rescales.
    ComPtr<IWICBitmap> done = ToBitmap(sc.Get());
    if (done) done->QueryInterface(IID_PPV_ARGS(result.Put()));
    return result;
}

bool IsExe(const std::wstring& t) {
    std::wstring l = ToLower(t);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, L".exe") == 0 || l.compare(l.size() - 4, 4, L".dll") == 0);
}

} // namespace

// ---------------------------------------------------------------------------
ComPtr<IWICBitmapSource> ImageFromIcon(HICON icon, int px) {
    ComPtr<IWICBitmapSource> out;
    if (!icon || !Wic()) return out;
    ComPtr<IWICBitmap> wb;
    if (FAILED(Wic()->CreateBitmapFromHICON(icon, wb.Put()))) return out;
    return Finish(wb.Get(), px);
}

ComPtr<IWICBitmapSource> LoadAppImage(const std::wstring& target, int px) {
    ComPtr<IWICBitmapSource> out;
    if (target.empty() || !Wic() || px <= 0) return out;

    // 1. Win32 program: its own largest icon
    if (IsExe(target)) {
        if (HICON big = LargestResourceIcon(target)) {
            ICONINFO ii{};
            int size = 0;
            if (GetIconInfo(big, &ii)) {
                BITMAP bm{};
                if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm)) size = bm.bmWidth;
                else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof bm, &bm)) size = bm.bmWidth;
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
            }
            if (size >= 32) out = ImageFromIcon(big, px);
            DestroyIcon(big);
            if (out) return out;
        }
    }

    // 2. Anything the shell knows (Store apps, shortcuts, exes without resources)
    ComPtr<IShellItemImageFactory> f;
    if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(f.Put())))) {
        int ask = px <= 48 ? 96 : 256;
        HBITMAP hb = nullptr;
        if (SUCCEEDED(f->GetImage({ask, ask}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
            ComPtr<IWICBitmap> wb;
            if (SUCCEEDED(Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, wb.Put())))
                out = Finish(wb.Get(), px);
            DeleteObject(hb);
        }
    }
    return out;
}

void DrawImageHQ(ID2D1RenderTarget* rt, ID2D1Bitmap* bmp, D2D1_RECT_F d, float opacity) {
    if (!rt || !bmp) return;
    // Snap to whole device pixels: half-pixel positions are what make icons soft.
    float dx = 96.0f, dy = 96.0f;
    rt->GetDpi(&dx, &dy);
    float s = dx / 96.0f;
    auto snap = [s](float v) { return std::round(v * s) / s; };
    float w = snap(d.right - d.left), h = snap(d.bottom - d.top);
    d.left = snap(d.left);
    d.top = snap(d.top);
    d.right = d.left + w;
    d.bottom = d.top + h;

    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(rt->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(dc.Put()))) && dc) {
        D2D1_SIZE_U src = bmp->GetPixelSize();
        bool exact = std::fabs(src.width - w * s) < 0.5f && std::fabs(src.height - h * s) < 0.5f;
        dc->DrawBitmap(bmp, &d, opacity,
                       exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                       nullptr, nullptr);
    } else {
        rt->DrawBitmap(bmp, d, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

ComPtr<IWICBitmapSource> CachedAppImage(const std::wstring& target, int px) {
    static std::map<std::pair<std::wstring, int>, ComPtr<IWICBitmapSource>> cache;
    auto key = std::make_pair(ToLower(target), px);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 200) cache.clear();
    auto img = LoadAppImage(target, px);
    cache[key] = img;   // remember misses too
    return img;
}
