// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Enhanced Tiling: a second tiling engine next to the plain one (tiling.cpp, untouched).
// Layout tree per screen, live resizing, size hotkeys, per-app rules.
#pragma once
#include "common.h"

void EnhancedApply();                 // use instead of g_app.tiler->ApplySettings(): starts/stops the right engine
void EnhancedShutdown();              // on exit
bool EnhancedManages(HWND h);         // Enhanced Tiling placed this window
bool EnhancedSpacesAvailable();       // Enhanced Tiling is on: the top bar shows the workspace button
int EnhancedCurrentSpace(HMONITOR mon);   // 1-4
void EnhancedSpacesToggle(HMONITOR mon);  // open / close the overview of all workspaces on that screen
bool EnhancedIsHidden(HWND h);        // hidden by us (another workspace)
void EnhancedSnapshotKick(HWND h, bool force);   // take a new picture of the window in the background
ComPtr<IWICBitmapSource> EnhancedSnapshot(HWND h);   // picture of a hidden window (for the dock preview)
bool EnhancedReveal(HWND h);          // go to that window's workspace and focus it (false: it is not hidden)
void EnhancedAnimTimer();             // window glide timer (0x7113)
void EnhancedFocusTimer();            // focus-follows-mouse timer (0x7114)
void EnhancedTimer();                 // debounce timer (0x7112)
bool EnhancedHotkey(int id);          // true if the id was ours
void EnhancedFlushPending();          // after a game

// Either engine (for the bar button and the window menu)
bool TilingRunning();
bool TilingIsFloating(HWND h);
void TilingToggleFloat(HWND h);
void TilingSchedule(UINT ms);

// For Mission Control: the four workspaces of a screen with their windows (positions 0..1 of the screen's work area).
struct EnhancedSpaceWin { HWND h; float l, t, r, b; };
struct EnhancedSpaceInfo { int n; bool current; std::vector<EnhancedSpaceWin> wins; std::vector<EnhancedSpaceWin> mons = {}; };   // mons: where each screen sits (h = that screen's HMONITOR), only for the all-screens view
std::vector<EnhancedSpaceInfo> EnhancedSpacesInfo(HMONITOR mon, SIZE* workAreaSize);
std::vector<EnhancedSpaceInfo> EnhancedSpacesInfoAll(SIZE* totalSize);   // the same, but all screens side by side in one picture
bool EnhancedIsTiled(HWND h);   // the window sits in the tiling of the workspace on show (not minimized / floating)
bool EnhancedSpacesSync();   // "switch on all screens" is on
void EnhancedSwitchSpace(HMONITOR mon, int n);
void EnhancedSnapshotNow(HWND h);   // a fresh picture of one window right now (before it is put away); skipped if it answers slowly
void EnhancedSnapshotRefresh(HMONITOR mon);   // fresh pictures of the windows on show on that screen (background)
bool EnhancedMoveToSpaceOn(HWND h, int n, HMONITOR mon);   // the same, onto a given screen (the window may change screen)
bool EnhancedMoveToSpace(HWND h, int n);   // any workspace to any other (also a hidden window out of another workspace); false if not possible
bool EnhancedSendToSpace(HWND h, int n);   // move a (tiled) window of the current workspace to workspace n; false if not possible
bool EnhancedAddSpace();   // one more workspace (up to 8); false when not possible
bool EnhancedCanRemoveSpace();   // the last workspace is empty and not in use
bool EnhancedRemoveSpace();      // remove it (down to 2 workspaces)
