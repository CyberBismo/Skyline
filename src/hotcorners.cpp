// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Hot corners — see hotcorners.h
#include "flyouts.h"
#include "mission.h"
#include <shellscalingapi.h>
#include "hotcorners.h"
#include "app.h"
#include "startmenu.h"

namespace {
constexpr UINT kDwellMs = 70;       // rest this long in the corner: quick, but no accidents
constexpr int kZone = 4;            // corner size in pixels
constexpr int kNear = 220;          // poll faster when the pointer is this close to a corner
constexpr UINT kSlow = 200, kFast = 25;   // far from a corner: 5 looks a second is plenty (close to one: quick)
HotCorners* g_self = nullptr;
}

const wchar_t* HotCorners::ActionName(int a) {
    switch (a) {
    case 1: return TR(L"Oppgavevisning (alle vinduer)", L"Task view (all windows)");
    case 2: return TR(L"Vis skrivebord", L"Show desktop");
    case 3: return TR(L"Start-menyen", L"Start menu");
    case 6: return L"SkyLauncher";
    case 4: return TR(L"Varsler og kalender", L"Notifications and calendar");
    case 5: return TR(L"Hurtiginnstillinger", L"Quick settings");
    case 7: return TR(L"Skylines kalender (i hjørnet)", L"Skyline's calendar (in the corner)");
    default: return TR(L"Ingenting", L"Nothing");
    }
}

bool HotCorners::Start() {
    Stop();
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& r = mi.rcMonitor;
    rect_[0] = {r.left, r.top, r.left + kZone, r.top + kZone};
    rect_[1] = {r.right - kZone, r.top, r.right, r.top + kZone};
    rect_[2] = {r.left, r.bottom - kZone, r.left + kZone, r.bottom};
    rect_[3] = {r.right - kZone, r.bottom - kZone, r.right, r.bottom};
    near_ = r;
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        enabled_[i] = g_app.settings.cornerAction[i] != 0;
        armed_[i] = true;
        any |= enabled_[i];
    }
    current_ = -1;
    if (!any) return true;
    g_self = this;
    Arm(kSlow);
    return true;
}

void HotCorners::Stop() {
    if (timer_) { KillTimer(nullptr, timer_); timer_ = 0; }
    interval_ = 0;
    if (g_self == this) g_self = nullptr;
}

void HotCorners::Arm(UINT ms) {
    if (ms == interval_ && timer_) return;
    if (timer_) KillTimer(nullptr, timer_);
    timer_ = SetTimer(nullptr, 0, ms, Tick);
    interval_ = ms;
}

void CALLBACK HotCorners::Tick(HWND, UINT, UINT_PTR id, DWORD) {
    if (!g_self || g_self->timer_ != id) { KillTimer(nullptr, id); return; }
    g_self->Check();
}

void HotCorners::Check() {
    POINT p;
    if (!GetCursorPos(&p)) return;

    int i = -1;
    for (int k = 0; k < 4; ++k)
        if (enabled_[k] && PtInRect(&rect_[k], p)) { i = k; break; }

    DWORD now = GetTickCount();
    if (i != current_) {
        if (current_ >= 0) armed_[current_] = true;   // left a corner: ready for next time
        current_ = i;
        enteredAt_ = now;
    }
    if (i >= 0 && armed_[i] && now - enteredAt_ >= kDwellMs) {
        armed_[i] = false;                            // once per visit
        Run(i);
    }

    // Poll fast only when the pointer is near a corner
    bool closeBy = PtInRect(&near_, p) &&   // on another screen = not near our corners
                   (p.x - near_.left < kNear || near_.right - p.x < kNear) &&
                   (p.y - near_.top < kNear || near_.bottom - p.y < kNear);
    Arm(closeBy ? kFast : kSlow);
}

void HotCorners::Run(int corner) {
    if (corner < 0 || corner > 3 || GameMode()) return;   // never in the middle of a game
    switch (g_app.settings.cornerAction[corner]) {
    case 1:   // all windows: our Mission Control when the plugin is on, else Windows' Task view
        if (MissionControl::Enabled()) MissionControl::Toggle();
        else SendHotkey({VK_LWIN, VK_TAB});
        break;
    case 2: SendHotkey({VK_LWIN, 'D'}); break;
    case 3: StartMenus::Toggle(); break;   // Windows' Start, or ours if one is chosen
    case 6: StartMenus::Toggle(StartMenus::Launcher); break;
    case 4: SendHotkey({VK_LWIN, 'N'}); break;
    case 5: SendHotkey({VK_LWIN, 'A'}); break;
    case 7: {   // our calendar card, opening right there in the corner (Windows' own always sits bottom right)
        HMONITOR m = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(m, &mi);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy);
        int bar = MulDiv(g_app.settings.heightDip + 6, dx, 96), w = MulDiv(200, dx, 96);
        bool right = corner == 1 || corner == 3, bottom = corner >= 2;
        RECT a;
        a.left = right ? mi.rcMonitor.right - w : mi.rcMonitor.left;
        a.right = a.left + w;
        a.top = bottom ? mi.rcMonitor.bottom - bar : mi.rcMonitor.top;
        a.bottom = a.top + bar;
        Flyouts::Toggle(Flyouts::Calendar, a, dx);
        break;
    }
    default: break;
    }
}
