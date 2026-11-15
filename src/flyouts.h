// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Native Flyouts (plugin, off by default): our own glass panels for sound and network,
// opened from the top bar's status icons instead of Windows' Quick Settings.
//   Sound   : volume, mute, output device, volume per app
//   Network : Ethernet, Wi-Fi on/off + networks (connect, password, disconnect), Bluetooth on/off + paired devices
// Nothing runs while they're closed; while open they follow Windows' own change notifications.
#pragma once
#include "common.h"

namespace Flyouts {

enum Kind { Sound = 0, Network = 1, Calendar = 2, Stack = 3 };   // Calendar: from the clock; Stack: from the dock (own settings)

bool Enabled();                                     // the plugin is on
void Toggle(Kind kind, RECT anchor, UINT dpi);      // anchor: the icon on the bar, screen pixels
void Hide();
void ShowStack(RECT tile, UINT dpi);                // the dock's stack, opening upwards from its tile
bool StackOpen();                                   // the stack is showing (the dock stays up meanwhile)
void StatusChanged();                               // volume / device / network changed: refresh if open
void ApplySettings();                               // theme or glass changed

} // namespace Flyouts
