// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Themes (look presets) and performance mode.
//
// A theme only writes the glass settings of every part into skyline.ini (material + tint), plus Liquid Glass on/off;
// the look it replaced is kept in [LookUndo] so one click brings it back. "Standard" is Skyline's own defaults.
// Performance mode copies the whole skyline.ini aside, makes everything plain (no plugins but Spotlight, classic
// top bar, no transparency here or in Windows), and copies it back when it's turned off.
#include "app.h"
#include "backup.h"
#include <shlwapi.h>

namespace {

struct Key { const wchar_t* sec; const wchar_t* key; };
// Every setting a theme touches, in the order of Part (material, then tint)
const Key kMat[] = {{L"Bar", L"Material"}, {L"Dock", L"Material"}, {L"Start", L"Material"}, {L"Bar", L"CardMaterial"}, {L"NowPlaying", L"Material"},
                    {L"Garden", L"Material"}, {L"Spotlight", L"Material"}, {L"Vibe", L"Material"}, {L"Flyouts", L"Material"}};
const Key kTint[] = {{L"Bar", L"BarTint"}, {L"Bar", L"Tint"}, {L"Start", L"Tint"}, {L"Bar", L"CardTint"}, {L"NowPlaying", L"CardTint"},
                     {L"Garden", L"Tint"}, {L"Spotlight", L"Tint"}, {L"Vibe", L"Tint"}, {L"Flyouts", L"Tint"}};
const Key kExtra[] = {{L"Look", L"LiquidGlass"}, {L"Look", L"Glassiness"}, {L"Look", L"Theme"}, {L"Look", L"Aurora"}};
constexpr int kParts = 9;
static_assert(sizeof kMat / sizeof kMat[0] == kParts && sizeof kTint / sizeof kTint[0] == kParts);

std::wstring Read(const Key& k) {
    wchar_t b[64] = {};
    GetPrivateProfileStringW(k.sec, k.key, L"\x01", b, 64, g_app.settings.path.c_str());
    return b;
}
void Write(const Key& k, const std::wstring& v) {
    // "\x01" = the key wasn't there: remove it again, so the program default applies
    WritePrivateProfileStringW(k.sec, k.key, v == L"\x01" ? nullptr : v.c_str(), g_app.settings.path.c_str());
}

void Reload() { PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 2, 0); }

void SaveUndo() {
    std::wstring sec;
    auto add = [&](const Key& k, int i) { sec += L"k" + std::to_wstring(i) + L"=" + Read(k); sec.push_back(L'\0'); };
    int i = 0;
    for (auto& k : kMat) add(k, i++);
    for (auto& k : kTint) add(k, i++);
    for (auto& k : kExtra) add(k, i++);
    sec.push_back(L'\0');
    WritePrivateProfileSectionW(L"LookUndo", sec.c_str(), g_app.settings.path.c_str());
}

// Liquid Glass: the slider picks how much colour sits in the glass. Too little and what's behind bleeds through.
int LiquidTint(int glassiness, int extra) { return std::clamp(72 - glassiness * 45 / 100 + extra, 12, 100); }

std::wstring PerfBackupPath() { return g_app.settings.path + L".performance"; }

DWORD WindowsTransparency() {   // 1 = on (Windows' default), 0 = off
    DWORD v = 1, size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency",
                 RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
void SetWindowsTransparency(DWORD v) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"EnableTransparency", 0, REG_DWORD, (const BYTE*)&v, sizeof v);
    RegCloseKey(k);
    // tell Windows and the apps (Explorer picks it up from this)
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet");   // doesn't wait for every window
}

} // namespace

