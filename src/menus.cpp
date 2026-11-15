// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// App menus: per-app profiles from menus.txt, or mirrored classic Win32 menus.
#include "menus.h"
#include "app.h"
#include <map>

namespace {

// ---------------------------------------------------------------------------
// Default menus.txt — written next to the exe the first time.
// ---------------------------------------------------------------------------
const wchar_t kDefaultMenusEn[] = LR"(# Skyline – app menus
#
# The menus shown next to the app name in the top left corner.
# [program.exe, other.exe]  -> applies to these programs
# [*]                       -> default for apps without their own profile
# Line without indent       -> menu title (File, Edit …)
# Indented line             -> Name = Shortcut   (e.g. Ctrl+Shift+T, Alt+Left, F11)
#                              Name = Ctrl+K Ctrl+O   (two presses in a row)
#                              Name = run:ms-settings:display   (opens something)
# -                         -> separator
# Keys: Ctrl Shift Alt Win, A-Z 0-9 F1-F24, Tab Enter Esc Space Delete Backspace
#       Insert Home End PageUp PageDown Left Right Up Down Plus Minus Comma Period
# Classic programs with a real menu bar (Notepad++ etc.) get their own menus
# automatically if they have no profile here.

[brave.exe, chrome.exe, msedge.exe, vivaldi.exe, opera.exe, opera_gx.exe, thorium.exe, chromium.exe]
File
    New Tab = Ctrl+T
    New Window = Ctrl+N
    New Private Window = Ctrl+Shift+N
    Reopen Closed Tab = Ctrl+Shift+T
    -
    Open File… = Ctrl+O
    Save Page As… = Ctrl+S
    Print… = Ctrl+P
    -
    Close Tab = Ctrl+W
    Close Window = Ctrl+Shift+W
Edit
    Undo = Ctrl+Z
    Redo = Ctrl+Y
    -
    Cut = Ctrl+X
    Copy = Ctrl+C
    Paste = Ctrl+V
    Paste as Plain Text = Ctrl+Shift+V
    Select All = Ctrl+A
    -
    Find on Page… = Ctrl+F
View
    Reload = Ctrl+R
    Force Reload = Ctrl+Shift+R
    -
    Zoom In = Ctrl+Plus
    Zoom Out = Ctrl+Minus
    Actual Size = Ctrl+0
    -
    Full Screen = F11
    Bookmarks Bar = Ctrl+Shift+B
    -
    Developer Tools = F12
    View Source = Ctrl+U
History
    Back = Alt+Left
    Forward = Alt+Right
    Home = Alt+Home
    -
    Show All History = Ctrl+H
    Clear Browsing Data… = Ctrl+Shift+Delete
Bookmarks
    Bookmark This Page = Ctrl+D
    Bookmark All Tabs = Ctrl+Shift+D
    -
    Bookmark Manager = Ctrl+Shift+O
Window
    Next Tab = Ctrl+Tab
    Previous Tab = Ctrl+Shift+Tab
    -
    Downloads = Ctrl+J
    Task Manager = Shift+Esc
    -
    Minimize = Win+Down

[firefox.exe, zen.exe, librewolf.exe, waterfox.exe, floorp.exe]
File
    New Tab = Ctrl+T
    New Window = Ctrl+N
    New Private Window = Ctrl+Shift+P
    Reopen Closed Tab = Ctrl+Shift+T
    -
    Open File… = Ctrl+O
    Save Page As… = Ctrl+S
    Print… = Ctrl+P
    -
    Close Tab = Ctrl+W
    Close Window = Ctrl+Shift+W
Edit
    Undo = Ctrl+Z
    Redo = Ctrl+Shift+Z
    -
    Cut = Ctrl+X
    Copy = Ctrl+C
    Paste = Ctrl+V
    Paste as Plain Text = Ctrl+Shift+V
    Select All = Ctrl+A
    -
    Find on Page… = Ctrl+F
View
    Reload = Ctrl+R
    Force Reload = Ctrl+Shift+R
    -
    Zoom In = Ctrl+Plus
    Zoom Out = Ctrl+Minus
    Actual Size = Ctrl+0
    -
    Full Screen = F11
    Reader View = F9
    Bookmarks Sidebar = Ctrl+B
    -
    Developer Tools = F12
    View Source = Ctrl+U
History
    Back = Alt+Left
    Forward = Alt+Right
    Home = Alt+Home
    -
    Show All History = Ctrl+Shift+H
    Clear Recent History… = Ctrl+Shift+Delete
Bookmarks
    Bookmark This Page = Ctrl+D
    Bookmark All Tabs = Ctrl+Shift+D
    -
    Library = Ctrl+Shift+O
Tools
    Downloads = Ctrl+Shift+Y
    Add-ons = Ctrl+Shift+A

[explorer.exe]
File
    New Window = Ctrl+N
    New Tab = Ctrl+T
    New Folder = Ctrl+Shift+N
    -
    Properties = Alt+Enter
    -
    Close Tab = Ctrl+W
Edit
    Undo = Ctrl+Z
    Redo = Ctrl+Y
    -
    Cut = Ctrl+X
    Copy = Ctrl+C
    Paste = Ctrl+V
    Select All = Ctrl+A
    -
    Rename = F2
    Delete = Delete
View
    Refresh = F5
    Preview Pane = Alt+P
    Details Pane = Alt+Shift+P
    -
    Full Screen = F11
Go
    Back = Alt+Left
    Forward = Alt+Right
    Up One Level = Alt+Up
    -
    Address Bar = Ctrl+L
    Search = Ctrl+E
    -
    Settings = run:ms-settings:

[code.exe, cursor.exe, windsurf.exe, vscodium.exe]
File
    New File = Ctrl+N
    New Window = Ctrl+Shift+N
    -
    Open File… = Ctrl+O
    Open Folder… = Ctrl+K Ctrl+O
    -
    Save = Ctrl+S
    Save As… = Ctrl+Shift+S
    -
    Close File = Ctrl+W
Edit
    Undo = Ctrl+Z
    Redo = Ctrl+Y
    -
    Cut = Ctrl+X
    Copy = Ctrl+C
    Paste = Ctrl+V
    -
    Find = Ctrl+F
    Replace = Ctrl+H
    Find in Files = Ctrl+Shift+F
View
    Command Palette… = Ctrl+Shift+P
    -
    Explorer = Ctrl+Shift+E
    Source Control = Ctrl+Shift+G
    Extensions = Ctrl+Shift+X
    -
    Toggle Sidebar = Ctrl+B
    Zoom In = Ctrl+Plus
    Zoom Out = Ctrl+Minus
Go
    Go to File… = Ctrl+P
    Go to Line… = Ctrl+G
    Go to Symbol… = Ctrl+Shift+O
    -
    Back = Alt+Left
    Forward = Alt+Right
Run
    Start Debugging = F5
    Run Without Debugging = Ctrl+F5
    Stop = Shift+F5

[discord.exe]
View
    Quick Switcher… = Ctrl+K
    Search Channel = Ctrl+F
    Member List = Ctrl+U
    -
    Zoom In = Ctrl+Plus
    Zoom Out = Ctrl+Minus
Go
    Previous Channel = Alt+Up
    Next Channel = Alt+Down
    Next Unread = Alt+Shift+Down
    -
    Previous Server = Ctrl+Alt+Up
    Next Server = Ctrl+Alt+Down
Audio
    Mute Microphone = Ctrl+Shift+M
    Deafen = Ctrl+Shift+D

[spotify.exe]
Playback
    Play / Pause = Space
    Next = Ctrl+Right
    Previous = Ctrl+Left
    -
    Volume Up = Ctrl+Up
    Volume Down = Ctrl+Down
    -
    Shuffle = Ctrl+S
    Repeat = Ctrl+R
View
    Search = Ctrl+L
    Back = Alt+Left
    Forward = Alt+Right

[windowsterminal.exe, wt.exe]
File
    New Tab = Ctrl+Shift+T
    New Window = Ctrl+Shift+N
    Close Tab = Ctrl+Shift+W
Edit
    Copy = Ctrl+Shift+C
    Paste = Ctrl+Shift+V
    Find = Ctrl+Shift+F
View
    Command Palette = Ctrl+Shift+P
    Settings = Ctrl+Comma
    -
    Zoom In = Ctrl+Plus
    Zoom Out = Ctrl+Minus
    Full Screen = F11

[*]
File
    New = Ctrl+N
    Open… = Ctrl+O
    Save = Ctrl+S
    Print… = Ctrl+P
    -
    Close Window = Alt+F4
Edit
    Undo = Ctrl+Z
    Redo = Ctrl+Y
    -
    Cut = Ctrl+X
    Copy = Ctrl+C
    Paste = Ctrl+V
    Select All = Ctrl+A
    -
    Find = Ctrl+F
Window
    Minimize = Win+Down
    Maximize = Win+Up
    -
    Snap Left = Win+Left
    Snap Right = Win+Right
    Move to Next Display = Win+Shift+Right
)";
const wchar_t kDefaultMenusNo[] = LR"(# Skyline – appmenyer
#
# Menyene som vises ved siden av appnavnet øverst til venstre.
# [program.exe, annet.exe]  -> gjelder for disse programmene
# [*]                       -> standard for apper uten egen profil
# Linje uten innrykk        -> menytittel (Fil, Rediger …)
# Linje med innrykk         -> Navn = Snarvei   (f.eks. Ctrl+Shift+T, Alt+Left, F11)
#                              Navn = Ctrl+K Ctrl+O   (to trykk etter hverandre)
#                              Navn = run:ms-settings:display   (åpner noe)
# -                         -> skillelinje
# Taster: Ctrl Shift Alt Win, A-Z 0-9 F1-F24, Tab Enter Esc Space Delete Backspace
#         Insert Home End PageUp PageDown Left Right Up Down Plus Minus Comma Period
# Klassiske programmer med ekte menylinje (Notepad++ osv.) får sine egne menyer
# automatisk hvis de ikke har en profil her.

