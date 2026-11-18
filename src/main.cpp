// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline entry point, settings, theme, foreground tracking, helpers.
#include "app.h"
#include <exception>
#include "bar.h"
#include "menus.h"
#include "dock.h"
#include "startmenu.h"
#include "nowplaying.h"
#include "flyouts.h"
#include "mission.h"
#include "stage.h"
#include "systheme.h"
#include "writing.h"
#include "settings.h"
#include "garden.h"
#include "applist.h"
#include "tiling.h"
#include "enhancedtiling.h"
#include "deskclock.h"
#include "hotcorners.h"
#include "update.h"
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <map>
#include <thread>

App g_app;
Gfx g_gfx;

namespace {

constexpr UINT_PTR kTimerClock = 1, kTimerPurge = 2, kTimerNetwork = 3, kTimerDebounce = 4, kTimerFullscreen = 5, kTimerDeferred = 9,
                   kTimerDisplay = 10, kTimerGameWait = 11;
bool g_displayPending = false;   // the screens changed during a game: rebuild once it is over
static unsigned g_deferred = 0;   // bit per WM_SKY_STATUS code waiting for a menu to close
HWINEVENTHOOK g_fsLocHook = nullptr;
DWORD g_fsLocPid = 0;
HWINEVENTHOOK g_fgHook = nullptr;

struct AppInfo { std::wstring name; HICON icon = nullptr; };
std::map<std::wstring, AppInfo> g_appCache;   // exe path -> name + icon (kept for the session)

} // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    PathRemoveFileSpecW(p);
    return p;
}

std::wstring ProcessImagePath(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

void SendHotkey(std::initializer_list<WORD> keys) {
    std::vector<INPUT> in;
    for (WORD k : keys) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; in.push_back(i); }
    for (auto it = keys.end(); it != keys.begin();) {
        --it;
        INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = *it; i.ki.dwFlags = KEYEVENTF_KEYUP; in.push_back(i);
    }
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

ComPtr<ID2D1Bitmap> BitmapFromIcon(ID2D1RenderTarget* rt, HICON icon) {
    ComPtr<ID2D1Bitmap> out;
    if (!icon || !g_gfx.wic) return out;
    ComPtr<IWICBitmap> wb;
    if (FAILED(g_gfx.wic->CreateBitmapFromHICON(icon, wb.Put()))) return out;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(g_gfx.wic->CreateFormatConverter(conv.Put()))) return out;
    if (FAILED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut)))
        return out;
    rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, out.Put());
    return out;
}

bool Gfx::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.Put()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.Put()))))
        return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Put()));
    return true;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static const wchar_t kDefaultIniEn[] =
    L"; Skyline — settings. Most of this is easier in Settings (click the app name in the top left).\r\n"
    L"\r\n"
    L"[Bar]\r\n"
    L"; Height in points (scales with the display DPI)\r\n"
    L"Height=28\r\n"
    L"; Glass of the top bar: acrylic = frosted glass | clear = see-through, no blur\r\n"
    L"Material=acrylic\r\n"
    L"; How much colour the dock's glass has, 0-100 (the top bar's is BarTint)\r\n"
    L"Tint=72\r\n"
    L"; 1 = one bar on each display, 0 = main display only\r\n"
    L"AllMonitors=1\r\n"
    L"; 0 = classic, 1 = floating, 2 = islands, 3 = separated\r\n"
    L"Style=0\r\n"
    L"Autohide=0\r\n"
    L"\r\n"
    L"[Clock]\r\n"
    L"; Date format: ddd = short weekday, d = day, MMM = short month, yyyy = year. Empty = time only\r\n"
    L"DateFormat=ddd MMM d\r\n"
    L"ShowSeconds=0\r\n"
    L"\r\n"
    L"[Tray]\r\n"
    L"Enabled=1\r\n"
    L"; 1 = also keep the icons in Windows' own tray (at the bottom)\r\n"
    L"MirrorToExplorer=0\r\n"
    L"; Hide icons: exe name or part of the tooltip, comma-separated. E.g.: onedrive.exe, securityhealthsystray.exe\r\n"
    L"Hidden=\r\n"
    L"\r\n"
    L"[Dock]\r\n"
    L"Enabled=1\r\n"
    L"; Icon size in points\r\n"
    L"IconSize=36\r\n"
    L"; 1 = hide the Windows taskbar while Skyline is running (it comes back when you quit)\r\n"
    L"HideTaskbar=1\r\n"
    L"; 1 = the dock hides when a window overlaps it\r\n"
    L"SmartHide=1\r\n"
    L"\r\n"
    L"[Tiling]\r\n"
    L"; All of this can also be changed in Settings (click the app name in the top left)\r\n"
    L"Enabled=0\r\n"
    L"; 0 = BSP, 1 = Master, 2 = Columns, 3 = Monocle\r\n"
    L"Layout=0\r\n"
    L"Gap=8\r\n"
    L"DockSpace=1\r\n"
    L"Button=1\r\n"
    L"; 0 = automatic, 1 = automatic + hotkeys, 2 = hotkeys only\r\n"
    L"Mode=0\r\n"
    L"Float=\r\n";

static const wchar_t kDefaultIniNo[] =
    L"; Skyline — innstillinger. Det meste er enklere i Innstillinger (klikk appnavnet oppe til venstre).\r\n"
    L"\r\n"
    L"[Bar]\r\n"
    L"; Høyde i punkter (skaleres med skjermens DPI)\r\n"
    L"Height=28\r\n"
    L"; Glasset i topplinjen: acrylic = frostet glass | clear = gjennomsiktig, uten uskarphet\r\n"
    L"Material=acrylic\r\n"
    L"; Hvor mye farge glasset i docken har, 0-100 (topplinjens er BarTint)\r\n"
    L"Tint=72\r\n"
    L"; 1 = én bar på hver skjerm, 0 = bare hovedskjermen\r\n"
    L"AllMonitors=1\r\n"
    L"; 0 = klassisk, 1 = flytende, 2 = øyer, 3 = separert\r\n"
    L"Style=0\r\n"
    L"Autohide=0\r\n"
    L"\r\n"
    L"[Clock]\r\n"
    L"; Datoformat: ddd = ukedag kort, d = dag, MMM = måned kort, yyyy = år. Tomt = bare klokke\r\n"
    L"DateFormat=ddd d. MMM\r\n"
    L"ShowSeconds=0\r\n"
    L"\r\n"
    L"[Tray]\r\n"
    L"Enabled=1\r\n"
    L"; 1 = behold ikonene også i Windows' egen tray (nederst)\r\n"
    L"MirrorToExplorer=0\r\n"
    L"; Skjul ikoner: exe-navn eller del av tooltip, kommaseparert. F.eks: onedrive.exe, securityhealthsystray.exe\r\n"
    L"Hidden=\r\n"
    L"\r\n"
    L"[Dock]\r\n"
    L"Enabled=1\r\n"
    L"; Ikonstørrelse i punkter\r\n"
    L"IconSize=36\r\n"
    L"; 1 = skjul Windows-oppgavelinjen mens Skyline kjører (kommer tilbake når du avslutter)\r\n"
    L"HideTaskbar=1\r\n"
    L"; 1 = docken gjemmer seg når et vindu ligger over den\r\n"
    L"SmartHide=1\r\n"
    L"\r\n"
    L"[Tiling]\r\n"
    L"; Alt dette kan også endres i Innstillinger (klikk appnavnet oppe til venstre)\r\n"
    L"Enabled=0\r\n"
    L"; 0 = BSP, 1 = Master, 2 = Kolonner, 3 = Monocle\r\n"
    L"Layout=0\r\n"
    L"Gap=8\r\n"
    L"DockSpace=1\r\n"
    L"Button=1\r\n"
    L"; 0 = automatisk, 1 = automatisk + hurtigtaster, 2 = bare hurtigtaster\r\n"
    L"Mode=0\r\n"
    L"Float=\r\n";

