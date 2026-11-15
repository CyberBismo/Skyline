// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Hot corners: push the mouse into a corner of the primary screen to run an action.
// Checks where the pointer is with a light timer: ~10×/s normally, ~30×/s when
// the pointer is near a corner. No hooks (Windows can silently drop those) and
// no corner windows (other windows could cover them).
class HotCorners {
public:
    bool Start();
    void Stop();
    ~HotCorners() { Stop(); }

    static void Run(int corner);               // perform the action
    static const wchar_t* ActionName(int a);   // for the settings page
    static constexpr int kActions = 8;

private:
    static void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD);
    void Arm(UINT ms);
    void Check();

    UINT_PTR timer_ = 0;
    UINT interval_ = 0;
    RECT rect_[4]{};
    RECT near_{};               // the monitor minus its inner area: "close to an edge corner"
    bool enabled_[4]{};
    bool armed_[4] = {true, true, true, true};
    int current_ = -1;
    DWORD enteredAt_ = 0;
};