[brave.exe, chrome.exe, msedge.exe, vivaldi.exe, opera.exe, opera_gx.exe, thorium.exe, chromium.exe]
Fil
    Ny fane = Ctrl+T
    Nytt vindu = Ctrl+N
    Nytt privat vindu = Ctrl+Shift+N
    Gjenåpne lukket fane = Ctrl+Shift+T
    -
    Åpne fil … = Ctrl+O
    Lagre side som … = Ctrl+S
    Skriv ut … = Ctrl+P
    -
    Lukk fane = Ctrl+W
    Lukk vindu = Ctrl+Shift+W
Rediger
    Angre = Ctrl+Z
    Gjør om = Ctrl+Y
    -
    Klipp ut = Ctrl+X
    Kopier = Ctrl+C
    Lim inn = Ctrl+V
    Lim inn som ren tekst = Ctrl+Shift+V
    Merk alt = Ctrl+A
    -
    Finn på siden … = Ctrl+F
Vis
    Last inn på nytt = Ctrl+R
    Tving ny innlasting = Ctrl+Shift+R
    -
    Zoom inn = Ctrl+Plus
    Zoom ut = Ctrl+Minus
    Faktisk størrelse = Ctrl+0
    -
    Fullskjerm = F11
    Bokmerkelinje = Ctrl+Shift+B
    -
    Utviklerverktøy = F12
    Vis kildekode = Ctrl+U
