// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// SkyGarden — see garden.h
#include "garden.h"
#include "app.h"
#include "applist.h"
#include "icons.h"
#include "settings.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <map>
#include <set>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace {

constexpr float kTitle = 30, kPad = 8, kCellW = 84, kCellH = 80, kIcon = 38, kGap = 10;
constexpr wchar_t kBedClass[] = L"SkylineGardenBed";
const wchar_t* kRecycleBin = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

// ---------------------------------------------------------------------------
// Desktop items
// ---------------------------------------------------------------------------
struct Item {
    std::wstring path, name;
    bool dir = false;
    AppEntry entry;   // for the icon loader
};
std::map<std::wstring, Item> g_items;   // key: lowercase path
HANDLE g_watch[2] = {nullptr, nullptr};

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { out = p; CoTaskMemFree(p); }
    return out;
}

bool IsShortcut(const std::wstring& path) {
    std::wstring l = ToLower(path);
    auto ends = [&](const wchar_t* e) { size_t n = wcslen(e); return l.size() > n && l.compare(l.size() - n, n, e) == 0; };
    return ends(L".lnk") || ends(L".url") || ends(L".exe") || ends(L".appref-ms");
}

Item MakeItem(const std::wstring& path, bool dir) {
    Item it;
    it.path = path;
    it.dir = dir;
    std::wstring file = FileNameOf(path);
    std::wstring l = ToLower(file);
    if (!dir && (l.size() > 4 && (l.compare(l.size() - 4, 4, L".lnk") == 0 || l.compare(l.size() - 4, 4, L".url") == 0)))
        file = file.substr(0, file.size() - 4);   // like Explorer: shortcuts without extension
    it.name = file;
    it.entry.key = L"garden:" + ToLower(path);
    it.entry.target = path;
    it.entry.name = it.name;
    if (!dir && l.size() > 4 && l.compare(l.size() - 4, 4, L".exe") == 0) { it.entry.exe = path; it.entry.direct = true; }
    return it;
}

void ScanDesktop() {
    g_items.clear();
    for (const auto& dir : {KnownFolder(FOLDERID_Desktop), KnownFolder(FOLDERID_PublicDesktop)}) {
        if (dir.empty()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            g_items[ToLower(p)] = MakeItem(p, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    Item bin;
    bin.path = kRecycleBin;
    bin.name = TR(L"Papirkurv", L"Recycle Bin");
    bin.dir = true;
    bin.entry.key = L"garden:recyclebin";
    bin.entry.target = kRecycleBin;
    bin.entry.name = bin.name;
    g_items[ToLower(kRecycleBin)] = bin;
}

void WatchDesktop() {
    std::wstring dirs[2] = {KnownFolder(FOLDERID_Desktop), KnownFolder(FOLDERID_PublicDesktop)};
    for (int i = 0; i < 2; ++i) {
        if (g_watch[i]) continue;
        if (dirs[i].empty()) continue;
        HANDLE h = FindFirstChangeNotificationW(dirs[i].c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        g_watch[i] = h == INVALID_HANDLE_VALUE ? nullptr : h;
    }
}

void UnwatchDesktop() {
    for (auto& h : g_watch) if (h) { FindCloseChangeNotification(h); h = nullptr; }
}

// ---------------------------------------------------------------------------
// Windows' own desktop icons
// ---------------------------------------------------------------------------
HWND DesktopDefView(HWND* host = nullptr) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND dv = progman ? FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    HWND h = progman;
    if (!dv) {
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
            dv = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
            if (dv) { h = w; break; }
        }
    }
    if (host) *host = h;
    return dv;
}

// Explorer's own "View > Show desktop icons" setting: never turn icons on if the user has them off
bool UserHidesIcons() {
    SHELLSTATEW ss{};
    SHGetSetSettings(&ss, SSF_HIDEICONS, FALSE);
    return ss.fHideIcons != 0;
}

void ShowDesktopIcons(bool show) {
    if (show && UserHidesIcons()) return;
    HWND dv = DesktopDefView();
    HWND lv = dv ? FindWindowExW(dv, nullptr, L"SysListView32", nullptr) : nullptr;
    if (lv && (IsWindowVisible(lv) != 0) != show) ShowWindow(lv, show ? SW_SHOWNA : SW_HIDE);
}

// The window right above the desktop: beds go there (below every normal window,
// above the wallpaper). After Win+D the desktop is on top, so beds follow it up.
HWND InsertAfterForBeds(const std::set<HWND>& beds) {
    HWND host = nullptr;
    DesktopDefView(&host);
    if (!host) return nullptr;   // Explorer is restarting: leave things as they are
    HWND prev = GetWindow(host, GW_HWNDPREV);
    while (prev && beds.count(prev)) prev = GetWindow(prev, GW_HWNDPREV);
    return prev ? prev : HWND_TOP;
}

// ---------------------------------------------------------------------------
// Persistence: garden.ini next to the exe
// ---------------------------------------------------------------------------
std::wstring GardenIni() {
    std::wstring p = ExeDir() + L"\\garden.ini";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w;
            WORD bom = 0xFEFF;
            WriteFile(f, &bom, 2, &w, nullptr);   // UTF-16: bed names can be anything
            CloseHandle(f);
        }
    }
    return p;
}

std::wstring IniStr(const std::wstring& ini, const wchar_t* sec, const wchar_t* key) {
    std::vector<wchar_t> buf(65536);
    GetPrivateProfileStringW(sec, key, L"", buf.data(), (DWORD)buf.size(), ini.c_str());
    return buf.data();
}

class Bed;
std::vector<std::unique_ptr<Bed>> g_beds;
UINT_PTR g_timer = 0;
bool g_running = false;
UINT_PTR g_saveTimer = 0;
void ScheduleSave();
void Distribute();
std::set<HWND> BedWindows();
Bed* BedAt(POINT screen);
void DeleteBed(Bed* b);

int g_modal = 0;   // a bed is inside a menu or a shell dialog: don't destroy beds now

// Dragging an item between beds
struct Drag {
    bool active = false;
    Bed* from = nullptr;
    std::wstring key;
    Bed* target = nullptr;
    int index = -1;
    std::vector<std::wstring> keys;   // everything being dragged (the selection), in bed order
} g_drag;

// Files dragged in from Explorer (Downloads, Documents …): where they would land
struct OleHover { Bed* target = nullptr; int index = -1; } g_ole;
IDropTarget* NewBedDrop(Bed* bed);   // below the Bed class
constexpr UINT kMsgDropFiles = WM_APP + 7;

// ---------------------------------------------------------------------------
// A bed
// ---------------------------------------------------------------------------
class Bed {
public:
    std::wstring name;
    RECT rc{};                 // screen pixels (expanded size)
    bool collapsed = false;
    bool catchAll = false;     // new desktop items land here
    std::vector<std::wstring> keys;
    HWND hwnd = nullptr;