bool WriteDefaultIni(const std::wstring& path) {
    const wchar_t* kDefaultIni = g_norsk ? kDefaultIniNo : kDefaultIniEn;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    // UTF-16 LE with BOM: what GetPrivateProfileString expects for non-ASCII text.
    DWORD w;
    WORD bom = 0xFEFF;
    WriteFile(f, &bom, 2, &w, nullptr);
    WriteFile(f, kDefaultIni, (DWORD)(wcslen(kDefaultIni) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
    WritePrivateProfileStringW(L"General", L"Language", g_norsk ? L"no" : L"en", path.c_str());
    return true;
}

Settings LoadSettings() {
    // Language first: everything below (defaults included) depends on it.
    std::wstring iniPath = ExeDir() + L"\\skyline.ini";
    bool hadIni = GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    wchar_t lang[16] = {};
    GetPrivateProfileStringW(L"General", L"Language", L"", lang, 16, iniPath.c_str());
    // New installs start in English. An older install without the key was Norwegian-only, so it stays Norwegian.
    g_norsk = *lang ? lstrcmpiW(lang, L"no") == 0 : hadIni;

    Settings s;
    s.path = iniPath;
    if (!hadIni) WriteDefaultIni(s.path);
    if (!*lang) WritePrivateProfileStringW(L"General", L"Language", g_norsk ? L"no" : L"en", s.path.c_str());
    const wchar_t* p = s.path.c_str();
    auto str = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
        static wchar_t buf[8192];
        GetPrivateProfileStringW(sec, key, def, buf, 8192, p);
        return std::wstring(buf);
    };
    s.heightDip = std::clamp((int)GetPrivateProfileIntW(L"Bar", L"Height", 28, p), 20, 48);
    s.tintPercent = std::clamp((int)GetPrivateProfileIntW(L"Bar", L"Tint", 72, p), 0, 100);
    // Tints and materials of older versions started out following the dock; write them down once
    // so moving the dock's slider never changes the others again.
    auto seed = [&](const wchar_t* sec, const wchar_t* key, int v) {
        if (GetPrivateProfileIntW(sec, key, -1, p) == -1) WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), p);
    };
    seed(L"Bar", L"BarTint", s.tintPercent);
    seed(L"Start", L"Tint", s.tintPercent);
    seed(L"Bar", L"CardTint", std::min(100, s.tintPercent + 12));
    s.barTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Bar", L"BarTint", s.tintPercent, p), 0, 100);
    s.startTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Start", L"Tint", s.tintPercent, p), 0, 100);
    s.cardTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Bar", L"CardTint", std::min(100, s.tintPercent + 12), p), 0, 100);
    seed(L"NowPlaying", L"CardTint", s.cardTintPercent);
    s.npTintPercent = std::clamp((int)GetPrivateProfileIntW(L"NowPlaying", L"CardTint", s.cardTintPercent, p), 0, 100);
    seed(L"Garden", L"Tint", s.tintPercent);
    s.gardenTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Garden", L"Tint", s.tintPercent, p), 0, 100);
    {
        // Material per part: "acrylic" or "clear". Older versions had one Backdrop for everything
        // (acrylic/blur/clear/mica/solid) and a Glass number for SkyGarden.
        int def = ToLower(str(L"Bar", L"Backdrop", L"acrylic")) == L"clear" ? MatClear : MatAcrylic;
        int gardenDef = GetPrivateProfileIntW(L"Garden", L"Glass", 0, p) == 2 ? MatClear : MatAcrylic;
        auto mat = [&](const wchar_t* sec, const wchar_t* key, int d) {
            std::wstring v = ToLower(str(sec, key, L""));
            return v == L"clear" ? MatClear : v == L"acrylic" ? MatAcrylic : v == L"vibe" ? MatVibe : d;
        };
        s.material[(int)Part::Bar] = mat(L"Bar", L"Material", def);
        s.material[(int)Part::Dock] = mat(L"Dock", L"Material", def);
        s.material[(int)Part::Start] = mat(L"Start", L"Material", def);
        s.material[(int)Part::Card] = mat(L"Bar", L"CardMaterial", def);
        s.material[(int)Part::Media] = mat(L"NowPlaying", L"Material", def);
        s.material[(int)Part::Garden] = mat(L"Garden", L"Material", gardenDef);
        s.material[(int)Part::Spotlight] = mat(L"Spotlight", L"Material", s.material[(int)Part::Start]);
        s.material[(int)Part::Vibe] = mat(L"Vibe", L"Material", s.material[(int)Part::Start]);
        s.material[(int)Part::Flyout] = mat(L"Flyouts", L"Material", s.material[(int)Part::Start]);
        // VibeMenu paints its own colours of the hour: "vibe" there just means acrylic
        if (s.material[(int)Part::Vibe] == MatVibe) s.material[(int)Part::Vibe] = MatAcrylic;
    }
    s.barStyle = std::clamp((int)GetPrivateProfileIntW(L"Bar", L"Style", 0, p), 0, 3);
    s.barAutohide = GetPrivateProfileIntW(L"Bar", L"Autohide", 0, p) != 0;
    s.allMonitors = GetPrivateProfileIntW(L"Bar", L"AllMonitors", 1, p) != 0;
    s.dateFormat = str(L"Clock", L"DateFormat", TR(L"ddd d. MMM", L"ddd MMM d"));
    s.showSeconds = GetPrivateProfileIntW(L"Clock", L"ShowSeconds", 0, p) != 0;
    s.trayEnabled = GetPrivateProfileIntW(L"Tray", L"Enabled", 1, p) != 0;
    s.mirrorToExplorer = GetPrivateProfileIntW(L"Tray", L"MirrorToExplorer", 0, p) != 0;
    {
        std::wstring order = ToLower(str(L"Tray", L"Order", L""));
        size_t a = 0;
        while (a < order.size()) {
            size_t b = order.find(L'|', a);
            std::wstring t = order.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
            if (!t.empty()) s.trayOrder.push_back(t);
            if (b == std::wstring::npos) break;
            a = b + 1;
        }
    }
    s.dockEnabled = GetPrivateProfileIntW(L"Dock", L"Enabled", 1, p) != 0;
    // v1.2 wrote 44 as the default; it was too big.
    if (GetPrivateProfileIntW(L"Dock", L"ConfigVersion", 1, p) < 2) {
        if (GetPrivateProfileIntW(L"Dock", L"IconSize", 36, p) == 44) WritePrivateProfileStringW(L"Dock", L"IconSize", L"36", p);
        WritePrivateProfileStringW(L"Dock", L"ConfigVersion", L"2", p);
    }
    s.dockIconDip = std::clamp((int)GetPrivateProfileIntW(L"Dock", L"IconSize", 36, p), 24, 96);
    s.hideTaskbar = GetPrivateProfileIntW(L"Dock", L"HideTaskbar", 1, p) != 0;
    s.smartHide = GetPrivateProfileIntW(L"Dock", L"SmartHide", 1, p) != 0;
    s.dockRevealWidth = std::clamp((int)GetPrivateProfileIntW(L"Dock", L"RevealWidth", 1, p), 0, 3);
    s.dockPreview = GetPrivateProfileIntW(L"Dock", L"Preview", 1, p) != 0;
    s.dockMonitor = std::clamp((int)GetPrivateProfileIntW(L"Dock", L"Monitor", 0, p), 0, kDockChosen);
    for (auto& n : SplitList(str(L"Dock", L"Screens", L""), L','))
        if (int v = _wtoi(n.c_str()); v >= 1 && v <= 16 && std::find(s.dockScreens.begin(), s.dockScreens.end(), v - 1) == s.dockScreens.end())
            s.dockScreens.push_back(v - 1);
    if (s.dockMonitor == kDockChosen && s.dockScreens.empty()) s.dockMonitor = 0;
    s.npEnabled = GetPrivateProfileIntW(L"NowPlaying", L"Enabled", 0, p) != 0;
    s.npWhere = std::clamp((int)GetPrivateProfileIntW(L"NowPlaying", L"Where", 2, p), 0, 2);
    s.npBarPos = std::clamp((int)GetPrivateProfileIntW(L"NowPlaying", L"BarPosition", 0, p), 0, 3);
    s.npTint = GetPrivateProfileIntW(L"NowPlaying", L"Tint", 1, p) != 0;
    s.gardenEnabled = GetPrivateProfileIntW(L"Garden", L"Enabled", 0, p) != 0;
    s.gardenDesktopMenu = GetPrivateProfileIntW(L"Garden", L"DesktopMenu", 1, p) != 0;
    s.dockClock = GetPrivateProfileIntW(L"DockClock", L"Enabled", 0, p) != 0;
    s.dockClockAnalog = GetPrivateProfileIntW(L"DockClock", L"Analog", 0, p) != 0;
    s.dockClockSeconds = GetPrivateProfileIntW(L"DockClock", L"Seconds", 1, p) != 0;
    s.startMenu = (int)GetPrivateProfileIntW(L"Start", L"Menu", 0, p);
    if (s.startMenu == 4) s.startMenu = 2;   // 4 was Apple-Ish (removed in 2.26): SkyLauncher is the closest
    if (s.startMenu < 0 || s.startMenu > 3) s.startMenu = 0;   // 5 was GnomeLauncher (removed in 2.22): back to Windows' own
    s.startWinKey = GetPrivateProfileIntW(L"Start", L"WinKey", 1, p) != 0;
    s.menuFileSearch = GetPrivateProfileIntW(L"Start", L"MenuFiles", 1, p) != 0;
    s.menuRecent = GetPrivateProfileIntW(L"Start", L"MenuRecent", 1, p) != 0;
    s.menuSize = std::clamp((int)GetPrivateProfileIntW(L"Start", L"MenuSize", 1, p), 0, 4);
    s.menuScalePct = std::clamp((int)GetPrivateProfileIntW(L"Start", L"MenuScale", 100, p), 60, 200);
    s.menuResize = GetPrivateProfileIntW(L"Start", L"MenuResize", 0, p) != 0;
    s.launcherFileSearch = GetPrivateProfileIntW(L"Start", L"LauncherFiles", 0, p) != 0;
    s.spotlightEnabled = GetPrivateProfileIntW(L"Spotlight", L"Enabled", 0, p) != 0;
    s.spotlightMods = GetPrivateProfileIntW(L"Spotlight", L"Modifiers", MOD_ALT, p) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    s.spotlightVk = std::clamp((int)GetPrivateProfileIntW(L"Spotlight", L"Key", VK_SPACE, p), 1, 254);
    if (!s.spotlightMods) s.spotlightMods = MOD_ALT;   // a hotkey always has a modifier
    s.vibeFullscreen = GetPrivateProfileIntW(L"Vibe", L"Fullscreen", 0, p) != 0;
    s.flyoutsEnabled = GetPrivateProfileIntW(L"Flyouts", L"Enabled", 0, p) != 0;
    s.clockCalendar = GetPrivateProfileIntW(L"Clock", L"Calendar", 0, p) != 0;
    s.lookTheme = std::clamp((int)GetPrivateProfileIntW(L"Look", L"Theme", 0, p), 0, Look::kThemes - 1);
    s.liquidGlass = GetPrivateProfileIntW(L"Look", L"LiquidGlass", 0, p) != 0;
    s.aurora = GetPrivateProfileIntW(L"Look", L"Aurora", 0, p) != 0;
    s.glassTone = std::clamp((int)GetPrivateProfileIntW(L"Look", L"Tone", 0, p), 0, 5);
    s.auroraStrength = std::clamp((int)GetPrivateProfileIntW(L"Look", L"AuroraStrength", 60, p), 0, 100);
    s.auroraFjord = GetPrivateProfileIntW(L"Look", L"AuroraFjord", 1, p) != 0;
    s.glassiness = std::clamp((int)GetPrivateProfileIntW(L"Look", L"Glassiness", 60, p), 0, 100);
    s.performanceMode = GetPrivateProfileIntW(L"Advanced", L"Performance", 0, p) != 0;
    s.startButton = GetPrivateProfileIntW(L"Start", L"Button", 0, p) == 1 ? 1 : 0;
    s.missionEnabled = GetPrivateProfileIntW(L"MissionControl", L"Enabled", 0, p) != 0;
    s.missionWinTab = GetPrivateProfileIntW(L"MissionControl", L"WinTab", 1, p) != 0;
    s.stageEnabled = GetPrivateProfileIntW(L"StageManager", L"Enabled", 0, p) != 0;
    s.stageMode = GetPrivateProfileIntW(L"StageManager", L"Mode", 0, p) == 1 ? 1 : 0;
    s.stageHover = GetPrivateProfileIntW(L"StageManager", L"Hover", 0, p) == 1 ? 1 : 0;
    s.stageMinimized = GetPrivateProfileIntW(L"StageManager", L"Minimized", 0, p) != 0;
    s.dockTrash = GetPrivateProfileIntW(L"Dock", L"Trash", 0, p) != 0;
    s.dockMinimize = GetPrivateProfileIntW(L"Dock", L"MinimizeToDock", 0, p) != 0;
    s.writingEnabled = GetPrivateProfileIntW(L"SkyWritingTools", L"Enabled", 0, p) != 0;
    s.writingMods = GetPrivateProfileIntW(L"SkyWritingTools", L"Modifiers", MOD_CONTROL, p) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (!s.writingMods) s.writingMods = MOD_CONTROL;   // a hotkey always has a modifier
    s.writingVk = std::clamp((int)GetPrivateProfileIntW(L"SkyWritingTools", L"Key", VK_SPACE, p), 1, 254);
    s.writingProvider = std::clamp((int)GetPrivateProfileIntW(L"SkyWritingTools", L"Provider", 0, p), 0, 1);
    {
        wchar_t b[512];
        GetPrivateProfileStringW(L"SkyWritingTools", L"GeminiModel", L"gemini-flash-lite-latest", b, 512, p);
        s.writingGeminiModel = b;
        GetPrivateProfileStringW(L"SkyWritingTools", L"OpenAIModel", L"", b, 512, p);
        s.writingOpenAiModel = b;
        GetPrivateProfileStringW(L"SkyWritingTools", L"BaseUrl", L"https://api.openai.com/v1", b, 512, p);
        s.writingBaseUrl = b;
    }
    s.dockStack = GetPrivateProfileIntW(L"Dock", L"Stack", 0, p) != 0;
    s.dockStackFolder = str(L"Dock", L"StackFolder", L"");
    s.barDoubleClick = GetPrivateProfileIntW(L"Bar", L"DoubleClick", 0, p) != 0;
    s.flyoutTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Flyouts", L"Tint", std::min(100, s.startTintPercent + 8), p), 0, 100);
    s.vibeTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Vibe", L"Tint", s.startTintPercent + 4, p), 0, 100);
    s.spotlightTintPercent = std::clamp((int)GetPrivateProfileIntW(L"Spotlight", L"Tint", std::min(100, s.startTintPercent + 8), p), 0, 100);
    s.hotCorners = GetPrivateProfileIntW(L"HotCorners", L"Enabled", 0, p) != 0;
    s.centerStart = GetPrivateProfileIntW(L"General", L"CenterStart", 1, p) != 0;
    {
        static const wchar_t* keys[4] = {L"TopLeft", L"TopRight", L"BottomLeft", L"BottomRight"};
        for (int i = 0; i < 4; ++i)
            s.cornerAction[i] = std::clamp((int)GetPrivateProfileIntW(L"HotCorners", keys[i], i == 0 ? 1 : 0, p), 0, HotCorners::kActions - 1);
    }
    s.tilingEnabled = GetPrivateProfileIntW(L"Tiling", L"Enabled", 0, p) != 0;
    s.enhancedTiling = GetPrivateProfileIntW(L"Tiling", L"Enhanced", 0, p) != 0;
    s.tilingLayout = std::clamp((int)GetPrivateProfileIntW(L"Tiling", L"Layout", 0, p), 0, 3);
    s.tilingGap = std::clamp((int)GetPrivateProfileIntW(L"Tiling", L"Gap", 8, p), 0, 40);
    s.tilingDockSpace = GetPrivateProfileIntW(L"Tiling", L"DockSpace", 1, p) != 0;
    s.tilingButton = GetPrivateProfileIntW(L"Tiling", L"Button", 1, p) != 0;
    s.tilingMode = std::clamp((int)GetPrivateProfileIntW(L"Tiling", L"Mode", 0, p), 0, 2);
    s.updateCheck = GetPrivateProfileIntW(L"Update", L"Check", 1, p) != 0;
    for (auto& f : SplitList(ToLower(str(L"Tiling", L"Float", L"")), L',')) s.tilingFloat.push_back(f);
    {
        std::wstring pins = str(L"Dock", L"Pinned", L"\x01");
        s.pinnedSet = pins != L"\x01";
        if (s.pinnedSet) {
            size_t a = 0;
            while (a < pins.size()) {
                size_t b = pins.find(L'|', a);
                std::wstring t = pins.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
                if (!t.empty()) s.pinned.push_back(t);
                if (b == std::wstring::npos) break;
                a = b + 1;
            }
        }
    }
    std::wstring hidden = ToLower(str(L"Tray", L"Hidden", L""));
    size_t start = 0;
    while (start <= hidden.size()) {
        size_t comma = hidden.find(L',', start);
        std::wstring part = hidden.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        while (!part.empty() && iswspace(part.front())) part.erase(part.begin());
        while (!part.empty() && iswspace(part.back())) part.pop_back();
        if (!part.empty()) s.hiddenTray.push_back(part);
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return s;
}

