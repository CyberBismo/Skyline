// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"
#include "preview.h"

// Hides Explorer's taskbar while Skyline runs, and always gives it back.
class TaskbarHider {
public:
    void Hide();
    void Restore();
    void ExplorerRestarted();       // re-hide the new taskbar
    void Release();                 // stop watching, but leave the taskbar hidden
    static void RestoreAfterCrash(); // called at startup if a previous run didn't clean up
private:
    static void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void Vanish(HWND h);
    HWINEVENTHOOK hook_ = nullptr;
    bool active_ = false;
    bool wanted_ = false;            // hiding was asked for (even if there was no taskbar yet to hide)
};

class Dock {
public:
    // mon: the screen this dock lives on. Only the owner hides Explorer's taskbar.
    bool Create(HMONITOR mon, bool owner);
    ~Dock();
    HMONITOR Monitor() const { return mon_; }
    // Where the dock is on screen right now (false while it's hidden)
    bool ScreenRect(RECT* r) const { if (!hwnd_ || !shown_ || !IsWindowVisible(hwnd_)) return false; GetWindowRect(hwnd_, r); return true; }
    void RebuildSoon();             // another dock changed the pinned apps
    void MediaChanged(bool layout);  // Now Playing: track/state changed
    void KeepTaskbarHidden() { taskbar_.Release(); }   // a new dock is taking over