    bool Create() {
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = Proc;
            wc.hInstance = g_app.hinst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kBedClass;
            RegisterClassExW(&wc);
            reg = true;
        }
        int h = collapsed ? TitlePx() : rc.bottom - rc.top;
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBedClass, name.c_str(), WS_POPUP,
                               rc.left, rc.top, rc.right - rc.left, h, nullptr, nullptr, g_app.hinst, this);
        if (!hwnd) return false;
        dpi_ = GetDpiForWindow(hwnd);
        if (collapsed) SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, TitlePx(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyGlass();
        BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof exclude);
        ShowWindow(hwnd, SW_SHOWNA);
        Place();
        if (IDropTarget* dt = NewBedDrop(this)) {   // files from anywhere on the PC can be dropped in
            if (SUCCEEDED(RegisterDragDrop(hwnd, dt))) dropReg_ = true;
            dt->Release();
        }
        return true;
    }

    ~Bed();

    // ---- files dropped in from Explorer ----
    struct PendingDrop { std::vector<std::wstring> files; DWORD effect; int index; };
    void QueueDrop(std::vector<std::wstring> files, DWORD effect, int index) {
        // done a moment later, after the drag has ended: a long copy must not hold up Explorer's drag
        pending_.push_back({std::move(files), effect, index});
        PostMessageW(hwnd, kMsgDropFiles, 0, 0);
    }

    void ApplyGlass() { SetGlass(false); rt_.Reset(); bmps_.clear(); InvalidateRect(hwnd, nullptr, FALSE); }

    // cheap = plain blur while the bed is being moved/resized: acrylic re-renders on
    // every step and makes dragging lag and stick.
    void SetGlass(bool cheap) {
        const Theme& t = g_app.theme;
        BOOL dark = t.dark;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
        MARGINS m{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd, &m);
        DWORD corner = 2;
        DwmSetWindowAttribute(hwnd, 33, &corner, sizeof corner);
        DWORD noBackdrop = 1;   // no Mica/frame colour behind us: only our glass
        DwmSetWindowAttribute(hwnd, 38, &noBackdrop, sizeof noBackdrop);
        // Acrylic (tint inside the glass) or see-through (tint painted in Paint). Moving: plain blur, faster.
        ApplyGlassEffect(hwnd, Part::Garden, cheap);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    // Keep the bed on the desktop layer
    void Place() {
        if (!hwnd || moving_ || sizing_) return;
        auto beds = BedWindows();
        HWND after = InsertAfterForBeds(beds);
        if (!after || after == hwnd) return;
        // already sitting in the bed group right above the desktop? nothing to do
        HWND host = nullptr;
        DesktopDefView(&host);
        HWND w = GetWindow(hwnd, GW_HWNDNEXT);
        while (w && beds.count(w)) w = GetWindow(w, GW_HWNDNEXT);
        HWND prev = GetWindow(hwnd, GW_HWNDPREV);
        while (prev && beds.count(prev)) prev = GetWindow(prev, GW_HWNDPREV);
        if (w == host && prev == (after == HWND_TOP ? nullptr : after)) return;
        SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    int TitlePx() const { return MulDiv((int)kTitle, dpi_ ? dpi_ : 96, 96); }
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    // Index where a dropped item would go (screen point)
    int IndexAt(POINT screen) {
        POINT c = screen;
        ScreenToClient(hwnd, &c);
        float x = c.x * 96.0f / dpi_, y = c.y * 96.0f / dpi_ + scroll_;
        if (cols_ <= 0) return (int)Visible().size();
        int col = std::clamp((int)((x - gridLeft_) / kCellW + 0.5f), 0, cols_);
        int row = std::max(0, (int)((y - kTitle - kPad / 2) / kCellH));
        int i = row * cols_ + std::min(col, cols_ - 1) + (col >= cols_ ? 1 : 0);
        return std::clamp(i, 0, (int)Visible().size());
    }

    std::vector<std::wstring> Visible() const {
        std::vector<std::wstring> v;
        for (const auto& k : keys) if (g_items.count(k)) v.push_back(k);
        return v;
    }

    void BedMenu(POINT screen);
    void Rename();

    void ToggleCollapse() {
        collapsed = !collapsed;
        animFrom_ = (float)CurrentHeight();
        animTo_ = (float)(collapsed ? TitlePx() : rc.bottom - rc.top);
        animStart_ = GetTickCount();
        SetTimer(hwnd, 1, 10, nullptr);
        ScheduleSave();
    }

private:
    int CurrentHeight() const { RECT r; GetWindowRect(hwnd, &r); return r.bottom - r.top; }

    void Layout(float W, float H) {
        cols_ = std::max(1, (int)((W - 2 * kPad) / kCellW));
        gridLeft_ = (W - cols_ * kCellW) / 2;
        int n = (int)Visible().size();
        int rows = (n + cols_ - 1) / cols_;
        contentH_ = rows * kCellH + kPad;
        viewH_ = H - kTitle;
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - viewH_));
    }

    D2D1_RECT_F Cell(int i) const {
        float x = gridLeft_ + (i % cols_) * kCellW, y = kTitle + kPad / 2 + (i / cols_) * kCellH - scroll_;
        return D2D1::RectF(x, y, x + kCellW, y + kCellH);
    }

    int HitItem(float x, float y) const {
        if (collapsed || y < kTitle) return -1;
        int n = (int)Visible().size();
        for (int i = 0; i < n; ++i) {
            D2D1_RECT_F c = Cell(i);
            if (x >= c.left + 4 && x < c.right - 4 && y >= c.top + 2 && y < c.bottom - 2) return i;
        }
        return -1;
    }

    ComPtr<IDWriteTextFormat> Fmt(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT al, bool wrap) {
        ComPtr<IDWriteTextFormat> f;
        g_gfx.dwrite->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
        if (!f) return f;
        f->SetTextAlignment(al);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ell;
        g_gfx.dwrite->CreateEllipsisTrimmingSign(f.Get(), ell.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ell.Get());
        return f;
    }

    void Paint() {
        if (!rt_) {
            RECT r;
            GetClientRect(hwnd, &r);
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                      (float)dpi_, (float)dpi_);
            if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(r.right, r.bottom)),
                                                         rt_.Put())))
                return;
            rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), brush_.Put());
            fTitle_ = Fmt(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, false);
            fLabel_ = Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
            std::wstring icons = L"Segoe MDL2 Assets";
            ComPtr<IDWriteFontCollection> fc;
            if (SUCCEEDED(g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE)) && fc) {
                UINT32 idx;
                BOOL ex = FALSE;
                if (SUCCEEDED(fc->FindFamilyName(L"Segoe Fluent Icons", &idx, &ex)) && ex) icons = L"Segoe Fluent Icons";
            }
            g_gfx.dwrite->CreateTextFormat(icons.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 10, L"", fGlyph_.Put());
            if (fGlyph_) {
                fGlyph_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fGlyph_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            bmps_.clear();
        }
        const Theme& th = g_app.theme;
        D2D1_SIZE_F sz = rt_->GetSize();
        Layout(sz.width, collapsed ? kTitle : sz.height);
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        if (D2D1_COLOR_F tint = GlassTint(Part::Garden); tint.a > 0) {
            // see-through: the tint, 0 % = only the glass, 100 % = solid
            brush_->SetColor(tint);
            rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, sz.height), brush_.Get());
        }
        PaintVibe(rt_.Get(), D2D1::RectF(0, 0, sz.width, collapsed ? kTitle : sz.height), Part::Garden);
        // Liquid Glass / Nordlys: each bed gets its own light (the seed shifts the colours a little per bed)
        PaintLiquid(rt_.Get(), D2D1::RectF(0, 0, sz.width, collapsed ? kTitle : sz.height), 8, 4, (int)(((UINT_PTR)this >> 6) % 4));

        // title
        if (titleHover_) {
            brush_->SetColor(th.hover);
            rt_->FillRectangle(D2D1::RectF(0, 0, sz.width, kTitle), brush_.Get());
        }
        brush_->SetColor(th.fg);
        if (fTitle_) rt_->DrawTextW(name.c_str(), (UINT32)name.size(), fTitle_.Get(), D2D1::RectF(14, 0, sz.width - 30, kTitle), brush_.Get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
        wchar_t chev = collapsed ? 0xE70D : 0xE70E;
        brush_->SetColor(titleHover_ ? th.fg : th.fgDim);
        if (fGlyph_) rt_->DrawTextW(&chev, 1, fGlyph_.Get(), D2D1::RectF(sz.width - 30, 0, sz.width - 8, kTitle), brush_.Get());

        if (!collapsed) {
            brush_->SetColor(th.hairline);
            rt_->FillRectangle(D2D1::RectF(10, kTitle - 0.5f, sz.width - 10, kTitle + 0.5f), brush_.Get());
            rt_->PushAxisAlignedClip(D2D1::RectF(0, kTitle + 1, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
            auto vis = Visible();
            int px = (int)std::lround(kIcon * dpi_ / 96.0f);
            bool oleHere = g_ole.target == this;
            bool dropHere = (g_drag.active && g_drag.target == this) || oleHere;
            int dropIndex = oleHere ? g_ole.index : g_drag.index;
            for (int i = 0; i < (int)vis.size(); ++i) {
                D2D1_RECT_F c = Cell(i);
                if (c.bottom < kTitle || c.top > sz.height) continue;
                const Item& it = g_items[vis[i]];
                bool dragged = g_drag.active && g_drag.from == this &&
                               std::find(g_drag.keys.begin(), g_drag.keys.end(), vis[i]) != g_drag.keys.end();
                bool selected = selected_.count(vis[i]) != 0;
                if ((i == hover_ || selected) && !g_drag.active) {
                    brush_->SetColor(selected ? D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.22f) : th.hover);
                    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 4, c.top + 2, c.right - 4, c.bottom - 2), 8, 8), brush_.Get());
                }
                float cx = (c.left + c.right) / 2;
                D2D1_RECT_F ir = D2D1::RectF(cx - kIcon / 2, c.top + 6, cx + kIcon / 2, c.top + 6 + kIcon);
                ID2D1Bitmap* b = Icon(it, px);
                if (b) DrawImageHQ(rt_.Get(), b, ir, dragged ? 0.35f : 1.0f);
                brush_->SetColor(dragged ? th.fgDim : th.fg);
                if (fLabel_)
                    rt_->DrawTextW(it.name.c_str(), (UINT32)it.name.size(), fLabel_.Get(),
                                   D2D1::RectF(c.left + 5, ir.bottom + 4, c.right - 5, c.bottom - 2), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            if (dropHere && cols_ > 0) {
                // insertion mark
                int i = dropIndex < 0 ? (int)vis.size() : dropIndex;
                D2D1_RECT_F c = Cell(std::max(0, std::min(i, (int)vis.size())));
                if (i >= (int)vis.size() && !vis.empty()) { c = Cell((int)vis.size() - 1); c.left = c.right; }
                brush_->SetColor(th.accent);
                rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left - 1.5f, c.top + 8, c.left + 1.5f, c.top + kCellH - 10), 1.5f, 1.5f),
                                          brush_.Get());
            }
            if (band_) {   // rubber-band selection
                D2D1_RECT_F b = D2D1::RectF(std::min(bandA_.x, bandB_.x), std::min(bandA_.y, bandB_.y) - scroll_,
                                            std::max(bandA_.x, bandB_.x), std::max(bandA_.y, bandB_.y) - scroll_);
                brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.16f));
                rt_->FillRectangle(b, brush_.Get());
                brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.75f));
                rt_->DrawRectangle(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), brush_.Get(), 1.0f);
            }
            rt_->PopAxisAlignedClip();
        }
        if ((g_drag.active && g_drag.target == this) || g_ole.target == this) {
            brush_->SetColor(D2D1::ColorF(th.accent.r, th.accent.g, th.accent.b, 0.8f));
            rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1, 1, sz.width - 1, sz.height - 1), 7, 7), brush_.Get(), 1.5f);
        }
        static int fails = 0;
        if (FAILED(rt_->EndDraw())) { rt_.Reset(); bmps_.clear(); if (++fails < 3) Invalidate(); } else fails = 0;
    }

    ID2D1Bitmap* Icon(const Item& it, int px) {
        auto f = bmps_.find(it.entry.key);
        if (f != bmps_.end()) return f->second.Get();
        if (!AppIconCached(it.entry, px)) { RequestIcon(it.entry, px, true); return nullptr; }
        ComPtr<ID2D1Bitmap> b;
        if (auto src = AppIcon(it.entry, px)) rt_->CreateBitmapFromWicBitmap(src.Get(), nullptr, b.Put());
        bmps_[it.entry.key] = b;
        return b.Get();
    }

    // ---- shell ----
    static bool OpenItem(const std::wstring& path) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
        SHELLEXECUTEINFOW sei{sizeof sei};
        sei.fMask = SEE_MASK_IDLIST;
        sei.lpIDList = pidl;
        sei.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&sei);
        CoTaskMemFree(pidl);
        return ok != FALSE;
    }

    // Windows' own right-click menu for one or more items. paths[0] is the one clicked; the others
    // join it when they live in the same folder (your desktop and the shared Public desktop are two).
    void ShellMenu(const std::vector<std::wstring>& paths, POINT screen) {
        if (paths.empty()) return;
        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const auto& p : paths) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) pidls.push_back(pidl);
            else if (pidls.empty()) return;   // the clicked one must work
        }
        PIDLIST_ABSOLUTE pidl = pidls[0];
        ComPtr<IShellFolder> parent;
        PCUITEMID_CHILD child = nullptr;
        if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(parent.Put()), &child))) {
            std::vector<PCUITEMID_CHILD> children{child};
            PIDLIST_ABSOLUTE folder = ILClone(pidl);
            if (folder) ILRemoveLastID(reinterpret_cast<PUIDLIST_RELATIVE>(folder));
            for (size_t k = 1; k < pidls.size() && folder; ++k) {
                PIDLIST_ABSOLUTE f2 = ILClone(pidls[k]);
                if (!f2) continue;
                ILRemoveLastID(reinterpret_cast<PUIDLIST_RELATIVE>(f2));
                if (ILIsEqual(folder, f2)) children.push_back(reinterpret_cast<PCUITEMID_CHILD>(ILFindLastID(pidls[k])));
                ILFree(f2);
            }
            if (folder) ILFree(folder);
            ComPtr<IContextMenu> cm;
            if (SUCCEEDED(parent->GetUIObjectOf(hwnd, (UINT)children.size(), children.data(), IID_IContextMenu, nullptr,
                                                reinterpret_cast<void**>(cm.Put())))) {
                HMENU m = CreatePopupMenu();
                UINT flags = CMF_NORMAL | ((GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0);
                if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, flags))) {
                    cm->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(cm2_.Put()));
                    cm->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(cm3_.Put()));
                    SetForegroundWindow(hwnd);
                    ++g_modal;
                    int cmd;
                    {
                        MenuGuard guard;   // no settings reload (and SkyGarden restart) while the menu is open
                        cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd, nullptr);
                    }
                    PostMessageW(hwnd, WM_NULL, 0, 0);
                    cm2_.Reset();
                    cm3_.Reset();
                    if (cmd > 0) {
                        CMINVOKECOMMANDINFOEX ici{sizeof ici};
                        ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
                        ici.hwnd = hwnd;
                        ici.lpVerb = MAKEINTRESOURCEA(cmd - 1);
                        ici.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
                        ici.nShow = SW_SHOWNORMAL;
                        ici.ptInvoke = screen;
                        cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
                    }
                    --g_modal;
                }
                DestroyMenu(m);
            }
        }
        for (auto p : pidls) CoTaskMemFree(p);
    }

    // Rubber band: the items under the rectangle (plus what was selected before, with Ctrl)
    void UpdateBand() {
        auto vis = Visible();
        float l = std::min(bandA_.x, bandB_.x), r = std::max(bandA_.x, bandB_.x);
        float t = std::min(bandA_.y, bandB_.y), b = std::max(bandA_.y, bandB_.y);
        selected_ = bandBase_;
        for (int i = 0; i < (int)vis.size(); ++i) {
            D2D1_RECT_F c = Cell(i);
            float ct = c.top + scroll_ + 2, cb = c.bottom + scroll_ - 2;   // content coordinates
            if (c.left + 4 < r && c.right - 4 > l && ct < b && cb > t) selected_.insert(vis[i]);
        }
        Invalidate();
    }

    // Delete: to the Recycle Bin (Shift+Delete: for good, Windows asks first). The desktop watcher sees the change.
    void DeleteSelected(bool permanent) {
        ComPtr<IFileOperation> op;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.Put())))) return;
        op->SetOwnerWindow(hwnd);
        op->SetOperationFlags(permanent ? FOF_NOCONFIRMMKDIR : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD));
        int n = 0;
        for (const auto& k : Visible()) {
            if (!selected_.count(k)) continue;
            const Item& it = g_items[k];
            if (it.path.compare(0, 2, L"::") == 0) continue;   // the Recycle Bin itself
            ComPtr<IShellItem> item;
            if (SUCCEEDED(SHCreateItemFromParsingName(it.path.c_str(), nullptr, IID_PPV_ARGS(item.Put())))) {
                op->DeleteItem(item.Get(), nullptr);
                ++n;
            }
        }
        if (!n) return;
        ++g_modal;
        op->PerformOperations();
        --g_modal;
        selected_.clear();
        Invalidate();
    }


    // Snap to the screen edges and other beds while moving
    void Snap(RECT* r) {
        int d = MulDiv(10, dpi_, 96), gap = MulDiv((int)kGap, dpi_, 96);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi);
        int w = r->right - r->left, h = r->bottom - r->top;
        std::vector<int> xs{(int)mi.rcWork.left + gap, (int)mi.rcWork.right - gap - w};
        std::vector<int> ys{(int)mi.rcWork.top + gap, (int)mi.rcWork.bottom - gap - h};
        for (auto& b : g_beds) {
            if (b.get() == this || !b->hwnd) continue;
            RECT o;
            GetWindowRect(b->hwnd, &o);
            xs.insert(xs.end(), {(int)o.right + gap, (int)o.left - gap - w, (int)o.left, (int)o.right - w});
            ys.insert(ys.end(), {(int)o.bottom + gap, (int)o.top - gap - h, (int)o.top, (int)o.bottom - h});
        }
        for (int x : xs) if (std::abs(r->left - x) <= d) { r->left = x; r->right = x + w; break; }
        for (int y : ys) if (std::abs(r->top - y) <= d) { r->top = y; r->bottom = y + h; break; }
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* b = static_cast<Bed*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)b);
            b->hwnd = h;
        }
        auto* self = reinterpret_cast<Bed*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        // Submenus of the shell's right-click menu (Send to, Open with …) are drawn by the shell
        if (cm3_ && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR)) {
            LRESULT r = 0;
            if (SUCCEEDED(cm3_->HandleMenuMsg2(msg, wp, lp, &r))) return r;
        } else if (cm2_ && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM)) {
            if (SUCCEEDED(cm2_->HandleMenuMsg(msg, wp, lp))) return 0;
        }
        switch (msg) {
        case kMsgDropFiles: {
            auto jobs = std::move(pending_);
            pending_.clear();
            for (auto& j : jobs) DoDrop(j);
            return 0;
        }
        case WM_NCCALCSIZE:
            if (wp) return 0;   // the whole window is client area
            break;
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT r;
            GetWindowRect(hwnd, &r);
            int e = MulDiv(6, dpi_, 96);
            bool L = p.x < r.left + e, R = p.x >= r.right - e, T = p.y < r.top + e / 2, B = p.y >= r.bottom - e;
            if (!collapsed) {
                if (T && L) return HTTOPLEFT;
                if (T && R) return HTTOPRIGHT;
                if (B && L) return HTBOTTOMLEFT;
                if (B && R) return HTBOTTOMRIGHT;
                if (L) return HTLEFT;
                if (R) return HTRIGHT;
                if (B) return HTBOTTOM;
                if (T) return HTTOP;
            } else {
                if (L) return HTLEFT;
                if (R) return HTRIGHT;
            }
            return HTCLIENT;   // the title too: we move the bed ourselves (smooth, and a click can roll it up)
        }
        case WM_NCLBUTTONDOWN:
            if (wp >= HTLEFT && wp <= HTBOTTOMRIGHT) {
                // resize ourselves (a plain popup has no system sizing)
                GetCursorPos(&start_);
                GetWindowRect(hwnd, &sizeStart_);
                sizeEdge_ = (int)wp;
                resizing_ = true;
                sizing_ = true;
                SetGlass(true);
                SetCapture(hwnd);
                return 0;
            }
            if (wp == HTCAPTION) {
                // Drag = move. A plain click (it didn't move) = roll up / roll down.
                RECT before;
                GetWindowRect(hwnd, &before);
                DefWindowProcW(hwnd, msg, wp, lp);
                RECT after;
                GetWindowRect(hwnd, &after);
                if (EqualRect(&before, &after)) ToggleCollapse();
                return 0;
            }
            break;
        case WM_NCLBUTTONDBLCLK: return 0;
        case WM_NCRBUTTONUP:
        case WM_NCRBUTTONDOWN:
            if (msg == WM_NCRBUTTONUP) BedMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_CONTEXTMENU: return 0;
        case WM_NCMOUSEMOVE:
            if (!titleHover_ && wp == HTCAPTION) {
                titleHover_ = true;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&t);
                Invalidate();
            } else if (titleHover_ && wp != HTCAPTION) { titleHover_ = false; Invalidate(); }
            return 0;
        case WM_NCMOUSELEAVE:
            if (titleHover_) { titleHover_ = false; Invalidate(); }
            return 0;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_WINDOWPOSCHANGING: {
            // Stay on the desktop layer, whatever Windows tries
            auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
            if (!(wpos->flags & SWP_NOZORDER)) {
                HWND after = InsertAfterForBeds(BedWindows());
                if (after && after != hwnd) wpos->hwndInsertAfter = after;
            }
            return 0;
        }
        case WM_MOVING: Snap(reinterpret_cast<RECT*>(lp)); return TRUE;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = MulDiv(180, dpi_, 96);
            mmi->ptMinTrackSize.y = TitlePx();
            return 0;
        }
        case WM_SIZING: {
            auto* r = reinterpret_cast<RECT*>(lp);
            int mw = MulDiv(180, dpi_, 96), mh = collapsed ? TitlePx() : MulDiv(120, dpi_, 96);
            if (collapsed) r->bottom = r->top + TitlePx();
            if (r->right - r->left < mw) { if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - mw; else r->right = r->left + mw; }
            if (r->bottom - r->top < mh) { if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT) r->top = r->bottom - mh; else r->bottom = r->top + mh; }
            return TRUE;
        }
        case WM_ENTERSIZEMOVE:
            sizing_ = true;
            SetGlass(true);
            return 0;
        case WM_EXITSIZEMOVE: {
            sizing_ = false;
            SetGlass(false);
            RememberRect();
            return 0;
        }
        case WM_SIZE:
            if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            Invalidate();
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            auto* r = reinterpret_cast<RECT*>(lp);
            // while we drag it, the mouse decides where it goes; only take the new size
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE | (moving_ ? SWP_NOMOVE : 0));
            rt_.Reset();
            bmps_.clear();
            if (!moving_) RememberRect();   // keep garden.ini in step with the new scale
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {   // roll up / down
                float t = std::min(1.0f, (GetTickCount() - animStart_) / 150.0f);
                float e = 1 - (1 - t) * (1 - t) * (1 - t);
                int hh = (int)std::lround(animFrom_ + (animTo_ - animFrom_) * e);
                SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, hh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (t >= 1) KillTimer(hwnd, 1);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEWHEEL:
            scroll_ -= GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * kCellH * 0.6f;
            Invalidate();
            return 0;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            if (moving_) { MoveTo(); return 0; }
            if (resizing_) { ResizeTo(); return 0; }
            if (band_) { bandB_ = {x, y + scroll_}; UpdateBand(); return 0; }
            bool th = y < kTitle;
            if (th != titleHover_) { titleHover_ = th; Invalidate(); }
            if (down_ >= 0 && (wp & MK_LBUTTON)) {
                if (!g_drag.active && (std::fabs(x - downX_) > 6 || std::fabs(y - downY_) > 6)) {
                    auto vis = Visible();
                    if (down_ < (int)vis.size()) {
                        // dragging a selected item takes the whole selection along
                        std::vector<std::wstring> keys;
                        if (selected_.count(vis[down_]))
                            for (const auto& k : vis) { if (selected_.count(k)) keys.push_back(k); }
                        else
                            keys.push_back(vis[down_]);
                        g_drag = Drag{true, this, vis[down_], nullptr, -1, keys};
                        clickOnSelected_ = -1;
                        SetCapture(hwnd);
                        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    }
                }
                if (g_drag.active) {
                    POINT s;
                    GetCursorPos(&s);
                    Bed* t = BedAt(s);
                    Bed* old = g_drag.target;
                    g_drag.target = t && !t->collapsed ? t : nullptr;
                    g_drag.index = g_drag.target ? g_drag.target->IndexAt(s) : -1;
                    if (old && old != g_drag.target) old->Invalidate();
                    if (g_drag.target) g_drag.target->Invalidate();
                    Invalidate();
                    return 0;
                }
            }
            int hv = HitItem(x, y);
            if (hv != hover_) { hover_ = hv; Invalidate(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_ != -1 || titleHover_) { hover_ = -1; titleHover_ = false; Invalidate(); }
            return 0;
        case WM_LBUTTONDOWN: {
            float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
            if (y < kTitle) {
                // title: drag to move, click to roll up
                POINT c;
                GetCursorPos(&c);
                RECT r;
                GetWindowRect(hwnd, &r);
                grab_ = {c.x - r.left, c.y - r.top};
                start_ = c;
                moving_ = true;
                moved_ = false;
                SetCapture(hwnd);
                return 0;
            }
            down_ = HitItem(x, y);
            downX_ = x;
            downY_ = y;
            clickOnSelected_ = -1;
            SetForegroundWindow(hwnd);   // so Ctrl+A, Delete and Enter come to this bed
            for (auto& b : g_beds) if (b.get() != this && !b->selected_.empty()) { b->selected_.clear(); b->Invalidate(); }
            bool ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0;
            auto vis = Visible();
            if (down_ >= 0 && down_ < (int)vis.size()) {
                const std::wstring& k = vis[down_];
                if (shift && anchor_ >= 0) {          // everything from the last clicked one to here
                    if (!ctrl) selected_.clear();
                    for (int i = std::min(anchor_, down_); i <= std::max(anchor_, down_) && i < (int)vis.size(); ++i) selected_.insert(vis[i]);
                } else if (ctrl) {                    // add or remove just this one
                    if (selected_.erase(k)) down_ = -1;
                    else selected_.insert(k);
                    anchor_ = HitItem(x, y);
                } else if (selected_.count(k)) {      // maybe dragging the whole selection; a plain click picks just this one
                    clickOnSelected_ = down_;
                    anchor_ = down_;
                } else {
                    selected_ = {k};
                    anchor_ = down_;
                }
            } else if (y >= kTitle && !collapsed) {
                // empty space: drag a rectangle to select (Ctrl adds to what's selected)
                band_ = true;
                bandA_ = bandB_ = {x, y + scroll_};
                bandBase_ = ctrl ? selected_ : std::set<std::wstring>{};
                if (!ctrl) selected_.clear();
                SetCapture(hwnd);
            }
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP:
            if (moving_) { EndMove(true); return 0; }
            if (resizing_) { EndResize(true); return 0; }
            if (band_) { band_ = false; ReleaseCapture(); Invalidate(); return 0; }
            down_ = -1;
            if (!g_drag.active && clickOnSelected_ >= 0) {
                auto vis = Visible();
                if (clickOnSelected_ < (int)vis.size()) selected_ = {vis[clickOnSelected_]};
                clickOnSelected_ = -1;
                Invalidate();
            }
            if (g_drag.active) {
                Drag d = g_drag;
                g_drag.active = false;      // so WM_CAPTURECHANGED doesn't cancel it
                ReleaseCapture();
                Drop(d);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (moving_ && (HWND)lp != hwnd) EndMove(false);
            if (resizing_ && (HWND)lp != hwnd) EndResize(false);
            if (band_ && (HWND)lp != hwnd) { band_ = false; Invalidate(); }
            if (g_drag.active && (HWND)lp != hwnd) { g_drag = Drag(); for (auto& b : g_beds) b->Invalidate(); }
            return 0;
        case WM_LBUTTONDBLCLK: {
            float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
            int i = HitItem(x, y);
            auto vis = Visible();
            if (i >= 0 && i < (int)vis.size()) OpenItem(g_items[vis[i]].path);
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) * 96.0f / dpi_, y = GET_Y_LPARAM(lp) * 96.0f / dpi_;
            POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &s);
            int i = HitItem(x, y);
            auto vis = Visible();
            if (i >= 0 && i < (int)vis.size()) {
                if (!selected_.count(vis[i])) { selected_ = {vis[i]}; anchor_ = i; }
                Invalidate();
                std::vector<std::wstring> paths{g_items[vis[i]].path};   // the clicked one first
                for (const auto& k : vis)
                    if (selected_.count(k) && k != vis[i]) paths.push_back(g_items[k].path);
                ShellMenu(paths, s);
            } else {
                BedMenu(s);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            auto vis = Visible();
            if (ctrl && wp == 'A') {
                selected_.clear();
                for (const auto& k : vis) selected_.insert(k);
                Invalidate();
            } else if (wp == VK_ESCAPE) {
                selected_.clear();
                Invalidate();
            } else if (wp == VK_DELETE) {
                DeleteSelected((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            } else if (wp == VK_RETURN) {
                int opened = 0;
                for (const auto& k : vis)
                    if (selected_.count(k) && opened++ < 15) OpenItem(g_items[k].path);
            }
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void MoveTo() {
        POINT c;
        GetCursorPos(&c);
        if (!moved_) {
            int t = GetSystemMetrics(SM_CXDRAG);
            if (std::abs(c.x - start_.x) <= t && std::abs(c.y - start_.y) <= t) return;
            moved_ = true;
            SetGlass(true);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        }
        RECT r;
        GetWindowRect(hwnd, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        RECT n{c.x - grab_.x, c.y - grab_.y, c.x - grab_.x + w, c.y - grab_.y + h};
        Snap(&n);
        if (n.left != r.left || n.top != r.top)
            SetWindowPos(hwnd, nullptr, n.left, n.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void EndMove(bool release) {
        bool moved = moved_;
        moving_ = moved_ = false;
        if (release) ReleaseCapture();
        if (moved) { SetGlass(false); RememberRect(); Place(); }
        else if (release) ToggleCollapse();   // a plain click on the title
    }

    void ResizeTo() {
        POINT c;
        GetCursorPos(&c);
        int dx = c.x - start_.x, dy = c.y - start_.y;
        RECT r = sizeStart_;
        int e = sizeEdge_;
        bool L = e == HTLEFT || e == HTTOPLEFT || e == HTBOTTOMLEFT, R = e == HTRIGHT || e == HTTOPRIGHT || e == HTBOTTOMRIGHT;
        bool T = e == HTTOP || e == HTTOPLEFT || e == HTTOPRIGHT, B = e == HTBOTTOM || e == HTBOTTOMLEFT || e == HTBOTTOMRIGHT;
        int mw = MulDiv(180, dpi_, 96), mh = collapsed ? TitlePx() : MulDiv(120, dpi_, 96);
        if (L) r.left = std::min(r.left + dx, r.right - mw);
        if (R) r.right = std::max(r.right + dx, r.left + mw);
        if (!collapsed) {
            if (T) r.top = std::min(r.top + dy, r.bottom - mh);
            if (B) r.bottom = std::max(r.bottom + dy, r.top + mh);
        }
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void EndResize(bool release) {
        resizing_ = sizing_ = false;
        if (release) ReleaseCapture();
        SetGlass(false);
        RememberRect();
        Place();
    }

    void RememberRect() {
        RECT r;
        GetWindowRect(hwnd, &r);
        if (collapsed) { int h = rc.bottom - rc.top; rc = r; rc.bottom = rc.top + h; }
        else rc = r;
        ScheduleSave();
    }

    // Files from elsewhere go onto the desktop (so they show up here), like dropping them on the desktop in Windows:
    // moved on the same drive, copied from another drive, Ctrl copies, Shift moves, Alt makes a shortcut.
    // Windows does the moving itself: its usual "file already exists" question, progress and Ctrl+Z all work.
    void DoDrop(const PendingDrop& j) {
        std::wstring desk = KnownFolder(FOLDERID_Desktop), pub = KnownFolder(FOLDERID_PublicDesktop);
        if (desk.empty()) return;
        auto onDesktop = [&](const std::wstring& f) {
            std::wstring dir = ToLower(f.substr(0, f.find_last_of(L"\\")));
            return dir == ToLower(desk) || (!pub.empty() && dir == ToLower(pub));
        };
        std::vector<std::wstring> landed, todo;
        for (const auto& f : j.files) (onDesktop(f) ? landed : todo).push_back(f);
        if (!todo.empty()) {
            if (j.effect & DROPEFFECT_LINK) {
                for (const auto& f : todo) {
                    std::wstring made = MakeShortcut(f, desk);
                    if (!made.empty()) landed.push_back(made);
                }
            } else {
                auto more = MoveOrCopy(todo, desk, (j.effect & DROPEFFECT_MOVE) != 0);
                landed.insert(landed.end(), more.begin(), more.end());
            }
        }
        if (landed.empty()) return;
        // the bed may have been rebuilt while Windows was busy (settings change): then the catch-all bed takes them
        if (std::none_of(g_beds.begin(), g_beds.end(), [&](auto& b) { return b.get() == this; })) {
            ScanDesktop(); Distribute(); for (auto& b : g_beds) b->Invalidate(); ScheduleSave();
            return;
        }
        std::vector<std::wstring> ks;
        for (const auto& f : landed) ks.push_back(ToLower(f));
        auto isNew = [&](const std::wstring& k) { return std::find(ks.begin(), ks.end(), k) != ks.end(); };
        auto vis = Visible();
        std::wstring before;
        for (int i = std::max(j.index, 0); j.index >= 0 && i < (int)vis.size(); ++i)
            if (!isNew(vis[i])) { before = vis[i]; break; }
        for (auto& b : g_beds) b->keys.erase(std::remove_if(b->keys.begin(), b->keys.end(), isNew), b->keys.end());
        auto pos = before.empty() ? keys.end() : std::find(keys.begin(), keys.end(), before);
        keys.insert(pos, ks.begin(), ks.end());
        ScanDesktop();
        Distribute();
        for (auto& b : g_beds) { b->selected_.clear(); b->Invalidate(); }
        selected_.insert(ks.begin(), ks.end());   // what just arrived is selected
        ScheduleSave();
    }

    std::vector<std::wstring> MoveOrCopy(const std::vector<std::wstring>& files, const std::wstring& dest, bool move);
    static std::wstring MakeShortcut(const std::wstring& target, const std::wstring& dir);

    void Drop(Drag d) {
        g_drag = Drag();
        if (d.keys.empty()) d.keys.push_back(d.key);
        auto dragged = [&](const std::wstring& k) { return std::find(d.keys.begin(), d.keys.end(), k) != d.keys.end(); };
        if (d.target && d.from) {
            // index counts visible items; the items land before the first one there that isn't moving
            auto vis = d.target->Visible();
            std::wstring before;
            for (int i = std::max(d.index, 0); d.index >= 0 && i < (int)vis.size(); ++i)
                if (!dragged(vis[i])) { before = vis[i]; break; }
            auto& src = d.from->keys;
            src.erase(std::remove_if(src.begin(), src.end(), dragged), src.end());
            auto& dst = d.target->keys;
            auto pos = before.empty() ? dst.end() : std::find(dst.begin(), dst.end(), before);
            dst.insert(pos, d.keys.begin(), d.keys.end());
            ScheduleSave();
        }
        for (auto& b : g_beds) { b->selected_.clear(); b->Invalidate(); }
        if (d.target) d.target->selected_.insert(d.keys.begin(), d.keys.end());   // still selected where they landed
    }

    UINT dpi_ = 96;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fTitle_, fLabel_, fGlyph_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> bmps_;
    ComPtr<IContextMenu2> cm2_;
    ComPtr<IContextMenu3> cm3_;
    int cols_ = 1, hover_ = -1, down_ = -1, anchor_ = -1, clickOnSelected_ = -1;
    std::set<std::wstring> selected_, bandBase_;   // selected item keys (and those kept while dragging a band with Ctrl)
    bool band_ = false;
    D2D1_POINT_2F bandA_{}, bandB_{};                // rubber band corners, content coordinates
    float gridLeft_ = 0, contentH_ = 0, viewH_ = 0, scroll_ = 0, downX_ = 0, downY_ = 0;
    bool tracking_ = false, titleHover_ = false;
    bool moving_ = false, moved_ = false, sizing_ = false, resizing_ = false;
    POINT grab_{}, start_{};
    RECT sizeStart_{};
    int sizeEdge_ = 0;
    float animFrom_ = 0, animTo_ = 0;
    DWORD animStart_ = 0;
    bool dropReg_ = false;
    std::vector<PendingDrop> pending_;

};

std::set<HWND> BedWindows() {
    std::set<HWND> s;
    for (auto& b : g_beds) if (b->hwnd) s.insert(b->hwnd);
    return s;
}

std::vector<std::unique_ptr<Bed>> g_graveyard;
void Bury(std::vector<std::unique_ptr<Bed>>& beds) {
    for (auto& b : beds) {
        if (b->hwnd) ShowWindow(b->hwnd, SW_HIDE);
        g_graveyard.push_back(std::move(b));
    }
    beds.clear();
    SetTimer(nullptr, 0, 50, [](HWND, UINT, UINT_PTR id, DWORD) {
        if (g_modal) return;   // try again on the next tick
        KillTimer(nullptr, id);
        g_graveyard.clear();
    });
}

Bed* BedAt(POINT screen) {
    HWND w = WindowFromPoint(screen);
    w = w ? GetAncestor(w, GA_ROOT) : nullptr;
    for (auto& b : g_beds) if (b->hwnd == w) return b.get();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dropping files from anywhere into a bed
// ---------------------------------------------------------------------------
// Tells us the final name of each moved/copied item (Windows may have named it "file (2)")
class LandedSink final : public IFileOperationProgressSink {
public:
    std::vector<std::wstring> paths;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IFileOperationProgressSink)) { *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* made) override { Note(hr, made); return S_OK; }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* made) override { Note(hr, made); return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
private:
    void Note(HRESULT hr, IShellItem* made) {
        if (FAILED(hr) || !made) return;   // skipped, cancelled or failed
        PWSTR p = nullptr;
        if (SUCCEEDED(made->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { paths.push_back(p); CoTaskMemFree(p); }
    }
    ULONG ref_ = 1;
};

std::vector<std::wstring> Bed::MoveOrCopy(const std::vector<std::wstring>& files, const std::wstring& dest, bool move) {
    std::vector<std::wstring> out;
    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.Put())))) return out;
    ComPtr<IShellItem> to;
    if (FAILED(SHCreateItemFromParsingName(dest.c_str(), nullptr, IID_PPV_ARGS(to.Put())))) return out;
    op->SetOwnerWindow(hwnd);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_ADDUNDORECORD);
    auto* sink = new LandedSink;
    DWORD cookie = 0;
    bool advised = SUCCEEDED(op->Advise(sink, &cookie));
    int n = 0;
    for (const auto& f : files) {
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(item.Put())))) continue;
        if (SUCCEEDED(move ? op->MoveItem(item.Get(), to.Get(), nullptr, nullptr) : op->CopyItem(item.Get(), to.Get(), nullptr, nullptr))) ++n;
    }
    if (n) {
        ++g_modal;   // beds must not be torn down while Windows' dialog is up
        op->PerformOperations();
        --g_modal;
    }
    if (advised) op->Unadvise(cookie);
    out = sink->paths;
    sink->Release();
    return out;
}

std::wstring Bed::MakeShortcut(const std::wstring& target, const std::wstring& dir) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.Put())))) return {};
    link->SetPath(target.c_str());
    size_t slash = target.find_last_of(L"\\");
    if (slash != std::wstring::npos) link->SetWorkingDirectory(target.substr(0, slash).c_str());
    ComPtr<IPersistFile> pf;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(pf.Put())))) return {};
    std::wstring stem = FileNameOf(target);
    if (stem.empty()) stem = TR(L"Snarvei", L"Shortcut");
    for (int i = 1; i < 100; ++i) {
        std::wstring p = dir + L"\\" + stem + (i > 1 ? L" (" + std::to_wstring(i) + L")" : L"") + L".lnk";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        return SUCCEEDED(pf->Save(p.c_str(), TRUE)) ? p : std::wstring();
    }
    return {};
}