Historikk
    Tilbake = Alt+Left
    Frem = Alt+Right
    Startside = Alt+Home
    -
    Vis all historikk = Ctrl+H
    Slett nettleserdata … = Ctrl+Shift+Delete
Bokmerker
    Bokmerk denne siden = Ctrl+D
    Bokmerk alle faner = Ctrl+Shift+D
    -
    Bokmerkebehandling = Ctrl+Shift+O
Vindu
    Neste fane = Ctrl+Tab
    Forrige fane = Ctrl+Shift+Tab
    -
    Nedlastinger = Ctrl+J
    Oppgavebehandling = Shift+Esc
    -
    Minimer = Win+Down

[firefox.exe, zen.exe, librewolf.exe, waterfox.exe, floorp.exe]
Fil
    Ny fane = Ctrl+T
    Nytt vindu = Ctrl+N
    Nytt privat vindu = Ctrl+Shift+P
    Gjenåpne lukket fane = Ctrl+Shift+T
    -
    Åpne fil … = Ctrl+O
    Lagre side som … = Ctrl+S
    Skriv ut … = Ctrl+P
    -
    Lukk fane = Ctrl+W
    Lukk vindu = Ctrl+Shift+W
Rediger
    Angre = Ctrl+Z
    Gjør om = Ctrl+Shift+Z
    -
    Klipp ut = Ctrl+X
    Kopier = Ctrl+C
    Lim inn = Ctrl+V
    Lim inn som ren tekst = Ctrl+Shift+V
    Merk alt = Ctrl+A
    -
    Finn på siden … = Ctrl+F
