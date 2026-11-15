// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// The app list behind SkyMenu and SkyLauncher — see applist.h
#include "applist.h"
#include "app.h"
#include "icons.h"
#include "dock.h"
#include "settings.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <map>
#include <algorithm>
#include <deque>
#include <set>
#include <mutex>
#include <condition_variable>

namespace {

std::vector<AppEntry> g_apps;
DWORD g_appsAt = 0;
std::map<std::pair<std::wstring, int>, ComPtr<IWICBitmapSource>> g_icons;

bool IsUrl(const std::wstring& s) { return s.find(L"://") != std::wstring::npos; }

// "{6D809377-…}\Steam\steam.exe" -> "C:\Program Files\Steam\steam.exe"
std::wstring ResolveKnownFolderPath(const std::wstring& p) {
    if (p.size() > 2 && p[1] == L':' && p[2] == L'\\') return p;
    if (p.size() < 39 || p[0] != L'{' || p[37] != L'}') return L"";
    GUID g{};
    if (FAILED(CLSIDFromString(p.substr(0, 38).c_str(), &g))) return L"";
    PWSTR base = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) {
        out = std::wstring(base) + p.substr(38);
        CoTaskMemFree(base);
    }
    return out;
}

bool LooksLikeUninstaller(const std::wstring& name, const std::wstring& exe) {
    std::wstring n = ToLower(name), e = ToLower(FileNameOf(exe));
    return n.rfind(L"uninstall", 0) == 0 || n.rfind(L"avinstaller", 0) == 0 || n.find(L"uninstall ") != std::wstring::npos ||
           e.rfind(L"unins", 0) == 0 || e == L"uninstall.exe";
}

int Compare(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), -1, b.c_str(), -1,
                           nullptr, nullptr, 0) - 2;
}

void Enumerate() {
    g_appsAt = GetTickCount();   // even if it fails: don't retry on every call
    std::vector<AppEntry> out;
    ComPtr<IShellItem> folder;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(folder.Put())))) return;
    ComPtr<IEnumShellItems> en;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(en.Put())))) return;
    std::map<std::wstring, bool> seen;
    for (;;) {
        ComPtr<IShellItem> it;
        ULONG got = 0;
        if (en->Next(1, it.Put(), &got) != S_OK || !got) break;
        PWSTR nm = nullptr, pn = nullptr;
        it->GetDisplayName(SIGDN_NORMALDISPLAY, &nm);
        it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &pn);
        AppEntry a;
        if (nm) { a.name = nm; CoTaskMemFree(nm); }
        std::wstring parse;
        if (pn) { parse = pn; CoTaskMemFree(pn); }
        if (a.name.empty() || parse.empty() || IsUrl(parse)) continue;
        a.target = L"shell:AppsFolder\\" + parse;
        a.exe = ResolveKnownFolderPath(parse);
        a.direct = !a.exe.empty();
        if (a.exe.empty()) {
            ComPtr<IShellItem2> it2;
            if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(it2.Put())))) {
                PWSTR lt = nullptr;
                if (SUCCEEDED(it2->GetString(PKEY_Link_TargetParsingPath, &lt)) && lt) { a.exe = lt; CoTaskMemFree(lt); }
            }
        }
        {
            std::wstring l = ToLower(a.exe);
            if (l.size() < 4 || l.compare(l.size() - 4, 4, L".exe") != 0) a.exe.clear();   // only real programs
        }
        a.direct = a.direct && !a.exe.empty();
        if (LooksLikeUninstaller(a.name, a.exe)) continue;
        a.key = ToLower(a.target);
        if (seen[a.key]) continue;
        seen[a.key] = true;
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const AppEntry& x, const AppEntry& y) { return Compare(x.name, y.name) < 0; });
    g_apps = std::move(out);
    g_appsAt = GetTickCount();
}

// How well `q` (lowercase) matches: 0 = not at all, higher = better
int Score(const std::wstring& name, const std::wstring& q) {
    std::wstring n = ToLower(name);
    if (n == q) return 100;
    if (n.rfind(q, 0) == 0) return 80;
    // start of a word: "code" finds "Visual Studio Code"
    for (size_t i = n.find(q); i != std::wstring::npos; i = n.find(q, i + 1))
        if (i > 0 && !iswalnum(n[i - 1])) return 60;
    // initials: "vsc" finds "Visual Studio Code"
    std::wstring ini;
    bool start = true;
    for (wchar_t c : n) { if (iswalnum(c)) { if (start) ini += c; start = false; } else start = true; }
    if (q.size() >= 2 && ini.rfind(q, 0) == 0) return 50;
    if (n.find(q) != std::wstring::npos) return 40;
    return 0;
}

} // namespace