// The bed as a drop target for files from Explorer, the browser's download list …
class BedDrop final : public IDropTarget {
public:
    explicit BedDrop(Bed* b) : bed_(b) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper_.Put()));   // Explorer's drag picture
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *out = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        files_ = Files(d);
        allDesk_ = !files_.empty();
        std::wstring desk = ToLower(KnownFolder(FOLDERID_Desktop)), pub = ToLower(KnownFolder(FOLDERID_PublicDesktop));
        for (const auto& f : files_) {
            std::wstring dir = ToLower(f.substr(0, f.find_last_of(L"\\")));
            if (dir != desk && (pub.empty() || dir != pub)) { allDesk_ = false; break; }
        }
        sameDrive_ = !files_.empty() && SameDrive(files_[0], desk);
        *eff = Choose(keys, *eff);
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragEnter(bed_->hwnd, d, &p, *eff);
        Hover(p, *eff != DROPEFFECT_NONE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* eff) override {
        *eff = Choose(keys, *eff);
        POINT p{pt.x, pt.y};
        if (helper_) helper_->DragOver(&p, *eff);
        Hover(p, *eff != DROPEFFECT_NONE);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        Hover({}, false);
        files_.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD keys, POINTL pt, DWORD* eff) override {
        DWORD e = Choose(keys, *eff);
        POINT p{pt.x, pt.y};
        if (helper_) helper_->Drop(d, &p, e);
        int index = g_ole.target == bed_ ? g_ole.index : -1;
        Hover({}, false);
        if (e != DROPEFFECT_NONE && !files_.empty()) bed_->QueueDrop(std::move(files_), e, index);
        files_.clear();
        // We move/copy the files ourselves (a moment later). Answer "nothing" so the program the files came from
        // never deletes its own copy thinking a move is up to it.
        *eff = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    static std::vector<std::wstring> Files(IDataObject* d) {
        std::vector<std::wstring> out;
        if (!d) return out;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (FAILED(d->GetData(&fe, &sm))) return out;
        if (auto drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && i < 500; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring f(len, L'\0');
                DragQueryFileW(drop, i, f.data(), len + 1);
                if (!f.empty()) out.push_back(f);
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        return out;
    }
    static bool SameDrive(const std::wstring& a, const std::wstring& b) {
        wchar_t va[MAX_PATH] = {}, vb[MAX_PATH] = {};
        if (!GetVolumePathNameW(a.c_str(), va, MAX_PATH) || !GetVolumePathNameW(b.c_str(), vb, MAX_PATH)) return false;
        return ToLower(va) == ToLower(vb);
    }
    // Same rules as dropping on Windows' desktop
    DWORD Choose(DWORD keys, DWORD allowed) const {
        if (files_.empty()) return DROPEFFECT_NONE;
        DWORD want;
        if (allDesk_) want = DROPEFFECT_MOVE;   // already on the desktop: it just changes bed
        else if ((keys & MK_ALT) || ((keys & MK_CONTROL) && (keys & MK_SHIFT))) want = DROPEFFECT_LINK;
        else if (keys & MK_CONTROL) want = DROPEFFECT_COPY;
        else if (keys & MK_SHIFT) want = DROPEFFECT_MOVE;
        else want = sameDrive_ ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        if (allowed & want) return want;
        for (DWORD e : {DROPEFFECT_MOVE, DROPEFFECT_COPY, DROPEFFECT_LINK}) if (allowed & e) return e;
        return DROPEFFECT_NONE;
    }
    void Hover(POINT screen, bool on) {
        Bed* old = g_ole.target;
        int oldIndex = g_ole.index;
        if (on) { g_ole.target = bed_; g_ole.index = bed_->collapsed ? -1 : bed_->IndexAt(screen); }
        else g_ole = OleHover();
        if (old && (old != g_ole.target || oldIndex != g_ole.index)) old->Invalidate();
        if (g_ole.target && (old != g_ole.target || oldIndex != g_ole.index)) g_ole.target->Invalidate();
    }

    Bed* bed_;
    ComPtr<IDropTargetHelper> helper_;
    std::vector<std::wstring> files_;
    bool allDesk_ = false, sameDrive_ = false;
    ULONG ref_ = 1;
};

IDropTarget* NewBedDrop(Bed* bed) { return new BedDrop(bed); }

// ---------------------------------------------------------------------------
// Rename: a little text box over the title
// ---------------------------------------------------------------------------
struct RenameState { Bed* bed = nullptr; HWND box = nullptr, edit = nullptr; HFONT font = nullptr; bool done = false; } g_rename;

void FinishRename(bool commit) {
    if (!g_rename.box || g_rename.done) return;
    g_rename.done = true;
    if (commit && g_rename.bed) {
        wchar_t buf[128] = {};
        GetWindowTextW(g_rename.edit, buf, 128);
        if (*buf) { g_rename.bed->name = buf; g_rename.bed->Invalidate(); ScheduleSave(); }
    }
    HWND box = g_rename.box;
    g_rename.box = g_rename.edit = nullptr;
    DestroyWindow(box);
    if (g_rename.font) { DeleteObject(g_rename.font); g_rename.font = nullptr; }
    g_rename.bed = nullptr;
}

Bed::~Bed() {
    if (g_rename.bed == this) FinishRename(false);
    if (g_drag.from == this || g_drag.target == this) g_drag = Drag();
    if (g_ole.target == this) g_ole = OleHover();
    if (hwnd && dropReg_) RevokeDragDrop(hwnd);
    if (hwnd) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); DestroyWindow(hwnd); }
}

