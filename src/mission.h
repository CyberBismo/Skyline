// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Mission Control (plugin, off by default): every window on the screen, side by side, live (DWM thumbnails).
// The windows glide from where they are into a grid and back; click one to go there, Esc or a click
// on empty space to go back. Win+Tab and the "Task view" hot corner open it while the plugin is on.
// Minimised windows come along too: they rise from the dock and are restored when you pick them.
#pragma once
#include "common.h"

// Live thumbnails of the app windows on one monitor, drawn into a window of ours by DWM itself
// (no copying of pixels: they cost nothing to keep up to date).
class WindowThumbs {
public:
    struct Item {
        HWND h = nullptr;
        HTHUMBNAIL thumb = nullptr;
        RECT from{}, to{};          // where the window is, and its place in the grid (client pixels of the destination)
        RECT cur{};
        std::wstring title;
        std::wstring exe;           // full path, for the icon
        RECT source{};              // the visible window inside its rect (no invisible resize borders)
        SIZE full{};                // the window's real size (for a minimised one: its size when restored)
        bool minimized = false;
    };
    ~WindowThumbs() { Release(); }
    void Collect(HMONITOR mon, HWND dest);           // the windows on that monitor, front to back, registered for `dest`
    void Layout(RECT area, float scale = 1);         // grid inside `area` (client pixels), keeping each window's shape; scale = dpi/96
    void Step(float t);                              // 0 = where the windows really are, 1 = in the grid
    void Release();                                  // unregister everything
    void SetVisible(bool on);                        // hide them all (e.g. while an app grid is showing)
    int HitTest(POINT client) const;                 // item under the point, or -1
    const std::vector<Item>& Items() const { return items_; }
    bool Empty() const { return items_.empty(); }
private:
    std::vector<Item> items_;
    HWND dest_ = nullptr;
    POINT origin_{};                                 // the destination's top-left on screen
};

namespace MissionControl {
bool Enabled();                 // the plugin is on
void Toggle();                  // open on the screen with the mouse, or close
bool Open();
}