Vis
    Last inn på nytt = Ctrl+R
    Tving ny innlasting = Ctrl+Shift+R
    -
    Zoom inn = Ctrl+Plus
    Zoom ut = Ctrl+Minus
    Faktisk størrelse = Ctrl+0
    -
    Fullskjerm = F11
    Lesevisning = F9
    Sidepanel for bokmerker = Ctrl+B
    -
    Utviklerverktøy = F12
    Vis kildekode = Ctrl+U
Historikk
    Tilbake = Alt+Left
    Frem = Alt+Right
    Startside = Alt+Home
    -
    Vis all historikk = Ctrl+Shift+H
    Slett nylig historikk … = Ctrl+Shift+Delete
Bokmerker
    Bokmerk denne siden = Ctrl+D
    Bokmerk alle faner = Ctrl+Shift+D
    -
    Bibliotek = Ctrl+Shift+O
Verktøy
    Nedlastinger = Ctrl+Shift+Y
    Tillegg = Ctrl+Shift+A

[explorer.exe]
Fil
    Nytt vindu = Ctrl+N
    Ny fane = Ctrl+T
    Ny mappe = Ctrl+Shift+N
    -
    Egenskaper = Alt+Enter
    -
    Lukk fane = Ctrl+W
Rediger
    Angre = Ctrl+Z
    Gjør om = Ctrl+Y
    -
    Klipp ut = Ctrl+X
    Kopier = Ctrl+C
    Lim inn = Ctrl+V
    Merk alt = Ctrl+A
    -
    Gi nytt navn = F2
    Slett = Delete
Vis
    Oppdater = F5
    Forhåndsvisning = Alt+P
    Detaljer-rute = Alt+Shift+P
    -
    Fullskjerm = F11
Gå
    Tilbake = Alt+Left
    Frem = Alt+Right
    Opp ett nivå = Alt+Up
    -
    Adressefelt = Ctrl+L
    Søk = Ctrl+E
    -
    Innstillinger = run:ms-settings:

[code.exe, cursor.exe, windsurf.exe, vscodium.exe]
Fil
    Ny fil = Ctrl+N
    Nytt vindu = Ctrl+Shift+N
    -
    Åpne fil … = Ctrl+O
    Åpne mappe … = Ctrl+K Ctrl+O
    -
    Lagre = Ctrl+S
    Lagre som … = Ctrl+Shift+S
    -
    Lukk fil = Ctrl+W
Rediger
    Angre = Ctrl+Z
    Gjør om = Ctrl+Y
    -
    Klipp ut = Ctrl+X
    Kopier = Ctrl+C
    Lim inn = Ctrl+V
    -
    Finn = Ctrl+F
    Erstatt = Ctrl+H
    Finn i filer = Ctrl+Shift+F
Vis
    Kommandopalett … = Ctrl+Shift+P
    -
    Utforsker = Ctrl+Shift+E
    Kildekontroll = Ctrl+Shift+G
    Utvidelser = Ctrl+Shift+X
    -
    Sidepanel av/på = Ctrl+B
    Zoom inn = Ctrl+Plus
    Zoom ut = Ctrl+Minus
Gå
    Gå til fil … = Ctrl+P
    Gå til linje … = Ctrl+G
    Gå til symbol … = Ctrl+Shift+O
    -
    Tilbake = Alt+Left
    Frem = Alt+Right
Kjør
    Start feilsøking = F5
    Kjør uten feilsøking = Ctrl+F5
    Stopp = Shift+F5

[discord.exe]
Vis
    Hurtigsøk … = Ctrl+K
    Søk i kanal = Ctrl+F
    Medlemsliste = Ctrl+U
    -
    Zoom inn = Ctrl+Plus
    Zoom ut = Ctrl+Minus
