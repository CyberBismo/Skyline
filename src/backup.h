// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
#pragma once
#include "common.h"

// Backup, restore and factory settings.
// One .skyline file holds skyline.ini, menus.txt and garden.ini exactly as they are on disk
// (the pinned apps, SkyMenu pins and SkyLauncher order live in skyline.ini).
namespace Backup {

std::wstring Folder();                         // "Backups" next to the exe (automatic backups go here)
bool Save(const std::wstring& file);           // write a backup of the current settings
std::wstring AutoSave(const wchar_t* why);     // automatic backup in Folder(); returns its path ("" on failure)
// Replaces the settings with the backup (after an automatic backup of the current ones) and reloads.
bool Restore(const std::wstring& file, std::wstring* error);
// Back to how Skyline starts the very first time (after an automatic backup) and reloads.
bool FactoryReset();

// File dialogs, owned by `owner`. Empty string = cancelled.
std::wstring AskSavePath(HWND owner);
std::wstring AskOpenPath(HWND owner);

// Keys in skyline.ini that describe what Skyline changed in *Windows* (to undo it later), not the user's settings.
// They must survive anything that replaces the whole file (restore, factory reset, leaving performance mode).
struct MachineState {
    std::vector<std::pair<std::wstring, std::wstring>> values;   // "Section\nKey" -> value ("\x01" = wasn't there)
    void Read(const std::wstring& ini);
    void Write(const std::wstring& ini) const;
};

} // namespace Backup
