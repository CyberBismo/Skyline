// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

void LoadMenuProfiles();                                  // reads (or creates) menus.txt
AppMenus MenusFor(HWND hwnd, const std::wstring& exeLower);
void ApplyMenuTheme(bool dark);                           // dark/light native popup menus

// Shows one top-level menu below `anchor`. While it is open, moving the mouse
// over another title (hitTest returns its index) closes this one and returns
// that index so the caller can open it — the way a real menu bar behaves.
// Returns -1 when the menu was closed or a command ran.
int TrackTopMenu(const AppMenus& m, int index, POINT anchor, HWND owner, HWND target,
                 const std::function<int(POINT)>& hitTest);