int TintPercent(Part part) {
    const Settings& s = g_app.settings;
    switch (part) {
    case Part::Bar: return s.barTintPercent;
    case Part::Start: return s.startTintPercent;
    case Part::Card: return s.cardTintPercent;
    case Part::Media: return s.npTintPercent;
    case Part::Garden: return s.gardenTintPercent;
    case Part::Spotlight: return s.spotlightTintPercent;
    case Part::Vibe: return s.vibeTintPercent;
    case Part::Flyout: return s.flyoutTintPercent;
    default: return s.tintPercent;
    }
}

bool ToneColor(bool dark, BYTE rgb[3]) {
    // dark mode, light mode: deep and quiet colours, so text and icons stay easy to read
    static const BYTE tones[6][2][3] = {
        {{0, 0, 0}, {0, 0, 0}},
        {{4, 4, 8}, {236, 236, 242}},        // Midnight: almost black (OLED)
        {{6, 32, 58}, {216, 235, 248}},      // Ocean: deep blue
        {{56, 18, 42}, {252, 228, 220}},     // Sunset: plum
        {{12, 38, 26}, {220, 240, 226}},     // Forest: dark green
        {{52, 22, 38}, {250, 226, 236}},     // Rosé: muted pink
    };
    int t = g_app.settings.glassTone;
    if (t <= 0 || t > 5 || g_app.settings.performanceMode) return false;
    for (int i = 0; i < 3; ++i) rgb[i] = tones[t][dark ? 0 : 1][i];
    return true;
}

D2D1_COLOR_F GlassColor(int percent) {
    float a = std::clamp(percent, 0, 100) / 100.0f;
    if (BYTE rgb[3]; !AuroraOn() && ToneColor(g_app.theme.dark, rgb)) return D2D1::ColorF(rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, a);
    if (AuroraOn()) return g_app.theme.dark ? D2D1::ColorF(0.04f, 0.08f, 0.17f, a) : D2D1::ColorF(0.87f, 0.93f, 0.98f, a);
    return g_app.theme.dark ? D2D1::ColorF(0.08f, 0.08f, 0.09f, a) : D2D1::ColorF(0.96f, 0.96f, 0.97f, a);
}

D2D1_COLOR_F GlassTint(Part part) {
    // Acrylic: the tint lives inside the glass (ApplyGlassEffect), nothing to paint on top.
    if (g_app.settings.material[(int)part] != MatClear) return D2D1::ColorF(0, 0, 0, 0);   // acrylic and Vibe: inside the glass
    return GlassColor(TintPercent(part));
}

void VibePalette(D2D1_COLOR_F out[3]) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    static const UINT pal[4][3] = {
        {0x3B2B8F, 0x0E6BA8, 0x7A2E8E},   // night: indigo, deep blue, plum
        {0xFF9A62, 0xFFC75F, 0xF06292},   // morning: peach, gold, pink
        {0x3FA7D6, 0x59CD90, 0x6C8EF5},   // day: sky, mint, periwinkle
        {0x8E44AD, 0xE84A5F, 0x3D5AFE},   // evening: violet, coral, blue
    };
    int k = t.wHour < 5 ? 0 : t.wHour < 10 ? 1 : t.wHour < 17 ? 2 : t.wHour < 22 ? 3 : 0;
    for (int i = 0; i < 3; ++i) {
        UINT c = pal[k][i];
        out[i] = D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1);
    }
    D2D1_COLOR_F acc = g_app.theme.accent;
    out[0] = D2D1::ColorF(out[0].r + (acc.r - out[0].r) * 0.45f, out[0].g + (acc.g - out[0].g) * 0.45f, out[0].b + (acc.b - out[0].b) * 0.45f, 1);
}

bool AuroraOn() { return g_app.settings.aurora && !g_app.settings.performanceMode; }

static bool FjordNow() {
    if (!g_app.settings.auroraFjord) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t.wHour >= 8 && t.wHour < 17;   // daylight hours: the fjord; the rest of the time the northern lights
}

void AuroraColors(D2D1_COLOR_F out[3], int seed) {
    static const D2D1_COLOR_F night[4] = {{0.22f, 0.96f, 0.62f, 1}, {0.16f, 0.82f, 0.92f, 1}, {0.62f, 0.42f, 1.0f, 1}, {0.30f, 0.95f, 0.80f, 1}};
    static const D2D1_COLOR_F fjord[4] = {{0.55f, 0.82f, 1.0f, 1}, {0.86f, 0.95f, 1.0f, 1}, {0.42f, 0.62f, 0.90f, 1}, {0.66f, 0.88f, 0.98f, 1}};
    const D2D1_COLOR_F* p = FjordNow() ? fjord : night;
    int o = ((seed % 4) + 4) % 4;
    for (int i = 0; i < 3; ++i) out[i] = p[(i + o) % 4];
}

