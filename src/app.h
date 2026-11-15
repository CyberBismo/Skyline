// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

class Bar;
class TrayHost;
class StatusMonitor;
class Dock;
class Tiler;
class HotCorners;

// Global application state. Everything runs on the UI thread; worker callbacks
// (audio, Wi-Fi, IP change) only PostMessage to ctlWnd.
struct App {
    HINSTANCE hinst = nullptr;
    Settings settings;
    Theme theme;
    std::vector<std::unique_ptr<Bar>> bars;
    std::unique_ptr<TrayHost> tray;
    std::unique_ptr<StatusMonitor> status;
    std::vector<std::unique_ptr<Dock>> docks;   // one per chosen screen; [0] owns the taskbar hiding
    void RebuildDocks();
    Dock* DockOn(HMONITOR mon) const;
    std::unique_ptr<Tiler> tiler;
    std::unique_ptr<HotCorners> corners;
    void ApplyHotCorners();
    ForegroundApp fg;
    HWND ctlWnd = nullptr;          // hidden window that receives worker posts
    UINT msgTaskbarCreated = 0;
    DWORD ignoreTaskbarCreatedUntil = 0;

    void RebuildBars();
    void InvalidateAll();
    void InvalidateTray();
    Bar* PrimaryBar();
    bool IsOurWindow(HWND h);
    void UpdateForeground(HWND h);
    void ReloadTheme();
    void Quit();
};
extern App g_app;

// ---------------------------------------------------------------------------
class TrayHost {
public:
    bool Start();
    void Stop();
    std::vector<TrayIcon>& Icons() { return icons_; }

    // The hidden Shell_TrayWnd sits on the primary bar's rect so apps that ask
    // "where is the taskbar?" place their flyouts next to our icons.
    void SetHostRect(const RECT& rc);
    // The TrayNotifyWnd child must sit exactly over the icons: apps like Steam
    // read its rect to decide where their menu goes.
    void SetTrayArea(const RECT& screen);

    // Mouse interaction, coordinates in screen space
    void MouseButton(TrayIcon& t, UINT msg, POINT pt);
    void HoverEnter(TrayIcon& t, POINT pt);
    void HoverLeave(TrayIcon& t, POINT pt);
    void MouseMove(TrayIcon& t, POINT pt);

    bool PurgeDead();                  // drop icons whose owner window died
    void Reannounce();                 // ask every app to send its tray icons again
    void ScheduleReannounce(UINT ms);  // same, a little later (debounced)
    bool Visible(const TrayIcon& t) const;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK MonitorTimer(HWND, UINT, UINT_PTR, DWORD);
    LRESULT OnCopyData(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Forward(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    bool CreateWindows();
    void DestroyWindows();
    HWND FindExplorerTray();
    void LinkTaskband();   // see tray.cpp
    void MakeTopmost();
    void BroadcastTaskbarCreated();
    void Send(const TrayIcon& t, UINT mouseMsg, POINT pt);
    int  Find(HWND h, UINT uid, const GUID* guid);

    HWND hwnd_ = nullptr;
    HWND notify_ = nullptr;
    HWND explorer_ = nullptr;
    UINT_PTR timer_ = 0;
    bool waitingForExplorer_ = false;
    RECT hostRect_{};
    RECT trayArea_{};
    UINT_PTR reannTimer_ = 0;
    int startupStep_ = 0;
    DWORD lastReannounce_ = 0;
    int reannCount_ = 0;             // re-announces lately (capped)
    static void CALLBACK ReannounceTimer(HWND, UINT, UINT_PTR, DWORD);
    std::vector<TrayIcon> icons_;
};

// ---------------------------------------------------------------------------
class StatusMonitor {
public:
    bool Start(HWND notify);
    void Stop();
    void Refresh();                    // re-read everything (cheap)
    const StatusState& State() const { return s_; }
    void ChangeVolume(float delta);
    void SetVolume(float v);           // 0..1 (unmutes when > 0)
    void ToggleMute();
    void RefreshAudioDevice();
    void RefreshNetwork();
    void RefreshBattery();
    void RefreshVolume();
private:
    StatusState s_;
    HWND notify_ = nullptr;
};