Gå
    Forrige kanal = Alt+Up
    Neste kanal = Alt+Down
    Neste uleste = Alt+Shift+Down
    -
    Forrige server = Ctrl+Alt+Up
    Neste server = Ctrl+Alt+Down
Lyd
    Demp mikrofon = Ctrl+Shift+M
    Slå av lyd = Ctrl+Shift+D

[spotify.exe]
Avspilling
    Spill / pause = Space
    Neste = Ctrl+Right
    Forrige = Ctrl+Left
    -
    Volum opp = Ctrl+Up
    Volum ned = Ctrl+Down
    -
    Tilfeldig rekkefølge = Ctrl+S
    Gjenta = Ctrl+R
Vis
    Søk = Ctrl+L
    Tilbake = Alt+Left
    Frem = Alt+Right

[windowsterminal.exe, wt.exe]
Fil
    Ny fane = Ctrl+Shift+T
    Nytt vindu = Ctrl+Shift+N
    Lukk fane = Ctrl+Shift+W
Rediger
    Kopier = Ctrl+Shift+C
    Lim inn = Ctrl+Shift+V
    Finn = Ctrl+Shift+F
Vis
    Kommandopalett = Ctrl+Shift+P
    Innstillinger = Ctrl+Comma
    -
    Zoom inn = Ctrl+Plus
    Zoom ut = Ctrl+Minus
    Fullskjerm = F11

[*]
Fil
    Ny = Ctrl+N
    Åpne … = Ctrl+O
    Lagre = Ctrl+S
    Skriv ut … = Ctrl+P
    -
    Lukk vindu = Alt+F4
Rediger
    Angre = Ctrl+Z
    Gjør om = Ctrl+Y
    -
    Klipp ut = Ctrl+X
    Kopier = Ctrl+C
    Lim inn = Ctrl+V
    Merk alt = Ctrl+A
    -
    Søk = Ctrl+F
Vindu
    Minimer = Win+Down
    Maksimer = Win+Up
    -
    Fest til venstre = Win+Left
    Fest til høyre = Win+Right
    Flytt til neste skjerm = Win+Shift+Right
)";

std::map<std::wstring, std::vector<TopMenu>> g_profiles;   // exe (lowercase) or "*"

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