LRESULT CALLBACK EditSub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { FinishRename(true); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { FinishRename(false); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (msg == WM_KILLFOCUS) {   // finish after the focus change is done, not in the middle of it
        if (g_rename.box) PostMessageW(g_rename.box, WM_APP + 1, 0, 0);
        return DefSubclassProc(h, msg, wp, lp);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void Bed::Rename() {
    FinishRename(true);
    RECT r;
    GetWindowRect(hwnd, &r);
    int pad = MulDiv(8, dpi_, 96);
    RECT box{r.left + pad, r.top + MulDiv(3, dpi_, 96), r.right - pad, r.top + TitlePx() - MulDiv(3, dpi_, 96)};
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_APP + 1) { FinishRename(true); return 0; }
            return DefWindowProcW(h, m, w, l);
        };
        wc.hInstance = g_app.hinst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"SkylineGardenRename";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_rename = RenameState();
    g_rename.bed = this;
    g_rename.box = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SkylineGardenRename", L"", WS_POPUP | WS_BORDER,
                                   box.left, box.top, box.right - box.left, box.bottom - box.top, hwnd, nullptr, g_app.hinst, nullptr);
    if (!g_rename.box) return;
    RECT c;
    GetClientRect(g_rename.box, &c);
    g_rename.edit = CreateWindowExW(0, L"EDIT", name.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, MulDiv(4, dpi_, 96), 0,
                                    c.right - MulDiv(8, dpi_, 96), c.bottom, g_rename.box, nullptr, g_app.hinst, nullptr);
    g_rename.font = CreateFontW(-MulDiv(13, dpi_, 72) * 72 / 96, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(g_rename.edit, WM_SETFONT, (WPARAM)g_rename.font, TRUE);
    SetWindowSubclass(g_rename.edit, EditSub, 1, 0);
    ShowWindow(g_rename.box, SW_SHOW);
    INPUT in{};
    in.type = INPUT_MOUSE;
    SendInput(1, &in, sizeof in);   // lets us take the keyboard focus
    SetForegroundWindow(g_rename.box);
    SetFocus(g_rename.edit);
    SendMessageW(g_rename.edit, EM_SETSEL, 0, -1);
}

void Bed::BedMenu(POINT screen) {
    enum { CNew = 1, CRename, CSort, CCollapse, CDelete, CSettings };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CNew, TR(L"Nytt bed", L"New bed"));
    AppendMenuW(m, MF_STRING, CRename, TR(L"Gi nytt navn …", L"Rename …"));
    AppendMenuW(m, MF_STRING, CSort, TR(L"Sorter etter navn", L"Sort by name"));
    AppendMenuW(m, MF_STRING, CCollapse, collapsed ? TR(L"Rull ned", L"Roll down") : TR(L"Rull opp", L"Roll up"));
    if (!catchAll) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, CDelete, TR(L"Slett bed (filene blir liggende)", L"Delete bed (files stay)"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CSettings, TR(L"SkyGarden-innstillinger …", L"SkyGarden settings …"));
    SetForegroundWindow(hwnd);
    ++g_modal;
    int cmd;
    {
        MenuGuard guard;   // no settings reload (and SkyGarden restart) while the menu is open
        cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, hwnd, nullptr);
    }
    --g_modal;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case CNew: Garden::NewBed(); break;
    case CRename: Rename(); break;
    case CSort:
        std::sort(keys.begin(), keys.end(), [](const std::wstring& a, const std::wstring& b) {
            auto ia = g_items.find(a), ib = g_items.find(b);
            std::wstring na = ia != g_items.end() ? ia->second.name : a, nb = ib != g_items.end() ? ib->second.name : b;
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, na.c_str(), -1, nb.c_str(), -1,
                                   nullptr, nullptr, 0) == CSTR_LESS_THAN;
        });
        Invalidate();
        ScheduleSave();
        break;
    case CCollapse: ToggleCollapse(); break;
    case CDelete: DeleteBed(this); break;
    case CSettings: ShowSettings(SettingsPage::Plugins); break;
    }
}

