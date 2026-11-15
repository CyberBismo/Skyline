// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Now Playing (plugin): what's playing in Spotify, a browser, Apple Music … read
// from Windows' own media sessions (the same ones the volume flyout shows).
// Shown as a tile at the end of the dock and/or next to the tray in the top bar,
// with a glass card (artwork, progress, controls) on hover/click.
namespace NowPlaying {

struct State {
    bool active = false;          // something is playing or paused
    bool playing = false;
    std::wstring title, artist, album;
    std::wstring app;             // "Spotify", "Firefox" …
    double position = 0, duration = 0;   // seconds (position extrapolated while playing)
    ComPtr<IWICBitmapSource> art; // album art, square, or null
    D2D1_COLOR_F tint{0, 0, 0, 0};       // average colour of the artwork
    UINT gen = 0;                 // changes when the track/art changes
};

void ApplySettings();             // start/stop following media sessions
const State& Current();
bool InDock();                    // enabled and placed in the dock (and something is active)
bool InBar();
bool BarAtClock();
bool BarCentered();               // on its own in the middle of the top bar (like an island)
bool BarAfterMenus();             // on the left, right after the app's menus                // in the top bar next to the clock (Separated style) instead of the tray

void TogglePlay();
void Next();
void Previous();
void Seek(double seconds);

// The card. anchor = screen rect of the tile/item; above = open upwards (dock) or downwards (bar).
// align (top bar): 0 centred under the anchor, -1 left edges lined up, 1 right edges lined up.
void ShowCard(RECT anchor, bool above, UINT dpi, int align = 0);
// The dock: a slim drawer as tall as the dock, sliding out from behind its side.
void ShowDrawer(RECT tile, RECT dock, HWND dockWnd, UINT dpi);
void HideCard();
void Restyle();                  // glass/theme changed: the card is rebuilt on next show
bool CardHasMouse();
void Shutdown();

ComPtr<IWICBitmapSource> AppIconFor(int px);   // the playing app's icon (small)
}