void PaintStars(ID2D1RenderTarget* rt, D2D1_RECT_F a, float alpha) {
    if (!rt || !AuroraOn() || FjordNow()) return;
    ComPtr<ID2D1SolidColorBrush> b;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), b.Put());
    if (!b) return;
    unsigned x = 2166136261u;   // the same sky every time: a fixed pseudo-random sequence
    float w = a.right - a.left, h = a.bottom - a.top;
    for (int i = 0; i < 140; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        float px = a.left + (x % 10000) / 10000.0f * w;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        float py = a.top + (x % 10000) / 10000.0f * h * 0.75f;   // mostly in the upper sky
        float r = 0.5f + (x % 7) * 0.18f;
        b->SetColor(D2D1::ColorF(1, 1, 1, alpha * (0.25f + (x % 5) * 0.12f)));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(px, py), r, r), b.Get());
    }
}

// ---------------------------------------------------------------------------
// Soft washes of colour on big panels (menus, cards, SkyGarden beds).
// Direct2D's gradients land in 8 bits per channel, and a faint glow spread over a big panel then shows as
// rings. So the wash is worked out here in full precision, with a dither of less than one step (invisible,
// but it hides the steps), and kept: it is only worked out again when the size or the colours change.
struct WashBlob { float cx, cy, rx, ry; D2D1_COLOR_F c; float a; };   // centre and radii in the panel's own units (0..w, 0..h)

static void PaintWash(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, const WashBlob* blobs, int n, float opacity) {
    float w = r.right - r.left, h = r.bottom - r.top;
    if (!rt || w < 2 || h < 2 || n <= 0 || opacity <= 0.003f) return;
    // big surfaces (full-screen launchers) are worked out at a lower resolution and stretched: a glow has no detail to lose
    float dx = 96, dy = 96;
    rt->GetDpi(&dx, &dy);
    float dpiScale = std::max(1.0f, dx / 96.0f);   // one bitmap pixel per screen pixel where we can afford it
    float s = std::min(dpiScale, std::sqrt(300000.0f / (w * h)));
    UINT bw = (UINT)std::max(1.0f, std::ceil(w * s)), bh = (UINT)std::max(1.0f, std::ceil(h * s));
    // what this wash looks like, as one number
    UINT64 key = 1469598103934665603ull;
    auto mix = [&](float v) { key = (key ^ (UINT64)(int)std::lround(v * 1000)) * 1099511628211ull; };
    mix((float)bw); mix((float)bh);
    for (int i = 0; i < n; ++i) {
        const WashBlob& b = blobs[i];
        mix(b.cx / w); mix(b.cy / h); mix(b.rx / w); mix(b.ry / h); mix(b.c.r); mix(b.c.g); mix(b.c.b); mix(b.a);
    }
    struct Entry { ComPtr<ID2D1RenderTarget> rt; UINT64 key = 0; ComPtr<ID2D1Bitmap> bmp; ULONGLONG used = 0; };
    static std::vector<Entry> cache;   // the render target is held so its address can't be reused by another one
    {   // entries not used for a while go (each one keeps a render target, and its memory, alive)
        ULONGLONG now = GetTickCount64();
        cache.erase(std::remove_if(cache.begin(), cache.end(), [&](const Entry& e) { return e.rt.Get() != rt && now - e.used > 20000; }), cache.end());
    }
    Entry* hit = nullptr;
    for (auto& e : cache) if (e.rt.Get() == rt && e.key == key) { hit = &e; break; }
    if (!hit) {
        std::vector<UINT32> px((size_t)bw * bh);
        for (UINT y = 0; y < bh; ++y)
            for (UINT x = 0; x < bw; ++x) {
                float fx = (x + 0.5f) / s, fy = (y + 0.5f) / s;
                float pr = 0, pg = 0, pb = 0, pa = 0;   // premultiplied, one blob over the other
                for (int i = 0; i < n; ++i) {
                    const WashBlob& b = blobs[i];
                    float dx = (fx - b.cx) / b.rx, dy = (fy - b.cy) / b.ry;
                    float t = 1 - std::sqrt(dx * dx + dy * dy);
                    if (t <= 0) continue;
                    float a = b.a * t * t * (3 - 2 * t);   // smoothstep: no edge anywhere
                    pr = b.c.r * a + pr * (1 - a);
                    pg = b.c.g * a + pg * (1 - a);
                    pb = b.c.b * a + pb * (1 - a);
                    pa = a + pa * (1 - a);
                }
                // the same tiny dither on all four channels (no colour speckles), fixed per pixel (no shimmer)
                UINT32 hsh = (x * 73856093u) ^ (y * 19349663u);
                hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                float d = (hsh & 0xFFFF) / 65536.0f;
                UINT32 A = (UINT32)std::min(255.0f, pa * 255 + d);
                auto ch = [&](float v) { return std::min<UINT32>(A, (UINT32)std::min(255.0f, v * 255 + d)); };
                px[(size_t)y * bw + x] = A << 24 | ch(pr) << 16 | ch(pg) << 8 | ch(pb);
            }
        ComPtr<ID2D1Bitmap> bmp;
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(bw, bh), px.data(), bw * 4,
                                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), bmp.Put())))
            return;
        if (cache.size() >= 8) {   // forget the one used longest ago
            auto old = std::min_element(cache.begin(), cache.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
            cache.erase(old);
        }
        Entry e;
        rt->AddRef();
        e.rt.Attach(rt);
        e.key = key;
        e.bmp = bmp;
        cache.push_back(e);
        hit = &cache.back();
    }
    hit->used = GetTickCount64();
    ComPtr<ID2D1BitmapBrush> br;
    if (FAILED(rt->CreateBitmapBrush(hit->bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), br.Put())))
        return;
    br->SetTransform(D2D1::Matrix3x2F::Scale(1 / s, 1 / s) * D2D1::Matrix3x2F::Translation(r.left, r.top));
    br->SetOpacity(std::min(1.0f, opacity));
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.Get());   // the glass's own shape clips it
}

// The northern lights on a glass surface (Nordlys theme)
static void PaintAurora(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, int role, int seed) {
    D2D1_COLOR_F c[3];
    AuroraColors(c, seed);
    float k = g_app.settings.auroraStrength / 100.0f * (g_app.theme.dark ? 1.0f : 0.8f);
    float w = r.right - r.left, h = r.bottom - r.top;
    auto with = [](D2D1_COLOR_F col, float a) { col.a = a; return col; };
    auto linear = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, const D2D1_GRADIENT_STOP* st, UINT32 n) {
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1LinearGradientBrush> lb;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, n, sc.Put()))) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), sc.Get(), lb.Put());
        return lb;
    };
    auto veil = [&](float cx, float cy, float rx, float ry, D2D1_COLOR_F col, float a) {
        // an eased fall-off (no hard edge where the light ends)
        D2D1_GRADIENT_STOP st[5] = {{0, with(col, a)}, {0.3f, with(col, a * 0.78f)}, {0.6f, with(col, a * 0.36f)}, {0.85f, with(col, a * 0.09f)}, {1, with(col, 0)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1RadialGradientBrush> rb;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 5, sc.Put())) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry), sc.Get(), rb.Put())))
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), rb.Get());   // the glass's own shape clips it
    };
    if (role == 1) {   // the top bar: the lights hang along its bottom edge, like over a horizon
        D2D1_GRADIENT_STOP glow[2] = {{0, with(c[1], 0)}, {1, with(c[1], 0.20f * k)}};
        if (auto g = linear(D2D1::Point2F(0, r.bottom - 14), D2D1::Point2F(0, r.bottom), glow, 2))
            rt->FillRectangle(D2D1::RectF(r.left + rad, std::max(r.top, r.bottom - 14), r.right - rad, r.bottom), g.Get());
        D2D1_GRADIENT_STOP band[3] = {{0, with(c[0], 0.85f * k)}, {0.5f, with(c[1], 0.95f * k)}, {1, with(c[2], 0.85f * k)}};
        if (auto g = linear(D2D1::Point2F(r.left, 0), D2D1::Point2F(r.right, 0), band, 3))
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + rad, r.bottom - 2.2f, r.right - rad, r.bottom), 1, 1), g.Get());
        return;
    }
    // panels, the dock and the beds: soft veils of light inside, and a glowing rim
    if (role == 2) {   // the dock is small: Direct2D's own gradients are smooth enough there
        veil(r.left + w * 0.22f, r.top - h * 0.05f, w * 0.75f, h * 0.95f, c[0], 0.16f * k);
        veil(r.right - w * 0.18f, r.top + h * 0.35f, w * 0.6f, h * 0.9f, c[2], 0.12f * k);
    } else {
        WashBlob b[2] = {{w * 0.22f, -h * 0.05f, w * 0.75f, h * 0.95f, c[0], 0.22f * k},
                         {w * 0.82f, h * 0.35f, w * 0.6f, h * 0.9f, c[2], 0.16f * k}};
        PaintWash(rt, r, rad, b, 2, 1);
    }
    D2D1_GRADIENT_STOP rim[3] = {{0, with(c[0], 0.75f * k)}, {0.5f, with(c[1], 0.45f * k)}, {1, with(c[2], 0.70f * k)}};
    if (auto g = linear(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom), rim, 3)) {
        D2D1_RECT_F in = D2D1::RectF(r.left + 0.75f, r.top + 0.75f, r.right - 0.75f, r.bottom - 0.75f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.0f, rad - 0.75f), std::max(0.0f, rad - 0.75f)), g.Get(), role == 2 ? 1.5f : 1.2f);
    }
}

