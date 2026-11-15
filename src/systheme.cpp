// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Clean-up after the experimental "system-wide theme" (2.27–2.29, removed in 2.30) — see systheme.h
#include "systheme.h"
#include "app.h"
#include <string>

namespace {

constexpr DWORD kBorder = 34, kCaption = 35, kText = 36;   // DWMWA_BORDER_COLOR / CAPTION_COLOR / TEXT_COLOR
constexpr COLORREF kDefault = 0xFFFFFFFF;                    // DWMWA_COLOR_DEFAULT
constexpr wchar_t kUndo[] = L"SystemThemeUndo";
constexpr wchar_t kAccentKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";
constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";

// ---- Windows' accent colour -------------------------------------------------
bool Unhex(const std::wstring& s, BYTE* p, DWORD n) {
    if (s.size() != n * 2) return false;
    auto v = [](wchar_t c) { return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10 : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1; };
    for (DWORD i = 0; i < n; ++i) {
        int a = v(s[i * 2]), b = v(s[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        p[i] = (BYTE)(a * 16 + b);
    }
    return true;
}
std::wstring IniGet(const wchar_t* key) {
    wchar_t b[128] = {};
    GetPrivateProfileStringW(kUndo, key, L"", b, 128, g_app.settings.path.c_str());
    return b;
}

void WriteDword(const wchar_t* key, const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &v, sizeof v);
}

struct DwordValue { const wchar_t* key; const wchar_t* name; const wchar_t* ini; };
const DwordValue kDwords[] = {
    {kAccentKey, L"AccentColorMenu", L"AccentColorMenu"},
    {kAccentKey, L"StartColorMenu", L"StartColorMenu"},
    {kDwmKey, L"AccentColor", L"DwmAccentColor"},
    {kDwmKey, L"ColorizationColor", L"ColorizationColor"},
    {kDwmKey, L"ColorizationAfterglow", L"ColorizationAfterglow"},
};

void Broadcast() { SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet"); }

void RestoreOriginal() {
    std::wstring p = IniGet(L"Palette");
    if (p.empty()) return;
    BYTE pal[32];
    if (p == L"-") RegDeleteKeyValueW(HKEY_CURRENT_USER, kAccentKey, L"AccentPalette");
    else if (Unhex(p, pal, 32)) RegSetKeyValueW(HKEY_CURRENT_USER, kAccentKey, L"AccentPalette", REG_BINARY, pal, 32);
    for (const auto& d : kDwords) {
        std::wstring v = IniGet(d.ini);
        if (v.empty()) continue;
        if (v == L"-") RegDeleteKeyValueW(HKEY_CURRENT_USER, d.key, d.name);
        else WriteDword(d.key, d.name, (DWORD)std::wcstoul(v.c_str(), nullptr, 10));
    }
    WritePrivateProfileStringW(kUndo, nullptr, nullptr, g_app.settings.path.c_str());   // forget the copy
    Broadcast();
}

} // namespace

namespace SystemTheme {

void CleanUpLeftovers() {
    const std::wstring& ini = g_app.settings.path;
    bool wasOn = GetPrivateProfileIntW(L"Advanced", L"SystemTheme", 0, ini.c_str()) != 0;
    if (!wasOn && IniGet(L"Palette").empty()) return;   // never used: nothing to do
    if (wasOn) {
        // windows coloured by the old version: Windows' own colours again
        EnumWindows([](HWND h, LPARAM) -> BOOL {
            COLORREF c = kDefault;
            DwmSetWindowAttribute(h, kBorder, &c, sizeof c);
            DwmSetWindowAttribute(h, kCaption, &c, sizeof c);
            DwmSetWindowAttribute(h, kText, &c, sizeof c);
            return TRUE;
        }, 0);
    }
    RestoreOriginal();   // the user's own accent colour, if we had changed it
    for (const wchar_t* k : {L"SystemTheme", L"SystemThemeBorders", L"SystemThemeAccent"})
        WritePrivateProfileStringW(L"Advanced", k, nullptr, ini.c_str());
}

} // namespace SystemTheme