const std::vector<AppEntry>& AllApps(DWORD maxAgeMs) {
    if (g_apps.empty() || GetTickCount() - g_appsAt > maxAgeMs) Enumerate();
    return g_apps;
}

std::vector<AppEntry> SettingsPages() {
    struct P { const wchar_t* uri; const wchar_t* no; const wchar_t* en; };
    static const P pages[] = {
        {L"ms-settings:display", L"Skjerm", L"Display"},
        {L"ms-settings:sound", L"Lyd", L"Sound"},
        {L"ms-settings:bluetooth", L"Bluetooth og enheter", L"Bluetooth & devices"},
        {L"ms-settings:network", L"Nettverk og Internett", L"Network & internet"},
        {L"ms-settings:network-wifi", L"Wi-Fi", L"Wi-Fi"},
        {L"ms-settings:personalization", L"Personalisering", L"Personalization"},
        {L"ms-settings:personalization-background", L"Bakgrunn", L"Background"},
        {L"ms-settings:colors", L"Farger", L"Colors"},
        {L"ms-settings:nightlight", L"Nattlys", L"Night light"},
        {L"ms-settings:appsfeatures", L"Installerte apper", L"Installed apps"},
        {L"ms-settings:defaultapps", L"Standardapper", L"Default apps"},
        {L"ms-settings:startupapps", L"Oppstartsapper", L"Startup apps"},
        {L"ms-settings:windowsupdate", L"Windows Update", L"Windows Update"},
        {L"ms-settings:privacy", L"Personvern og sikkerhet", L"Privacy & security"},
        {L"ms-settings:dateandtime", L"Dato og klokkeslett", L"Date & time"},
        {L"ms-settings:regionlanguage", L"Språk og område", L"Language & region"},
        {L"ms-settings:mousetouchpad", L"Mus", L"Mouse"},
        {L"ms-settings:typing", L"Skriving og tastatur", L"Typing & keyboard"},
        {L"ms-settings:printers", L"Skrivere og skannere", L"Printers & scanners"},
        {L"ms-settings:powersleep", L"Strøm og batteri", L"Power & battery"},
        {L"ms-settings:storagesense", L"Lagring", L"Storage"},
        {L"ms-settings:yourinfo", L"Kontoen din", L"Your account"},
        {L"ms-settings:gaming-gamebar", L"Spill", L"Gaming"},
        {L"ms-settings:taskbar", L"Oppgavelinje", L"Taskbar"},
        {L"ms-settings:notifications", L"Varsler", L"Notifications"},
        {L"ms-settings:about", L"Om PC-en", L"About this PC"},
    };
    std::vector<AppEntry> out;
    for (const auto& p : pages) {
        AppEntry a;
        a.name = TR(p.no, p.en);
        a.alt = g_norsk ? p.en : p.no;
        a.target = p.uri;
        a.key = p.uri;
        a.setting = true;
        out.push_back(a);
    }
    AppEntry s;
    s.name = TR(L"Skyline-innstillinger", L"Skyline settings");
    s.alt = TR(L"Skyline settings", L"Skyline-innstillinger");
    s.target = s.key = L"skyline:settings";
    s.setting = true;
    out.push_back(s);
    return out;
}

// Squirrel apps (Discord, Teams classic, Slack …) live in "<App>\app-1.2.3\x.exe" and move on every update:
// drop the version folder so old and new versions are the same app.
static std::wstring WithoutVersionDir(const std::wstring& p) {
    size_t f = p.find_last_of(L'\\');
    if (f == std::wstring::npos || f == 0) return p;
    size_t d = p.find_last_of(L'\\', f - 1);
    if (d == std::wstring::npos) return p;
    if (p.compare(d + 1, 4, L"app-") == 0) return p.substr(0, d) + p.substr(f);
    return p;
}