namespace Look {

const wchar_t* ThemeName(int i) {
    switch (i) {
    case 1: return L"Liquid Glass";
    case 2: return L"Vibe";
    case 3: return TR(L"Frostet", L"Frosted");
    case 4: return TR(L"Matt (minst ressurser)", L"Matte (lightest)");
    case 5: return TR(L"Nordlys (vibe coded)", L"Nordlys – northern lights (vibe coded)");
    default: return TR(L"Standard (Skyline)", L"Standard (Skyline)");
    }
}

void ApplyTheme(int theme) {
    SaveUndo();
    const Settings& s = g_app.settings;
    // Standard: Skyline's own defaults
    int tint[kParts] = {72, 72, 72, 84, 84, 72, 80, 76, 80};
    std::wstring mat[kParts];
    for (auto& m : mat) m = L"acrylic";
    bool liquid = false;
    switch (theme) {
    case 1:   // Liquid Glass: less colour, bright rims; cards and menus a little more opaque so text stays easy to read
        liquid = true;
        for (int i = 0; i < kParts; ++i) tint[i] = LiquidTint(s.glassiness, i >= 2 ? 10 : 0);
        break;
    case 2:   // Vibe: everything in the colours of the hour (VibeMenu has them already)
        for (int i = 0; i < kParts; ++i)
            if (i != 7) mat[i] = L"vibe";
        tint[0] = tint[1] = 62;
        break;
    case 3:   // Frosted: more of the blur, less colour
        for (int i = 0; i < kParts; ++i) tint[i] = i >= 2 ? 62 : 50;
        break;
    case 4:   // Matte: no blur at all (cheapest for the graphics card), almost solid
        for (int i = 0; i < kParts; ++i) { mat[i] = L"clear"; tint[i] = 94; }
        break;
    case 5:   // Nordlys: night-blue glass, the lights painted on every part (see PaintLiquid)
        for (int i = 0; i < kParts; ++i) tint[i] = i >= 2 ? 72 : 60;
        break;
    default: break;
    }
    for (int i = 0; i < kParts; ++i) {
        Write(kMat[i], mat[i]);
        Write(kTint[i], std::to_wstring(tint[i]));
    }
    Write(kExtra[0], liquid ? L"1" : L"0");
    Write(kExtra[3], theme == 5 ? L"1" : L"0");
    Write(kExtra[2], std::to_wstring(std::clamp(theme, 0, kThemes - 1)));
    Reload();
}

bool CanUndo() {
    wchar_t b[8] = {};
    return GetPrivateProfileStringW(L"LookUndo", L"k0", L"", b, 8, g_app.settings.path.c_str()) > 0;
}

void Undo() {
    if (!CanUndo()) return;
    std::vector<std::wstring> v;
    int n = kParts * 2 + (int)(sizeof kExtra / sizeof kExtra[0]);
    for (int i = 0; i < n; ++i) {
        wchar_t b[64] = {};
        GetPrivateProfileStringW(L"LookUndo", (L"k" + std::to_wstring(i)).c_str(), L"\x01", b, 64, g_app.settings.path.c_str());
        v.push_back(b);
    }
    SaveUndo();   // undo the undo with the same button
    int i = 0;
    for (auto& k : kMat) Write(k, v[i++]);
    for (auto& k : kTint) Write(k, v[i++]);
    for (auto& k : kExtra) Write(k, v[i++]);
    Reload();
}

void SetGlassiness(int g) {
    g = std::clamp(g, 0, 100);
    WritePrivateProfileStringW(L"Look", L"Glassiness", std::to_wstring(g).c_str(), g_app.settings.path.c_str());
    g_app.settings.glassiness = g;
    for (int i = 0; i < kParts; ++i) Write(kTint[i], std::to_wstring(LiquidTint(g, i >= 2 ? 10 : 0)));
    // no reload here: the slider batches it (the settings window applies once you stop dragging)
}

// Performance mode turns Windows' transparency off; that is given back whenever Skyline quits, and taken again at start
void LeavingWindows() {
    if (!g_app.settings.performanceMode) return;
    wchar_t b[8] = {};
    GetPrivateProfileStringW(L"Advanced", L"WindowsTransparency", L"1", b, 8, g_app.settings.path.c_str());
    SetWindowsTransparency((DWORD)_wtoi(b));
}
// Before a restore or factory reset: performance mode ends here and now (its saved copy would otherwise undo the
// restore the day it's turned off).
void DropPerformanceMode() {
    if (!g_app.settings.performanceMode) return;
    LeavingWindows();
    DeleteFileW(PerfBackupPath().c_str());
    WritePrivateProfileStringW(L"Advanced", L"Performance", L"0", g_app.settings.path.c_str());
}

void Starting() {
    if (g_app.settings.performanceMode && WindowsTransparency() != 0) SetWindowsTransparency(0);
}

bool SetPerformanceMode(bool on) {
    const std::wstring& ini = g_app.settings.path;
    if (on) {
        if (g_app.settings.performanceMode) return true;
        // everything as it is now, to come back to
        WritePrivateProfileStringW(L"Advanced", L"WindowsTransparency", std::to_wstring(WindowsTransparency()).c_str(), ini.c_str());
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());   // flush the ini cache
        if (!CopyFileW(ini.c_str(), PerfBackupPath().c_str(), FALSE)) return false;
        auto w = [&](const wchar_t* s, const wchar_t* k, const wchar_t* v) { WritePrivateProfileStringW(s, k, v, ini.c_str()); };
        // plugins off, except Spotlight
        w(L"NowPlaying", L"Enabled", L"0");
        w(L"Garden", L"Enabled", L"0");
        w(L"DockClock", L"Enabled", L"0");
        w(L"Flyouts", L"Enabled", L"0");
        w(L"SkyWritingTools", L"Enabled", L"0");
        w(L"Dock", L"Trash", L"0");
        w(L"Dock", L"Stack", L"0");
        // plain: classic top bar, no styles, no transparency
        w(L"Bar", L"Style", L"0");
        w(L"Vibe", L"Fullscreen", L"0");
        w(L"Look", L"LiquidGlass", L"0");
        w(L"Look", L"Aurora", L"0");
        for (int i = 0; i < kParts; ++i) { Write(kMat[i], L"clear"); Write(kTint[i], L"100"); }
        w(L"Advanced", L"Performance", L"1");
        SetWindowsTransparency(0);
    } else {
        if (!g_app.settings.performanceMode) return true;
        wchar_t b[8] = {};
        GetPrivateProfileStringW(L"Advanced", L"WindowsTransparency", L"1", b, 8, ini.c_str());
        DWORD prev = (DWORD)_wtoi(b);
        std::wstring backup = PerfBackupPath();
        if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            Backup::MachineState state;   // what Skyline changed in Windows meanwhile still has to be undone later
            state.Read(ini);
            CopyFileW(backup.c_str(), ini.c_str(), FALSE);   // exactly what you had before
            DeleteFileW(backup.c_str());
            state.Write(ini);
        }
        WritePrivateProfileStringW(L"Advanced", L"Performance", L"0", ini.c_str());
        SetWindowsTransparency(prev);
    }
    Reload();
    return true;
}

} // namespace Look
