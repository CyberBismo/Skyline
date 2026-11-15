// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"
#include "app.h"

// One top bar per monitor.
class Bar {
public:
    Bar(HMONITOR mon, bool primary) : mon_(mon), primary_(primary) {}
    ~Bar();

    bool Create();
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void Reposition();
    void ApplyBackdrop();
    void ThemeChanged();
    void SetFullscreen(bool fs);    // a fullscreen app/game is on this monitor
    void SetShown(bool shown);      // autohide: slide in / out
    void KeepOnTop();               // back above every window (and our island panels right below)
    void StartMenuChanged(bool open);   // a Start menu opened/closed: show the bar while it's open

    HWND Hwnd() const { return hwnd_; }
    // The lowest y the bar takes on its monitor when it doesn't auto-hide (0 when it does): windows go below it
    int ReservedBottom() const { return g_app.settings.barAutohide || IsRectEmpty(&home_) ? 0 : home_.bottom + MarginPx(); }
    HMONITOR Monitor() const { return mon_; }
    bool Primary() const { return primary_; }

private:
    enum class Kind { App, Menu, Tray, Tiling, Status, Clock, Media };
    struct Item {
        Kind kind;
        D2D1_RECT_F rc;          // DIPs, client space
        HWND trayHwnd = nullptr; // for Kind::Tray
        UINT trayUid = 0;
        int menu = -1;           // for Kind::Menu
    };
    struct TrayBmp { HWND hwnd; UINT uid; UINT gen; ComPtr<ID2D1Bitmap> bmp; };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TriggerProc(HWND, UINT, WPARAM, LPARAM);
    void ApplyShape();               // corners / island region for the current style
    void SyncPanels();               // glass windows behind each island (styles 2 and 3)
    static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM);
    void SlideStep();
    int MarginPx() const;
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void RegisterAppBar();
    void EnsureWorkArea();           // keep the bar's strip out of the work area, even if Windows forgets
    void ReleaseWorkArea();
    void UnregisterAppBar();
    void OnAppBarNotify(WPARAM wp, LPARAM lp);

    bool CreateResources();
    void DiscardResources();
    void Paint();
    void Layout(float width, float height);
    void DrawStatus(const D2D1_RECT_F& rc);
    void DrawGlyph(wchar_t glyph, const D2D1_RECT_F& rc, const D2D1_COLOR_F& color);
    void DrawBattery(const D2D1_RECT_F& rc);
    void DrawWifi(const D2D1_RECT_F& rc);

    int  HitTest(POINT client) const;
    TrayIcon* TrayFor(const Item& it);
    void SetHover(int idx, POINT client);
    void OnButton(UINT msg, POINT client);
    void UpdateTooltip();
    std::wstring TooltipFor(const Item& it);
    void ShowAppMenu(const Item& it);
    void OpenMenus(int itemIdx);
    bool TrayDragMove(POINT client);
    void TrayDragEnd(POINT client);
    D2D1_POINT_2F ToDip(POINT p) const { return {p.x * 96.0f / dpi_, p.y * 96.0f / dpi_}; }
    RECT ToPixels(const D2D1_RECT_F& r) const;

    HWND hwnd_ = nullptr;
    HWND tip_ = nullptr;
    HMONITOR mon_;
    bool primary_;
    UINT dpi_ = 96;
    bool appbar_ = false;
    bool forcedWork_ = false;        // we set the work area ourselves (EnsureWorkArea)
    bool repositioning_ = false;
    HWND trayDownHwnd_ = nullptr;     // the tray icon pressed (identity, not index)
    UINT trayDownUid_ = 0;
    bool fullscreenHidden_ = false;
    // autohide + styles
    HWND trigger_ = nullptr;         // 2 px strip at the top edge that reveals the bar
    RECT home_{};                    // where the bar sits when shown (screen px)
    bool shown_ = true;
    float slide_ = 0, slideTarget_ = 0;
    bool sliding_ = false;
    bool inMenu_ = false;            // one of our menus is open: don't auto-hide
    std::vector<D2D1_RECT_F> islands_, appliedIslands_;
    std::vector<HWND> panels_;
    ComPtr<IDWriteTextLayout> mediaLayout_;   // "Title — Artist"
    ComPtr<ID2D1Bitmap> mediaBmp_;
    UINT mediaGen_ = 0;
    float MediaWidth(float H);       // one real window per island, so the glass is separate too
    HPOWERNOTIFY pwr1_ = nullptr, pwr2_ = nullptr;

    std::vector<Item> items_;
    int hover_ = -1;
    int pressed_ = -1;
    int menuOpen_ = -1;          // item whose dropdown is showing
    // tray drag & drop
    int trayDown_ = -1;          // item index of a pressed tray icon (left button)
    UINT trayDownMsg_ = 0;       // WM_LBUTTONDOWN or WM_LBUTTONDBLCLK, sent on release if no drag
    POINT trayDownPt_{};
    std::wstring dragKey_;       // exe of the icon being dragged
    bool trayDragging_ = false;
    bool trayDropHide_ = false;
    float dragX_ = 0;            // DIP x of the cursor while dragging
    bool tracking_ = false;
    std::wstring tipText_;

    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fBold_, fText_, fIcon_;
    ComPtr<IDWriteTextLayout> appLayout_, clockLayout_;
    std::vector<ComPtr<IDWriteTextLayout>> menuLayouts_;
    ComPtr<ID2D1Bitmap> appBmp_;
    UINT appBmpGen_ = 0;
    std::wstring appBmpPath_;
    std::vector<TrayBmp> trayBmps_;
};
