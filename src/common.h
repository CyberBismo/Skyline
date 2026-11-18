// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline — a native macOS-style top bar for Windows 11.
// Win32 + Direct2D/DirectWrite. No webview, no runtime.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Language: English by default, Norwegian if chosen in Settings (or kept from an
// older Norwegian-only install). Every visible text is written TR(L"norsk", L"English").
inline bool g_norsk = false;
#define TR(no, en) (g_norsk ? (const wchar_t*)(no) : (const wchar_t*)(en))

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Tiny COM smart pointer (keeps us free of WRL/ATL)
// ---------------------------------------------------------------------------
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr& operator=(const ComPtr& o) { if (this != &o) { Reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    ~ComPtr() { Reset(); }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    T** Put() { Reset(); return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    void Attach(T* p) { Reset(); p_ = p; }   // takes over a reference
private:
    T* p_ = nullptr;
};

// ---------------------------------------------------------------------------
// Private messages
// ---------------------------------------------------------------------------
constexpr UINT WM_SKY_APPBAR       = WM_APP + 1;   // appbar notifications
constexpr UINT WM_SKY_STATUS       = WM_APP + 2;   // volume/network/battery changed (posted from worker threads)
constexpr UINT WM_SKY_TRAY_CHANGED = WM_APP + 3;   // tray icons changed
constexpr UINT WM_SKY_FOREGROUND   = WM_APP + 4;   // foreground window changed

// ---------------------------------------------------------------------------
// Settings (skyline.ini next to the exe)
// ---------------------------------------------------------------------------
// Glass, chosen per part of Skyline. Acrylic = Windows' frosted glass with the tint inside it;
// See-through = no blur at all, the tint painted by us on top.
enum class Part { Bar, Dock, Start, Card, Media, Garden, Spotlight, Vibe, Flyout, Count };
enum Material { MatAcrylic = 0, MatClear = 1, MatVibe = 2 };   // Vibe: acrylic + the colours of the hour (every part but VibeMenu itself)

constexpr int kDockChosen = 10;          // Settings::dockMonitor: the screens the user picked

struct Settings {
    int heightDip = 28;
    int material[(int)Part::Count] = {};   // MatAcrylic / MatClear (/ MatVibe) per part
    int tintPercent = 72;               // how much colour is laid over the blur
    int barTintPercent = 72;            // the top bar's own tint (defaults to tintPercent)
    int startTintPercent = 72;          // SkyMenu + SkyLauncher
    int cardTintPercent = 84;           // dock previews
    int npTintPercent = 84;             // Now Playing card
    int barStyle = 0;                   // 0 classic, 1 floating, 2 islands
    bool barAutohide = false;
    std::wstring dateFormat = TR(L"ddd d. MMM", L"ddd MMM d");
    bool showSeconds = false;
    bool trayEnabled = true;
    bool mirrorToExplorer = false;      // also keep icons in Explorer's tray
    bool allMonitors = true;
    std::vector<std::wstring> hiddenTray; // exe names or tooltip fragments (lowercase)
    std::vector<std::wstring> trayOrder;  // exe names (lowercase), left to right
    // dock
    bool dockEnabled = true;
    int dockIconDip = 36;
    bool hideTaskbar = true;
    bool smartHide = true;
    bool dockPreview = true;              // live window previews when hovering an icon
    int dockMonitor = 0;                  // 0 main screen, 1 all screens, 2+k = screen k (left to right), kDockChosen = the screens below
    std::vector<int> dockScreens;         // kDockChosen: these screens (0 = leftmost), [Dock] Screens=1,3 (counted from 1)
    int dockRevealWidth = 1;              // 0 under the dock, 1 a bit wider, 2 much wider, 3 whole bottom edge
    bool hotCorners = false;
    // plugins
    bool npEnabled = false;               // Now Playing
    int npWhere = 2;                      // 0 dock, 1 top bar, 2 both
    int npBarPos = 0;                     // 0 next to the tray, 1 next to the clock, 2 in the middle, 3 after the app menus
    bool npTint = true;                   // card takes a tint from the album art
    bool gardenEnabled = false;           // SkyGarden
    bool gardenDesktopMenu = true;        // right-click the empty desktop: "New bed here"
    bool dockClock = false;               // Clock in the dock (plugin)
    bool dockClockAnalog = false;         // false = digital
    bool dockClockSeconds = true;         // analog: a live second hand
    int gardenTintPercent = 72;           // SkyGarden's own glass tint (starts equal to the dock's)
    int startMenu = 0;                    // 0 Windows' own, 1 SkyMenu, 2 SkyLauncher, 3 VibeMenu
    bool startWinKey = true;              // the Windows key opens ours
    bool menuFileSearch = true;           // SkyMenu: search shows files too
    int menuSize = 1;                     // SkyMenu size: 0 small, 1 medium, 2 large, 3 extra large, 4 your own (dragged)
    int menuScalePct = 100;               // the dragged size, in percent
    bool menuResize = false;              // SkyMenu: drag its edges to resize
    bool menuRecent = true;               // SkyMenu: "Recommended" – your latest files under the pinned apps
    bool launcherFileSearch = false;      // SkyLauncher: search shows files too (off: many don't want files there)
    // Spotlight (plugin): search apps, settings and files from a hotkey
    bool spotlightEnabled = false;
    UINT spotlightMods = MOD_ALT;         // MOD_ALT / MOD_CONTROL / MOD_SHIFT / MOD_WIN
    UINT spotlightVk = VK_SPACE;
    int spotlightTintPercent = 80;
    int vibeTintPercent = 76;             // VibeMenu's own glass tint
    bool vibeFullscreen = false;
    bool flyoutsEnabled = false;
    bool clockCalendar = false;
    // Look: themes, Liquid Glass and performance mode
    int lookTheme = 0;                    // 0 Standard, 1 Liquid Glass, 2 Vibe, 3 Frosted, 4 Matte, 5 Nordlys (what was last applied)
    int glassTone = 0;                    // the colour of the glass: 0 Skyline's grey, 1 Midnight, 2 Ocean, 3 Sunset, 4 Forest, 5 Rosé
    bool aurora = false;                  // Nordlys: night-blue glass lit by the northern lights
    int auroraStrength = 60;              // how bright the lights are
    bool auroraFjord = true;              // in the daytime: "Fjord", icy blue, instead of the night colours
    bool liquidGlass = false;             // bright rims and a soft sheen on the glass (Liquid Glass theme)
    int glassiness = 60;                  // Liquid Glass: 0 = more colour, 100 = more glass
    bool performanceMode = false;
    int startButton = 0;                  // the dock's Start button: 0 Windows logo, 1 Skyline's own icon
    bool missionEnabled = false;          // Mission Control (plugin)
    bool missionWinTab = true;            // … opens with Win+Tab instead of Windows' Task view         // Advanced: everything plain and cheap; the previous settings come back when it's off
    bool dockTrash = false;               // Recycle Bin in the dock (plugin)
    bool dockMinimize = false;            // minimised windows sit in the dock as little live pictures, like on a Mac
    // SkyWritingTools (plugin, writing.cpp): select text, press the hotkey, an AI rewrites it
    bool writingEnabled = false;
    UINT writingMods = MOD_CONTROL;
    UINT writingVk = VK_SPACE;
    int writingProvider = 0;              // 0 Gemini, 1 OpenAI-compatible (ChatGPT, OpenRouter, Ollama …)
    std::wstring writingGeminiModel = L"gemini-flash-lite-latest";
    std::wstring writingOpenAiModel;
    std::wstring writingBaseUrl = L"https://api.openai.com/v1";
    bool dockStack = false;               // a folder as a stack in the dock (plugin)
    std::wstring dockStackFolder;         // empty = Downloads           // click on the clock: Skyline's own calendar instead of Windows' (Win+N)
    bool barDoubleClick = false;          // classic top bar: double-click maximizes / restores the app in front          // Native Flyouts (plugin): our own sound and network panels from the top bar
    int flyoutTintPercent = 80;          // VibeMenu: the whole screen instead of a card above the dock
    bool centerStart = true;              // put Windows' Start menu in the middle (above the dock)
    int cornerAction[4] = {1, 0, 0, 0};   // top-left, top-right, bottom-left, bottom-right (see hotcorners.cpp)
    bool pinnedSet = false;               // false = first run, use defaults
    std::vector<std::wstring> pinned;     // exe paths or shell:AppsFolder\AUMID
    // tiling
    bool tilingEnabled = false;
    int tilingLayout = 0;                 // 0 BSP, 1 Master, 2 Columns, 3 Monocle
    int tilingGap = 8;
    bool tilingDockSpace = true;
    bool tilingButton = true;             // toggle button in the top bar
    int tilingMode = 0;                   // 0 automatic, 1 automatic + hotkeys, 2 hotkeys only
    std::vector<std::wstring> tilingFloat; // exe names (lowercase) that never tile
    bool updateCheck = true;              // look for new versions now and then (update.cpp)
    std::wstring path;
};

// Writes one value to skyline.ini
void IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value);
// Glass: Windows supplies the material. Acrylic carries the tint inside it (the frosted look);
// See-through has no blur, and its tint is painted by us (GlassTint gives alpha 0 for acrylic).
// > 0 while one of our popup menus is open: rebuilding bars/docks then would free the window the menu belongs to
inline int g_menuDepth = 0;
struct MenuGuard { MenuGuard() { ++g_menuDepth; } ~MenuGuard() { --g_menuDepth; } };
void ApplyGlassEffect(HWND h, Part part, bool cheap = false);   // cheap: plain blur while dragging
int TintPercent(Part part);
D2D1_COLOR_F GlassTint(Part part);            // colour to paint over the glass (alpha 0 = nothing to paint)
D2D1_COLOR_F GlassColor(int percent);
// The glass colour of the chosen tone (Settings::glassTone) for light or dark mode; false = Skyline's own grey
bool ToneColor(bool dark, BYTE rgb[3]);         // the tint colour itself
// "Vibe": the colours of the hour (VibeMenu's), mixed with the Windows accent
void VibePalette(D2D1_COLOR_F out[3]);
// Paints the Vibe colours over `area` when that part uses the Vibe material (does nothing otherwise).
// Still colours, no animation: they only change with the time of day, at the next repaint.
void PaintVibe(ID2D1RenderTarget* rt, D2D1_RECT_F area, Part part, float opacity = 1);
// Liquid Glass: a soft sheen over the top and a bright rim along the edge of a glass surface (nothing when it's off)
// role: 0 a panel/menu/card, 1 the top bar (a glowing band along the bottom), 2 the dock (a glowing rim),
// 4 a SkyGarden bed (seed varies its colours). With Nordlys on, this paints the northern lights instead.
void PaintLiquid(ID2D1RenderTarget* rt, D2D1_RECT_F area, float radius, int role = 0, int seed = 0);
bool AuroraOn();
void AuroraColors(D2D1_COLOR_F out[3], int seed = 0);   // night (or Fjord in the day) colours, shifted by seed
void PaintStars(ID2D1RenderTarget* rt, D2D1_RECT_F area, float alpha);   // a still, faint starry sky (Nordlys at night)
// Themes (look presets) and performance mode, in look.cpp
namespace Look {
const wchar_t* ThemeName(int i);
constexpr int kThemes = 11;
void ApplyTheme(int theme);          // writes the look into skyline.ini (the previous look is kept for "Undo") and reloads
bool CanUndo();
void Undo();
void SetGlassiness(int v);           // Liquid Glass: re-tints every part
bool SetPerformanceMode(bool on);    // saves / restores all settings, and Windows' transparency effects
void LeavingWindows();               // Skyline quits: Windows' transparency back (if performance mode took it)
void Starting();                     // Skyline starts: performance mode takes it again
void DropPerformanceMode();          // before a restore / factory reset
}
inline void IniWrite(const wchar_t* section, const wchar_t* key, int value) { IniWrite(section, key, std::to_wstring(value)); }
std::wstring JoinList(const std::vector<std::wstring>& v, const wchar_t* sep);
std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep);