// ---------------------------------------------------------------------------
// Load / save / distribute
// ---------------------------------------------------------------------------
void Save() {
    if (!g_running || g_beds.empty()) return;   // stopped, or nothing loaded: never write an empty garden over a real one
    std::wstring real = GardenIni(), ini = real + L".tmp";
    // written to a copy and swapped in at the end, so a crash or power cut mid-save can't leave half a layout
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, real.c_str());
    if (!CopyFileW(real.c_str(), ini.c_str(), FALSE)) DeleteFileW(ini.c_str());
    // wipe old sections
    for (int i = 0; i < 64; ++i) WritePrivateProfileStringW((L"Bed" + std::to_wstring(i)).c_str(), nullptr, nullptr, ini.c_str());
    WritePrivateProfileStringW(L"Garden", L"Count", std::to_wstring(g_beds.size()).c_str(), ini.c_str());
    for (size_t i = 0; i < g_beds.size(); ++i) {
        Bed& b = *g_beds[i];
        std::wstring sec = L"Bed" + std::to_wstring(i);
        auto w = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(sec.c_str(), k, v.c_str(), ini.c_str()); };
        w(L"Name", b.name);
        w(L"X", std::to_wstring(b.rc.left));
        w(L"Y", std::to_wstring(b.rc.top));
        w(L"W", std::to_wstring(b.rc.right - b.rc.left));
        w(L"H", std::to_wstring(b.rc.bottom - b.rc.top));
        w(L"Collapsed", b.collapsed ? L"1" : L"0");
        w(L"CatchAll", b.catchAll ? L"1" : L"0");
        w(L"Items", JoinList(b.keys, L"|"));
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());   // flush
    if (!MoveFileExW(ini.c_str(), real.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(ini.c_str());
}

void CALLBACK SaveTick(HWND, UINT, UINT_PTR id, DWORD) { KillTimer(nullptr, id); g_saveTimer = 0; Save(); }
void ScheduleSave() {
    if (g_saveTimer) KillTimer(nullptr, g_saveTimer);
    g_saveTimer = SetTimer(nullptr, 0, 600, SaveTick);
}

// Every desktop item lives in exactly one bed; new ones go to the catch-all bed
void Distribute() {
    std::set<std::wstring> placed;
    for (auto& b : g_beds) {
        std::vector<std::wstring> keep;
        for (auto& k : b->keys) if (!placed.count(k)) { keep.push_back(k); placed.insert(k); }
        b->keys = keep;   // items that are gone stay listed (they may come back, e.g. a drive), but aren't drawn
    }
    Bed* catchAll = nullptr;
    for (auto& b : g_beds) if (b->catchAll) catchAll = b.get();
    if (!catchAll && !g_beds.empty()) { catchAll = g_beds.front().get(); catchAll->catchAll = true; }
    if (!catchAll) return;
    for (auto& [k, it] : g_items)
        if (!placed.count(k)) { catchAll->keys.push_back(k); placed.insert(k); }
}

void DefaultLayout() {
    Bury(g_beds);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    auto px = [&](int v) { return MulDiv(v, dx, 96); };
    int x = mi.rcWork.left + px(24), y = mi.rcWork.top + px(24);
    auto apps = std::make_unique<Bed>();
    apps->name = TR(L"Apper", L"Apps");
    apps->rc = {x, y, x + px(372), y + px(300)};
    auto desk = std::make_unique<Bed>();
    desk->name = TR(L"Skrivebord", L"Desktop");
    desk->catchAll = true;
    desk->rc = {apps->rc.right + px(16), y, apps->rc.right + px(16) + px(372), y + px(300)};
    // shortcuts to Apps, everything else to Desktop (Recycle Bin first)
    desk->keys.push_back(ToLower(kRecycleBin));
    for (auto& [k, it] : g_items) {
        if (k == ToLower(kRecycleBin)) continue;
        if (!it.dir && IsShortcut(it.path)) apps->keys.push_back(k);
        else desk->keys.push_back(k);
    }
    g_beds.push_back(std::move(apps));
    g_beds.push_back(std::move(desk));
    Save();
}

void Load() {
    Bury(g_beds);
    std::wstring ini = GardenIni();
    int n = (int)GetPrivateProfileIntW(L"Garden", L"Count", 0, ini.c_str());
    for (int i = 0; i < std::min(n, 64); ++i) {
        std::wstring sec = L"Bed" + std::to_wstring(i);
        auto b = std::make_unique<Bed>();
        b->name = IniStr(ini, sec.c_str(), L"Name");
        int x = GetPrivateProfileIntW(sec.c_str(), L"X", 100, ini.c_str()), y = GetPrivateProfileIntW(sec.c_str(), L"Y", 100, ini.c_str());
        int w = GetPrivateProfileIntW(sec.c_str(), L"W", 360, ini.c_str()), h = GetPrivateProfileIntW(sec.c_str(), L"H", 280, ini.c_str());
        b->rc = {x, y, x + std::max(120, w), y + std::max(80, h)};
        b->collapsed = GetPrivateProfileIntW(sec.c_str(), L"Collapsed", 0, ini.c_str()) != 0;
        b->catchAll = GetPrivateProfileIntW(sec.c_str(), L"CatchAll", 0, ini.c_str()) != 0;
        b->keys = SplitList(IniStr(ini, sec.c_str(), L"Items"), L'|');
        // off-screen after a monitor change? bring it back
        RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        RECT inter;
        if (!IntersectRect(&inter, &b->rc, &vs)) OffsetRect(&b->rc, vs.left + 60 - b->rc.left, vs.top + 60 - b->rc.top);
        if (b->name.empty()) b->name = TR(L"Bed", L"Bed");
        g_beds.push_back(std::move(b));
    }
    if (g_beds.empty()) DefaultLayout();
}

void DeleteBed(Bed* b) {
    if (!b || b->catchAll) return;
    Bed* catchAll = nullptr;
    for (auto& x : g_beds) if (x->catchAll) catchAll = x.get();
    if (catchAll) for (auto& k : b->keys) catchAll->keys.push_back(k);
    auto it = std::find_if(g_beds.begin(), g_beds.end(), [&](auto& p) { return p.get() == b; });
    if (it != g_beds.end()) {
        // destroyed a moment later: we're inside this bed's own window procedure
        std::vector<std::unique_ptr<Bed>> doomed;
        doomed.push_back(std::move(*it));
        g_beds.erase(it);
        Bury(doomed);
    }
    if (catchAll) catchAll->Invalidate();
    ScheduleSave();
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_running) return;
    bool changed = false;
    for (auto& h : g_watch)
        if (h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { FindNextChangeNotification(h); changed = true; }
    if (changed) {
        ScanDesktop();
        Distribute();
        for (auto& b : g_beds) b->Invalidate();
        ScheduleSave();
    }
    ShowDesktopIcons(false);                      // Explorer sometimes shows them again (F5, theme change)
    for (auto& b : g_beds) b->Place();
}

// ---------------------------------------------------------------------------
// Right-click on the empty desktop → "New bed here" on top of Windows' own desktop menu.
// A low-level mouse hook on its own little thread, only while SkyGarden runs and the
// option is on. It only looks at right-button presses; everything else passes straight on.
// Shift + right-click still gives Windows' own menu untouched.
// ---------------------------------------------------------------------------
std::atomic<bool> g_rclickWanted{false};
HANDLE g_mouseThread = nullptr;
DWORD g_mouseThreadId = 0;
HHOOK g_mouseHook = nullptr;
bool g_rDownOnDesktop = false;   // hook thread only

bool IsDesktopAt(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    HWND root = GetAncestor(w, GA_ROOT);
    wchar_t cls[32] = {};
    GetClassNameW(root ? root : w, cls, 32);
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
}

LRESULT CALLBACK DesktopMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) && g_rclickWanted.load()) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (!(m->flags & LLMHF_INJECTED)) {
            if (wp == WM_RBUTTONDOWN) {
                g_rDownOnDesktop = !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && IsDesktopAt(m->pt);
                if (g_rDownOnDesktop) return 1;
            } else if (g_rDownOnDesktop) {
                g_rDownOnDesktop = false;
                PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 22, MAKELPARAM((short)m->pt.x, (short)m->pt.y));
                return 1;
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

DWORD WINAPI DesktopMouseThread(LPVOID) {
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, DesktopMouseProc, g_app.hinst, 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {}
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
    return 0;
}

void SyncDesktopHook() {
    bool want = g_running && g_app.settings.gardenDesktopMenu;
    g_rclickWanted = want;
    if (want && !g_mouseThread) {
        g_mouseThread = CreateThread(nullptr, 0, DesktopMouseThread, nullptr, 0, &g_mouseThreadId);
    } else if (!want && g_mouseThread) {
        PostThreadMessageW(g_mouseThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_mouseThread, 1000);
        CloseHandle(g_mouseThread);
        g_mouseThread = nullptr;
    }
}

// The desktop menu needs a window of its own to own it (the shell draws some submenus itself).
ComPtr<IContextMenu2> g_deskCm2;
ComPtr<IContextMenu3> g_deskCm3;

HWND DesktopMenuOwner() {
    static HWND owner = nullptr;
    if (owner && IsWindow(owner)) return owner;
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (g_deskCm3 && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM || msg == WM_MENUCHAR)) {
            LRESULT r = 0;
            if (SUCCEEDED(g_deskCm3->HandleMenuMsg2(msg, wp, lp, &r))) return r;
        } else if (g_deskCm2 && (msg == WM_INITMENUPOPUP || msg == WM_DRAWITEM || msg == WM_MEASUREITEM)) {
            if (SUCCEEDED(g_deskCm2->HandleMenuMsg(msg, wp, lp))) return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    };
    wc.hInstance = g_app.hinst;
    wc.lpszClassName = L"SkylineDesktopMenu";
    RegisterClassW(&wc);
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_app.hinst, nullptr);
    return owner;
}

} // namespace

