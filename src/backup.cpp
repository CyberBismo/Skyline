// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Backup, restore and factory settings — Settings → General.
//
// A .skyline file is plain and simple:
//   SKYLINE-BACKUP 1\r\n
//   FILE skyline.ini <bytes>\r\n <the file, byte for byte> \r\n
//   FILE menus.txt <bytes>\r\n ...
//   END\r\n
// Only these known file names are ever written back, and only next to Skyline.exe.
#include "backup.h"
#include "startmenu.h"
#include "app.h"
#include "garden.h"
#include <shobjidl.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <string>
#include <map>

namespace {

const wchar_t* const kFiles[] = {L"skyline.ini", L"menus.txt", L"garden.ini", L"vibe.ini"};
const char kMagic[] = "SKYLINE-BACKUP 1\r\n";
constexpr int kKeepAuto = 10;   // automatic backups kept in the Backups folder

std::wstring PathOf(const wchar_t* name) { return ExeDir() + L"\\" + name; }

bool ReadAll(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < 16 * 1024 * 1024;   // settings are tiny; refuse anything odd
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

// Written to a temporary file first and then swapped in, so a half-written file never replaces a good one.
bool WriteAll(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = data.empty() || (WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string Pack() {
    std::string out = kMagic;
    for (const wchar_t* name : kFiles) {
        std::string data;
        if (!ReadAll(PathOf(name), data)) continue;   // a file that doesn't exist yet simply isn't in the backup
        out += "FILE " + Utf8(name) + " " + std::to_string(data.size()) + "\r\n";
        out += data;
        out += "\r\n";
    }
    out += "END\r\n";
    return out;
}

// Reads a backup into name -> bytes. Nothing on disk is touched here.
bool Unpack(const std::string& in, std::map<std::wstring, std::string>& files) {
    files.clear();
    size_t pos = sizeof kMagic - 1;
    if (in.compare(0, pos, kMagic) != 0) return false;
    for (;;) {
        size_t eol = in.find("\r\n", pos);
        if (eol == std::string::npos) return false;
        std::string line = in.substr(pos, eol - pos);
        pos = eol + 2;
        if (line == "END") break;
        if (line.compare(0, 5, "FILE ") != 0) return false;
        size_t sp = line.rfind(' ');
        if (sp <= 5) return false;
        std::string name = line.substr(5, sp - 5);
        size_t size = 0;
        try { size = (size_t)std::stoull(line.substr(sp + 1)); } catch (...) { return false; }
        if (size > in.size() - pos) return false;
        std::wstring wname;
        for (const wchar_t* known : kFiles)
            if (Utf8(known) == name) wname = known;
        std::string data = in.substr(pos, size);
        pos += size;
        if (in.compare(pos, 2, "\r\n") != 0) return false;
        pos += 2;
        if (!wname.empty()) files[wname] = std::move(data);   // unknown names (a newer Skyline) are skipped
    }
    return files.count(L"skyline.ini") != 0;   // without its settings it isn't a Skyline backup
}

std::wstring Stamp(const wchar_t* fmt) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    swprintf(buf, 64, fmt, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

// Keep the newest automatic backups, drop the rest (names sort by date).
void PruneAuto() {
    std::vector<std::wstring> autos;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((Backup::Folder() + L"\\* (auto*).skyline").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do autos.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(autos.begin(), autos.end());
    for (size_t i = 0; i + kKeepAuto < autos.size(); ++i) DeleteFileW((Backup::Folder() + L"\\" + autos[i]).c_str());
}

// Gives back what's only in memory: SkyGarden saves its beds when it stops, VibeMenu its usage counts.
void StopWriters() {
    Look::DropPerformanceMode();   // performance mode ends cleanly (Windows' transparency back); the new settings decide
    Garden::Shutdown();
    StartMenus::SaveVibeUsage();
    StartMenus::UnloadVibeUsage();   // forget what's in memory, so the old counts don't overwrite the restored file
}

const std::pair<const wchar_t*, const wchar_t*> kMachineKeys[] = {
    {L"Dock", L"RestoreAutohide"}, {L"General", L"RestoreTaskbarAl"}, {L"General", L"StartupTask"}};

void ReloadEverything() { PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 2, 0); }

} // namespace

namespace Backup {

void MachineState::Read(const std::wstring& ini) {
    values.clear();
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());   // flush the ini cache
    for (auto& [sec, key] : kMachineKeys) {
        wchar_t b[64] = {};
        GetPrivateProfileStringW(sec, key, L"\x01", b, 64, ini.c_str());
        values.push_back({std::wstring(sec) + L"\n" + key, b});
    }
}

void MachineState::Write(const std::wstring& ini) const {
    for (auto& [id, v] : values) {
        size_t nl = id.find(L'\n');
        std::wstring sec = id.substr(0, nl), key = id.substr(nl + 1);
        WritePrivateProfileStringW(sec.c_str(), key.c_str(), v == L"\x01" ? nullptr : v.c_str(), ini.c_str());
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

std::wstring Folder() { return ExeDir() + L"\\Backups"; }

bool Save(const std::wstring& file) { return !file.empty() && WriteAll(file, Pack()); }

std::wstring AutoSave(const wchar_t* why) {
    CreateDirectoryW(Folder().c_str(), nullptr);
    std::wstring path = Folder() + L"\\Skyline " + Stamp(L"%04u-%02u-%02u %02u.%02u.%02u") + L" (auto, " + why + L").skyline";
    if (!Save(path)) return L"";
    PruneAuto();
    return path;
}

bool Restore(const std::wstring& file, std::wstring* error) {
    std::string raw;
    std::map<std::wstring, std::string> files;
    if (!ReadAll(file, raw) || !Unpack(raw, files)) {
        if (error) *error = TR(L"Fila er ikke en Skyline-backup, eller den er skadet.", L"The file isn't a Skyline backup, or it is damaged.");
        return false;
    }
    StopWriters();
    if (AutoSave(TR(L"før gjenoppretting", L"before restore")).empty()) {
        if (error) *error = TR(L"Klarte ikke å ta backup av innstillingene du har nå, så ingenting er endret.",
                               L"Couldn't back up your current settings, so nothing was changed.");
        ReloadEverything();
        return false;
    }
    MachineState state;   // what Skyline changed in Windows must still be undone later, whatever the backup says
    state.Read(PathOf(L"skyline.ini"));
    bool ok = true;
    for (const wchar_t* name : kFiles) {
        auto it = files.find(name);
        if (it != files.end()) ok = WriteAll(PathOf(name), it->second) && ok;
        else DeleteFileW(PathOf(name).c_str());   // wasn't there when the backup was made: start fresh
    }
    state.Write(PathOf(L"skyline.ini"));
    if (!ok && error) *error = TR(L"Noen filer kunne ikke skrives. Den automatiske backupen ligger i Backups-mappa.",
                                  L"Some files couldn't be written. The automatic backup is in the Backups folder.");
    ReloadEverything();
    return ok;
}

bool FactoryReset() {
    StopWriters();
    if (AutoSave(TR(L"før fabrikkinnstillinger", L"before factory reset")).empty()) { ReloadEverything(); return false; }
    MachineState state;
    state.Read(PathOf(L"skyline.ini"));
    for (const wchar_t* name : kFiles) DeleteFileW(PathOf(name).c_str());
    WriteDefaultIni(PathOf(L"skyline.ini"));   // in the language you use now; menus.txt is recreated on reload
    state.Write(PathOf(L"skyline.ini"));
    ReloadEverything();
    return true;
}

// ---------------------------------------------------------------------------
static std::wstring Ask(HWND owner, bool save) {
    ComPtr<IFileDialog> dlg;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dlg.Put()))))
        return L"";
    COMDLG_FILTERSPEC types[] = {{TR(L"Skyline-backup", L"Skyline backup"), L"*.skyline"}};
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"skyline");
    dlg->SetTitle(save ? TR(L"Lagre backup", L"Save backup") : TR(L"Gjenopprett fra backup", L"Restore from backup"));
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (save) {
        dlg->SetFileName((L"Skyline backup " + Stamp(L"%04u-%02u-%02u")).c_str());
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(docs, nullptr, IID_PPV_ARGS(folder.Put())))) dlg->SetFolder(folder.Get());
            CoTaskMemFree(docs);
        }
    }
    MenuGuard guard;   // a modal loop: no rebuilding of bars/docks underneath it
    if (FAILED(dlg->Show(owner))) return L"";
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.Put()))) return L"";
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
    return out;
}

std::wstring AskSavePath(HWND owner) { return Ask(owner, true); }
std::wstring AskOpenPath(HWND owner) { return Ask(owner, false); }

} // namespace Backup