void PaintLiquid(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, int role, int seed) {
    if (!rt || g_app.settings.performanceMode) return;
    if (AuroraOn()) { PaintAurora(rt, r, rad, role, seed); return; }
    if (!g_app.settings.liquidGlass) return;
    bool dark = g_app.theme.dark;
    float k = dark ? 1.0f : 0.85f;
    float h = r.bottom - r.top;
    // a soft sheen over the top, like light catching curved glass
    D2D1_GRADIENT_STOP st[3] = {{0, D2D1::ColorF(1, 1, 1, 0.13f * k)}, {0.5f, D2D1::ColorF(1, 1, 1, 0.035f * k)}, {1, D2D1::ColorF(1, 1, 1, 0)}};
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> sheen;
    if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, sc.Put())) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.top + h * 0.62f)),
                                                sc.Get(), sheen.Put())))
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), sheen.Get());
    // the rim: bright where the light comes from (top left), fading round to the bottom right
    D2D1_GRADIENT_STOP rs[3] = {{0, D2D1::ColorF(1, 1, 1, 0.55f * k)}, {0.45f, D2D1::ColorF(1, 1, 1, 0.14f * k)}, {1, D2D1::ColorF(1, 1, 1, 0.28f * k)}};
    ComPtr<ID2D1GradientStopCollection> rc;
    ComPtr<ID2D1LinearGradientBrush> rim;
    if (SUCCEEDED(rt->CreateGradientStopCollection(rs, 3, rc.Put())) &&
        SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)),
                                                rc.Get(), rim.Put()))) {
        D2D1_RECT_F in = D2D1::RectF(r.left + 0.75f, r.top + 0.75f, r.right - 0.75f, r.bottom - 0.75f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.0f, rad - 0.75f), std::max(0.0f, rad - 0.75f)), rim.Get(), 1.2f);
    }
}

void PaintVibe(ID2D1RenderTarget* rt, D2D1_RECT_F a, Part part, float opacity) {
    if (!rt || g_app.settings.material[(int)part] != MatVibe) return;
    D2D1_COLOR_F c[3];
    VibePalette(c);
    float w = a.right - a.left, h = a.bottom - a.top;
    bool strip = h < w * 0.35f;   // the top bar and the dock: a long strip; menus, cards and beds: a panel
    // panels hold text: a little softer, so it stays easy to read
    float alpha = (strip ? (g_app.theme.dark ? 0.40f : 0.32f) : (g_app.theme.dark ? 0.32f : 0.24f)) * opacity;
    // three soft colours, like the aurora behind VibeMenu (but still): along a strip, or round the corners of a panel
    const float xs[3] = {0.12f, 0.55f, 0.90f}, rs[3] = {0.45f, 0.40f, 0.42f}, as[3] = {1.0f, 0.85f, 0.8f};
    const float px[3] = {0.12f, 0.92f, 0.40f}, py[3] = {0.08f, 0.45f, 0.98f}, pr[3] = {0.75f, 0.62f, 0.70f};
    if (!strip) {   // a panel: worked out smoothly (no rings), see PaintWash
        float base = g_app.theme.dark ? 0.32f : 0.24f, m = std::max(w, h);
        WashBlob b[3];
        for (int i = 0; i < 3; ++i) b[i] = {w * px[i], h * py[i], m * pr[i], m * pr[i], c[i], base * as[i]};
        PaintWash(rt, a, 0, b, 3, opacity);
        return;
    }
    for (int i = 0; i < 3; ++i) {
        float r = strip ? std::max(w * rs[i], h * 1.6f) : std::max(w, h) * pr[i];
        D2D1_POINT_2F p = strip ? D2D1::Point2F(a.left + w * xs[i], a.top + h * (i == 1 ? 0.9f : 0.2f))
                                : D2D1::Point2F(a.left + w * px[i], a.top + h * py[i]);
        float pa = alpha * as[i];
        D2D1_GRADIENT_STOP st[5] = {{0, D2D1::ColorF(c[i].r, c[i].g, c[i].b, pa)}, {0.3f, D2D1::ColorF(c[i].r, c[i].g, c[i].b, pa * 0.78f)},
                                    {0.6f, D2D1::ColorF(c[i].r, c[i].g, c[i].b, pa * 0.36f)}, {0.85f, D2D1::ColorF(c[i].r, c[i].g, c[i].b, pa * 0.09f)},
                                    {1, D2D1::ColorF(c[i].r, c[i].g, c[i].b, 0)}};
        ComPtr<ID2D1GradientStopCollection> sc;
        ComPtr<ID2D1RadialGradientBrush> rb;
        if (FAILED(rt->CreateGradientStopCollection(st, 5, sc.Put())) ||
            FAILED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(p, D2D1::Point2F(0, 0), r, r),
                                                 sc.Get(), rb.Put())))
            continue;
        rt->FillRectangle(a, rb.Get());
    }
}

void ApplyGlassEffect(HWND h, Part part, bool cheap) {
    struct AP { int s, f; DWORD c; int a; };
    struct WD { int a; PVOID d; SIZE_T n; };
    using Fn = BOOL(WINAPI*)(HWND, WD*);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    const Theme& t = g_app.theme;
    BOOL dark = t.dark;
    DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    MARGINS m{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(h, &m);
    DWORD sbt = 1;   // DWMWA_SYSTEMBACKDROP_TYPE: none (only our accent)
    DwmSetWindowAttribute(h, 38, &sbt, sizeof sbt);
    if (!fn) return;
    // Switch the effect off first: Windows only picks up a change when it's switched on again
    AP off{0, 0, 0, 0};
    WD d0{19, &off, sizeof off};
    fn(h, &d0);
    bool acrylic = g_app.settings.material[(int)part] != MatClear;   // Vibe sits on acrylic too
    // Acrylic: the tint is the glass's own colour (like Skyline 1.x — the most frost).
    // Moving (cheap): plain blur with the same colour, which redraws much faster.
    int state = acrylic ? (cheap ? 3 : 4) : 2;
    BYTE c = t.dark ? 32 : 243;   // same as 2.0
    BYTE a = acrylic ? (BYTE)std::lround(std::clamp(TintPercent(part), 0, 100) * 2.55) : 0;
    if (acrylic && a < 0x08) a = 0x08;   // acrylic with no colour at all goes grey-white on some systems
    DWORD col = (DWORD)a << 24 | c << 16 | c << 8 | c;
    if (AuroraOn())   // Nordlys: the glass itself is night blue (icy blue in light mode); colour is 0xAABBGGRR
        col = t.dark ? ((DWORD)a << 24 | 44u << 16 | 20u << 8 | 10u) : ((DWORD)a << 24 | 250u << 16 | 236u << 8 | 222u);
    else if (BYTE rgb[3]; ToneColor(t.dark, rgb))   // a coloured theme: the glass takes its colour
        col = (DWORD)a << 24 | (DWORD)rgb[2] << 16 | (DWORD)rgb[1] << 8 | rgb[0];
    AP p{state, 2, col, 0};
    WD d{19, &p, sizeof p};
    fn(h, &d);
}

void IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), g_app.settings.path.c_str());
}

std::wstring JoinList(const std::vector<std::wstring>& v, const wchar_t* sep) {
    std::wstring out;
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}

std::vector<std::wstring> SplitList(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(sep, a);
        std::wstring t = s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
        while (!t.empty() && iswspace(t.back())) t.pop_back();
        if (!t.empty()) out.push_back(t);
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return out;
}

void SaveTraySettings() {
    const Settings& s = g_app.settings;
    std::wstring order, hidden;
    for (size_t i = 0; i < s.trayOrder.size(); ++i) { if (i) order += L"|"; order += s.trayOrder[i]; }
    for (size_t i = 0; i < s.hiddenTray.size(); ++i) { if (i) hidden += L", "; hidden += s.hiddenTray[i]; }
    WritePrivateProfileStringW(L"Tray", L"Order", order.c_str(), s.path.c_str());
    WritePrivateProfileStringW(L"Tray", L"Hidden", hidden.c_str(), s.path.c_str());
}

void UnhideTray(const std::wstring& key) {
    auto& h = g_app.settings.hiddenTray;
    if (key.empty()) h.clear();
    else h.erase(std::remove(h.begin(), h.end(), ToLower(key)), h.end());
    SaveTraySettings();
    // Show it right away, and ask apps to re-send their icons in case one
    // dropped or replaced its icon while it was hidden.
    if (g_app.tray) {
        g_app.tray->Reannounce();
        for (auto& t : g_app.tray->Icons()) t.iconGen = NextTrayGen();   // redraw bitmaps too
    }
    g_app.InvalidateAll();
    SetTimer(g_app.ctlWnd, 0x7111, 600, nullptr);   // and once more when the apps have answered
}

// ---------------------------------------------------------------------------
// Start menu alignment (Windows: Settings → Personalization → Taskbar → alignment).
// The dock is centred, so Start should open in the middle too. We remember what
// the user had and put it back when Skyline quits or the option is turned off.
static const wchar_t kAdvKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

static void TellExplorer() {
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 300, &r);
}

// Crash-safe: only the registry, the ini (path copied at start) and a non-blocking broadcast
wchar_t g_crashIni[MAX_PATH * 2] = {};
void RestoreStartAfterCrash() {
    if (!g_crashIni[0]) return;
    int saved = (int)GetPrivateProfileIntW(L"General", L"RestoreTaskbarAl", -1, g_crashIni);
    if (saved < 0) return;
    DWORD v = (DWORD)saved;
    RegSetKeyValueW(HKEY_CURRENT_USER, kAdvKey, L"TaskbarAl", REG_DWORD, &v, sizeof v);
    WritePrivateProfileStringW(L"General", L"RestoreTaskbarAl", nullptr, g_crashIni);
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings");
}

