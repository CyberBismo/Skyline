// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"
#include "applist.h"

// File search through Windows' own search index (the one Explorer and Start use).
// Skyline keeps no index of its own and never scans the disk: it only asks Windows,
// on a background thread, while someone is typing. The thread and the search
// connection go away by themselves after a minute without searches.
namespace FileSearch {

// Message posted to `notify` when results are ready: wParam = ticket, lParam = std::vector<AppEntry>*
// (the receiver deletes it). Entries have file = true; target = the full path.
constexpr UINT WM_SKY_FILES = WM_APP + 40;

// Newer queries replace older ones that haven't run yet. Empty or 1-letter text gives no search.
void Query(HWND notify, unsigned ticket, const std::wstring& text, size_t max);
void Shutdown();

} // namespace FileSearch