bool ParseCombo(const std::wstring& text, KeyCombo& out) {
    out = {};
    std::wstring s = text;
    size_t start = 0;
    std::vector<std::wstring> parts;
    while (true) {
        size_t p = s.find(L'+', start);
        parts.push_back(Trim(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        std::wstring k = ToLower(parts[i]);
        bool last = i + 1 == parts.size();
        if (!last) {
            if (k == L"ctrl" || k == L"control") out.mods.push_back(VK_CONTROL);
            else if (k == L"shift") out.mods.push_back(VK_SHIFT);
            else if (k == L"alt") out.mods.push_back(VK_MENU);
            else if (k == L"win") out.mods.push_back(VK_LWIN);
            else return false;
            continue;
        }
        if (k.size() == 1 && ((k[0] >= L'a' && k[0] <= L'z') || (k[0] >= L'0' && k[0] <= L'9'))) {
            out.vk = (WORD)towupper(k[0]);
        } else if (k.size() >= 2 && k[0] == L'f' && iswdigit(k[1])) {
            int n = _wtoi(k.c_str() + 1);
            if (n < 1 || n > 24) return false;
            out.vk = (WORD)(VK_F1 + n - 1);
        } else {
            struct Named { const wchar_t* name; WORD vk; bool ext; };
            static const Named named[] = {
                {L"tab", VK_TAB, false}, {L"enter", VK_RETURN, false}, {L"return", VK_RETURN, false},
                {L"esc", VK_ESCAPE, false}, {L"escape", VK_ESCAPE, false}, {L"space", VK_SPACE, false},
                {L"backspace", VK_BACK, false}, {L"delete", VK_DELETE, true}, {L"del", VK_DELETE, true},
                {L"insert", VK_INSERT, true}, {L"home", VK_HOME, true}, {L"end", VK_END, true},
                {L"pageup", VK_PRIOR, true}, {L"pgup", VK_PRIOR, true}, {L"pagedown", VK_NEXT, true},
                {L"pgdn", VK_NEXT, true}, {L"left", VK_LEFT, true}, {L"right", VK_RIGHT, true},
                {L"up", VK_UP, true}, {L"down", VK_DOWN, true}, {L"plus", VK_OEM_PLUS, false},
                {L"minus", VK_OEM_MINUS, false}, {L"comma", VK_OEM_COMMA, false}, {L"period", VK_OEM_PERIOD, false},
            };
            bool found = false;
            for (const auto& n : named)
                if (k == n.name) { out.vk = n.vk; out.extended = n.ext; found = true; break; }
            if (!found) return false;
        }
    }
    return out.vk != 0;
}

void ParseProfiles(const std::wstring& text) {
    g_profiles.clear();
    std::vector<std::wstring> targets;
    std::vector<TopMenu> current;
    auto flush = [&]() {
        for (auto& t : targets) g_profiles[t] = current;
        current.clear();
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring raw = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (!raw.empty() && raw.back() == L'\r') raw.pop_back();
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;

        if (line[0] == L'[' && line.back() == L']') {
            flush();
            targets.clear();
            std::wstring list = line.substr(1, line.size() - 2);
            size_t s = 0;
            while (true) {
                size_t c = list.find(L',', s);
                std::wstring t = ToLower(Trim(list.substr(s, c == std::wstring::npos ? std::wstring::npos : c - s)));
                if (!t.empty()) targets.push_back(t);
                if (c == std::wstring::npos) break;
                s = c + 1;
            }
            continue;
        }
        bool indented = iswspace(raw[0]);
        if (!indented) {
            TopMenu m;
            m.title = line;
            current.push_back(m);
            continue;
        }
        if (current.empty()) continue;
        MenuEntry e;
        if (line == L"-") {
            e.separator = true;
        } else {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            e.label = Trim(line.substr(0, eq));
            std::wstring val = Trim(line.substr(eq + 1));
            if (ToLower(val).rfind(L"run:", 0) == 0) {
                e.run = Trim(val.substr(4));
            } else {
                // chords are separated by spaces: "Ctrl+K Ctrl+O"
                size_t s = 0;
                bool ok = true;
                while (s < val.size()) {
                    size_t sp = val.find(L' ', s);
                    std::wstring part = val.substr(s, sp == std::wstring::npos ? std::wstring::npos : sp - s);
                    if (!part.empty()) {
                        KeyCombo kc;
                        if (!ParseCombo(part, kc)) { ok = false; break; }
                        e.keys.push_back(kc);
                    }
                    if (sp == std::wstring::npos) break;
                    s = sp + 1;
                }
                if (!ok || e.keys.empty()) continue;
                e.shortcutText = val;
            }
        }
        current.back().items.push_back(e);
    }
    flush();
}

std::wstring ReadUtf8File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string bytes((size_t)std::min<LONGLONG>(size.QuadPart, 4 * 1024 * 1024), '\0');
    DWORD read = 0;
    ReadFile(f, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
    CloseHandle(f);
    bytes.resize(read);
    size_t skip = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
                   (unsigned char)bytes[2] == 0xBF) ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + skip, (int)(bytes.size() - skip), out.data(), n);
    return out;
}