Settings LoadSettings();
bool WriteDefaultIni(const std::wstring& path);   // a fresh skyline.ini in the current language (only if none exists)
std::wstring ExeDir();

// ---------------------------------------------------------------------------
// Theme — follows Windows light/dark for system surfaces
// ---------------------------------------------------------------------------
struct Theme {
    bool dark = true;
    D2D1_COLOR_F fg{1, 1, 1, 0.92f};
    D2D1_COLOR_F fgDim{1, 1, 1, 0.55f};
    D2D1_COLOR_F hover{1, 1, 1, 0.08f};
    D2D1_COLOR_F press{1, 1, 1, 0.05f};
    D2D1_COLOR_F hairline{1, 1, 1, 0.07f};
    D2D1_COLOR_F solid{0.125f, 0.125f, 0.125f, 1};
    D2D1_COLOR_F accent{0.376f, 0.804f, 1.0f, 1};   // Windows accent (light shade in dark mode)
    D2D1_COLOR_F green{0.42f, 0.82f, 0.46f, 1};
    D2D1_COLOR_F red{1.0f, 0.36f, 0.36f, 1};
};
Theme LoadTheme();

// ---------------------------------------------------------------------------
// Shared graphics factories
// ---------------------------------------------------------------------------
struct Gfx {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    bool Init();
};
extern Gfx g_gfx;

