// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// SkyGarden (plugin): your desktop icons, tidied into glass "beds" on the desktop.
// Beds can be moved, resized, renamed and rolled up (click the title). Windows'
// own desktop icons are hidden while it runs; files never move: a bed only
// decides where a desktop item is shown. A folder bed shows a folder of your
// choice instead (Documents, Downloads …), live; files dropped on it go there. Turning SkyGarden off gives the normal
// desktop back exactly as it was.
namespace Garden {
void ApplySettings();            // start/stop
void OnForeground(HWND h);       // show-desktop (Win+D) brings the desktop forward: keep beds on it
void IconsArrived();             // background-loaded icons are ready
void ThemeChanged();
void ExplorerRestarted();
void NewBed();                   // from Settings
void NewBedAt(POINT screen);     // a new bed centred on that point
void NewFolderBedAt(POINT screen);   // a bed that shows a folder of your choice (Documents …); drops go into it
void DesktopMenu(POINT screen);  // right-click on the empty desktop: "New bed" + Windows' own desktop menu
void ResetLayout();              // from Settings
void RestoreAfterCrash();        // give Windows' desktop icons back
void Shutdown();
}
