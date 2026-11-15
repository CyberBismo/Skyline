// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Built-in updates from skyline.skybyn.com, signed with the release key.
namespace Update {

enum class State { Idle, Checking, UpToDate, Available, Downloading, Installing };

// Call first thing in wWinMain. Returns true when this process only ran as the
// elevated install helper; exit with *exitCode then.
bool HandleCommandLine(int* exitCode);
void Start();                      // cleanup + periodic checks (after the bar is up)

void CheckNow();                   // "Check now" in Settings
void Install();                    // download, verify, replace Skyline.exe, restart

State GetState();
const std::wstring& AvailableVersion();
std::wstring Notes();              // in the current language
const std::wstring& Error();       // last problem, empty if none
std::wstring LastChecked();        // "14:32", empty if never
bool HasKey();                     // false = this build can't update itself
bool JustUpdated();                // started by the previous version right after updating

} // namespace Update
