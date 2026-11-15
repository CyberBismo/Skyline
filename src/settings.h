// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

enum class SettingsPage { General, Appearance, Bar, Tray, Dock, Start, Tiling, Corners, Plugins, Advanced, About };

// Opens (or focuses) the native settings window on the given page.
void ShowSettings(SettingsPage page);
// Something changed outside the window (e.g. the tiling button) — refresh it if open.
void SettingsChanged();
