// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Every app in Windows' "All apps" list (shell:AppsFolder): desktop programs and
// Store apps alike. Shared by SkyMenu and SkyLauncher.
struct AppEntry {
    std::wstring name;
    std::wstring target;     // what we launch: shell:AppsFolder\... or an ms-settings: page
    std::wstring exe;        // full exe path when known (sharper icon, run as admin, file location)
    std::wstring key;        // lowercase target: identity for pins and ordering
    std::wstring alt;        // extra words to match in search (settings pages: the other language)
    bool setting = false;    // a Windows settings page, not an app
    bool direct = false;     // the AppsFolder entry *is* the exe (not a shortcut to a launcher like Update.exe)
    bool file = false;       // a file or folder from file search (target = its path)
    bool folder = false;     // … and it's a folder
};

// The app list, sorted by name. Re-read from the shell if older than maxAgeMs.
const std::vector<AppEntry>& AllApps(DWORD maxAgeMs = 30000);
// Windows settings pages that search should find.
std::vector<AppEntry> SettingsPages();
// An entry for a pinned target (exe path or shell:AppsFolder\...).
AppEntry EntryFor(const std::wstring& target);
// The running exe belongs to the pinned one: the same file, another version (app-1.2.3\), or a helper inside its folder
bool SameAppExe(const std::wstring& pinned, const std::wstring& running);

// Search: best matches first. Empty query = nothing.
std::vector<AppEntry> SearchApps(const std::wstring& query, bool includeSettings, size_t max = 40);

// Icons are loaded on a background thread so drawing and animation never wait.
// AppIcon returns the cached icon (null if not loaded yet or the app has none);
// RequestIcon queues it (urgent = on screen now, jumps the queue).
ComPtr<IWICBitmapSource> AppIcon(const AppEntry& a, int px);
bool AppIconCached(const AppEntry& a, int px);
void RequestIcon(const AppEntry& a, int px, bool urgent);
void PrewarmIcons(int px);                 // every app, quietly, in the background
void OnIconLoaded(LPARAM result);          // UI thread: a finished icon arrives (WM_SKY_STATUS 21)
void PurgeAppIcons();
void StopIconLoader();

void LaunchApp(const AppEntry& a, bool admin = false);
void OpenFileLocation(const AppEntry& a);
void OpenFileSearch(const std::wstring& query);   // Windows' own file search
void PinToDock(const AppEntry& a);
