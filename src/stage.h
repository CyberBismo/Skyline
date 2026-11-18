// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Stage Manager (plugin, off by default): like on a Mac. The app you work in stays on the screen; the other apps
// sit as little pictures in a strip along the left edge, one group per app. Click a group to bring it forward.
// Every screen has its own strip. Two ways to show it: always (Mac), or hidden until the mouse touches the left
// edge. Never on a screen with a game (or any fullscreen window). Enhanced / plain tiling is off while this is on.
#pragma once
#include "common.h"

namespace StageManager {
bool Enabled();                  // the plugin is on
void Apply();                    // settings changed (or startup): start, stop or update
void Shutdown();                 // give every hidden window back
void OnForeground(HWND h);       // another window came to the front
}