// ===========================================================================
namespace Garden {

void ApplySettings() {
    bool on = g_app.settings.gardenEnabled;
    if (on && !g_running) {
        g_running = true;
        ScanDesktop();
        Load();
        Distribute();
        for (auto& b : g_beds) b->Create();
        ShowDesktopIcons(false);
        WatchDesktop();
        g_timer = SetTimer(nullptr, 0, 1500, Tick);
    } else if (!on && g_running) {
        Shutdown();
    } else if (on) {
        // Re-applying the glass blinks the beds; only do it when their look changed.
        static std::wstring last;
        std::wstring key = std::to_wstring(g_app.settings.material[(int)Part::Garden]) + L"," + std::to_wstring(g_app.settings.gardenTintPercent) +
                           L"," + std::to_wstring((int)g_app.theme.dark);
        if (key != last) { last = key; ThemeChanged(); }
        else for (auto& b : g_beds) b->Invalidate();
    }
    SyncDesktopHook();
}

void OnForeground(HWND h) {
    if (!g_running || !h) return;
    for (auto& b : g_beds) b->Place();
}

void IconsArrived() { for (auto& b : g_beds) b->Invalidate(); }

void ThemeChanged() { for (auto& b : g_beds) if (b->hwnd) b->ApplyGlass(); }

void ExplorerRestarted() {
    if (!g_running) return;
    SetTimer(nullptr, 0, 2000, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        if (!g_running) return;
        ShowDesktopIcons(false);
        for (auto& b : g_beds) b->Place();
    });
}