    void OnForeground(HWND h);      // from the global foreground hook
    void SetFullscreen(bool fs);
    void FlushPending();   // after a game: do what was put off
    void KeepOnTop();               // back above every window (not while it steps behind Windows' Start)
    void StartMenuChanged(bool open);   // show the dock while a Start menu is open
    int TopEdge(HMONITOR mon) const;   // y where the dock starts on that monitor, or INT_MAX
    void ExplorerRestarted();
    void ThemeChanged();
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

private:
    struct App {
        std::wstring target;         // what we launch: exe path or shell:AppsFolder\AUMID
        std::wstring aumid;          // lowercase
        std::wstring exe;            // lowercase full path
        std::wstring name;
        bool pinned = false;
        bool start = false;          // the Windows/Start button
        std::vector<HWND> windows;
        bool attention = false;
        ComPtr<IWICBitmapSource> img, imgHover;   // pre-scaled, survive device loss
        ComPtr<ID2D1Bitmap> bmp, bmpHover;
        float scale = 1, targetScale = 1;
        D2D1_RECT_F rc{};            // slot, DIPs
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TriggerProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK OnLocation(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    static void CALLBACK OnCloak(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void LoadPinned();
    void SavePinned();
    void Rebuild();                  // re-scan windows
    void LoadIcon(App& a);
    void Layout();                   // sizes window + slots
    void Paint();
    bool CreateResources();
    void DiscardResources();

    void Evaluate();                 // smart hide decision
    bool CursorInRevealZone() const;
    RECT RevealStrip() const;
    void UpdateReservation(bool force = false);
    bool pendingRes_ = false;                    // Windows moved app bars during a game: catch up afterwards   // reserve screen space when the dock never hides
    RECT reserved_{};                // what we last told Windows
    void SetShown(bool shown);
    void Animate();
    void StartAnimating();

    int  HitTest(POINT client) const;
    void SetHover(int idx);
    void Click(int idx);
    void ContextMenu(int idx, POINT screen);
    void EmptyAreaMenu(POINT screen);
    void Launch(const App& a);
    void Activate(App& a);
    void UpdateLabel();
    void ShowPreview(int idx);
    void HidePreview();
    bool PreviewHasMouse() const { return preview_.MouseInside(); }

    HWND hwnd_ = nullptr, trigger_ = nullptr, tip_ = nullptr;
    HMONITOR mon_ = nullptr;
    bool owner_ = false;
    DockPreview preview_;
    int previewIdx_ = -1;
    D2D1_RECT_F mediaRc_{};          // Now Playing tile (empty when not shown), DIPs
    bool mediaHover_ = false;
    float mediaScale_ = 1, mediaTarget_ = 1;
    ComPtr<ID2D1Bitmap> mediaBmp_;
    UINT mediaGen_ = 0;
    void UpdateMediaHover(POINT client);
    // Clock plugin: a tile at the very end of the dock
    D2D1_RECT_F clockRc_{};          // empty when off, DIPs
    bool clockHover_ = false;
    float clockScale_ = 1, clockTarget_ = 1;
    ComPtr<IDWriteTextFormat> clockBig_, clockSmall_;
    float clockFmtS_ = 0;
    void PaintClock(float S, float base, float height);
    // Stack and Recycle Bin plugins: tiles at the very end of the dock
    struct Tile { D2D1_RECT_F rc{}; bool hover = false, tried = false; float scale = 1, target = 1; ComPtr<IWICBitmapSource> img; ComPtr<ID2D1Bitmap> bmp; };
    Tile stack_, trash_;
    bool trashFull_ = false, dropOnTrash_ = false;
    ULONG trashNotify_ = 0;
    IDropTarget* drop_ = nullptr;
    void PaintTile(Tile& t, float S, float base, float height, bool divider);
    // Minimized windows in the dock (setting, off by default): live pictures of the windows, like on a Mac.
    // DWM draws the pictures on top of the dock, so the app badges live on a see-through layer above it (badges_).
    struct Mini {
        HWND h = nullptr;
        HTHUMBNAIL thumb = nullptr;
        SIZE src{};                  // the window's own size (its shape)
        RECT crop{};                 // the visible window inside it (no invisible resize borders)
        D2D1_RECT_F rc{};            // slot, DIPs
        D2D1_RECT_F pic{};           // where the picture is, DIPs
        bool hover = false;
        float scale = 1, target = 1; // hover magnification
        float appear = 0;            // grows in when it arrives
        ComPtr<IWICBitmapSource> icon;
        ComPtr<ID2D1Bitmap> badge;   // on the badge layer
        ComPtr<ID2D1Bitmap> bmp;     // on the dock (only if there's no live picture)
    };
    std::vector<Mini> minis_;
    void RefreshMinis();
    void ReleaseMinis();
    void PaintMinis(float S, float base, float height, bool divider);
    void RenderBadges();
    int MiniHover() const;
    void MiniMenu(int i, POINT screen);
    HWND badges_ = nullptr;
    HDC badgeDC_ = nullptr;
    HBITMAP badgeBmp_ = nullptr, badgeOld_ = nullptr;
    SIZE badgeSize_{};
    ComPtr<ID2D1DCRenderTarget> badgeRt_;
    static void CALLBACK OnMinimize(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    // The plugins after the divider can be dragged into any order (within their part of the dock)
    enum { PlugMedia, PlugClock, PlugWindows, PlugStack, PlugTrash, PlugCount };
    std::vector<int> pluginOrder_;
    int plugDown_ = -1;              // the plugin pressed (it moves once the mouse does)
    bool plugDragging_ = false;
    void LoadPluginOrder();
    void SavePluginOrder();
    int PluginAt(float x) const;
    void DragPluginTo(float x);
    void TrashChanged();             // the Recycle Bin got fuller or emptier
    void PluginMenu(bool trash, POINT screen);
    void RecycleFiles(std::vector<std::wstring>* files);   // takes ownership
    friend class DockDrop;
public:
    bool DropAt(POINT screen, bool drop, IDataObject* data);   // files dragged over the dock (Recycle Bin)
private:
    void ArmClock();                 // next tick: the next second (analog with seconds) or minute
    UINT dpi_ = 96;
    UINT shellMsg_ = 0;
    HWINEVENTHOOK locHook_ = nullptr, cloakHook_ = nullptr;
    DWORD locPid_ = 0;
    HWND fg_ = nullptr;

    std::vector<App> apps_;
    int hover_ = -1;
    int downIdx_ = -1;
    POINT downPt_{};
    bool dragging_ = false;
    bool removing_ = false;
    bool tracking_ = false;
    bool mouseInside_ = false;
    bool flyoutOpen_ = false;        // Start/Search/etc. is open
    bool fullscreen_ = false;        // a game/video covers the primary monitor

    bool shown_ = true;
    float slide_ = 0, slideTarget_ = 0;  // 0 = fully visible, 1 = hidden below the screen
    bool animating_ = false;
    RECT home_{};                        // visible position, pixels
    bool appbar_ = false;                // reserving the strip at the bottom

    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    TaskbarHider taskbar_;
};