// ---------------------------------------------------------------------------
// Tray icon model
// ---------------------------------------------------------------------------
UINT NextTrayGen();   // tray.cpp: a fresh, never-reused number for a tray icon picture

struct TrayIcon {
    HWND hwnd = nullptr;
    UINT uid = 0;
    GUID guid{};
    bool hasGuid = false;
    UINT callbackMsg = 0;
    UINT version = 0;
    HICON icon = nullptr;           // our own copy (CopyIcon)
    std::wstring tip;
    bool hidden = false;            // NIS_HIDDEN
    bool showTip = false;           // NIF_SHOWTIP
    DWORD pid = 0;
    std::wstring exe;               // lowercase exe file name, for filtering
    RECT screenRect{};              // where it is drawn on the primary bar
    UINT iconGen = 0;               // bumps when the icon image changes
};

// ---------------------------------------------------------------------------
// Status snapshot (read by bars when painting)
// ---------------------------------------------------------------------------
struct StatusState {
    // volume
    bool audioPresent = false;
    float volume = 0;
    bool muted = false;
    // network
    enum Net { Offline, Wifi, Ethernet } net = Offline;
    int wifiQuality = 0;            // 0..100
    std::wstring ssid;
    // battery
    bool hasBattery = false;
    int batteryPct = 0;
    bool charging = false;
    bool onAc = true;
};