void NewBed() {
    POINT c;
    GetCursorPos(&c);
    NewBedAt(c);
}

void NewBedAt(POINT c) {
    if (!g_running) return;
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    int w = MulDiv(320, dx, 96), h = MulDiv(220, dx, 96);
    int x = std::clamp((int)c.x - w / 2, (int)mi.rcWork.left + 8, std::max((int)mi.rcWork.left + 8, (int)mi.rcWork.right - w - 8));
    int y = std::clamp((int)c.y - MulDiv(15, dx, 96), (int)mi.rcWork.top + 8, std::max((int)mi.rcWork.top + 8, (int)mi.rcWork.bottom - h - 8));
    auto b = std::make_unique<Bed>();
    b->name = TR(L"Nytt bed", L"New bed");
    b->rc = {x, y, x + w, y + h};
    Bed* raw = b.get();
    g_beds.push_back(std::move(b));
    if (raw->Create()) raw->Rename();
    ScheduleSave();
}

void ResetLayout() {
    std::wstring ini = ExeDir() + L"\\garden.ini";
    DeleteFileW(ini.c_str());
    if (!g_running) return;
    FinishRename(false);
    g_drag = Drag();
    Bury(g_beds);
    ScanDesktop();
    Load();
    Distribute();
    for (auto& b : g_beds) b->Create();
}

