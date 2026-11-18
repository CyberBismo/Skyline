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
    void FlushPending() { if (pendingRetile_ && running_) { pendingRetile_ = false; Schedule(300); } }   // after a game

    void Schedule(UINT ms = 60);          // debounced re-tile
    void Retile();                        // arrange now
    void OnTimer();
    void OnHotkey(int id);
    void ToggleFloat(HWND h);             // float / unfloat one window
    bool IsFloating(HWND h) const { return floating_.count(h) != 0; }
    bool Manages(HWND h) const { return running_ && targets_.count(h) != 0; }   // we placed it (never "fullscreen")
    void ApplySettings();
    void DebugDump();                     // writes skyline-debug.txt next to the settings (monitors, bars, windows)                 // re-read hotkeys etc. after a settings change

private:
    struct Split {                             // one divider in a layout (for resizing by dragging an edge)
        int k;                                 // index in the ratio list
        bool horiz;                            // true: a vertical divider (split left | right)
        RECT parent;                           // the area that was split
        LONG mid;                              // divider position (x or y)
        LONG firstEdge, secondEdge;            // where the first part ends / the second part begins
    };
    struct Ratios {
        int layout = -1;
        std::vector<HWND> key;                       // the windows these fractions belong to (sorted)
        std::vector<float> fr;
        std::map<std::pair<int, std::vector<HWND>>, std::vector<float>> saved;   // remembered sizes, for when a window comes back
    };   // split fractions per monitor, 0 = default

    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    bool Manageable(HWND h) const;
    std::vector<RECT> Layout(int n, const RECT& area, Ratios* rat = nullptr, std::vector<Split>* splits = nullptr, const std::vector<HWND>* wins = nullptr) const;
    bool ApplyResize(HWND h, const RECT& now);   // the user dragged an edge: move the matching divider
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
    std::map<HMONITOR, Ratios> ratios_;
    std::map<HMONITOR, std::vector<Split>> splits_;
    std::map<HWND, int> memIndex_;            // where a minimized window sat in the order, to put it back there
    std::map<HWND, RECT> seen_;               // where a window was at the last check (an app moving itself restarts the counting)
    std::wstring dbg_;                        // DebugDump scratch
    bool pendingRetile_ = false;              // something changed during a game: arrange once it's over
    bool hotkeys_ = false;
    bool retiling_ = false;
};