// ---------------------------------------------------------------------------
// App menus (the macOS-style "Fil  Rediger  Vis …" next to the app name)
// ---------------------------------------------------------------------------
struct KeyCombo {
    std::vector<WORD> mods;
    WORD vk = 0;
    bool extended = false;
};
struct MenuEntry {
    std::wstring label;
    std::wstring shortcutText;      // shown right-aligned in the menu
    std::vector<KeyCombo> keys;     // one combo, or several for chords (Ctrl+K Ctrl+O)
    std::wstring run;               // "run:" action instead of keys
    bool separator = false;
    UINT nativeId = 0;              // classic Win32 menu command id
    bool disabled = false;
    bool checked = false;
    std::vector<MenuEntry> sub;     // submenus (native menus only)
};
struct TopMenu {
    std::wstring title;
    std::vector<MenuEntry> items;
    HMENU native = nullptr;         // for mirrored classic menus: read at click time
    int nativeIndex = -1;
};
struct AppMenus {
    std::vector<TopMenu> menus;
    bool native = false;
};

struct ForegroundApp {
    HWND hwnd = nullptr;
    std::wstring name;
    std::wstring exe;               // lowercase file name
    HICON icon = nullptr;           // owned by the app cache
    std::wstring path;              // exe path, for the high-quality icon
    UINT iconGen = 0;
    AppMenus menus;
    UINT menusGen = 0;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline float Dip(float v, UINT dpi) { return v * dpi / 96.0f; }
inline std::wstring ToLower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
std::wstring ProcessImagePath(DWORD pid);
std::wstring FileNameOf(const std::wstring& path);
std::wstring FileDescriptionOf(const std::wstring& exePath);
void SendHotkey(std::initializer_list<WORD> keys);
bool AutostartEnabled();
bool PrimaryFullscreen();
// A game (or video) is fullscreen on any screen, or a game runs in exclusive fullscreen. While it is, Skyline keeps
// completely still: no windows created or brought forward, no work-area changes, no tray announcements, no prompts.
bool GameMode();
// A Start menu is open (ours or Windows' own): the bar and the dock stay visible, even over a game.
bool StartMenuOpen();
void OurStartMenuShown(bool open);         // SkyMenu / SkyLauncher opened or closed
void KeepBarsOnTop();                      // put the bars and docks back at the top of the z-order
void RecheckFullscreen();                  // look again shortly whether a fullscreen app is in front (after tiling moved windows)
std::vector<HMONITOR> MonitorsLeftToRight();
void ApplyStartAlignment(bool center);   // center = our setting; false = give back the user's own          // a game/video is fullscreen on the primary monitor
void SetAutostart(bool on);
bool FastStartupEnabled();                // start via a sign-in task instead of the (slow) Run key
bool SetFastStartup(bool on);
void MigrateStartup(std::wstring ini);      // from the Run key to fast startup, once (call on a worker thread)              // false if Windows wouldn't let us make the task
void SaveTraySettings();
void UnhideTray(const std::wstring& key);   // empty key = show all
ComPtr<ID2D1Bitmap> BitmapFromIcon(ID2D1RenderTarget* rt, HICON icon);
std::wstring StackFolder();                // the dock's stack: the chosen folder, or Downloads