void ApplyStartAlignment(bool center) {
    const wchar_t* ini = g_app.settings.path.c_str();
    DWORD cur = 1, size = sizeof cur;
    bool has = RegGetValueW(HKEY_CURRENT_USER, kAdvKey, L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &cur, &size) == ERROR_SUCCESS;
    if (!has) cur = 1;   // Windows 11 default is centred
    int saved = (int)GetPrivateProfileIntW(L"General", L"RestoreTaskbarAl", -1, ini);
    if (center) {
        if (cur != 1) {
            if (saved < 0) WritePrivateProfileStringW(L"General", L"RestoreTaskbarAl", std::to_wstring(cur).c_str(), ini);
            DWORD one = 1;
            RegSetKeyValueW(HKEY_CURRENT_USER, kAdvKey, L"TaskbarAl", REG_DWORD, &one, sizeof one);
            TellExplorer();
        }
    } else if (saved >= 0) {
        DWORD v = (DWORD)saved;
        RegSetKeyValueW(HKEY_CURRENT_USER, kAdvKey, L"TaskbarAl", REG_DWORD, &v, sizeof v);
        WritePrivateProfileStringW(L"General", L"RestoreTaskbarAl", nullptr, ini);
        TellExplorer();
    }
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
Theme LoadTheme() {
    Theme t;
    DWORD light = 0, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    t.dark = light == 0;
    {
        // Windows accent colour (AccentPalette holds 8 shades; [1] = Light2, [5] = Dark1)
        BYTE pal[32];
        DWORD ps = sizeof pal;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                         L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &ps) == ERROR_SUCCESS && ps >= 32) {
            int i = t.dark ? 1 : 5;
            t.accent = {pal[i * 4] / 255.0f, pal[i * 4 + 1] / 255.0f, pal[i * 4 + 2] / 255.0f, 1};
        }
    }
    if (!t.dark) {
        t.fg = {0, 0, 0, 0.9f};
        t.fgDim = {0, 0, 0, 0.45f};
        t.hover = {0, 0, 0, 0.06f};
        t.press = {0, 0, 0, 0.035f};
        t.hairline = {0, 0, 0, 0.08f};
        t.solid = {0.953f, 0.953f, 0.953f, 1};
        t.green = {0.06f, 0.55f, 0.24f, 1};
        t.red = {0.77f, 0.17f, 0.11f, 1};
    }
    // The coloured themes bring their own accent (Midnight keeps Windows' own). LoadTheme runs after the settings.
    static const float accents[6][2][3] = {
        {{0, 0, 0}, {0, 0, 0}}, {{0, 0, 0}, {0, 0, 0}},
        {{0.31f, 0.82f, 0.91f}, {0.02f, 0.47f, 0.62f}},   // Ocean: turquoise
        {{1.00f, 0.62f, 0.42f}, {0.78f, 0.33f, 0.12f}},   // Sunset: warm orange
        {{0.43f, 0.86f, 0.60f}, {0.10f, 0.50f, 0.27f}},   // Forest: green
        {{1.00f, 0.56f, 0.72f}, {0.72f, 0.22f, 0.43f}},   // Rosé: pink
    };
    int tone = g_app.settings.glassTone;
    if (tone >= 2 && tone <= 5 && !g_app.settings.performanceMode) {
        const float* c = accents[tone][t.dark ? 0 : 1];
        t.accent = {c[0], c[1], c[2], 1};
    }
    return t;
}

// ---------------------------------------------------------------------------
// Foreground app
// ---------------------------------------------------------------------------
std::wstring FileDescriptionOf(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return L"";
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return L"";
    struct LangCp { WORD lang, cp; }* tr = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) || len < sizeof(LangCp)) return L"";
    wchar_t key[80];
    swprintf(key, 80, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].cp);
    wchar_t* val = nullptr;
    UINT vlen = 0;
    if (VerQueryValueW(buf.data(), key, (void**)&val, &vlen) && val && vlen > 1) {
        std::wstring s(val);
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        return s;
    }
    return L"";
}

static bool IgnoredForeground(HWND h, const std::wstring& exe) {
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    static const wchar_t* classes[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                                       L"TopLevelWindowForOverflowXamlIsland", L"XamlExplorerHostIslandWindow",
                                       L"ForegroundStaging", L"MultitaskingViewFrame", L"TaskSwitcherWnd",
                                       L"#32768" /* menus */, L"tooltips_class32"};
    for (auto c : classes) if (lstrcmpiW(cls, c) == 0) return true;
    static const wchar_t* exes[] = {L"searchhost.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
                                    L"searchapp.exe", L"lockapp.exe", L"shellhost.exe"};
    for (auto e : exes) if (exe == e) return true;
    return false;
}

struct ChildFind { DWORD hostPid; DWORD found; };
static BOOL CALLBACK FindUwpChild(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<ChildFind*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid && pid != f->hostPid) { f->found = pid; return FALSE; }
    return TRUE;
}