bool SameAppExe(const std::wstring& pinned, const std::wstring& running) {
    std::wstring a = ToLower(pinned), b = ToLower(running);
    if (a.empty() || b.empty()) return false;
    if (a == b || WithoutVersionDir(a) == WithoutVersionDir(b)) return true;
    // A helper living inside the app's own folder (Steam's steamwebhelper.exe, Discord's app-x\Discord.exe
    // next to Update.exe). Not for shallow folders, Windows itself, or Steam's game library.
    std::wstring dir = a.substr(0, a.find_last_of(L'\\') + 1);
    std::wstring bdir = b.substr(0, b.find_last_of(L'\\') + 1);
    if (dir.empty() || bdir.size() <= dir.size() || b.compare(0, dir.size(), dir) != 0) return false;
    if (std::count(dir.begin(), dir.end(), L'\\') < 3) return false;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring wd = ToLower(win) + L"\\";
    if (dir.compare(0, wd.size(), wd) == 0) return false;
    if (bdir.find(L"\\steamapps\\", dir.size() - 1) != std::wstring::npos) return false;
    return true;
}

AppEntry EntryFor(const std::wstring& target) {
    std::wstring k = ToLower(target);
    for (const auto& a : AllApps(0xFFFFFFFF)) if (a.key == k || (a.direct && ToLower(a.exe) == k)) return a;
    AppEntry a;
    a.target = target;
    a.key = k;
    if (k.size() > 4 && k.compare(k.size() - 4, 4, L".exe") == 0) {
        a.exe = target;
        a.direct = true;
        a.name = FileDescriptionOf(target);
        if (a.name.empty()) a.name = FileNameOf(target);
    } else {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(it.Put())))) {
            PWSTR nm = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &nm)) && nm) { a.name = nm; CoTaskMemFree(nm); }
        }
        if (a.name.empty()) a.name = target;
    }
    return a;
}

std::vector<AppEntry> SearchApps(const std::wstring& query, bool includeSettings, size_t max) {
    std::vector<AppEntry> out;
    std::wstring q = ToLower(query);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    while (!q.empty() && q.front() == L' ') q.erase(0, 1);
    if (q.empty()) return out;
    std::vector<std::pair<int, const AppEntry*>> hits;
    for (const auto& a : AllApps(0xFFFFFFFF)) {   // refreshing is the menus' job, not every keystroke's
        int s = Score(a.name, q);
        if (!s && !a.exe.empty()) s = Score(FileNameOf(a.exe), q) / 2;   // "cmd" finds Command Prompt
        if (s) hits.push_back({s, &a});
    }
    static std::vector<AppEntry> pages;
    if (includeSettings) {
        pages = SettingsPages();
        for (const auto& p : pages) {
            int s = std::max(Score(p.name, q), Score(p.alt, q));
            if (s) hits.push_back({s - 15, &p});   // apps first when equally good
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](auto& x, auto& y) { return x.first > y.first; });
    for (auto& h : hits) {
        if (out.size() >= max) break;
        out.push_back(*h.second);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Background icon loader
// ---------------------------------------------------------------------------
namespace {
struct IconJob { std::wstring key, target, exe; bool direct = false, setting = false; int px = 0; };
struct IconResult { std::wstring key; int px; ComPtr<IWICBitmapSource> img; };

std::mutex g_qLock;
std::condition_variable g_qCv;
std::deque<IconJob> g_queue;
bool g_quit = false;
HANDLE g_worker = nullptr;
std::set<std::pair<std::wstring, int>> g_pending;   // UI thread only

ComPtr<IWICBitmapSource> LoadIconFor(const IconJob& j) {
    ComPtr<IWICBitmapSource> img;
    if (j.setting)
        img = LoadAppImage(L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", j.px);
    else {
        if (j.direct) img = LoadAppImage(j.exe, j.px);   // the exe's own largest icon: sharpest
        if (!img) img = LoadAppImage(j.target, j.px);    // shortcuts keep their own icon (Discord isn't Update.exe)
    }
    return img;
}

DWORD WINAPI IconWorker(LPVOID) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ComPtr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.Put()));
    SetThreadWicFactory(wic.Get());
    for (;;) {
        IconJob j;
        {
            std::unique_lock<std::mutex> l(g_qLock);
            g_qCv.wait(l, [] { return g_quit || !g_queue.empty(); });
            if (g_quit) break;
            j = std::move(g_queue.front());
            g_queue.pop_front();
        }
        auto* r = new IconResult{j.key, j.px, {}};
        if (wic) {
            // A private copy of the pixels, so the UI thread owns it outright
            if (auto img = LoadIconFor(j)) {
                ComPtr<IWICBitmap> copy;
                if (SUCCEEDED(wic->CreateBitmapFromSource(img.Get(), WICBitmapCacheOnLoad, copy.Put())))
                    copy->QueryInterface(IID_PPV_ARGS(r->img.Put()));
            }
        }
        if (!PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 21, (LPARAM)r)) delete r;
    }
    SetThreadWicFactory(nullptr);
    wic.Reset();
    CoUninitialize();
    return 0;
}

