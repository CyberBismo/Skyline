// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"
#include <map>
#include <set>

// Automatic tiling window manager.
//   Automatic: windows arrange themselves when they open, close, minimize or move.
//   Hotkeys (optional): Ctrl+Alt+arrows etc. — see RegisterHotkeys().
class Tiler {
public:
    void Start();
    void Stop();
    bool Running() const { return running_; }

    void Schedule(UINT ms = 60);          // debounced re-tile
    void Retile();                        // arrange now
    void OnTimer();
    void OnHotkey(int id);
    void ToggleFloat(HWND h);             // float / unfloat one window
    bool IsFloating(HWND h) const { return floating_.count(h) != 0; }
    bool Manages(HWND h) const { return running_ && targets_.count(h) != 0; }   // we placed it (never "fullscreen")
    void ApplySettings();                 // re-read hotkeys etc. after a settings change

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    bool Manageable(HWND h) const;
    std::vector<RECT> Layout(int n, const RECT& area) const;
    RECT WorkArea(HMONITOR mon) const;
    void Place(HWND h, const RECT& target);
    void SwapAt(HWND moved, POINT pt);
    void RegisterHotkeys();
    void UnregisterHotkeys();
    HWND Neighbor(HWND from, int dx, int dy) const;
    void SyncLocationHooks();                 // LOCATIONCHANGE only for processes we tile

    bool running_ = false;
    std::vector<HWINEVENTHOOK> hooks_;
    std::vector<HWND> order_;                 // tiling order, oldest first
    std::set<HWND> floating_;
    std::set<HWND> known_;                    // seen before (restore-from-maximize only once)
    std::set<HWND> wasZoomed_;                // maximized by the user, waiting to be restored
    mutable std::map<HWND, std::wstring> exeCache_;
    std::map<HWND, RECT> placed_;             // last rect we set, to avoid fighting
    std::map<HWND, RECT> targets_;            // current target slot per window (for swap / neighbors)
    std::map<DWORD, HWINEVENTHOOK> locHooks_; // per process id
    std::map<HWND, int> tries_;               // re-place attempts when an app won't fit its slot
    bool hotkeys_ = false;
    bool retiling_ = false;
};
