// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Skyline's own Start menus.
//   SkyMenu     — a classic two-column menu (pinned apps / all apps on the left,
//                 your folders and the power button on the right), above the dock.
//   SkyLauncher — a full-screen grid of every app, like Launchpad on the Mac.
//   VibeMenu    — Claude's own: one search for everything, the apps you use at this time of day,
//                 your latest files, number keys to start things.
// Settings -> Start menu picks Windows' own (default), SkyMenu, SkyLauncher or VibeMenu.
// With one of ours chosen, a lone press of the Windows key opens it instead.
namespace StartMenus {
enum Kind { Configured = 0, Menu = 1, Launcher = 2, Vibe = 3 };

void ApplySettings();                 // install/remove the Windows-key hook, close open menus
bool Active();                        // one of ours replaces Windows' Start
void Toggle(Kind k = Configured, HMONITOR mon = nullptr);
void IconsArrived();                  // a background-loaded icon is ready
void Purge();                         // free icon memory when unused for a while
void Shutdown();

// Spotlight (plugin): search apps, settings and files from a hotkey of your choice.
constexpr int kSpotlightHotkey = 0x6100;   // RegisterHotKey id on the control window
bool ApplySpotlightHotkey();          // (re)register from settings; false = Windows or another app already uses it
void SuspendSpotlightHotkey();        // while Settings records a new hotkey
void ToggleSpotlight();
std::wstring HotkeyName(UINT mods, UINT vk);   // "Alt + Space"

// VibeMenu learns which apps you use at which time of day (only while it's the chosen menu)
void NoteForeground(HWND h, const std::wstring& exePath);
void ForgetVibeUsage();
void SaveVibeUsage();                 // also when Windows signs out / shuts down
void UnloadVibeUsage();               // forget what is in memory (the file is read again when needed)
}