void RestoreAfterCrash() { ShowDesktopIcons(true); }   // respects "Show desktop icons" being off

void Shutdown() {
    if (!g_running) return;
    FinishRename(true);
    g_drag = Drag();
    if (g_saveTimer) { KillTimer(nullptr, g_saveTimer); g_saveTimer = 0; }
    Save();
    g_running = false;
    if (g_timer) { KillTimer(nullptr, g_timer); g_timer = 0; }
    UnwatchDesktop();
    g_drag = Drag();
    Bury(g_beds);
    ShowDesktopIcons(true);
    if (!g_modal) g_graveyard.clear();   // nothing of ours is on the stack: free them now
    SyncDesktopHook();
}

void DesktopMenu(POINT pt) {
    if (!g_running) return;
    HWND owner = DesktopMenuOwner();
    if (!owner) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, TR(L"Nytt bed her", L"New bed here"));
    // Windows' own desktop menu (New, Display settings, Personalize …) below ours
    ComPtr<IShellFolder> desk;
    ComPtr<IContextMenu> cm;
    if (SUCCEEDED(SHGetDesktopFolder(desk.Put())) &&
        SUCCEEDED(desk->CreateViewObject(owner, IID_IContextMenu, reinterpret_cast<void**>(cm.Put())))) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        if (SUCCEEDED(cm->QueryContextMenu(m, GetMenuItemCount(m), 100, 0x7FFF, CMF_NORMAL))) {
            cm->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(g_deskCm2.Put()));
            cm->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(g_deskCm3.Put()));
        }
    }
    SetForegroundWindow(owner);
    ++g_modal;
    int cmd;
    {
        MenuGuard guard;
        cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, owner, nullptr);
    }
    PostMessageW(owner, WM_NULL, 0, 0);
    g_deskCm2.Reset();
    g_deskCm3.Reset();
    if (cmd == 1) {
        NewBedAt(pt);
    } else if (cmd >= 100 && cm) {
        CMINVOKECOMMANDINFOEX ici{sizeof ici};
        ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        ici.hwnd = owner;
        ici.lpVerb = MAKEINTRESOURCEA(cmd - 100);
        ici.lpVerbW = MAKEINTRESOURCEW(cmd - 100);
        ici.nShow = SW_SHOWNORMAL;
        ici.ptInvoke = pt;
        cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
    }
    --g_modal;
    DestroyMenu(m);
    if (!g_modal) g_graveyard.clear();
}

} // namespace Garden
