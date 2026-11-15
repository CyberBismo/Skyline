// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// The experimental "system-wide theme" (2.27–2.29) was removed in 2.30: it didn't work well enough.
// What is left puts things back once for anyone who had it on: the borders and title bars of open windows,
// and Windows' accent colour (the user's own was kept in [SystemThemeUndo]). Can go in a later version.
#pragma once
#include "common.h"

namespace SystemTheme {
void CleanUpLeftovers();   // at start-up, before the theme is read
}