void WriteUtf8File(const std::wstring& path, const wchar_t* text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string bytes(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    WriteFile(f, bom, 3, &w, nullptr);
    std::string crlf;
    for (char ch : bytes) { if (ch == '\n') crlf += '\r'; crlf += ch; }
    WriteFile(f, crlf.data(), (DWORD)crlf.size(), &w, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------
// Classic Win32 menus
// ---------------------------------------------------------------------------
std::wstring StripMnemonic(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'&') {
            if (i + 1 < s.size() && s[i + 1] == L'&') { out += L'&'; ++i; }
            continue;
        }
        out += s[i];
    }
    return out;
}

std::vector<MenuEntry> CopyNative(HMENU m, HWND target, int depth) {
    std::vector<MenuEntry> out;
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n && i < 200; ++i) {
        wchar_t buf[256] = {};
        MENUITEMINFOW mii{sizeof mii};
        mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mii.dwTypeData = buf;
        mii.cch = 255;
        if (!GetMenuItemInfoW(m, i, TRUE, &mii)) continue;
        MenuEntry e;
        if (mii.fType & MFT_SEPARATOR) {
            if (!out.empty() && !out.back().separator) { e.separator = true; out.push_back(e); }
            continue;
        }
        std::wstring text = buf;
        if (text.empty()) continue;   // owner-drawn items we can't reproduce
        size_t tab = text.find(L'\t');
        e.label = text.substr(0, tab);
        if (tab != std::wstring::npos) e.shortcutText = text.substr(tab + 1);
        e.nativeId = mii.wID;
        e.disabled = (mii.fState & MFS_DISABLED) != 0;
        e.checked = (mii.fState & MFS_CHECKED) != 0;
        if (mii.hSubMenu && depth < 4) {
            DWORD_PTR r;
            SendMessageTimeoutW(target, WM_INITMENUPOPUP, (WPARAM)mii.hSubMenu, i, SMTO_ABORTIFHUNG, 150, &r);
            e.sub = CopyNative(mii.hSubMenu, target, depth + 1);
            e.nativeId = 0;
        }
        out.push_back(e);
    }
    while (!out.empty() && out.back().separator) out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// Popup tracking with hover switching
// ---------------------------------------------------------------------------
HHOOK g_hook = nullptr;
const std::function<int(POINT)>* g_hit = nullptr;
int g_current = -1, g_switchTo = -1, g_count = 0;
bool g_hasSubmenus = false;

LRESULT CALLBACK MenuFilter(int code, WPARAM wp, LPARAM lp) {
    if (code == MSGF_MENU && g_hit) {
        MSG* m = reinterpret_cast<MSG*>(lp);
        if (m->message == WM_MOUSEMOVE) {
            int i = (*g_hit)(m->pt);
            if (i >= 0 && i != g_current) { g_switchTo = i; EndMenu(); }
        } else if (m->message == WM_KEYDOWN && !g_hasSubmenus && g_count > 1 &&
                   (m->wParam == VK_LEFT || m->wParam == VK_RIGHT)) {
            g_switchTo = (g_current + (m->wParam == VK_RIGHT ? 1 : g_count - 1)) % g_count;
            EndMenu();
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

HMENU Build(const std::vector<MenuEntry>& items, std::vector<const MenuEntry*>& byId, bool& hasSub) {
    HMENU h = CreatePopupMenu();
    for (const auto& e : items) {
        if (e.separator) { AppendMenuW(h, MF_SEPARATOR, 0, nullptr); continue; }
        UINT flags = (e.disabled ? MF_GRAYED : 0) | (e.checked ? MF_CHECKED : 0);
        std::wstring text = e.label;
        if (!e.shortcutText.empty()) text += L"\t" + e.shortcutText;
        if (!e.sub.empty()) {
            hasSub = true;
            AppendMenuW(h, MF_POPUP | flags, (UINT_PTR)Build(e.sub, byId, hasSub), text.c_str());
            continue;
        }
        byId.push_back(&e);
        AppendMenuW(h, MF_STRING | flags, byId.size(), text.c_str());
    }
    return h;
}

void SendCombo(const KeyCombo& k) {
    std::vector<INPUT> in;
    auto key = [&](WORD vk, bool up, bool ext) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
        in.push_back(i);
    };
    for (WORD m : k.mods) key(m, false, false);
    key(k.vk, false, k.extended);
    key(k.vk, true, k.extended);
    for (auto it = k.mods.rbegin(); it != k.mods.rend(); ++it) key(*it, true, false);
    SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

void Execute(const MenuEntry& e, HWND target) {
    if (!e.run.empty()) {
        ShellExecuteW(nullptr, L"open", e.run.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (!target || !IsWindow(target)) return;
    SetForegroundWindow(target);
    if (e.nativeId) {
        PostMessageW(target, WM_COMMAND, MAKEWPARAM(e.nativeId, 0), 0);
        return;
    }
    // Wait until the app really has focus before typing its shortcut.
    for (int i = 0; i < 20 && GetForegroundWindow() != target; ++i) Sleep(10);
    Sleep(30);
    for (size_t i = 0; i < e.keys.size(); ++i) {
        if (i) Sleep(40);
        SendCombo(e.keys[i]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
void LoadMenuProfiles() {
    std::wstring path = ExeDir() + L"\\menus.txt";
    const wchar_t* kDefaultMenus = g_norsk ? kDefaultMenusNo : kDefaultMenusEn;
    const wchar_t* other = g_norsk ? kDefaultMenusEn : kDefaultMenusNo;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) WriteUtf8File(path, kDefaultMenus);
    std::wstring text = ReadUtf8File(path);
    // Language switched and the file is still the untouched default of the other
    // language: swap it for this language's. Edited files are never overwritten.
    auto plain = [](std::wstring t) { std::erase(t, L'\r'); while (!t.empty() && (t.back() == L'\n' || t.back() == L' ')) t.pop_back(); if (!t.empty() && t[0] == 0xFEFF) t.erase(0, 1); return t; };
    if (!text.empty() && plain(text) == plain(other)) { WriteUtf8File(path, kDefaultMenus); text = kDefaultMenus; }
    if (text.empty()) text = kDefaultMenus;
    ParseProfiles(text);
}

AppMenus MenusFor(HWND hwnd, const std::wstring& exe) {
    AppMenus out;
    auto it = g_profiles.find(exe);
    if (it != g_profiles.end()) { out.menus = it->second; return out; }

    // Classic app with a real menu bar? Mirror it.
    HMENU bar = hwnd ? GetMenu(hwnd) : nullptr;
    if (bar && IsMenu(bar)) {
        int n = GetMenuItemCount(bar);
        for (int i = 0; i < n && i < 16; ++i) {
            wchar_t buf[128] = {};
            MENUITEMINFOW mii{sizeof mii};
            mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
            mii.dwTypeData = buf;
            mii.cch = 127;
            if (!GetMenuItemInfoW(bar, i, TRUE, &mii) || !mii.hSubMenu || !buf[0]) continue;
            TopMenu t;
            t.title = StripMnemonic(buf);
            t.native = mii.hSubMenu;
            t.nativeIndex = i;
            out.menus.push_back(t);
        }
        if (!out.menus.empty()) { out.native = true; return out; }
    }

    auto def = g_profiles.find(L"*");
    if (def != g_profiles.end()) out.menus = def->second;
    return out;
}

void ApplyMenuTheme(bool dark) {
    // uxtheme ordinals 135 (SetPreferredAppMode) and 136 (FlushMenuThemes):
    // what Explorer itself uses to get dark Win32 menus.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using Flush = void(WINAPI*)();
    auto set = (SetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (Flush)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set) set(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

int TrackTopMenu(const AppMenus& m, int index, POINT anchor, HWND owner, HWND target,
                 const std::function<int(POINT)>& hitTest) {
    if (index < 0 || index >= (int)m.menus.size()) return -1;
    const TopMenu& top = m.menus[index];

    std::vector<MenuEntry> nativeItems;
    const std::vector<MenuEntry>* items = &top.items;
    if (top.native && IsWindow(target)) {
        DWORD_PTR r;
        HMENU bar = GetMenu(target);
        SendMessageTimeoutW(target, WM_INITMENU, (WPARAM)bar, 0, SMTO_ABORTIFHUNG, 150, &r);
        SendMessageTimeoutW(target, WM_INITMENUPOPUP, (WPARAM)top.native, top.nativeIndex, SMTO_ABORTIFHUNG, 150, &r);
        nativeItems = CopyNative(top.native, target, 0);
        items = &nativeItems;
    }

    std::vector<const MenuEntry*> byId;
    bool hasSub = false;
    HMENU popup = Build(*items, byId, hasSub);

    g_hit = &hitTest;
    g_current = index;
    g_switchTo = -1;
    g_count = (int)m.menus.size();
    g_hasSubmenus = hasSub;
    g_hook = SetWindowsHookExW(WH_MSGFILTER, MenuFilter, nullptr, GetCurrentThreadId());

    SetForegroundWindow(owner);
    int cmd = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                               anchor.x, anchor.y, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);

    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_hit = nullptr;
    DestroyMenu(popup);

    if (cmd > 0 && cmd <= (int)byId.size()) {
        MenuEntry chosen = *byId[cmd - 1];
        Execute(chosen, target);
        return -1;
    }
    if (g_switchTo >= 0) return g_switchTo;
    // Closed without choosing: give focus back to the app.
    if (IsWindow(target)) SetForegroundWindow(target);
    return -1;
}