void App::UpdateForeground(HWND h) {
    if (!h || IsOurWindow(h)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::wstring path = ProcessImagePath(pid);
    std::wstring exe = ToLower(FileNameOf(path));
    if (IgnoredForeground(h, exe)) return;

    // Store apps live inside ApplicationFrameHost; the real process is a child window.
    if (exe == L"applicationframehost.exe") {
        ChildFind f{pid, 0};
        EnumChildWindows(h, FindUwpChild, (LPARAM)&f);
        if (f.found) { path = ProcessImagePath(f.found); exe = ToLower(FileNameOf(path)); }
    }

    std::wstring name;
    HICON icon = nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (exe == L"explorer.exe" && (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW"))) {
        name = TR(L"Skrivebord", L"Desktop");
    }

    auto it = g_appCache.find(path);
    if (it == g_appCache.end() && !path.empty()) {
        AppInfo info;
        info.name = FileDescriptionOf(path);
        if (info.name.empty()) {
            info.name = FileNameOf(path);
            size_t dot = info.name.find_last_of(L'.');
            if (dot != std::wstring::npos) info.name.resize(dot);
            if (!info.name.empty()) info.name[0] = towupper(info.name[0]);
        }
        HICON large = nullptr;
        if (SUCCEEDED(SHDefExtractIconW(path.c_str(), 0, 0, &large, nullptr, MAKELONG(32, 16))) && large)
            info.icon = large;
        it = g_appCache.emplace(path, info).first;
    }
    if (it != g_appCache.end()) {
        if (name.empty()) name = it->second.name;
        icon = it->second.icon;
    }
    if (exe == L"explorer.exe" && name.empty()) name = TR(L"Filutforsker", L"File Explorer");
    if (!icon) {
        // Fall back to the window's own icon
        DWORD_PTR r = 0;
        if (SendMessageTimeoutW(h, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 50, &r) && r) icon = (HICON)r;
        if (!icon) icon = (HICON)GetClassLongPtrW(h, GCLP_HICONSM);
        if (!icon) icon = (HICON)GetClassLongPtrW(h, GCLP_HICON);
        if (it != g_appCache.end() && icon) it->second.icon = CopyIcon(icon), icon = it->second.icon;
    }
    if (name.empty()) name = L"Windows";

    bool changed = name != fg.name || icon != fg.icon || h != fg.hwnd;
    fg.hwnd = h;
    fg.exe = exe;
    fg.path = path;
    if (changed) {
        fg.name = name;
        if (icon != fg.icon) { fg.icon = icon; fg.iconGen++; }
        fg.menus = MenusFor(h, exe);
        fg.menusGen++;
        InvalidateAll();
    }
}

// ---------------------------------------------------------------------------
// Fullscreen detection (games, videos). Our own, because Explorer's
// ABN_FULLSCREENAPP depends on its taskbar, which we hide.
// ---------------------------------------------------------------------------
static bool CoversMonitor(HWND f, HMONITOR* monOut) {
    if (!f || !IsWindowVisible(f) || IsIconic(f) || g_app.IsOurWindow(f)) return false;
    // A window the tiler placed is an ordinary window, even when it fills the screen (no gaps).
    if ((g_app.tiler && g_app.tiler->Manages(f)) || EnhancedManages(f)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(f, cls, 64);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW") || !lstrcmpW(cls, L"Shell_TrayWnd")) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(f, &pid);
    std::wstring exe = ToLower(FileNameOf(ProcessImagePath(pid)));
    // Shell surfaces (Start, Search, Task View, desktop …) are never a fullscreen game.
    static const wchar_t* shell[] = {L"startmenuexperiencehost.exe", L"searchhost.exe", L"shellexperiencehost.exe",
                                     L"shellhost.exe", L"lockapp.exe", L"explorer.exe"};
    for (auto s : shell) if (exe == s) return false;

    HMONITOR mon = MonitorFromWindow(f, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT r{};
    GetWindowRect(f, &r);
    // Borderless/fullscreen: covers the whole monitor (a pixel short is still "the whole": some games leave one).
    const int slack = 1;
    bool covers = r.left <= mi.rcMonitor.left + slack && r.top <= mi.rcMonitor.top + slack &&
                  r.right >= mi.rcMonitor.right - slack && r.bottom >= mi.rcMonitor.bottom - slack;
    if (!covers) return false;
    LONG style = GetWindowLongW(f, GWL_STYLE);
    bool caption = (style & WS_CAPTION) == WS_CAPTION;
    // Fullscreen = covers the monitor *without* a title bar. Games (also Unity's "fullscreen window", which is often
    // a maximized window) and browsers/players in fullscreen (F11, video) all drop the title bar. A window that keeps
    // it is just a big window: Vivaldi and Brave can start out exactly as big as the screen, and that hid the
    // top bar and the dock.
    if (!caption) { *monOut = mon; return true; }

    // Exclusive fullscreen D3D/Vulkan games report themselves here (even with an odd window style).
    QUERY_USER_NOTIFICATION_STATE q{};
    if (!IsZoomed(f) && SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN) {
        *monOut = mon;
        return true;
    }
    return false;
}

static bool g_primaryFullscreen = false, g_anyFullscreen = false;
bool PrimaryFullscreen() { return g_primaryFullscreen; }

static bool ExclusiveGame() {
    QUERY_USER_NOTIFICATION_STATE q{};
    return SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN;
}

bool GameMode() {
    if (g_anyFullscreen || ExclusiveGame()) return true;
    HMONITOR m = nullptr;
    return !StartMenuOpen() && CoversMonitor(GetForegroundWindow(), &m);   // right now, before the next check has run
}

// Start menus: ours (SkyMenu/SkyLauncher) or Windows' own (Start, Search).
static bool g_ourStart = false, g_winStart = false;
bool StartMenuOpen() { return g_ourStart || g_winStart; }

static void CheckFullscreen() {
    HMONITOR mon = nullptr;
    // With a Start menu open you want the bar and the dock, even on top of a game.
    bool fs = !StartMenuOpen() && CoversMonitor(GetForegroundWindow(), &mon);
    g_primaryFullscreen = fs && mon == MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    g_anyFullscreen = fs;
    // The screens changed while a game was up (it switched resolution): now that it's gone, catch up
    if (!fs && g_displayPending && g_app.ctlWnd && !ExclusiveGame()) SetTimer(g_app.ctlWnd, kTimerDisplay, 300, nullptr);
    for (auto& b : g_app.bars) b->SetFullscreen(fs && b->Monitor() == mon);
    for (auto& d : g_app.docks) d->SetFullscreen(fs && mon == d->Monitor());
    // While the bars are hidden for a fullscreen window, look again now and then: a missed event (a window that
    // changed size without telling) must never leave the bar and dock hidden for good.
    if (fs && g_app.ctlWnd) SetTimer(g_app.ctlWnd, kTimerFullscreen, 2000, nullptr);
    if (!fs && !GameMode()) {   // nothing is covering the screens: do what was put off during a game
        for (auto& b : g_app.bars) b->FlushPending();
        for (auto& d : g_app.docks) d->FlushPending();
        if (g_app.tiler) g_app.tiler->FlushPending();
        EnhancedFlushPending();
    }
}

void RecheckFullscreen() {
    if (g_app.ctlWnd) SetTimer(g_app.ctlWnd, kTimerFullscreen, 150, nullptr);
}

void KeepBarsOnTop() {
    for (auto& b : g_app.bars) b->KeepOnTop();
    for (auto& d : g_app.docks) d->KeepOnTop();
}

static void StartChanged() {
    CheckFullscreen();
    for (auto& b : g_app.bars) b->StartMenuChanged(StartMenuOpen());
    for (auto& d : g_app.docks) d->StartMenuChanged(StartMenuOpen());
    if (!StartMenuOpen()) SetTimer(g_app.ctlWnd, kTimerFullscreen, 150, nullptr);   // back to the game: check again once it's in front
}

void OurStartMenuShown(bool open) {
    if (open == g_ourStart) return;
    g_ourStart = open;
    StartChanged();
}

static bool IsWindowsStart(HWND h) {
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::wstring exe = ToLower(FileNameOf(ProcessImagePath(pid)));
    return exe == L"startmenuexperiencehost.exe" || exe == L"searchhost.exe" || exe == L"searchapp.exe";
}

static void CALLBACK FsLocationHook(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj == OBJID_WINDOW && h == GetForegroundWindow()) SetTimer(g_app.ctlWnd, kTimerFullscreen, 100, nullptr);
}

static void WatchForegroundMoves(HWND h) {
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (pid == g_fsLocPid) return;
    if (g_fsLocHook) UnhookWinEvent(g_fsLocHook);
    g_fsLocHook = nullptr;
    g_fsLocPid = pid;
    if (pid && pid != GetCurrentProcessId())
        g_fsLocHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, FsLocationHook,
                                      pid, 0, WINEVENT_OUTOFCONTEXT);
}

static void CALLBACK ForegroundHook(HWINEVENTHOOK, DWORD, HWND h, LONG idObj, LONG, DWORD, DWORD) {
    if (idObj != OBJID_WINDOW) return;
    static HWND prevFg = nullptr;   // the window that just lost focus is still on screen: keep a fresh picture of it
    if ((g_app.settings.dockMinimize || EnhancedSpacesAvailable() || g_app.settings.stageEnabled) && prevFg && prevFg != h && IsWindow(prevFg) && !IsIconic(prevFg)) EnhancedSnapshotKick(prevFg, false);
    prevFg = h;
    WatchForegroundMoves(h);
    bool winStart = IsWindowsStart(h);
    if (winStart != g_winStart) { g_winStart = winStart; StartChanged(); }
    CheckFullscreen();                                        // right away when switching apps
    SetTimer(g_app.ctlWnd, kTimerFullscreen, 400, nullptr);   // and again once the game has settled
    for (auto& d : g_app.docks) d->OnForeground(h);
    Garden::OnForeground(h);
    StageManager::OnForeground(h);
    g_app.UpdateForeground(h);
    if (g_app.fg.hwnd == h) StartMenus::NoteForeground(h, g_app.fg.path);   // VibeMenu learns what you use (only when it's chosen)
    // A window coming to the front must never cover the top bar (seen with tiling)
    if (!winStart) for (auto& b : g_app.bars) b->KeepOnTop();
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
static BOOL CALLBACK CollectMonitors(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
    return TRUE;
}

// Screens from left to right, the way "Skjerm 1, 2, 3" is counted in Settings.
std::vector<HMONITOR> MonitorsLeftToRight() {
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(m);
        return TRUE;
    }, (LPARAM)&mons);
    auto pos = [](HMONITOR m) { MONITORINFO mi{sizeof mi}; GetMonitorInfoW(m, &mi); return std::make_pair(mi.rcMonitor.left, mi.rcMonitor.top); };
    std::sort(mons.begin(), mons.end(), [&](HMONITOR a, HMONITOR b) { return pos(a) < pos(b); });
    return mons;
}

void App::RebuildDocks() {
    auto old = std::move(docks);   // same as the bars: out of the list first
    docks.clear();
    // Docks are rebuilt on every settings change. If the taskbar stays hidden,
    // don't flash it back in between.
    if (settings.dockEnabled && settings.hideTaskbar)
        for (auto& d : old) d->KeepTaskbarHidden();
    old.clear();
    if (!settings.dockEnabled) return;
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::vector<HMONITOR> mons;
    auto all = MonitorsLeftToRight();
    int k = settings.dockMonitor - 2;
    if (settings.dockMonitor == 1) {
        mons.push_back(primary);                       // primary first: it owns the taskbar hiding
        for (HMONITOR m : all) if (m != primary) mons.push_back(m);
    } else if (settings.dockMonitor == kDockChosen) {
        // the screens the user picked; the main screen first if it's one of them (that dock hides the taskbar)
        for (int i : settings.dockScreens)
            if (i >= 0 && i < (int)all.size() && all[i] == primary) mons.push_back(primary);
        for (int i : settings.dockScreens)
            if (i >= 0 && i < (int)all.size() && all[i] != primary && std::find(mons.begin(), mons.end(), all[i]) == mons.end()) mons.push_back(all[i]);
        if (mons.empty()) mons.push_back(primary);   // none of them connected right now
    } else if (k >= 0 && k < (int)all.size()) {
        mons.push_back(all[k]);
    } else {
        mons.push_back(primary);                       // 0, or a screen that's not connected now
    }
    for (size_t i = 0; i < mons.size(); ++i) {
        auto d = std::make_unique<Dock>();
        if (d->Create(mons[i], i == 0)) docks.push_back(std::move(d));
    }
}

Dock* App::DockOn(HMONITOR mon) const {
    for (auto& d : docks) if (d->Monitor() == mon) return d.get();
    return nullptr;
}

void App::RebuildBars() {
    // Take the old bars out of the list before destroying them: while they are
    // torn down Windows can deliver events (focus, fullscreen) that walk the list.
    auto old = std::move(bars);
    bars.clear();
    old.clear();
    std::vector<HMONITOR> mons;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitors, (LPARAM)&mons);
    for (HMONITOR m : mons) {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(m, &mi);
        bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        if (!primary && !settings.allMonitors) continue;
        auto b = std::make_unique<Bar>(m, primary);
        if (b->Create()) bars.push_back(std::move(b));
    }
}

void App::InvalidateAll() { for (auto& b : bars) b->Invalidate(); }

void App::InvalidateTray() {
    if (Bar* b = PrimaryBar()) b->Invalidate();
}

Bar* App::PrimaryBar() {
    for (auto& b : bars) if (b->Primary()) return b.get();
    return bars.empty() ? nullptr : bars.front().get();
}

bool App::IsOurWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

void App::ReloadTheme() {
    theme = LoadTheme();
    ApplyMenuTheme(theme.dark);
    for (auto& b : bars) b->ThemeChanged();
    for (auto& d : docks) d->ThemeChanged();
    Garden::ThemeChanged();
    NowPlaying::Restyle();
    Flyouts::ApplySettings();
}

void App::Quit() { PostQuitMessage(0); }

void App::ApplyHotCorners() {
    corners.reset();
    if (settings.hotCorners) {
        corners = std::make_unique<HotCorners>();
        corners->Start();
    }
}

// ---------------------------------------------------------------------------
static void ArmClockTimer() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    UINT ms = g_app.settings.showSeconds ? 1000 - st.wMilliseconds
                                         : (60 - st.wSecond) * 1000 - st.wMilliseconds;
    SetTimer(g_app.ctlWnd, kTimerClock, ms + 20, nullptr);
}