void Enqueue(const AppEntry& a, int px, bool urgent) {
    auto k = std::make_pair(a.key, px);
    if (g_icons.count(k)) return;
    IconJob j{a.key, a.target, a.exe, a.direct, a.setting, px};
    {
        std::lock_guard<std::mutex> l(g_qLock);
        if (g_pending.count(k)) {
            if (!urgent) return;
            // queued: move it to the front once. Not in the queue = the worker has it right now.
            auto it = std::find_if(g_queue.begin(), g_queue.end(), [&](const IconJob& q) { return q.key == a.key && q.px == px; });
            if (it == g_queue.end() || it == g_queue.begin()) return;
            IconJob moved = std::move(*it);
            g_queue.erase(it);
            g_queue.push_front(std::move(moved));
            return;
        }
        g_pending.insert(k);
        if (urgent) g_queue.push_front(std::move(j));
        else g_queue.push_back(std::move(j));
        if (!g_worker) { g_quit = false; g_worker = CreateThread(nullptr, 0, IconWorker, nullptr, 0, nullptr); }
    }
    g_qCv.notify_one();
}
} // namespace

ComPtr<IWICBitmapSource> AppIcon(const AppEntry& a, int px) {
    auto it = g_icons.find({a.key, px});
    return it != g_icons.end() ? it->second : ComPtr<IWICBitmapSource>();
}

bool AppIconCached(const AppEntry& a, int px) { return g_icons.count({a.key, px}) != 0; }

void RequestIcon(const AppEntry& a, int px, bool urgent) { Enqueue(a, px, urgent); }

void PrewarmIcons(int px) {
    for (const auto& a : AllApps(0xFFFFFFFF)) Enqueue(a, px, false);
}

void OnIconLoaded(LPARAM lp) {
    auto* r = reinterpret_cast<IconResult*>(lp);
    if (!r) return;
    auto k = std::make_pair(r->key, r->px);
    {
        std::lock_guard<std::mutex> l(g_qLock);
        g_pending.erase(k);
    }
    g_icons[k] = r->img;
    delete r;
}

void PurgeAppIcons() {
    std::lock_guard<std::mutex> l(g_qLock);
    if (!g_queue.empty()) return;   // still loading: not now
    g_icons.clear();
}

void StopIconLoader() {
    if (!g_worker) return;
    {
        std::lock_guard<std::mutex> l(g_qLock);
        g_quit = true;
        g_queue.clear();
    }
    g_qCv.notify_all();
    WaitForSingleObject(g_worker, 2000);   // a shell call may be slow; don't hang the exit on it
    CloseHandle(g_worker);
    g_worker = nullptr;
}

void LaunchApp(const AppEntry& a, bool admin) {
    if (a.target == L"skyline:settings") { ShowSettings(SettingsPage::General); return; }
    if (admin) {
        const std::wstring& t = a.exe.empty() ? a.target : a.exe;
        ShellExecuteW(nullptr, L"runas", t.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    ShellExecuteW(nullptr, nullptr, a.target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenFileLocation(const AppEntry& a) {
    const std::wstring& path = a.file ? a.target : a.exe;
    if (path.empty()) return;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
    }
}

void OpenFileSearch(const std::wstring& query) {
    std::wstring enc;
    for (wchar_t c : query) {
        if (iswalnum(c) || c == L'-' || c == L'_' || c == L'.') enc += c;
        else { wchar_t b[8]; swprintf(b, 8, L"%%%02X", (unsigned)(c & 0xFF)); enc += c > 0xFF ? std::wstring(1, c) : std::wstring(b); }
    }
    std::wstring uri = L"search-ms:query=" + enc;
    ShellExecuteW(nullptr, nullptr, uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PinToDock(const AppEntry& a) {
    if (a.setting) return;
    // Programs with a known exe are pinned by path: the dock matches running windows by exe.
    std::wstring t = a.direct ? a.exe : a.target;
    auto& pins = g_app.settings.pinned;
    for (auto& p : pins) if (ToLower(p) == ToLower(t)) return;
    pins.push_back(t);
    g_app.settings.pinnedSet = true;
    IniWrite(L"Dock", L"Pinned", JoinList(pins, L"|"));
    for (auto& d : g_app.docks) d->RebuildSoon();
}