static LRESULT CALLBACK CtlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case kTimerDeferred:
            if (g_menuDepth > 0) break;
            KillTimer(h, kTimerDeferred);
            for (unsigned c : {2u, 3u})
                if (g_deferred & (1u << c)) PostMessageW(h, WM_SKY_STATUS, c, 0);
            g_deferred = 0;
            break;
        case kTimerClock:
            g_app.InvalidateAll();
            ArmClockTimer();
            break;
        case kTimerPurge:
            if (g_app.tray && g_app.tray->PurgeDead()) g_app.InvalidateTray();
            StartMenus::Purge();
            break;
        case kTimerNetwork:
            g_app.status->RefreshNetwork();
            g_app.InvalidateAll();
            break;
        case 0x7111:   // after unhide
            KillTimer(h, 0x7111);
            g_app.InvalidateAll();
            break;
        case 0x7110:   // tiling debounce
            if (g_app.tiler) g_app.tiler->OnTimer();
            break;
        case 0x7112:   // Enhanced Tiling debounce
            EnhancedTimer();
            break;
        case 0x7113:   // Enhanced Tiling window glide
            EnhancedAnimTimer();
            break;
        case 0x7114:   // Enhanced Tiling focus follows mouse
            EnhancedFocusTimer();
            break;
        case kTimerFullscreen:
            KillTimer(h, kTimerFullscreen);
            CheckFullscreen();
            break;
        case kTimerDisplay:   // the screens changed (several windows say so at once: once is enough)
            KillTimer(h, kTimerDisplay);
            if (GameMode()) {
                // A game in (exclusive) fullscreen switches resolution: creating, moving or showing windows now
                // makes many games (Call of Duty and others) drop out of fullscreen, minimize or freeze. Wait.
                g_displayPending = true;
                SetTimer(h, kTimerGameWait, 2000, nullptr);
                break;
            }
            g_displayPending = false;
            KillTimer(h, kTimerGameWait);
            g_app.RebuildBars(); g_app.RebuildDocks(); g_app.ApplyHotCorners();
            CheckFullscreen();
            SetTimer(h, kTimerFullscreen, 300, nullptr);
            break;
        case kTimerGameWait:   // only while a rebuild is waiting for a game to end
            if (!g_displayPending) { KillTimer(h, kTimerGameWait); break; }
            if (!GameMode()) { KillTimer(h, kTimerGameWait); SetTimer(h, kTimerDisplay, 10, nullptr); }
            break;
        case kTimerDebounce:
            KillTimer(h, kTimerDebounce);
            g_app.status->RefreshVolume();
            g_app.status->RefreshNetwork();
            g_app.InvalidateAll();
            Flyouts::StatusChanged();
            break;
        }
        return 0;
    case WM_HOTKEY:
        if ((int)wp == StartMenus::kSpotlightHotkey) { StartMenus::ToggleSpotlight(); return 0; }
        if ((int)wp == WritingTools::kHotkeyId) { WritingTools::OnHotkey(); return 0; }
        if (EnhancedHotkey((int)wp)) return 0;
        if (g_app.tiler) g_app.tiler->OnHotkey((int)wp);
        return 0;
    case WM_SKY_STATUS:
        if ((wp == 2 || wp == 3) && g_menuDepth > 0) {
            // A menu of one of our bars/docks is open: rebuilding now would destroy its window. Later.
            g_deferred |= 1u << wp;
            SetTimer(h, kTimerDeferred, 300, nullptr);
            return 0;
        }
        switch (wp) {
        case 0: SetTimer(h, kTimerDebounce, 120, nullptr); break;                  // something changed
        case 1: g_app.status->RefreshAudioDevice(); g_app.InvalidateAll(); Flyouts::StatusChanged(); break;  // audio device
        case 2:                                                                     // reload settings
            g_app.settings = LoadSettings();
            g_app.theme = LoadTheme();   // a theme may bring its own accent colour
            LoadMenuProfiles();
            if (g_app.fg.hwnd) { HWND f = g_app.fg.hwnd; g_app.fg.hwnd = nullptr; g_app.UpdateForeground(f); }
            g_app.RebuildBars();
            g_app.RebuildDocks();
            if (g_app.settings.trayEnabled && !g_app.tray) {
                g_app.tray = std::make_unique<TrayHost>();
                if (!g_app.tray->Start()) g_app.tray.reset();
                else if (Bar* b = g_app.PrimaryBar()) b->Reposition();
            } else if (!g_app.settings.trayEnabled && g_app.tray) {
                g_app.tray->Stop();
                g_app.tray.reset();
                g_app.InvalidateAll();
            }
            EnhancedApply();
            StageManager::Apply();
            DeskClockApply();
            WritingTools::ApplyHotkey();
            g_app.ApplyHotCorners();
            ApplyStartAlignment(g_app.settings.centerStart);
            StartMenus::ApplySettings();
            NowPlaying::ApplySettings();
            Garden::ApplySettings();
            Flyouts::ApplySettings();
            ArmClockTimer();
            break;
        case 3:   // monitors changed (also when a game switches resolution): see kTimerDisplay
            SetTimer(h, kTimerDisplay, 250, nullptr);
            break;
        case 4: g_app.ReloadTheme(); break;                                         // light/dark
        case 5: SetTimer(h, kTimerFullscreen, 50, nullptr); break;                  // fullscreen hint
        case 10: case 11: case 12: case 13: HotCorners::Run((int)wp - 10); break;  // hot corner reached
        case 20: StartMenus::Toggle(); break;
        case 23: if (MissionControl::Enabled()) MissionControl::Toggle(); break;
        case 24: SettingsChanged(); break;   // SkyWritingTools fetched the list of models   // Win+Tab, caught by the keyboard hook
        case 22: Garden::DesktopMenu({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}); break;   // right-click on the empty desktop
        case 21: OnIconLoaded(lp); StartMenus::IconsArrived(); Garden::IconsArrived(); break;               // an icon finished loading                                       // Windows key -> our Start menu
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    int helperExit = 0;
    if (Update::HandleCommandLine(&helperExit)) return helperExit;   // elevated update helper, no UI

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Skyline.TopBar.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Right after an update the old Skyline is still giving the taskbar back: wait for it, then take over.
        DWORD w = Update::JustUpdated() ? WaitForSingleObject(mutex, 60000) : WAIT_TIMEOUT;
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    }

    // Fast startup can start us before Explorer has put its taskbar up: wait for it (we hide it and take over the tray)
    if (!FindWindowW(L"Shell_TrayWnd", nullptr)) {
        for (int i = 0; i < 300 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i) Sleep(100);
        Sleep(400);   // and let it settle
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);   // COM (single-threaded) + drag and drop (files onto the dock's Recycle Bin)
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    g_app.hinst = hinst;
    if (!g_gfx.Init()) {
        MessageBoxW(nullptr, TR(L"Direct2D kunne ikke startes.", L"Direct2D could not be started."), L"Skyline", MB_ICONERROR);
        return 1;
    }
    g_app.settings = LoadSettings();
    SystemTheme::CleanUpLeftovers();   // the removed system-wide theme: give back anything it changed
    g_app.theme = LoadTheme();
    g_app.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc{};
    wc.lpfnWndProc = CtlProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"SkylineCtl";
    RegisterClassW(&wc);
    g_app.ctlWnd = CreateWindowExW(0, L"SkylineCtl", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, nullptr);

    // Message-only windows don't hear about logoff/shutdown, so a hidden top-level window
    // listens for it and gives the taskbar, Start position and desktop icons back in time.
    WNDCLASSW sc{};
    sc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_QUERYENDSESSION) return TRUE;
        if (m == WM_ENDSESSION && w) {
            StartMenus::SaveVibeUsage();
            Garden::Shutdown();
            ApplyStartAlignment(false);
            Look::LeavingWindows();
            g_app.docks.clear();   // the owner dock restores the taskbar (auto-hide off, visible)
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    sc.hInstance = hinst;
    sc.lpszClassName = L"SkylineSession";
    RegisterClassW(&sc);
    CreateWindowExW(WS_EX_TOOLWINDOW, L"SkylineSession", L"Skyline", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hinst, nullptr);

    g_app.status = std::make_unique<StatusMonitor>();
    g_app.status->Start(g_app.ctlWnd);

    ApplyMenuTheme(g_app.theme.dark);
    LoadMenuProfiles();
    g_app.UpdateForeground(GetForegroundWindow());
    g_app.RebuildBars();

    if (g_app.settings.trayEnabled) {
        g_app.tray = std::make_unique<TrayHost>();
        if (!g_app.tray->Start()) g_app.tray.reset();
        else if (Bar* b = g_app.PrimaryBar()) b->Reposition();
    }

    // If a previous run died without giving the taskbar back, do it now.
    if (!g_app.settings.dockEnabled || !g_app.settings.hideTaskbar) TaskbarHider::RestoreAfterCrash();
    // Before anything is changed in Windows: if we crash, give it all back
    lstrcpynW(g_crashIni, g_app.settings.path.c_str(), MAX_PATH * 2);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        TaskbarHider::RestoreAfterCrash();   // never leave the user without a taskbar
        Garden::RestoreAfterCrash();         // … or without desktop icons
        RestoreStartAfterCrash();            // … or with Start moved
        return EXCEPTION_CONTINUE_SEARCH;
    });
    std::set_terminate([] {                  // std::terminate / abort paths skip the filter above
        TaskbarHider::RestoreAfterCrash();
        Garden::RestoreAfterCrash();
        RestoreStartAfterCrash();
        std::abort();
    });
    g_app.RebuildDocks();
    Look::Starting();   // performance mode: Windows' transparency off again (it's given back whenever we quit)

    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundHook, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_app.tiler = std::make_unique<Tiler>();
    EnhancedApply();
    StageManager::Apply();
    DeskClockApply();
    g_app.ApplyHotCorners();
    ApplyStartAlignment(g_app.settings.centerStart);
    StartMenus::ApplySettings();
    NowPlaying::ApplySettings();
    if (!g_app.settings.gardenEnabled) Garden::RestoreAfterCrash();   // a killed Skyline may have left them hidden
    Garden::ApplySettings();
    Update::Start();
    WatchForegroundMoves(GetForegroundWindow());
    SetTimer(g_app.ctlWnd, kTimerFullscreen, 500, nullptr);
    ArmClockTimer();
    SetTimer(g_app.ctlWnd, kTimerPurge, 5000, nullptr);
    SetTimer(g_app.ctlWnd, kTimerNetwork, 30000, nullptr);
    WritingTools::ApplyHotkey();   // SkyWritingTools (off by default: then nothing)
    std::thread(MigrateStartup, g_app.settings.path).detach();   // fast startup is the default now: move an old autostart over (once)

    // Trim the working set once everything is up: Skyline mostly sleeps.
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_app.corners.reset();
    StartMenus::Shutdown();
    NowPlaying::Shutdown();
    Garden::Shutdown();
    ApplyStartAlignment(false);   // give the user's own Start position back
    StageManager::Shutdown();   // every window put away comes back
    EnhancedShutdown();
    if (g_app.tiler) {
        g_app.tiler->Stop();
        g_app.settings.tilingMode = 0;
        g_app.settings.tilingEnabled = false;
        g_app.tiler->ApplySettings();   // unregisters hotkeys
        g_app.tiler.reset();
    }
    if (g_fgHook) UnhookWinEvent(g_fgHook);
    if (g_fsLocHook) UnhookWinEvent(g_fsLocHook);
    g_app.docks.clear();
    if (g_app.tray) { g_app.tray->Stop(); g_app.tray.reset(); }
    g_app.bars.clear();
    g_app.status->Stop();
    g_app.status.reset();
    Look::LeavingWindows();   // Windows' transparency back, if performance mode took it
    Flyouts::Hide();          // let go of the apps' audio sessions while COM is still up
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    OleUninitialize();
    return 0;
}
