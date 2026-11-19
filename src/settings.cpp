// Skyline — Copyright © 2026 Yen-Zen & Cyber. All rights reserved. See LICENSE.txt.
// Skyline settings — a native Windows 11 style window (Mica, navigation pane,
// setting cards) drawn with Direct2D. No webview, no XAML.
#include "flyouts.h"
#include "mission.h"
#include "settings.h"
#include "writing.h"
#include "app.h"
#include "tiling.h"
#include "enhancedtiling.h"
#include "deskclock.h"
#include "dock.h"
#include "bar.h"
#include "startmenu.h"
#include "garden.h"
#include "hotcorners.h"
#include "backup.h"
#include "update.h"
#include "dialog.h"
#include "version.h"
#include <psapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>

namespace {

constexpr wchar_t kClass[] = L"SkylineSettings";
constexpr UINT_PTR T_APPLY = 1;
constexpr float kNavW = 248, kContentPad = 28, kCardGap = 4, kTitleTop = 18;
constexpr float kSearchTop = 50, kSearchH = 32, kNavTop = 94;   // the search field sits above the pages

// ---------------------------------------------------------------------------
struct Row {
    enum Type { Header, Toggle, Slider, Choice, Button, Info } type = Info;
    std::wstring title, desc;
    std::function<bool()> getB;
    std::function<void(bool)> setB;
    int min = 0, max = 100, step = 1;
    std::wstring unit;
    std::function<int()> getI;
    std::function<void(int)> setI;
    std::vector<std::wstring> choices;
    std::wstring buttonText;
    bool accent = false;
    std::function<void()> onClick;
    bool enabled = true;
    // layout, DIPs (content space, before scrolling)
    D2D1_RECT_F card{}, ctl{};
    float textRight = 0, descH = 18;   // right edge of the title/desc text, height of the (wrapped) desc
};

struct NavItem {
    SettingsPage page; wchar_t glyph; const wchar_t* no; const wchar_t* en;
    const wchar_t* label() const { return TR(no, en); }
};
const NavItem kNav[] = {
    {SettingsPage::General, 0xE713, L"Generelt", L"General"},
    {SettingsPage::Appearance, 0xE790, L"Utseende", L"Appearance"},
    {SettingsPage::Bar, 0xE8A0, L"Topplinje", L"Top bar"},
    {SettingsPage::Tray, 0xE76C, L"Tray-ikoner", L"Tray icons"},
    {SettingsPage::Dock, 0xE75B, L"Dock", L"Dock"},
    {SettingsPage::Start, 0xE80F, L"Startmeny", L"Start menu"},
    {SettingsPage::Tiling, 0xE8A9, L"Tiling", L"Tiling"},
    {SettingsPage::Corners, 0xE7C4, L"Hot corners", L"Hot corners"},
    {SettingsPage::Plugins, 0xEA86, L"Tillegg", L"Plugins"},
    {SettingsPage::Advanced, 0xEC4A, L"Avansert", L"Advanced"},
    {SettingsPage::About, 0xE946, L"Om Skyline", L"About Skyline"},
};

// Date formats, the same five choices in each language
const wchar_t* kDateFormatsNo[] = {L"ddd d. MMM", L"dddd d. MMMM", L"d. MMM yyyy", L"dd.MM.yyyy", L""};
const wchar_t* kDateFormatsEn[] = {L"ddd MMM d", L"dddd, MMMM d", L"MMM d, yyyy", L"MM/dd/yyyy", L""};
const wchar_t* DateFormat(int i) { return g_norsk ? kDateFormatsNo[i] : kDateFormatsEn[i]; }

class SettingsWindow {
public:
    HWND hwnd = nullptr;
    SettingsPage page = SettingsPage::General;

    void Open(SettingsPage p);
    void CaptureKey(UINT vk);             // recording a new hotkey (Spotlight or SkyWritingTools)
    bool capturing_ = false, eatChar_ = false;
    int captureWhat_ = 0;                 // 0 Spotlight, 1 SkyWritingTools
    std::wstring hotkeyText_, hotkeyText2_, keyText_, modelText_;
    std::vector<std::wstring> wtModels_;   // SkyWritingTools: the models in the dropdown
    void Rebuild() { BuildRows(); Invalidate(); }
    void Invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    void BuildRows();
    void BuildPage(SettingsPage pg);   // appends one page's rows
    void LayoutRows(float width);
    std::wstring query_;               // search across all pages (typed anywhere in the window)
    void Paint();
    bool Resources();
    void Discard();

    void DrawText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c);
    float TextWidth(const std::wstring& s, IDWriteTextFormat* f);
    float TextHeight(const std::wstring& s, IDWriteTextFormat* f, float w);
    void DrawTooltip(D2D1_SIZE_F sz);   // full text of whatever is cut off under the cursor
    D2D1_POINT_2F mouse_{-1, -1};
    void DrawCard(const Row& r, D2D1_RECT_F card, bool hover);
    void DrawToggle(D2D1_RECT_F r, bool on, bool hover, bool enabled);
    void DrawSlider(const Row& row, D2D1_RECT_F r, int value, bool hover);
    void DrawButtonish(D2D1_RECT_F r, const std::wstring& text, bool accent, bool hover, bool chevron, bool enabled);

    D2D1_POINT_2F Dip(LPARAM lp) const { return {GET_X_LPARAM(lp) * 96.0f / dpi, GET_Y_LPARAM(lp) * 96.0f / dpi}; }
    int HitRow(D2D1_POINT_2F p, bool controlOnly) const;
    int HitNav(D2D1_POINT_2F p) const;
    void Click(int row, D2D1_POINT_2F p);
    void SetSliderFromX(int row, float x);
    void ShowChoice(int row);
    bool PickDockScreens(int count);   // tick the displays the dock should be on; false = cancelled
    std::wstring dockScreensText_;
    void ScheduleApply();
    void ScheduleGlass();   // glass/tint only: restyle in place, no rebuild

    UINT dpi = 96;
    float scroll = 0, contentH = 0, viewH = 0;
    std::vector<Row> rows;
    int hoverRow = -1, hoverNav = -1;
    int dragRow = -1;
    int dragValue = 0;
    bool tracking = false;

    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<IDWriteTextFormat> fTitle, fBody, fCaption, fSection, fIcon, fIconSmall, fApp;
};

SettingsWindow g_win;

// ---------------------------------------------------------------------------
// Colours (WinUI 3 theme resources)
struct Pal {
    D2D1_COLOR_F text, text2, text3, card, cardHover, cardStroke, ctl, ctlHover, ctlStroke, sel, accent, onAccent, track;
};
Pal P() {
    const Theme& t = g_app.theme;
    D2D1_COLOR_F a = t.accent;
    if (t.dark)
        return {{1, 1, 1, 1}, {1, 1, 1, 0.786f}, {1, 1, 1, 0.544f}, {1, 1, 1, 0.051f}, {1, 1, 1, 0.084f},
                {0, 0, 0, 0.1f}, {1, 1, 1, 0.061f}, {1, 1, 1, 0.084f}, {1, 1, 1, 0.07f}, {1, 1, 1, 0.061f},
                a, {0, 0, 0, 1}, {1, 1, 1, 0.544f}};
    return {{0, 0, 0, 0.896f}, {0, 0, 0, 0.606f}, {0, 0, 0, 0.446f}, {1, 1, 1, 0.7f}, {0.976f, 0.976f, 0.976f, 0.5f},
            {0, 0, 0, 0.058f}, {1, 1, 1, 0.7f}, {0.976f, 0.976f, 0.976f, 0.5f}, {0, 0, 0, 0.1f}, {0, 0, 0, 0.037f},
            {0, 0.37f, 0.72f, 1}, {1, 1, 1, 1}, {0, 0, 0, 0.446f}};
}

std::wstring IniPathOf() { return g_app.settings.path; }

void FullReload() { PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 2, 0); }

void TilingApply() {
    EnhancedApply();
    g_app.InvalidateAll();
}

std::wstring HiddenLabel(const std::wstring& key) {
    if (g_app.tray)
        for (auto& t : g_app.tray->Icons())
            if (t.exe == key && !t.tip.empty()) {
                std::wstring tip = t.tip;
                size_t nl = tip.find_first_of(L"\r\n");
                return nl == std::wstring::npos ? tip : tip.substr(0, nl);
            }
    std::wstring n = key;
    size_t dot = n.rfind(L".exe");
    if (dot != std::wstring::npos && dot == n.size() - 4) n.resize(dot);
    if (!n.empty()) n[0] = towupper(n[0]);
    return n;
}

// ---------------------------------------------------------------------------
// Rows for the current page, or, while searching, every row on any page that matches (with its page as a heading)
void SettingsWindow::BuildRows() {
    rows.clear();
    if (query_.empty()) { BuildPage(page); return; }
    std::wstring q = ToLower(query_);
    auto has = [&](const std::wstring& t) { return !t.empty() && ToLower(t).find(q) != std::wstring::npos; };
    std::vector<Row> found;
    for (const NavItem& n : kNav) {
        rows.clear();
        BuildPage(n.page);
        std::vector<Row> hits;
        bool sectionMatches = false;
        for (const Row& r : rows) {
            if (r.type == Row::Header) { sectionMatches = has(r.title); continue; }
            bool m = sectionMatches || has(r.title) || has(r.desc) || has(r.buttonText);
            for (const auto& c : r.choices) m = m || has(c);
            if (m) hits.push_back(r);
        }
        if (hits.empty()) continue;
        Row h;
        h.type = Row::Header;
        h.title = n.label();
        found.push_back(h);
        for (auto& r : hits) found.push_back(std::move(r));
    }
    rows = std::move(found);
    if (rows.empty()) {
        Row r;
        r.type = Row::Info;
        r.title = TR(L"Ingen treff", L"Nothing found");
        r.desc = TR(L"Prøv et annet ord, f.eks. «dock», «glass» eller «tiling».", L"Try another word, e.g. \u201cdock\u201d, \u201cglass\u201d or \u201ctiling\u201d.");
        rows.push_back(r);
    }
}

void SettingsWindow::BuildPage(SettingsPage pg) {
    Settings& s = g_app.settings;
    auto header = [&](const wchar_t* t) { Row r; r.type = Row::Header; r.title = t; rows.push_back(r); };
    auto toggle = [&](const wchar_t* t, const wchar_t* d, std::function<bool()> g, std::function<void(bool)> set) {
        Row r; r.type = Row::Toggle; r.title = t; r.desc = d ? d : L""; r.getB = g; r.setB = set; rows.push_back(r);
    };
    auto slider = [&](const wchar_t* t, const wchar_t* d, int mn, int mx, int st, const wchar_t* unit,
                      std::function<int()> g, std::function<void(int)> set) {
        Row r; r.type = Row::Slider; r.title = t; r.desc = d ? d : L""; r.min = mn; r.max = mx; r.step = st;
        r.unit = unit; r.getI = g; r.setI = set; rows.push_back(r);
    };
    auto choice = [&](const wchar_t* t, const wchar_t* d, std::vector<std::wstring> ch, std::function<int()> g,
                      std::function<void(int)> set) {
        Row r; r.type = Row::Choice; r.title = t; r.desc = d ? d : L""; r.choices = ch; r.getI = g; r.setI = set;
        rows.push_back(r);
    };
    auto button = [&](const std::wstring& t, const std::wstring& d, const wchar_t* text, std::function<void()> fn,
                      bool accent = false) {
        Row r; r.type = Row::Button; r.title = t; r.desc = d; r.buttonText = text; r.onClick = fn; r.accent = accent;
        rows.push_back(r);
    };
    auto info = [&](const std::wstring& t, const std::wstring& d) {
        Row r; r.type = Row::Info; r.title = t; r.desc = d; rows.push_back(r);
    };

    switch (pg) {
    case SettingsPage::General: {
        header(TR(L"Språk", L"Language"));
        choice(L"Language / Språk", nullptr, {L"English", L"Norsk"},
               [] { return g_norsk ? 1 : 0; },
               [this, &s](int v) {
                   bool no = v == 1;
                   if (no == g_norsk) return;
                   // Keep the date looking the same kind of way in the new language
                   for (int i = 0; i < 4; ++i)
                       if (s.dateFormat == DateFormat(i)) {
                           s.dateFormat = no ? kDateFormatsNo[i] : kDateFormatsEn[i];
                           IniWrite(L"Clock", L"DateFormat", s.dateFormat);
                           break;
                       }
                   g_norsk = no;
                   IniWrite(L"General", L"Language", std::wstring(no ? L"no" : L"en"));
                   SetWindowTextW(hwnd, TR(L"Skyline – Innstillinger", L"Skyline – Settings"));
                   SetTimer(hwnd, T_APPLY + 1, 10, nullptr);   // redraw this window in the new language (not from inside this row)
                   ScheduleApply();   // top bar, dock and menus follow
               });
        header(TR(L"Oppstart", L"Startup"));
        toggle(TR(L"Start Skyline med Windows", L"Start Skyline with Windows"), TR(L"Topplinjen og docken er klare når du logger inn", L"The top bar and dock are ready when you sign in"),
               [] { return AutostartEnabled(); }, [this](bool v) { SetAutostart(v); SetTimer(hwnd, T_APPLY + 1, 10, nullptr); });
        toggle(TR(L"Rask oppstart", L"Fast startup"),
               TR(L"Starter Skyline med en gang du logger inn (via Oppgaveplanlegging), i stedet for å vente på Windows' vanlige, trege oppstart av programmer. På som standard.",
                  L"Starts Skyline as soon as you sign in (through Task Scheduler), instead of waiting for Windows' usual, slow start of apps. On by default."),
               [] { return FastStartupEnabled(); },
               [this](bool v) {
                   if (!SetFastStartup(v))
                       MessageBoxW(hwnd, TR(L"Windows lot ikke Skyline lage oppstartsoppgaven, så Skyline starter på vanlig måte.",
                                            L"Windows didn't let Skyline create the startup task, so Skyline starts the usual way."),
                                   L"Skyline", MB_ICONINFORMATION);
                   SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
               });
        if (!AutostartEnabled()) rows.back().enabled = false;
        header(L"Windows");
        toggle(TR(L"Start-menyen i midten", L"Centered Start menu"), TR(L"Start åpnes midt på skjermen, over docken. Din egen innstilling kommer tilbake når Skyline avsluttes.", L"Start opens in the middle of the screen, above the dock. Your own setting is restored when Skyline quits."),
               [&s] { return s.centerStart; },
               [&s](bool v) { s.centerStart = v; IniWrite(L"General", L"CenterStart", v ? 1 : 0); ApplyStartAlignment(v); });
        header(TR(L"Filer", L"Files"));
        button(TR(L"Appmenyer", L"App menus"), TR(L"Menyene ved siden av appnavnet (Fil, Rediger, Vis …)", L"The menus next to the app name (File, Edit, View …)"), TR(L"Rediger", L"Edit"), [] {
            std::wstring p = L"\"" + ExeDir() + L"\\menus.txt\"";
            ShellExecuteW(nullptr, L"open", L"notepad.exe", p.c_str(), nullptr, SW_SHOWNORMAL);
        });
        button(TR(L"Innstillingsfil", L"Settings file"), TR(L"skyline.ini — alt på denne siden, som tekst", L"skyline.ini — everything on this page, as text"), TR(L"Åpne", L"Open"), [] {
            std::wstring p = L"\"" + IniPathOf() + L"\"";
            ShellExecuteW(nullptr, L"open", L"notepad.exe", p.c_str(), nullptr, SW_SHOWNORMAL);
        });
        button(TR(L"Last inn på nytt", L"Reload"), TR(L"Bruk endringer du har gjort i filene over", L"Apply changes you've made to the files above"), TR(L"Last inn", L"Reload"), [this] {
            FullReload();
            SetTimer(hwnd, T_APPLY + 1, 400, nullptr);
        });
        header(L"Backup");
        button(TR(L"Ta backup", L"Back up"),
               TR(L"Alle innstillingene, festede apper, appmenyene og SkyGarden i én .skyline-fil", L"All settings, pinned apps, app menus and SkyGarden in one .skyline file"),
               TR(L"Lagre …", L"Save …"), [this] {
            std::wstring path = Backup::AskSavePath(hwnd);
            if (path.empty()) return;
            if (Backup::Save(path))
                MessageBoxW(hwnd, TR(L"Backupen er lagret.", L"The backup has been saved."), L"Skyline", MB_ICONINFORMATION);
            else
                MessageBoxW(hwnd, TR(L"Backupen kunne ikke lagres der. Prøv en annen mappe.", L"The backup couldn't be saved there. Try another folder."),
                            L"Skyline", MB_ICONWARNING);
        });
        button(TR(L"Gjenopprett", L"Restore"),
               TR(L"Hent alt tilbake fra en .skyline-fil. Det du har nå, lagres automatisk først.", L"Bring everything back from a .skyline file. What you have now is saved automatically first."),
               TR(L"Velg fil …", L"Choose file …"), [this] {
            std::wstring path = Backup::AskOpenPath(hwnd);
            if (path.empty()) return;
            KillTimer(hwnd, T_APPLY);
            std::wstring err;
            if (!Backup::Restore(path, &err)) MessageBoxW(hwnd, err.c_str(), L"Skyline", MB_ICONWARNING);
            SetTimer(hwnd, T_APPLY + 1, 500, nullptr);
        });
        button(TR(L"Fabrikkinnstillinger", L"Factory settings"),
               TR(L"Tilbake til slik Skyline var første gang. Det tas backup først, så du kan angre med Gjenopprett.",
                  L"Back to how Skyline was the first time. A backup is made first, so you can undo it with Restore."),
               TR(L"Tilbakestill …", L"Reset …"), [this] {
            if (MessageBoxW(hwnd,
                            TR(L"Vil du sette Skyline tilbake til fabrikkinnstillingene?\n\n"
                               L"Innstillingene, festede apper, appmenyene og SkyGarden-bedene nullstilles. "
                               L"Det du har nå, lagres først i Backups-mappa, så du kan hente det tilbake med Gjenopprett.",
                               L"Reset Skyline to factory settings?\n\n"
                               L"Settings, pinned apps, app menus and SkyGarden beds are reset. "
                               L"What you have now is saved to the Backups folder first, so you can bring it back with Restore."),
                            L"Skyline", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return;
            KillTimer(hwnd, T_APPLY);
            if (!Backup::FactoryReset())
                MessageBoxW(hwnd, TR(L"Klarte ikke å ta backup først, så ingenting er endret.", L"Couldn't make a backup first, so nothing was changed."),
                            L"Skyline", MB_ICONWARNING);
            SetTimer(hwnd, T_APPLY + 1, 500, nullptr);
        });
        button(TR(L"Automatiske backuper", L"Automatic backups"),
               TR(L"De 10 siste, tatt før hver gjenoppretting og tilbakestilling", L"The last 10, made before every restore and reset"),
               TR(L"Åpne mappe", L"Open folder"), [] {
            CreateDirectoryW(Backup::Folder().c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", Backup::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        });
        break;
    }
    case SettingsPage::Appearance: {
        header(TR(L"Tema", L"Theme"));
        {
            std::vector<std::wstring> themes;
            for (int i = 0; i < Look::kThemes; ++i) themes.push_back(Look::ThemeName(i));
            choice(TR(L"Tema", L"Theme"),
                   TR(L"Setter glasset for alle delene på én gang. Standard tar deg alltid tilbake til Skylines eget utseende.",
                      L"Sets the glass of every part at once. Standard always takes you back to Skyline's own look."),
                   themes, [&s] { return s.lookTheme; },
                   [this](int v) { Look::ApplyTheme(v); SetTimer(hwnd, T_APPLY + 1, 450, nullptr); });
            if (s.performanceMode) rows.back().enabled = false;
            slider(TR(L"Glass-styrke", L"Glassiness"),
                   TR(L"Liquid Glass: høyere = mer glass, lavere = mer farge. For mye glass gjør det bak vanskelig å se forbi.",
                      L"Liquid Glass: higher = more glass, lower = more colour. Too much glass lets what's behind get in the way."),
                   0, 100, 5, L" %", [&s] { return s.glassiness; },
                   [this](int v) { Look::SetGlassiness(v); ScheduleApply(); });
            if (!s.liquidGlass || s.performanceMode) rows.back().enabled = false;
            slider(TR(L"Nordlysstyrke", L"Northern lights"), TR(L"Hvor sterkt nordlyset lyser", L"How brightly the northern lights glow"),
                   10, 100, 5, L" %", [&s] { return s.auroraStrength; },
                   [this, &s](int v) { s.auroraStrength = v; IniWrite(L"Look", L"AuroraStrength", v); ScheduleApply(); });
            if (!s.aurora || s.performanceMode) rows.back().enabled = false;
            toggle(TR(L"Fjord om dagen", L"Fjord in the daytime"),
                   TR(L"Mellom 8 og 17 blir nordlyset til isblått fjordlys; resten av døgnet er det nordlys (og stjerner i VibeMenu i fullskjerm)",
                      L"Between 8 and 17 the lights turn icy fjord blue; the rest of the day it's northern lights (and stars in full-screen VibeMenu)"),
                   [&s] { return s.auroraFjord; },
                   [this, &s](bool v) { s.auroraFjord = v; IniWrite(L"Look", L"AuroraFjord", v ? 1 : 0); ScheduleApply(); });
            if (!s.aurora || s.performanceMode) rows.back().enabled = false;
            button(TR(L"Angre siste tema", L"Undo last theme"), TR(L"Tilbake til glasset slik det var før du byttet tema", L"Back to the glass as it was before you switched theme"),
                   TR(L"Angre", L"Undo"), [this] { Look::Undo(); SetTimer(hwnd, T_APPLY + 1, 450, nullptr); });
            if (!Look::CanUndo() || s.performanceMode) rows.back().enabled = false;
        }
        header(TR(L"Glass per del", L"Glass per part"));
        info(TR(L"Hver del har sitt eget glass. Akryl er frostet glass fra Windows, med fargetonen inni glasset. "
                L"Gjennomsiktig har ingen uskarphet; fargetonen legges oppå. 0 % viser bare glasset, 100 % er helt tett.",
                L"Every part has its own glass. Acrylic is Windows' frosted glass, with the tint inside it. "
                L"See-through has no blur; the tint is laid on top. 0 % shows only the glass, 100 % is solid."), L"");
        struct PartRow { Part part; const wchar_t* name; const wchar_t* desc; const wchar_t* sec; const wchar_t* matKey; const wchar_t* tintKey; int* tint; };
        const PartRow parts[] = {
            {Part::Bar, TR(L"Topplinje", L"Top bar"), nullptr, L"Bar", L"Material", L"BarTint", &s.barTintPercent},
            {Part::Dock, L"Dock", nullptr, L"Dock", L"Material", L"Tint", &s.tintPercent},
            {Part::Start, TR(L"Startmeny", L"Start menu"), TR(L"SkyMenu og SkyLauncher", L"SkyMenu and SkyLauncher"), L"Start", L"Material", L"Tint", &s.startTintPercent},
            {Part::Card, TR(L"Forhåndsvisninger", L"Previews"), TR(L"Vinduene som vises når du holder over et ikon i docken", L"The windows shown when you hover an icon in the dock"), L"Bar", L"CardMaterial", L"CardTint", &s.cardTintPercent},
            {Part::Media, L"Now Playing", TR(L"Kortet med albumbilde og knapper (tillegg)", L"The card with artwork and buttons (plugin)"), L"NowPlaying", L"Material", L"CardTint", &s.npTintPercent},
            {Part::Garden, L"SkyGarden", TR(L"Bedene på skrivebordet (tillegg)", L"The beds on the desktop (plugin)"), L"Garden", L"Material", L"Tint", &s.gardenTintPercent},
            {Part::Vibe, L"VibeMenu", L"Vibe coded", L"Vibe", L"Material", L"Tint", &s.vibeTintPercent},
            {Part::Spotlight, L"Spotlight", TR(L"Søkefeltet (tillegg)", L"The search field (plugin)"), L"Spotlight", L"Material", L"Tint", &s.spotlightTintPercent},
            {Part::Flyout, L"Native Flyouts", TR(L"Panelene for lyd og nettverk fra topplinja (tillegg)", L"The sound and network panels from the top bar (plugin)"), L"Flyouts", L"Material", L"Tint", &s.flyoutTintPercent},
        };
        for (const PartRow& r : parts) {
            header(r.name);
            int idx = (int)r.part;
            // the dock's tint lives in [Bar] Tint (from the time there was only one)
            const wchar_t* tintSec = r.part == Part::Dock ? L"Bar" : r.sec;
            std::wstring sec = r.sec, matKey = r.matKey, tsec = tintSec, tkey = r.tintKey;
            int* tint = r.tint;
            // Every part can wear "Vibe": acrylic with the colours of the hour from VibeMenu (VibeMenu has them already)
            bool vibe = r.part != Part::Vibe;
            std::vector<std::wstring> mats = {TR(L"Akryl (frostet glass)", L"Acrylic (frosted glass)"), TR(L"Gjennomsiktig (uten uskarphet)", L"See-through (no blur)")};
            if (vibe) mats.push_back(TR(L"Vibe (fargene fra VibeMenu, etter tid på døgnet)", L"Vibe (VibeMenu's colours, by time of day)"));
            choice(TR(L"Materiale", L"Material"), r.desc, mats,
                   [&s, idx] { return s.material[idx]; },
                   [this, &s, idx, sec, matKey](int v) {
                       s.material[idx] = v == 2 ? MatVibe : v == 1 ? MatClear : MatAcrylic;
                       IniWrite(sec.c_str(), matKey.c_str(), std::wstring(v == 2 ? L"vibe" : v == 1 ? L"clear" : L"acrylic"));
                       ScheduleGlass();
                       for (auto& b : g_app.bars) b->Invalidate();   // Vibe colours are painted by us: redraw now
                       for (auto& d : g_app.docks) d->Invalidate();
                       ScheduleApply();   // menus, cards and beds pick it up on the next draw
                   });
            slider(TR(L"Fargetone", L"Tint"), nullptr, 0, 100, 1, L" %", [tint] { return *tint; },
                   [this, tint, tsec, tkey](int v) { *tint = v; IniWrite(tsec.c_str(), tkey.c_str(), v); ScheduleGlass(); });
        }
        break;
    }
    case SettingsPage::Bar: {
        header(TR(L"Utseende", L"Appearance"));
        choice(TR(L"Stil", L"Style"), TR(L"Klassisk går kant i kant. De andre har luft rundt seg, som docken. Separert har klokka i midten.", L"Classic runs edge to edge. The others float with space around them, like the dock. Separated puts the clock in the middle."),
               {TR(L"Klassisk", L"Classic"), TR(L"Flytende", L"Floating"), TR(L"Øyer (bokser)", L"Islands (boxes)"), TR(L"Separert (tre paneler)", L"Separated (three panels)")}, [&s] { return s.barStyle; },
               [this, &s](int v) { s.barStyle = v; IniWrite(L"Bar", L"Style", v); ScheduleApply(); });
        toggle(TR(L"Skjul automatisk", L"Automatically hide"), TR(L"Topplinjen glir bort og kommer tilbake når musen treffer toppkanten", L"The top bar slides away and comes back when the pointer hits the top edge"),
               [&s] { return s.barAutohide; },
               [this, &s](bool v) { s.barAutohide = v; IniWrite(L"Bar", L"Autohide", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Dobbeltklikk maksimerer", L"Double-click maximizes"),
               TR(L"Dobbeltklikk på et tomt sted i topplinjen maksimerer eller gjenoppretter appen du er i, som på Mac (bare stilen Klassisk)",
                  L"Double-click an empty spot on the top bar to maximize or restore the app you're in, like on a Mac (Classic style only)"),
               [&s] { return s.barDoubleClick; },
               [&s](bool v) { s.barDoubleClick = v; IniWrite(L"Bar", L"DoubleClick", v ? 1 : 0); });
        if (s.barStyle != 0) rows.back().enabled = false;
        header(TR(L"Topplinje", L"Top bar"));
        slider(TR(L"Høyde", L"Height"), nullptr, 22, 40, 1, L" px", [&s] { return s.heightDip; },
               [this, &s](int v) { s.heightDip = v; IniWrite(L"Bar", L"Height", v); ScheduleApply(); });
        toggle(TR(L"Vis på alle skjermer", L"Show on all displays"), TR(L"Av: bare på hovedskjermen", L"Off: main display only"), [&s] { return s.allMonitors; },
               [this, &s](bool v) { s.allMonitors = v; IniWrite(L"Bar", L"AllMonitors", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Tiling-knapp", L"Tiling button"), TR(L"Liten knapp til venstre for lyd og nett som slår tiling av og på", L"Small button left of sound and network that turns tiling on and off"),
               [&s] { return s.tilingButton; },
               [&s](bool v) { s.tilingButton = v; IniWrite(L"Tiling", L"Button", v ? 1 : 0); g_app.InvalidateAll(); });
        header(TR(L"Klokke", L"Clock"));
        std::vector<std::wstring> ex;
        for (int fi = 0; fi < 5; ++fi) {
            const wchar_t* f = DateFormat(fi);
            if (!*f) { ex.push_back(TR(L"Bare klokkeslett", L"Time only")); continue; }
            wchar_t b[96] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, f, b, 96, nullptr);
            ex.push_back(b);
        }
        choice(TR(L"Dato", L"Date"), nullptr, ex,
               [&s] {
                   for (int i = 0; i < 5; ++i) if (s.dateFormat == DateFormat(i)) return i;
                   return -1;   // a format typed into skyline.ini: none of the presets is ticked
               },
               [&s](int v) {
                   s.dateFormat = DateFormat(v);
                   IniWrite(L"Clock", L"DateFormat", s.dateFormat);
                   g_app.InvalidateAll();
               });
        toggle(TR(L"Vis sekunder", L"Show seconds"), nullptr, [&s] { return s.showSeconds; },
               [this, &s](bool v) { s.showSeconds = v; IniWrite(L"Clock", L"ShowSeconds", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Skylines kalender", L"Skyline's calendar"),
               TR(L"Klikk på klokka viser et glasskort med måneden og ukenumre. Av: Windows' kalender og varsler (Win+N), som før.",
                  L"Clicking the clock shows a glass card with the month and week numbers. Off: Windows' calendar and notifications (Win+N), as before."),
               [&s] { return s.clockCalendar; },
               [&s](bool v) { s.clockCalendar = v; IniWrite(L"Clock", L"Calendar", v ? 1 : 0); if (!v) Flyouts::Hide(); });
        break;
    }
    case SettingsPage::Tray: {
        header(L"Tray");
        toggle(TR(L"Tray-ikoner i topplinjen", L"Tray icons in the top bar"), TR(L"Ikonene fra Discord, Steam osv. vises oppe til høyre", L"Icons from Discord, Steam, etc. appear in the top right"),
               [&s] { return s.trayEnabled; },
               [this, &s](bool v) { s.trayEnabled = v; IniWrite(L"Tray", L"Enabled", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Behold også ikonene nederst", L"Also keep icons at the bottom"), TR(L"Vis dem i Windows' egen tray i tillegg", L"Show them in the Windows tray as well"), [&s] { return s.mirrorToExplorer; },
               [&s](bool v) {
                   s.mirrorToExplorer = v;
                   IniWrite(L"Tray", L"MirrorToExplorer", v ? 1 : 0);
                   if (g_app.tray) g_app.tray->ScheduleReannounce(200);   // apps re-add their icons: applies to all of them now
               });
        header(TR(L"Skjulte ikoner", L"Hidden icons"));
        if (s.hiddenTray.empty()) {
            info(TR(L"Ingen skjulte ikoner", L"No hidden icons"), TR(L"Dra et ikon ned fra topplinjen og slipp det for å skjule det.", L"Drag an icon down out of the top bar and drop it to hide it."));
        } else {
            for (auto key : s.hiddenTray) {
                button(HiddenLabel(key), key, TR(L"Vis", L"Show"), [this, key] {
                    UnhideTray(key);
                    Rebuild();
                });
            }
            if (s.hiddenTray.size() > 1)
                button(TR(L"Vis alle skjulte ikoner", L"Show all hidden icons"), L"", TR(L"Vis alle", L"Show all"), [this] {
                    UnhideTray(L"");
                    Rebuild();
                });
        }
        header(TR(L"Rekkefølge", L"Order"));
        button(TR(L"Dra ikonene sidelengs i topplinjen for å flytte dem", L"Drag icons sideways in the top bar to move them"), TR(L"Rekkefølgen huskes", L"The order is remembered"), TR(L"Tilbakestill", L"Reset"), [this] {
            g_app.settings.trayOrder.clear();
            SaveTraySettings();
            g_app.InvalidateTray();
            Rebuild();
        });
        break;
    }
    case SettingsPage::Dock: {
        header(L"Dock");
        toggle(TR(L"Vis dock", L"Show dock"), nullptr, [&s] { return s.dockEnabled; },
               [this, &s](bool v) { s.dockEnabled = v; IniWrite(L"Dock", L"Enabled", v ? 1 : 0); ScheduleApply(); });
        slider(TR(L"Ikonstørrelse", L"Icon size"), nullptr, 24, 64, 1, L" px", [&s] { return s.dockIconDip; },
               [this, &s](int v) { s.dockIconDip = v; IniWrite(L"Dock", L"IconSize", v); ScheduleApply(); });
        toggle(TR(L"Smart skjul", L"Smart hide"), TR(L"På: glir ned når et vindu ligger over den. Av: alltid synlig, og vinduer stopper over den.", L"On: slides down when a window overlaps it. Off: always visible, and windows stop above it."),
               [&s] { return s.smartHide; },
               [this, &s](bool v) { s.smartHide = v; IniWrite(L"Dock", L"SmartHide", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Forhåndsvisning av vinduer", L"Window previews"), TR(L"Hold musen over en app for å se vinduene dens. Klikk for å bytte, × eller midtklikk for å lukke.", L"Hover over an app to see its windows. Click to switch, × or middle-click to close."),
               [&s] { return s.dockPreview; },
               [this, &s](bool v) { s.dockPreview = v; IniWrite(L"Dock", L"Preview", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Minimer vinduer til docken", L"Minimize windows into the dock"),
               TR(L"Som på Mac: et minimert vindu legger seg som et lite, levende bilde helt til høyre i docken, med appens ikon i hjørnet. "
                  L"Klikk for å hente det fram igjen.",
                  L"Like on a Mac: a minimized window sits as a small live picture at the right end of the dock, with the app's icon in its corner. "
                  L"Click it to bring it back."),
               [&s] { return s.dockMinimize; },
               [this, &s](bool v) { s.dockMinimize = v; IniWrite(L"Dock", L"MinimizeToDock", v ? 1 : 0); ScheduleApply(); });
        {
            // Screens are numbered from left to right
            std::vector<std::wstring> screens{TR(L"Kun på hovedskjermen", L"Main display only"), TR(L"På alle skjermer", L"On all displays")};
            HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            auto mons = MonitorsLeftToRight();
            for (size_t i = 0; i < mons.size(); ++i) {
                MONITORINFO mi{sizeof mi};
                GetMonitorInfoW(mons[i], &mi);
                wchar_t t[96];
                swprintf(t, 96, TR(L"Skjerm %d  ·  %d×%d%s", L"Display %d  ·  %d×%d%s"), (int)i + 1, (int)(mi.rcMonitor.right - mi.rcMonitor.left),
                         (int)(mi.rcMonitor.bottom - mi.rcMonitor.top), mons[i] == primary ? TR(L"  (hoved)", L"  (main)") : L"");
                screens.push_back(t);
            }
            int single = s.dockMonitor == kDockChosen ? 0 : s.dockMonitor;
            for (int k = (int)screens.size(); k <= single; ++k) {   // a screen that's unplugged right now
                wchar_t t[64];
                swprintf(t, 64, TR(L"Skjerm %d  (ikke tilkoblet)", L"Display %d  (not connected)"), k - 1);
                screens.push_back(t);
            }
            screens.push_back(TR(L"Velg skjermer …", L"Choose displays …"));
            int chosenIdx = (int)screens.size() - 1;
            int count = (int)mons.size();
            choice(TR(L"Vis docken på", L"Show the dock on"), TR(L"Skjermene telles fra venstre mot høyre", L"Displays are numbered from left to right"), screens,
                   [&s, chosenIdx] { return s.dockMonitor == kDockChosen ? chosenIdx : s.dockMonitor; },
                   [this, &s, chosenIdx, count](int v) {
                       if (v == chosenIdx) {
                           if (!PickDockScreens(count)) return;
                           v = kDockChosen;
                       }
                       s.dockMonitor = v;
                       IniWrite(L"Dock", L"Monitor", v);
                       ScheduleApply();
                   });
            if (s.dockMonitor == kDockChosen) {
                std::wstring list;
                for (int i : s.dockScreens) list += (list.empty() ? L"" : L", ") + std::to_wstring(i + 1);
                dockScreensText_ = std::wstring(TR(L"Skjerm ", L"Display ")) + list;
                button(TR(L"Skjermer med dock", L"Displays with a dock"), dockScreensText_, TR(L"Endre …", L"Change …"), [this, count] {
                    if (PickDockScreens(count)) ScheduleApply();
                    SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
                });
            }
        }
        choice(TR(L"Treff-område nederst", L"Bottom reveal area"), TR(L"Hvor bredt området i bunnkanten er som henter frem docken", L"How wide the area along the bottom edge that brings up the dock is"),
               {TR(L"Bare under docken", L"Only under the dock"), TR(L"Litt bredere", L"A little wider"), TR(L"Mye bredere", L"Much wider"), TR(L"Hele bunnkanten", L"Entire bottom edge")},
               [&s] { return s.dockRevealWidth; },
               [this, &s](int v) { s.dockRevealWidth = v; IniWrite(L"Dock", L"RevealWidth", v); ScheduleApply(); });
        toggle(TR(L"Skjul Windows-oppgavelinjen", L"Hide the Windows taskbar"), TR(L"Den kommer tilbake når Skyline avsluttes", L"It comes back when Skyline quits"), [&s] { return s.hideTaskbar; },
               [this, &s](bool v) { s.hideTaskbar = v; IniWrite(L"Dock", L"HideTaskbar", v ? 1 : 0); ScheduleApply(); });
        header(TR(L"Festede apper", L"Pinned apps"));
        button(TR(L"Høyreklikk et ikon i docken for å feste eller fjerne det", L"Right-click an icon in the dock to pin or unpin it"), TR(L"Dra ikonene for å sortere dem", L"Drag icons to reorder them"),
               TR(L"Tilbakestill", L"Reset"), [this] {
                   WritePrivateProfileStringW(L"Dock", L"Pinned", nullptr, IniPathOf().c_str());
                   ScheduleApply();
               });
        break;
    }
    case SettingsPage::Start: {
        header(TR(L"Startmeny", L"Start menu"));
        choice(TR(L"Startmeny", L"Start menu"),
               TR(L"Hva Start-knappen i docken og Windows-tasten åpner", L"What the dock's Start button and the Windows key open"),
               {TR(L"Windows (standard)", L"Windows (default)"), L"SkyMenu", L"SkyLauncher", L"VibeMenu"},
               [&s] { return s.startMenu; },
               [this, &s](int v) { s.startMenu = v; IniWrite(L"Start", L"Menu", v); ScheduleApply(); });
        choice(TR(L"Startknapp i docken", L"Start button in the dock"), TR(L"Windows-logoen, eller Skylines eget ikon", L"The Windows logo, or Skyline's own icon"),
               {TR(L"Windows", L"Windows"), L"Skyline"},
               [&s] { return s.startButton; },
               [&s](int v) { s.startButton = v; IniWrite(L"Start", L"Button", v); for (auto& d : g_app.docks) d->Invalidate(); });
        toggle(TR(L"Windows-tasten åpner den", L"The Windows key opens it"),
               TR(L"Gjelder Skylines egne menyer. Snarveier som Win+E virker som før.", L"For Skyline's own menus. Shortcuts like Win+E still work."),
               [&s] { return s.startWinKey; },
               [this, &s](bool v) { s.startWinKey = v; IniWrite(L"Start", L"WinKey", v ? 1 : 0); ScheduleApply(); });
        header(L"VibeMenu");
        info(TR(L"Vibe coded. Ett søk for alt (apper, innstillinger, filer og regning med =), appene du pleier å bruke "
                L"på denne tiden av døgnet, og filene du jobbet med sist. Tallene 1–8 starter en app.",
                L"Vibe coded. One search for everything (apps, settings, files and maths with =), the apps you tend to use "
                L"at this time of day, and the files you worked on last. Keys 1–8 start an app."), L"");
        choice(TR(L"Visning", L"Display"), TR(L"Samme utseende: et kort over docken, eller større midt på hele skjermen", L"The same look: a card above the dock, or larger in the middle of the whole screen"),
               {TR(L"Vanlig", L"Normal"), TR(L"Fullskjerm", L"Full screen")},
               [&s] { return s.vibeFullscreen ? 1 : 0; },
               [&s](int v) { s.vibeFullscreen = v == 1; IniWrite(L"Vibe", L"Fullscreen", v); });
        button(TR(L"Prøv VibeMenu", L"Try VibeMenu"), TR(L"Lærer bare mens VibeMenu er valgt som startmeny", L"Only learns while VibeMenu is your Start menu"),
               TR(L"Åpne", L"Open"), [] { StartMenus::Toggle(StartMenus::Vibe); });
        button(TR(L"Glem hva jeg bruker", L"Forget what I use"),
               TR(L"Nullstiller forslagene under «Nå» (lagret i vibe.ini ved siden av Skyline.exe)", L"Clears the suggestions under “Now” (kept in vibe.ini next to Skyline.exe)"),
               TR(L"Glem", L"Forget"), [] { StartMenus::ForgetVibeUsage(); });
        header(L"SkyMenu");
        button(TR(L"Prøv SkyMenu", L"Try SkyMenu"), TR(L"Festede apper til venstre, mappene dine og av/på til høyre", L"Pinned apps on the left, your folders and power on the right"),
               TR(L"Åpne", L"Open"), [] { StartMenus::Toggle(StartMenus::Menu); });
        toggle(TR(L"Søk i filer", L"Search files"),
               TR(L"Når du søker i SkyMenu, vises også filer og mapper fra brukermappen din (Windows' egen søkeindeks)",
                  L"When you search in SkyMenu, files and folders from your user folder show up too (Windows' own search index)"),
               [&s] { return s.menuFileSearch; },
               [&s](bool v) { s.menuFileSearch = v; IniWrite(L"Start", L"MenuFiles", v ? 1 : 0); });
        choice(TR(L"SkyMenu: størrelse", L"SkyMenu: size"),
               TR(L"Hele menyen skaleres: tekst, ikoner og mellomrom", L"The whole menu scales: text, icons and spacing"),
               {TR(L"Liten", L"Small"), TR(L"Middels", L"Medium"), TR(L"Stor", L"Large"), TR(L"Ekstra stor", L"Extra large"),
                TR(L"Egendefinert (dratt)", L"Custom (dragged)")},
               [&s] { return s.menuSize; },
               [&s](int v) { s.menuSize = v; IniWrite(L"Start", L"MenuSize", v); });
        toggle(TR(L"SkyMenu: endre størrelse med musa", L"SkyMenu: resize with the mouse"),
               TR(L"Dra i venstre, høyre eller øvre kant (eller hjørnene) av menyen. Størrelsen huskes som «Egendefinert».",
                  L"Drag the left, right or top edge (or the corners) of the menu. The size is remembered as Custom."),
               [&s] { return s.menuResize; },
               [&s](bool v) { s.menuResize = v; IniWrite(L"Start", L"MenuResize", v ? 1 : 0); });
        toggle(TR(L"SkyMenu: Anbefalt", L"SkyMenu: Recommended"),
               TR(L"Filene du har åpnet sist, under de festede appene (fra Windows' egen liste over nylige filer)",
                  L"The files you opened last, under the pinned apps (from Windows' own list of recent files)"),
               [&s] { return s.menuRecent; },
               [&s](bool v) { s.menuRecent = v; IniWrite(L"Start", L"MenuRecent", v ? 1 : 0); });
        button(TR(L"Festede apper", L"Pinned apps"),
               TR(L"Høyreklikk en app i SkyMenu eller SkyLauncher for å feste eller løsne den", L"Right-click an app in SkyMenu or SkyLauncher to pin or unpin it"),
               TR(L"Tilbakestill", L"Reset"), [] { IniWrite(L"SkyMenu", L"PinnedSet", 0); });
        header(L"SkyLauncher");
        button(TR(L"Prøv SkyLauncher", L"Try SkyLauncher"), TR(L"Alle appene dine i et rutenett over hele skjermen", L"All your apps in a full-screen grid"),
               TR(L"Åpne", L"Open"), [] { StartMenus::Toggle(StartMenus::Launcher); });
        toggle(TR(L"Søk i filer", L"Search files"),
               TR(L"Av som standard: da finner søket i SkyLauncher bare apper", L"Off by default: then SkyLauncher's search finds apps only"),
               [&s] { return s.launcherFileSearch; },
               [&s](bool v) { s.launcherFileSearch = v; IniWrite(L"Start", L"LauncherFiles", v ? 1 : 0); });
        button(TR(L"Rekkefølge", L"Order"), TR(L"Dra ikonene i SkyLauncher for å sortere dem", L"Drag the icons in SkyLauncher to arrange them"),
               TR(L"Alfabetisk", L"Alphabetical"), [] { WritePrivateProfileStringW(L"SkyLauncher", L"Order", nullptr, IniPathOf().c_str()); WritePrivateProfileStringW(L"SkyLauncher", L"Layout", nullptr, IniPathOf().c_str()); });
        break;
    }
    case SettingsPage::Tiling: {
        header(L"Tiling");
        toggle(TR(L"Automatisk tiling", L"Automatic tiling"), TR(L"Vinduer ordner seg selv når de åpnes, lukkes eller minimeres", L"Windows arrange themselves when opened, closed or minimized"),
               [&s] { return s.tilingEnabled; },
               [&s](bool v) { s.tilingEnabled = v; IniWrite(L"Tiling", L"Enabled", v ? 1 : 0); TilingApply(); });
        toggle(L"Enhanced Tiling", TR(L"Egen tiling-motor: levende størrelsesendring, hurtigtaster for størrelse og regler per app. Bruker sitt eget oppsett (BSP), så «Oppsett» under gjelder ikke.", L"Its own tiling engine: live resizing, size shortcuts and per-app rules. Uses its own layout (BSP), so \u201cLayout\u201d below does not apply."),
               [&s] { return s.enhancedTiling; },
               [this, &s](bool v) { s.enhancedTiling = v; IniWrite(L"Tiling", L"Enhanced", v ? 1 : 0); TilingApply(); Rebuild(); });
        if (s.enhancedTiling) {
            toggle(TR(L"Skillelinjer", L"Dividers"), TR(L"Grip og dra mellom vinduene for å endre størrelse (dobbeltklikk = lik størrelse). Trenger litt avstand mellom vinduene.", L"Grab and drag between windows to resize (double-click = equal size). Needs a small gap between windows."),
                   [] { return GetPrivateProfileIntW(L"Tiling", L"Dividers", 1, IniPathOf().c_str()) != 0; },
                   [](bool v) { IniWrite(L"Tiling", L"Dividers", v ? 1 : 0); TilingApply(); });
            toggle(TR(L"Dra for å snappe", L"Drag to snap"), TR(L"Dra et vindu mot kanten av et annet: forhåndsvisning vises, og slipper du der deler det plassen. Midten bytter plass.", L"Drag a window to the edge of another: a preview shows, and dropping there splits that window's space. The middle swaps places."),
                   [] { return GetPrivateProfileIntW(L"Tiling", L"Snap", 1, IniPathOf().c_str()) != 0; },
                   [](bool v) { IniWrite(L"Tiling", L"Snap", v ? 1 : 0); TilingApply(); });
            choice(TR(L"Ikon i topplinja", L"Top bar icon"), TR(L"Hvordan arbeidsområde-knappen ser ut.", L"How the workspace button looks."),
                   {TR(L"Stablede skjermer", L"Stacked screens"), TR(L"Skjerm med tall", L"Screen with number")},
                   [] { return GetPrivateProfileIntW(L"Tiling", L"SpacesIcon", 1, IniPathOf().c_str()) == 2 ? 1 : 0; },
                   [this](int v) { IniWrite(L"Tiling", L"SpacesIcon", v == 1 ? 2 : 1); ScheduleApply(); });
            toggle(TR(L"Bytt arbeidsområde på alle skjermer", L"Switch workspace on all screens"), TR(L"Når du går til et arbeidsområde, går alle skjermene dit samtidig. Av: hver skjerm bytter for seg.", L"Going to a workspace takes every screen there together. Off: each screen switches on its own."),
                   [] { return GetPrivateProfileIntW(L"Tiling", L"SpacesSync", 0, IniPathOf().c_str()) != 0; },
                   [](bool v) { IniWrite(L"Tiling", L"SpacesSync", v ? 1 : 0); TilingApply(); });
            toggle(TR(L"Animasjon", L"Animation"), TR(L"Vinduene glir til ny plass (ca. 0,14 sekunder) i stedet for å hoppe.", L"Windows glide to their new place (about 0.14 s) instead of jumping."),
                   [] { return GetPrivateProfileIntW(L"Tiling", L"Animate", 1, IniPathOf().c_str()) != 0; },
                   [](bool v) { IniWrite(L"Tiling", L"Animate", v ? 1 : 0); TilingApply(); });
            toggle(TR(L"Fokus følger musa", L"Focus follows mouse"), TR(L"Vinduet under musepekeren får fokus uten klikk. Gjelder bare mellom flisede vinduer.", L"The window under the pointer gets focus without a click. Only between tiled windows."),
                   [] { return GetPrivateProfileIntW(L"Tiling", L"FocusFollowsMouse", 0, IniPathOf().c_str()) != 0; },
                   [](bool v) { IniWrite(L"Tiling", L"FocusFollowsMouse", v ? 1 : 0); TilingApply(); });
        }
        choice(TR(L"Oppsett", L"Layout"), nullptr,
               {TR(L"BSP – deler skjermen i to, igjen og igjen", L"BSP – splits the screen in two, again and again"), TR(L"Master – ett stort + stabel", L"Master – one large + stack"), TR(L"Kolonner", L"Columns"),
                TR(L"Monocle – ett vindu om gangen", L"Monocle – one window at a time")},
               [&s] { return s.tilingLayout; },
               [&s](int v) { s.tilingLayout = v; IniWrite(L"Tiling", L"Layout", v); TilingApply(); });
        choice(TR(L"Styring", L"Control"), TR(L"Hurtigtaster er helt valgfritt", L"Keyboard shortcuts are entirely optional"),
               {TR(L"Automatisk", L"Automatic"), TR(L"Automatisk + hurtigtaster", L"Automatic + shortcuts"), TR(L"Bare hurtigtaster", L"Shortcuts only")},
               [&s] { return s.tilingMode; },
               [this, &s](int v) { s.tilingMode = v; IniWrite(L"Tiling", L"Mode", v); TilingApply(); Rebuild(); });
        slider(TR(L"Avstand mellom vinduer", L"Gap between windows"), nullptr, 0, 32, 1, L" px", [&s] { return s.tilingGap; },
               [&s](int v) { s.tilingGap = v; IniWrite(L"Tiling", L"Gap", v); TilingApply(); });
        toggle(TR(L"Gi plass til docken", L"Make room for the dock"), TR(L"Vinduer stopper over docken i stedet for å dekke den", L"Windows stop above the dock instead of covering it"),
               [&s] { return s.tilingDockSpace; },
               [&s](bool v) { s.tilingDockSpace = v; IniWrite(L"Tiling", L"DockSpace", v ? 1 : 0); TilingApply(); });
        button(TR(L"Feilsøkingsfil", L"Debug file"), TR(L"Skriver skyline-debug.txt ved siden av skyline.ini (skjermer, topplinje, vinduer). Hurtigtast: Ctrl + Alt + D", L"Writes skyline-debug.txt next to skyline.ini (screens, bar, windows). Shortcut: Ctrl + Alt + D"),
               TR(L"Skriv nå", L"Write now"), [] { if (g_app.tiler) g_app.tiler->DebugDump(); });
        if (s.enhancedTiling)
            info(TR(L"Regler per app", L"Per-app rules"), TR(L"Skriv i skyline.ini under [Tiling]: Rules=firefox.exe=float, code.exe=monitor 2, spotify.exe=space 3 (flytende, skjerm 2, arbeidsområde 3). Gjelder nye vinduer.", L"In skyline.ini under [Tiling]: Rules=firefox.exe=float, code.exe=monitor 2, spotify.exe=space 3 (floating, screen 2, workspace 3). Applies to new windows."));
        info(TR(L"Bytt plass med musen", L"Swap with the mouse"), TR(L"Dra et vindu og slipp det oppå et annet, så bytter de plass.", L"Drag a window and drop it onto another to swap them."));
        if (s.tilingMode >= 1) {
            header(TR(L"Hurtigtaster", L"Keyboard shortcuts"));
            if (!s.enhancedTiling) info(L"Ctrl + Alt + T", TR(L"Slå tiling av og på", L"Turn tiling on and off"));
            info(TR(L"Ctrl + Alt + piltaster", L"Ctrl + Alt + arrow keys"), TR(L"Flytt fokus til vinduet ved siden av", L"Move focus to the adjacent window"));
            info(TR(L"Ctrl + Alt + Shift + piltaster", L"Ctrl + Alt + Shift + arrow keys"), TR(L"Bytt plass med vinduet ved siden av", L"Swap with the adjacent window"));
            info(L"Ctrl + Alt + F", TR(L"Gjør vinduet flytende / tile det igjen", L"Float the window / tile it again"));
            info(L"Ctrl + Alt + L", TR(L"Neste oppsett", L"Next layout"));
            info(TR(L"Ctrl + Alt + Mellomrom", L"Ctrl + Alt + Space"), TR(L"Ordne vinduene nå", L"Arrange windows now"));
            if (s.enhancedTiling) {
                info(TR(L"Ctrl + Alt + Win + piltaster", L"Ctrl + Alt + Win + arrow keys"), TR(L"Flytt skillelinjen nærmest vinduet (hold inne for å fortsette)", L"Move the divider nearest the window (hold to keep going)"));
                info(L"Ctrl + Alt + Win + E", TR(L"Lik størrelse på alle vinduene", L"Equal size for all windows"));
                info(TR(L"Ctrl + Alt + Enter", L"Ctrl + Alt + Enter"), TR(L"Gjør vinduet til hovedvindu (første plass)", L"Make the window the main window (first place)"));
                info(L"Ctrl + Alt + 1–8", TR(L"Bytt til arbeidsområde 1–8 på skjermen med musa", L"Go to workspace 1–8 on the screen the mouse is on"));
                info(L"Ctrl + Alt + Shift + 1–8", TR(L"Send vinduet til arbeidsområde 1–8", L"Send the window to workspace 1–8"));
                info(L"Ctrl + Alt + O", TR(L"Vis alle arbeidsområder (oversikt)", L"Show all workspaces (overview)"));
            }
        }
        header(TR(L"Apper som aldri tiles", L"Apps that are never tiled"));
        if (s.tilingFloat.empty()) {
            info(TR(L"Ingen", L"None"), TR(L"Klikk appnavnet oppe til venstre og velg «Aldri tile …» for å legge til en app.", L"Click the app name in the top left and choose “Never tile …” to add an app."));
        } else {
            for (auto exe : s.tilingFloat)
                button(HiddenLabel(exe), exe, TR(L"Fjern", L"Remove"), [this, exe] {
                    auto& f = g_app.settings.tilingFloat;
                    f.erase(std::remove(f.begin(), f.end(), exe), f.end());
                    IniWrite(L"Tiling", L"Float", JoinList(f, L", "));
                    TilingApply();
                    Rebuild();
                });
        }
        break;
    }
    case SettingsPage::Corners: {
        header(L"Hot corners");
        toggle(L"Hot corners", TR(L"Før musen helt inn i et hjørne av hovedskjermen for å gjøre noe. Pause i fullskjermspill.", L"Move the pointer all the way into a corner of the main display to trigger an action. Paused in fullscreen games."),
               [&s] { return s.hotCorners; },
               [&s](bool v) { s.hotCorners = v; IniWrite(L"HotCorners", L"Enabled", v ? 1 : 0); g_app.ApplyHotCorners(); });
        std::vector<std::wstring> acts;
        for (int a = 0; a < HotCorners::kActions; ++a) acts.push_back(HotCorners::ActionName(a));
        const wchar_t* names[4] = {TR(L"Øverst til venstre", L"Top left"), TR(L"Øverst til høyre", L"Top right"), TR(L"Nederst til venstre", L"Bottom left"), TR(L"Nederst til høyre", L"Bottom right")};
        static const wchar_t* keys[4] = {L"TopLeft", L"TopRight", L"BottomLeft", L"BottomRight"};
        for (int i = 0; i < 4; ++i) {
            choice(names[i], nullptr, acts, [&s, i] { return s.cornerAction[i]; }, [&s, i](int v) {
                s.cornerAction[i] = v;
                IniWrite(L"HotCorners", keys[i], v);
                g_app.ApplyHotCorners();
            });
            if (!s.hotCorners) rows.back().enabled = false;
        }
        info(TR(L"Oppgavevisning", L"Task view"), TR(L"Windows' egen oversikt over alle vinduer og skrivebord (Win+Tab). Vår egen Mission Control kommer senere.", L"Windows' own overview of all windows and desktops (Win+Tab). Our own Mission Control is coming later."));
        break;
    }
    case SettingsPage::Plugins: {
        header(L"Spotlight");
        toggle(L"Spotlight",
               TR(L"Søk etter apper, innstillinger og filer med en hurtigtast, fra hvor som helst. Filene finnes med Windows' egen søkeindeks, så Skyline indekserer ingenting selv.",
                  L"Search apps, settings and files with a hotkey, from anywhere. Files are found with Windows' own search index, so Skyline indexes nothing itself."),
               [&s] { return s.spotlightEnabled; },
               [this, &s](bool v) {
                   s.spotlightEnabled = v;
                   IniWrite(L"Spotlight", L"Enabled", v ? 1 : 0);
                   if (v && !StartMenus::ApplySpotlightHotkey())
                       MessageBoxW(hwnd, TR(L"Hurtigtasten er allerede i bruk av Windows eller et annet program. Velg en annen under Hurtigtast.",
                                            L"The hotkey is already used by Windows or another app. Pick another one under Hotkey."),
                                   L"Skyline", MB_ICONINFORMATION);
                   if (!v) StartMenus::ApplySpotlightHotkey();
               });
        button(TR(L"Hurtigtast", L"Hotkey"),
               capturing_ && captureWhat_ == 0 ? TR(L"Trykk tastene du vil bruke nå (Esc avbryter)", L"Press the keys you want now (Esc cancels)")
                          : TR(L"Klikk og trykk den nye kombinasjonen. Den må ha Ctrl, Alt, Shift eller Win. Kombinasjoner med Win er ofte opptatt av Windows.",
                               L"Click and press the new combination. It needs Ctrl, Alt, Shift or Win. Combinations with Win are often taken by Windows."),
               capturing_ && captureWhat_ == 0 ? TR(L"Trykk tastene …", L"Press the keys …")
                          : (hotkeyText_ = StartMenus::HotkeyName(s.spotlightMods, s.spotlightVk)).c_str(),
               [this] { capturing_ = true; captureWhat_ = 0; StartMenus::SuspendSpotlightHotkey(); SetTimer(hwnd, T_APPLY + 1, 10, nullptr); });
        if (!s.spotlightEnabled) rows.back().enabled = false;
        button(TR(L"Prøv Spotlight", L"Try Spotlight"), L"", TR(L"Åpne", L"Open"), [] { StartMenus::ToggleSpotlight(); });
        if (!s.spotlightEnabled) rows.back().enabled = false;
        // ---- SkyWritingTools ----
        header(L"SkyWritingTools");
        toggle(TR(L"SkyWritingTools", L"SkyWritingTools"),
               TR(L"Marker tekst hvor som helst og trykk hurtigtasten: en AI retter, skriver om, gjør teksten vennligere, mer profesjonell eller "
                  L"kortere, oppsummerer, lager hovedpunkter eller en tabell – eller gjør det du skriver. Uten markert tekst kan du spørre om hva som helst. "
                  L"Velg gjerne en annen hurtigtast hvis du bruker Ctrl + Mellomrom til noe annet (for eksempel i kodeprogrammer). "
                  L"Bygger på ideene i Writing Tools av theJayTea (GPL-3.0).",
                  L"Select text anywhere and press the hotkey: an AI proofreads, rewrites, makes it friendlier, more professional or shorter, "
                  L"summarises it, lists the key points or makes a table – or does what you type. With nothing selected you can ask anything. "
                  L"Pick another hotkey if you use Ctrl + Space for something else (code editors, for example). "
                  L"Built on the ideas of Writing Tools by theJayTea (GPL-3.0)."),
               [&s] { return s.writingEnabled; },
               [this, &s](bool v) {
                   s.writingEnabled = v;
                   IniWrite(L"SkyWritingTools", L"Enabled", v ? 1 : 0);
                   if (v && !WritingTools::ApplyHotkey())
                       MessageBoxW(hwnd, TR(L"Hurtigtasten er allerede i bruk av Windows eller et annet program. Velg en annen under Hurtigtast.",
                                            L"The hotkey is already used by Windows or another app. Pick another one under Hotkey."),
                                   L"Skyline", MB_ICONINFORMATION);
                   if (!v) WritingTools::ApplyHotkey();
                   SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
               });
        {
            bool on = s.writingEnabled;
            auto enable = [&] { if (!on) rows.back().enabled = false; };
            button(TR(L"Hurtigtast", L"Hotkey"),
                   capturing_ && captureWhat_ == 1 ? TR(L"Trykk tastene du vil bruke nå (Esc avbryter)", L"Press the keys you want now (Esc cancels)")
                              : TR(L"Klikk og trykk kombinasjonen du vil bruke. Den må ha Ctrl, Alt, Shift eller Win.",
                                   L"Click and press the combination you want. It needs Ctrl, Alt, Shift or Win."),
                   capturing_ && captureWhat_ == 1 ? TR(L"Trykk tastene …", L"Press the keys …")
                              : (hotkeyText2_ = StartMenus::HotkeyName(s.writingMods, s.writingVk)).c_str(),
                   [this] { capturing_ = true; captureWhat_ = 1; WritingTools::SuspendHotkey(); SetTimer(hwnd, T_APPLY + 1, 10, nullptr); });
            enable();
            choice(TR(L"AI-tjeneste", L"AI service"),
                   TR(L"Gemini har en gratis nøkkel. «OpenAI-kompatibel» passer for ChatGPT, OpenRouter og Ollama (kjører på denne PC-en, helt privat).",
                      L"Gemini has a free key. \"OpenAI-compatible\" fits ChatGPT, OpenRouter and Ollama (runs on this PC, fully private)."),
                   {L"Google Gemini", TR(L"OpenAI-kompatibel", L"OpenAI-compatible")},
                   [&s] { return s.writingProvider; },
                   [this, &s](int v) { s.writingProvider = v; IniWrite(L"SkyWritingTools", L"Provider", v); SetTimer(hwnd, T_APPLY + 1, 10, nullptr); });
            enable();
            std::wstring key = WritingTools::LoadKey(s.writingProvider);
            keyText_ = key.empty() ? std::wstring(TR(L"Ikke lagt inn", L"Not set"))
                                   : std::wstring(TR(L"Lagret, kryptert av Windows (", L"Saved, encrypted by Windows (")) + L"…" +
                                         key.substr(key.size() > 4 ? key.size() - 4 : 0) + L")";
            button(TR(L"API-nøkkel", L"API key"), keyText_, TR(L"Endre …", L"Change …"), [this, &s] {
                std::wstring v;
                if (WritingTools::AskText(hwnd, L"SkyWritingTools", TR(L"Lim inn API-nøkkelen. La feltet stå tomt for å fjerne den.",
                                                                        L"Paste the API key. Leave it empty to remove it."), v, true)) {
                    if (!WritingTools::SaveKey(s.writingProvider, v))
                        MessageBoxW(hwnd, TR(L"Kunne ikke lagre nøkkelen (har Skyline lov til å skrive i mappen sin?).",
                                             L"Couldn't save the key (may Skyline write to its folder?)."), L"Skyline", MB_ICONWARNING);
                    else if (!v.empty()) WritingTools::FetchModels(true);   // fill the model list for this key
                }
                SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
            });
            enable();
            if (s.writingProvider == 0) {
                button(TR(L"Hent en gratis Gemini-nøkkel", L"Get a free Gemini key"),
                       TR(L"Google AI Studio: logg inn med Google-kontoen din og trykk «Create API key». Lim den inn over.",
                          L"Google AI Studio: sign in with your Google account and press \"Create API key\". Paste it above."),
                       TR(L"Åpne", L"Open"), [] { ShellExecuteW(nullptr, L"open", L"https://aistudio.google.com/apikey", nullptr, nullptr, SW_SHOWNORMAL); });
                enable();
            }
            {   // the model: a list (fetched for your key), with how busy each one tends to be
                int prov = s.writingProvider;
                wtModels_ = WritingTools::ModelList(prov);
                std::wstring cur = prov == 0 ? s.writingGeminiModel : s.writingOpenAiModel;
                if (!cur.empty() && std::find(wtModels_.begin(), wtModels_.end(), cur) == wtModels_.end()) wtModels_.insert(wtModels_.begin(), cur);
                std::vector<std::wstring> labels;
                for (auto& id : wtModels_) labels.push_back(WritingTools::ModelLabel(prov, id));
                labels.push_back(TR(L"Annen modell …", L"Other model …"));
                auto models = wtModels_;   // the list these labels were made from (a fetch may replace wtModels_ while the menu is open)
                choice(TR(L"Modell", L"Model"),
                       prov == 0 ? TR(L"Flash-Lite er sjelden opptatt. Den nyeste Flash er ofte overbelastet på gratisnivået. Er modellen opptatt, "
                                      L"prøver Skyline igjen og bytter til Flash-Lite av seg selv.",
                                      L"Flash-Lite is rarely busy. The newest Flash is often overloaded on the free tier. If the model is busy, "
                                      L"Skyline tries again and switches to Flash-Lite by itself.")
                                 : TR(L"Trykk «Hent modellene dine» for å få listen fra tjenesten (for Ollama: modellene du har lastet ned).",
                                      L"Press \"Get your models\" for the service's list (for Ollama: the models you've downloaded)."),
                       labels,
                       [&s, models] {
                           const std::wstring& c = s.writingProvider == 0 ? s.writingGeminiModel : s.writingOpenAiModel;
                           auto it = std::find(models.begin(), models.end(), c);
                           return it == models.end() ? (int)models.size() : (int)(it - models.begin());
                       },
                       [this, &s, models](int v) {
                           std::wstring id;
                           if (v >= 0 && v < (int)models.size()) id = models[v];
                           else {   // "Other model …": type its name
                               id = s.writingProvider == 0 ? s.writingGeminiModel : s.writingOpenAiModel;
                               if (!WritingTools::AskText(hwnd, L"SkyWritingTools",
                                                         TR(L"Navnet på modellen, akkurat slik tjenesten skriver det.", L"The model's name, exactly as the service writes it."),
                                                         id, false) || id.empty()) {
                                   SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
                                   return;
                               }
                           }
                           if (s.writingProvider == 0) { s.writingGeminiModel = id; IniWrite(L"SkyWritingTools", L"GeminiModel", id); }
                           else { s.writingOpenAiModel = id; IniWrite(L"SkyWritingTools", L"OpenAIModel", id); }
                           SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
                       });
                enable();
                button(TR(L"Hent modellene dine", L"Get your models"),
                       TR(L"Henter listen over modellene nøkkelen din har tilgang til, rett fra tjenesten.", L"Fetches the list of models your key can use, straight from the service."),
                       TR(L"Hent", L"Get"), [] { WritingTools::FetchModels(false); });
                enable();
            }
            if (s.writingProvider == 1) {
                button(TR(L"Adresse", L"Address"), s.writingBaseUrl, TR(L"Endre …", L"Change …"), [this, &s] {
                    std::wstring v = s.writingBaseUrl;
                    if (WritingTools::AskText(hwnd, L"SkyWritingTools", TR(L"Adressen til tjenesten (den som slutter på /v1).", L"The service's address (the one ending in /v1)."), v, false)) {
                        if (v.empty()) v = L"https://api.openai.com/v1";
                        s.writingBaseUrl = v;
                        IniWrite(L"SkyWritingTools", L"BaseUrl", v);
                    }
                    SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
                });
                enable();
                button(TR(L"Bruk Ollama på denne PC-en", L"Use Ollama on this PC"),
                       TR(L"Setter adressen til http://localhost:11434/v1. Ingen nøkkel trengs, og teksten forlater aldri PC-en.",
                          L"Sets the address to http://localhost:11434/v1. No key needed, and the text never leaves this PC."),
                       TR(L"Bruk", L"Use"), [this, &s] {
                           s.writingBaseUrl = L"http://localhost:11434/v1";
                           IniWrite(L"SkyWritingTools", L"BaseUrl", s.writingBaseUrl);
                           SetTimer(hwnd, T_APPLY + 1, 10, nullptr);
                       });
                enable();
            }
            button(TR(L"Test tilkoblingen", L"Test the connection"), TR(L"Sender en bitteliten forespørsel og sier hvordan det gikk.", L"Sends a tiny request and tells you how it went."),
                   TR(L"Test", L"Test"), [] { WritingTools::TestConnection(); });
            enable();
            info(TR(L"Personvern", L"Privacy"),
                 TR(L"Teksten du markerer sendes til tjenesten du velger. Gratisnivået til Gemini kan bruke den til å forbedre Googles modeller. "
                    L"Med Ollama blir alt på PC-en. Nøkkelen lagres kryptert av Windows, og blir ikke med i backup-filer.",
                    L"The text you select is sent to the service you choose. Gemini's free tier may use it to improve Google's models. "
                    L"With Ollama everything stays on this PC. The key is stored encrypted by Windows and isn't included in backup files."));
        }
        header(TR(L"Skrivebordsklokke", L"Desktop clock"));
        toggle(TR(L"Klokke på skrivebordet", L"Clock on the desktop"), TR(L"En stor, gjennomsiktig klokke med dato bak alle vinduer.", L"A big, see-through clock with the date, behind all windows."),
               [] { return GetPrivateProfileIntW(L"DeskClock", L"Enabled", 0, IniPathOf().c_str()) != 0; },
               [](bool v) { IniWrite(L"DeskClock", L"Enabled", v ? 1 : 0); DeskClockApply(); });
        {
            std::vector<std::wstring> mons{TR(L"Hovedskjermen", L"Main screen")};
            for (int mi = 1; mi <= DeskClockMonitorCount(); ++mi) mons.push_back(TR(L"Skjerm ", L"Screen ") + std::to_wstring(mi));
            choice(TR(L"Skjerm", L"Screen"), TR(L"Hvilken skjerm klokka ligger på.", L"Which screen the clock sits on."), mons,
                   [] { return (int)GetPrivateProfileIntW(L"DeskClock", L"Monitor", 0, IniPathOf().c_str()); },
                   [](int v) { IniWrite(L"DeskClock", L"Monitor", v); DeskClockApply(); });
        }
        choice(TR(L"Plassering", L"Position"), nullptr,
               {TR(L"Midten", L"Centre"), TR(L"Oppe til venstre", L"Top left"), TR(L"Oppe til høyre", L"Top right"), TR(L"Nede til venstre", L"Bottom left"), TR(L"Nede til høyre", L"Bottom right")},
               [] { return (int)GetPrivateProfileIntW(L"DeskClock", L"Position", 0, IniPathOf().c_str()); },
               [](int v) { IniWrite(L"DeskClock", L"Position", v); DeskClockApply(); });
        choice(TR(L"Størrelse", L"Size"), nullptr, {TR(L"Liten", L"Small"), TR(L"Middels", L"Medium"), TR(L"Stor", L"Large")},
               [] { return (int)GetPrivateProfileIntW(L"DeskClock", L"Size", 1, IniPathOf().c_str()); },
               [](int v) { IniWrite(L"DeskClock", L"Size", v); DeskClockApply(); });
        header(L"Mission Control");
        toggle(L"Mission Control",
               TR(L"Alle vinduene på skjermen side om side, levende. Vinduene glir ut i et rutenett og tilbake; klikk ett for å gå dit, Esc for å gå tilbake.",
                  L"Every window on the screen side by side, live. The windows glide out into a grid and back; click one to go there, Esc to go back."),
               [&s] { return s.missionEnabled; },
               [this, &s](bool v) { s.missionEnabled = v; IniWrite(L"MissionControl", L"Enabled", v ? 1 : 0); ScheduleApply(); Rebuild(); });
        toggle(TR(L"Åpnes med Win+Tab", L"Opens with Win+Tab"),
               TR(L"I stedet for Windows' oppgavevisning. Hot corner-handlingen «Oppgavevisning» åpner også Mission Control.",
                  L"Instead of Windows' Task view. The \u201cTask view\u201d hot corner action opens Mission Control too."),
               [&s] { return s.missionWinTab; },
               [this, &s](bool v) { s.missionWinTab = v; IniWrite(L"MissionControl", L"WinTab", v ? 1 : 0); ScheduleApply(); });
        if (!s.missionEnabled) rows.back().enabled = false;
        button(TR(L"Prøv Mission Control", L"Try Mission Control"), L"", TR(L"Åpne", L"Open"), [] { MissionControl::Toggle(); });
        if (!s.missionEnabled) rows.back().enabled = false;
        header(L"Stage Manager");
        toggle(L"Stage Manager",
               TR(L"Som på Mac: appen du jobber i blir på skjermen, de andre appene ligger som små bilder i en stripe til venstre. Klikk en gruppe for å hente den fram. Hver skjerm har sin egen. Aldri på en skjerm med spill. Tiling er av mens dette er på.",
                  L"Like on a Mac: the app you work in stays on the screen, the other apps sit as little pictures in a strip on the left. Click a group to bring it forward. Every screen has its own. Never on a screen with a game. Tiling is off while this is on."),
               [&s] { return s.stageEnabled; },
               [this, &s](bool v) { s.stageEnabled = v; IniWrite(L"StageManager", L"Enabled", v ? 1 : 0); ScheduleApply(); Rebuild(); });
        choice(TR(L"Stripa", L"The strip"),
               TR(L"Vanlig viser stripa hele tiden (den gjemmer seg når et vindu dekker den). Skjult vises bare når musa går helt til venstre på skjermen.",
                  L"Normal keeps the strip on show (it slides away when a window covers it). Hidden only shows it when the mouse goes to the very left edge of the screen."),
               {TR(L"Vanlig (som på Mac)", L"Normal (like a Mac)"), TR(L"Skjult til musa går helt til venstre", L"Hidden until the mouse touches the left edge")},
               [&s] { return s.stageMode; },
               [this, &s](int v) { s.stageMode = v == 1 ? 1 : 0; IniWrite(L"StageManager", L"Mode", s.stageMode); ScheduleApply(); });
        if (!s.stageEnabled) rows.back().enabled = false;
        toggle(TR(L"Vis stripa selv om et vindu dekker den", L"Show the strip even when a window covers it"),
               TR(L"Av (standard): stripa gjemmer seg når et maksimert vindu eller et vindu helt til venstre dekker den, og kommer fram når musa går helt til venstre. På: stripa ligger alltid synlig oppå vinduene (bare i vanlig modus).",
                  L"Off (default): the strip hides when a maximised window, or one at the far left, covers it, and comes back when the mouse goes to the left edge. On: the strip always stays on top of the windows (normal mode only)."),
               [&s] { return s.stageShowOver; },
               [this, &s](bool v) { s.stageShowOver = v; IniWrite(L"StageManager", L"ShowOver", v ? 1 : 0); ScheduleApply(); });
        if (!s.stageEnabled) rows.back().enabled = false;
        choice(TR(L"Kortet under musa", L"Card under the mouse"),
               TR(L"Hvordan kortet du peker på vises i stripa.", L"How the card you point at looks in the strip."),
               {TR(L"Dimmer de andre", L"Dim the others"), TR(L"Myk glød", L"Soft glow")},
               [&s] { return s.stageHover; },
               [this, &s](int v) { s.stageHover = v == 1 ? 1 : 0; IniWrite(L"StageManager", L"Hover", s.stageHover); ScheduleApply(); });
        if (!s.stageEnabled) rows.back().enabled = false;
        toggle(TR(L"Ta med minimerte vinduer", L"Include minimised windows"),
               TR(L"Av (som på Mac): bare det Stage Manager selv legger bort er i stripa; det du minimerer ligger i docken. På: apper du allerede har minimert er også i stripa, og ett klikk henter dem fram.",
                  L"Off (like a Mac): only what Stage Manager puts away is in the strip; what you minimise sits in the dock. On: apps you already minimised are in the strip too, and one click brings them back."),
               [&s] { return s.stageMinimized; },
               [this, &s](bool v) { s.stageMinimized = v; IniWrite(L"StageManager", L"Minimized", v ? 1 : 0); ScheduleApply(); });
        if (!s.stageEnabled) rows.back().enabled = false;
        header(TR(L"Docken: papirkurv og stack", L"The dock: Recycle Bin and stack"));
        toggle(TR(L"Papirkurv i docken", L"Recycle Bin in the dock"),
               TR(L"Helt til høyre i docken, tom eller full. Dra filer dit for å slette dem, høyreklikk for å tømme (Windows spør først).",
                  L"At the far right of the dock, empty or full. Drag files onto it to delete them, right-click to empty it (Windows asks first)."),
               [&s] { return s.dockTrash; },
               [this, &s](bool v) { s.dockTrash = v; IniWrite(L"Dock", L"Trash", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Stack i docken", L"Stack in the dock"),
               TR(L"En mappe i docken. Klikk for å se de nyeste filene i et rutenett. Mappen leses først når du klikker.",
                  L"A folder in the dock. Click it to see the newest files in a grid. The folder is only read when you click."),
               [&s] { return s.dockStack; },
               [this, &s](bool v) { s.dockStack = v; IniWrite(L"Dock", L"Stack", v ? 1 : 0); ScheduleApply(); });
        {
            static const KNOWNFOLDERID* known[] = {&FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Pictures};
            auto knownPath = [](int i) {
                PWSTR p = nullptr;
                std::wstring out;
                if (SUCCEEDED(SHGetKnownFolderPath(*known[i], 0, nullptr, &p)) && p) { out = p; CoTaskMemFree(p); }
                return out;
            };
            std::wstring cur = StackFolder();
            std::vector<std::wstring> ch = {TR(L"Nedlastinger", L"Downloads"), TR(L"Dokumenter", L"Documents"), TR(L"Skrivebord", L"Desktop"),
                                            TR(L"Bilder", L"Pictures")};
            int sel = 4;
            for (int i = 0; i < 4; ++i) if (!lstrcmpiW(knownPath(i).c_str(), cur.c_str())) { sel = i; break; }
            if (sel == 4) ch.push_back(FileNameOf(cur));   // the folder you picked yourself
            ch.push_back(TR(L"Annen mappe …", L"Other folder …"));
            int other = (int)ch.size() - 1;
            choice(TR(L"Mappe for stacken", L"Folder for the stack"), nullptr, ch, [sel] { return sel; },
                   [this, &s, knownPath, other](int v) {
                       if (v < 4) s.dockStackFolder = v == 0 ? L"" : knownPath(v);
                       else if (v != other) return;   // the folder that's already chosen
                       else {   // pick any folder
                           ComPtr<IFileOpenDialog> dlg;
                           if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(dlg.Put())))) return;
                           FILEOPENDIALOGOPTIONS o = 0;
                           dlg->GetOptions(&o);
                           dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                           ComPtr<IShellItem> it;
                           PWSTR p = nullptr;
                           if (FAILED(dlg->Show(hwnd)) || FAILED(dlg->GetResult(it.Put())) || FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return;
                           s.dockStackFolder = p;
                           CoTaskMemFree(p);
                       }
                       IniWrite(L"Dock", L"StackFolder", s.dockStackFolder);
                       ScheduleApply();
                       Rebuild();
                   });
            if (!s.dockStack) rows.back().enabled = false;
        }
        header(L"Native Flyouts");
        toggle(L"Native Flyouts",
               TR(L"Klikk på lyd- eller nettverksikonet i topplinja åpner Skylines egne paneler i stedet for Windows' hurtiginnstillinger: "
                  L"volum, lydenhet og volum per app; Wi-Fi, nettverk og Bluetooth. Batteriet åpner fortsatt Windows' egne.",
                  L"Clicking the sound or network icon in the top bar opens Skyline's own panels instead of Windows' Quick Settings: "
                  L"volume, output device and volume per app; Wi-Fi, networks and Bluetooth. The battery still opens Windows' own."),
               [&s] { return s.flyoutsEnabled; },
               [&s](bool v) { s.flyoutsEnabled = v; IniWrite(L"Flyouts", L"Enabled", v ? 1 : 0); if (!v) Flyouts::Hide(); });
        header(L"Now Playing");
        toggle(L"Now Playing",
               TR(L"Det som spilles i Spotify, nettleseren og andre apper, med albumbilde og kontroller",
                  L"What's playing in Spotify, your browser and other apps, with artwork and controls"),
               [&s] { return s.npEnabled; },
               [this, &s](bool v) { s.npEnabled = v; IniWrite(L"NowPlaying", L"Enabled", v ? 1 : 0); ScheduleApply(); });
        choice(TR(L"Vis i", L"Show in"), nullptr,
               {TR(L"Docken", L"The dock"), TR(L"Topplinjen", L"The top bar"), TR(L"Begge", L"Both")},
               [&s] { return s.npWhere; },
               [this, &s](int v) { s.npWhere = v; IniWrite(L"NowPlaying", L"Where", v); ScheduleApply(); });
        choice(TR(L"Plassering i topplinjen", L"Position in the top bar"),
               TR(L"Ved klokka passer best med stilen Separert. I midten ligger den for seg selv, som en liten øy.",
                  L"Next to the clock suits the Separated style best. In the middle it sits on its own, like a little island."),
               {TR(L"Ved tray-ikonene", L"Next to the tray icons"), TR(L"Ved klokka", L"Next to the clock"),
                TR(L"I midten", L"In the middle"), TR(L"Etter appmenyene", L"After the app menus")},
               [&s] { return s.npBarPos; },
               [this, &s](int v) { s.npBarPos = v; IniWrite(L"NowPlaying", L"BarPosition", v); ScheduleApply(); });
        toggle(TR(L"Fargetone fra albumbildet", L"Tint from the artwork"),
               TR(L"Kortet får en svak farge hentet fra coveret", L"The card takes a soft colour from the cover"),
               [&s] { return s.npTint; },
               [&s](bool v) { s.npTint = v; IniWrite(L"NowPlaying", L"Tint", v ? 1 : 0); });
        header(L"SkyGarden");
        toggle(L"SkyGarden",
               TR(L"Skrivebordsikonene dine i pene glassbed. Windows' egne ikoner skjules mens det er på; filene flyttes aldri.",
                  L"Your desktop icons in neat glass beds. Windows' own icons are hidden while it's on; files never move."),
               [&s] { return s.gardenEnabled; },
               [this, &s](bool v) { s.gardenEnabled = v; IniWrite(L"Garden", L"Enabled", v ? 1 : 0); ScheduleApply(); });
        toggle(TR(L"Høyreklikk på skrivebordet", L"Right-click on the desktop"),
               TR(L"Høyreklikk et tomt sted på skrivebordet for «Nytt bed her», sammen med Windows' egne valg. Shift + høyreklikk gir Windows' meny som før.",
                  L"Right-click an empty spot on the desktop for “New bed here”, together with Windows' own items. Shift + right-click gives Windows' menu as before."),
               [&s] { return s.gardenDesktopMenu; },
               [&s](bool v) { s.gardenDesktopMenu = v; IniWrite(L"Garden", L"DesktopMenu", v ? 1 : 0); Garden::ApplySettings(); });
        if (!s.gardenEnabled) rows.back().enabled = false;
        button(TR(L"Nytt bed", L"New bed"), TR(L"Eller høyreklikk et bed. Dra i tittelen for å flytte, i kantene for å endre størrelse, klikk tittelen for å rulle opp.",
                                              L"Or right-click a bed. Drag the title to move, the edges to resize, click the title to roll up."),
               TR(L"Lag", L"Create"), [] { Garden::NewBed(); });
        if (!s.gardenEnabled) rows.back().enabled = false;
        button(TR(L"Nytt mappe-bed", L"New folder bed"),
               TR(L"Et bed som speiler en mappe, f.eks. Dokumenter eller Nedlastinger. Det oppdateres av seg selv, og filer du drar inn havner i mappen (ikke på skrivebordet).",
                  L"A bed that mirrors a folder, e.g. Documents or Downloads. It updates by itself, and files you drag in go into the folder (not onto the desktop)."),
               TR(L"Velg mappe …", L"Choose folder …"), [] { POINT c; GetCursorPos(&c); Garden::NewFolderBedAt(c); });
        if (!s.gardenEnabled) rows.back().enabled = false;
        button(TR(L"Oppsett", L"Layout"), TR(L"Start på nytt med Apper og Skrivebord", L"Start over with Apps and Desktop"),
               TR(L"Tilbakestill", L"Reset"), [] { Garden::ResetLayout(); });
        header(TR(L"Klokke i docken", L"Clock in the dock"));
        auto relayout = [] { for (auto& d : g_app.docks) d->MediaChanged(true); };
        toggle(TR(L"Klokke i docken", L"Clock in the dock"),
               TR(L"En klokke helt til høyre i docken. Klikk for kalender og varsler.",
                  L"A clock at the far right of the dock. Click it for the calendar and notifications."),
               [&s] { return s.dockClock; },
               [&s, relayout](bool v) { s.dockClock = v; IniWrite(L"DockClock", L"Enabled", v ? 1 : 0); relayout(); });
        choice(TR(L"Stil", L"Style"), nullptr, {TR(L"Digital", L"Digital"), TR(L"Analog", L"Analog")},
               [&s] { return s.dockClockAnalog ? 1 : 0; },
               [&s, relayout](int v) { s.dockClockAnalog = v == 1; IniWrite(L"DockClock", L"Analog", v == 1 ? 1 : 0); relayout(); });
        if (!s.dockClock) rows.back().enabled = false;
        toggle(TR(L"Sekundviser", L"Second hand"), TR(L"Den analoge klokka viser sekundene live", L"The analog clock shows the seconds live"),
               [&s] { return s.dockClockSeconds; },
               [&s, relayout](bool v) { s.dockClockSeconds = v; IniWrite(L"DockClock", L"Seconds", v ? 1 : 0); relayout(); });
        if (!s.dockClock || !s.dockClockAnalog) rows.back().enabled = false;
        break;
    }
    case SettingsPage::Advanced: {
        header(TR(L"Ytelse", L"Performance"));
        toggle(TR(L"Ytelsesmodus", L"Performance mode"),
               TR(L"Slår av alle tillegg unntatt Spotlight, gjør topplinjen klassisk, og slår av gjennomsiktighet i Skyline og i Windows. "
                  L"Alt du hadde kommer tilbake når du slår den av (endringer gjort mens den er på, glemmes).",
                  L"Turns off every plugin except Spotlight, makes the top bar classic, and turns off transparency in Skyline and in Windows. "
                  L"Everything you had comes back when you turn it off (changes made while it's on are forgotten)."),
               [&s] { return s.performanceMode; },
               [this](bool v) {
                   if (!Look::SetPerformanceMode(v))
                       MessageBoxW(hwnd, TR(L"Kunne ikke ta vare på innstillingene dine, så ytelsesmodus ble ikke slått på.",
                                            L"Couldn't keep a copy of your settings, so performance mode wasn't turned on."), L"Skyline", MB_ICONWARNING);
                   SetTimer(hwnd, T_APPLY + 1, 500, nullptr);
               });
        if (s.performanceMode)
            info(TR(L"Ytelsesmodus er på", L"Performance mode is on"),
                 TR(L"Skyline er så enkel og lett som mulig nå. Slå den av for å få tilbake utseendet og tilleggene dine.",
                    L"Skyline is as plain and light as it gets right now. Turn it off to get your look and plugins back."));
        break;
    }
    case SettingsPage::About: {
        PROCESS_MEMORY_COUNTERS pmc{sizeof pmc};
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        wchar_t mem[64];
        swprintf(mem, 64, TR(L"Bruker %.1f MB minne akkurat nå", L"Using %.1f MB of memory right now"), pmc.WorkingSetSize / (1024.0 * 1024.0));
        header(L"Skyline " SKYLINE_VERSION_W);
        info(TR(L"Native topplinje, dock og tiling for Windows 11", L"Native top bar, dock and tiling for Windows 11"),
             TR(L"C++, Win32 og Direct2D. Ingen webview.", L"C++, Win32 and Direct2D. No webview."));
        info(TR(L"Laget av Yen-Zen & Cyber og en hel haug med Claude", L"Made by Yen-Zen & Cyber and a whole bunch of Claude"),
             TR(L"© 2026 Yen-Zen & Cyber. Alle rettigheter forbeholdt.", L"© 2026 Yen-Zen & Cyber. All rights reserved."));
        info(TR(L"Takk til", L"Thanks to"),
             TR(L"SkyWritingTools bygger på ideene og instruksjonene i Writing Tools av theJayTea (github.com/theJayTea/WritingTools, GPL-3.0).",
                L"SkyWritingTools is built on the ideas and prompts of Writing Tools by theJayTea (github.com/theJayTea/WritingTools, GPL-3.0)."));
        button(TR(L"Lisens", L"License"), TR(L"Gratis å bruke og dele, men ikke å selge eller endre", L"Free to use and share, but not to sell or modify"),
               TR(L"Vis lisens", L"View license"), [this] {
                   Dialog::Spec d;
                   d.caption = TR(L"Skyline – lisens", L"Skyline – license");
                   d.heading = TR(L"Skyline-lisens (gratisprogram)", L"Skyline License (Freeware)");
                   d.body = TR(L"1. Du kan laste ned, installere og bruke Skyline gratis, hjemme eller på jobb.\n\n2. Du kan dele Skyline med andre, men bare den originale, uendrede nedlastingen, og bare gratis.\n\n3. Du kan ikke selge Skyline, ta betalt for den eller ta den med i et annet produkt, en pakke eller en tjeneste.\n\n4. Du kan ikke endre, dekompilere, disassemblere eller reversere Skyline, eller lage eller dele endrede versjoner, unntatt der loven uttrykkelig tillater det.\n\n5. Alle rettigheter som ikke uttrykkelig er gitt her, forbeholdes Yen-Zen & Cyber, inkludert alle rettigheter til navnet Skyline, ikonet og kildekoden.\n\n6. Skyline leveres «som den er», uten noen form for garanti. Så langt loven tillater, er utviklerne ikke ansvarlige for skade som følger av bruken.",
                               L"1. You may download, install and use Skyline free of charge, at home or at work.\n\n2. You may share Skyline with others, but only the original, unmodified download, and only free of charge.\n\n3. You may not sell Skyline, charge for it, or include it in another product, bundle or service.\n\n4. You may not modify, decompile, disassemble or reverse engineer Skyline, or make or share altered versions of it, except where the law expressly allows this.\n\n5. All rights not expressly granted here are reserved by Yen-Zen & Cyber, including all rights to the name Skyline, its icon and its source code.\n\n6. Skyline is provided \"as is\", without warranty of any kind. To the extent the law allows, the authors are not liable for any damage arising from its use.");
                   d.note = TR(L"Copyright © 2026 Yen-Zen & Cyber. Alle rettigheter forbeholdt.", L"Copyright © 2026 Yen-Zen & Cyber. All rights reserved.");
                   d.secondary = TR(L"Lukk", L"Close");
                   d.width = 560;
                   d.owner = hwnd;
                   Dialog::Show(d, nullptr);
               });
        info(TR(L"Takk til", L"Thanks to"), TR(L"ManagedShell, som inspirerte hvordan tray-ikonene fungerer", L"ManagedShell, which inspired how the tray icons work"));
        info(mem, L"");
        header(TR(L"Oppdateringer", L"Updates"));
        toggle(TR(L"Se etter oppdateringer automatisk", L"Check for updates automatically"),
               TR(L"Ser etter en ny versjon av og til, og spør før noe installeres", L"Looks for a new version now and then, and asks before installing anything"),
               [&s] { return s.updateCheck; }, [&s](bool v) { s.updateCheck = v; IniWrite(L"Update", L"Check", v ? 1 : 0); });
        if (!Update::HasKey()) {
            info(TR(L"Denne utgaven kan ikke oppdatere seg selv", L"This build can't update itself"),
                 TR(L"Den ble bygget uten oppdateringsnøkkel (src/update_key.h)", L"It was built without an update key (src/update_key.h)"));
        } else {
            using St = Update::State;
            St st = Update::GetState();
            std::wstring err = Update::Error(), title, desc;
            if (st == St::Available || st == St::Downloading || st == St::Installing) {
                title = TR(L"Skyline ", L"Skyline ") + Update::AvailableVersion() + TR(L" er klar", L" is available");
                desc = !err.empty() ? err : Update::Notes();
                if (st == St::Available)
                    button(title, desc, TR(L"Oppdater", L"Update"), [] { Update::Install(); }, true);
                else {
                    button(title, desc, st == St::Downloading ? TR(L"Laster ned …", L"Downloading …") : TR(L"Installerer …", L"Installing …"), nullptr, true);
                    rows.back().enabled = false;
                }
            } else {
                title = st == St::UpToDate ? TR(L"Du har nyeste versjon", L"You're up to date")
                                           : TR(L"Se etter oppdateringer", L"Check for updates");
                std::wstring when = Update::LastChecked();
                desc = !err.empty() ? err : when.empty() ? std::wstring() : TR(L"Sist sjekket kl. ", L"Last checked at ") + when;
                button(title, desc, st == St::Checking ? TR(L"Sjekker …", L"Checking …") : TR(L"Se etter nå", L"Check now"),
                       [] { Update::CheckNow(); });
                if (st == St::Checking) rows.back().enabled = false;
            }
        }
        header(L"");
        button(TR(L"Avslutt Skyline", L"Quit Skyline"), TR(L"Alt går tilbake til vanlig Windows", L"Everything goes back to regular Windows"), TR(L"Avslutt", L"Quit"), [] { g_app.Quit(); });
        break;
    }
    }
}

// ---------------------------------------------------------------------------
float SettingsWindow::TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> tl;
    g_gfx.dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000, 100, tl.Put());
    DWRITE_TEXT_METRICS m{};
    if (tl) tl->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float SettingsWindow::TextHeight(const std::wstring& s, IDWriteTextFormat* f, float w) {
    ComPtr<IDWriteTextLayout> tl;
    g_gfx.dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(w, 10.0f), 2000, tl.Put());
    DWRITE_TEXT_METRICS m{};
    if (tl) tl->GetMetrics(&m);
    return m.height;
}

void SettingsWindow::LayoutRows(float width) {
    float x0 = kNavW + 8, x1 = width - kContentPad;
    float y = kTitleTop + 52;
    for (auto& r : rows) {
        if (r.type == Row::Header) {
            r.card = D2D1::RectF(x0, y, x1, y + (r.title.empty() ? 16 : 40));
            y = r.card.bottom;
            continue;
        }
        float h = r.desc.empty() ? 52.0f : 66.0f;
        r.card = D2D1::RectF(x0, y, x1, y + h);
        float cy = y + h / 2;
        switch (r.type) {
        case Row::Toggle: r.ctl = D2D1::RectF(x1 - 16 - 40, cy - 10, x1 - 16, cy + 10); break;
        case Row::Slider: r.ctl = D2D1::RectF(x1 - 16 - 200, cy - 12, x1 - 16, cy + 12); break;
        case Row::Choice: {
            float w = 0;
            for (auto& c : r.choices) w = std::max(w, TextWidth(c, fBody.Get()));
            w = std::clamp(w + 48, 160.0f, 340.0f);
            r.ctl = D2D1::RectF(x1 - 16 - w, cy - 16, x1 - 16, cy + 16);
            break;
        }
        case Row::Button: {
            float w = std::max(96.0f, TextWidth(r.buttonText, fBody.Get()) + 40);
            r.ctl = D2D1::RectF(x1 - 16 - w, cy - 16, x1 - 16, cy + 16);
            break;
        }
        default: r.ctl = D2D1::RectF(0, 0, 0, 0); break;
        }
        r.textRight = (r.ctl.right > r.ctl.left ? r.ctl.left - (r.type == Row::Slider ? 80 : r.type == Row::Toggle ? 52 : 16) : r.card.right - 16);
        if (!r.desc.empty()) {   // a long description wraps onto more lines and the card grows, nothing is cut off
            r.descH = std::max(18.0f, std::ceil(TextHeight(r.desc, fCaption.Get(), r.textRight - (r.card.left + 16))));
            float extra = r.descH - 18;
            if (extra > 0) {
                h += extra;
                r.card.bottom += extra;
                r.ctl.top += extra / 2; r.ctl.bottom += extra / 2;
            }
        }
        y += h + kCardGap;
    }
    contentH = y + 24;
}

// ---------------------------------------------------------------------------
bool SettingsWindow::Resources() {
    if (rt) return true;
    RECT rc;
    GetClientRect(hwnd, &rc);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              (float)dpi, (float)dpi);
    if (FAILED(g_gfx.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                                                 rt.Put())))
        return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), br.Put());

    auto font = [](ComPtr<IDWriteTextFormat>& f, const wchar_t* fam, DWRITE_FONT_WEIGHT w, float size, bool center) {
        g_gfx.dwrite->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", f.Put());
        if (!f) return;
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteInlineObject> ell;
        g_gfx.dwrite->CreateEllipsisTrimmingSign(f.Get(), ell.Put());
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ell.Get());
    };
    ComPtr<IDWriteFontCollection> fc;
    g_gfx.dwrite->GetSystemFontCollection(fc.Put(), FALSE);
    auto has = [&](const wchar_t* n) { UINT32 i; BOOL e = FALSE; return fc && SUCCEEDED(fc->FindFamilyName(n, &i, &e)) && e; };
    const wchar_t* text = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* disp = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icons = has(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    font(fTitle, disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 28, false);
    font(fBody, text, DWRITE_FONT_WEIGHT_NORMAL, 14, false);
    font(fCaption, text, DWRITE_FONT_WEIGHT_NORMAL, 12, false);
    font(fSection, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, false);
    font(fApp, text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12, false);
    font(fIcon, icons, DWRITE_FONT_WEIGHT_NORMAL, 16, true);
    font(fIconSmall, icons, DWRITE_FONT_WEIGHT_NORMAL, 12, true);
    return true;
}

void SettingsWindow::Discard() {
    fTitle.Reset(); fBody.Reset(); fCaption.Reset(); fSection.Reset(); fIcon.Reset(); fIconSmall.Reset(); fApp.Reset();
    br.Reset();
    rt.Reset();
}

void SettingsWindow::DrawText(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    br->SetColor(c);
    rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void SettingsWindow::DrawToggle(D2D1_RECT_F r, bool on, bool hover, bool enabled) {
    Pal p = P();
    float rad = (r.bottom - r.top) / 2;
    auto rr = D2D1::RoundedRect(r, rad, rad);
    float cy = (r.top + r.bottom) / 2;
    if (on) {
        D2D1_COLOR_F a = p.accent;
        if (hover) { a.r = std::min(1.0f, a.r * 1.08f); a.g = std::min(1.0f, a.g * 1.08f); a.b = std::min(1.0f, a.b * 1.08f); }
        br->SetColor(a);
        rt->FillRoundedRectangle(rr, br.Get());
        br->SetColor(p.onAccent);
        float k = hover ? 7.0f : 6.0f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.right - rad, cy), k, k), br.Get());
    } else {
        br->SetColor(hover ? p.ctlHover : D2D1::ColorF(0, 0, 0, g_app.theme.dark ? 0.1f : 0.02f));
        rt->FillRoundedRectangle(rr, br.Get());
        br->SetColor(p.text2);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f),
                                 br.Get(), 1.0f);
        float k = hover ? 6.0f : 5.0f;
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + rad, cy), k, k), br.Get());
    }
    DrawText(on ? TR(L"På", L"On") : TR(L"Av", L"Off"), fBody.Get(), D2D1::RectF(r.left - 40, r.top - 6, r.left - 10, r.bottom + 6),
             enabled ? P().text : P().text3);
}

void SettingsWindow::DrawSlider(const Row& row, D2D1_RECT_F r, int value, bool hover) {
    Pal p = P();
    float cy = (r.top + r.bottom) / 2;
    float frac = row.max > row.min ? std::clamp(float(value - row.min) / float(row.max - row.min), 0.0f, 1.0f) : 0;
    float x = r.left + 10 + frac * (r.right - r.left - 20);
    br->SetColor(p.track);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 10, cy - 2, r.right - 10, cy + 2), 2, 2), br.Get());
    br->SetColor(p.accent);
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 10, cy - 2, x, cy + 2), 2, 2), br.Get());
    // thumb: outer ring + accent dot
    br->SetColor(g_app.theme.dark ? D2D1::ColorF(0.27f, 0.27f, 0.27f) : D2D1::ColorF(1, 1, 1));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 10, 10), br.Get());
    br->SetColor(p.ctlStroke);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 10, 10), br.Get(), 1.0f);
    br->SetColor(p.accent);
    float k = (hover || dragRow >= 0) ? 7.0f : 6.0f;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), k, k), br.Get());
    std::wstring v = std::to_wstring(value) + row.unit;
    DrawText(v, fBody.Get(), D2D1::RectF(r.left - 70, r.top, r.left - 6, r.bottom), p.text2);
}

void SettingsWindow::DrawButtonish(D2D1_RECT_F r, const std::wstring& text, bool accent, bool hover, bool chevron, bool enabled) {
    Pal p = P();
    auto rr = D2D1::RoundedRect(r, 4, 4);
    if (accent) {
        br->SetColor(p.accent);
        rt->FillRoundedRectangle(rr, br.Get());
    } else {
        br->SetColor(hover ? p.ctlHover : p.ctl);
        rt->FillRoundedRectangle(rr, br.Get());
        br->SetColor(p.ctlStroke);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3.5f, 3.5f),
                                 br.Get(), 1.0f);
    }
    D2D1_COLOR_F c = accent ? p.onAccent : (enabled ? p.text : p.text3);
    if (chevron) {
        DrawText(text, fBody.Get(), D2D1::RectF(r.left + 11, r.top, r.right - 30, r.bottom), c);
        wchar_t g = 0xE70D;
        br->SetColor(p.text2);
        rt->DrawTextW(&g, 1, fIconSmall.Get(), D2D1::RectF(r.right - 30, r.top, r.right - 8, r.bottom), br.Get());
    } else {
        ComPtr<IDWriteTextFormat> f = fBody;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawText(text, f.Get(), r, c);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
}

void SettingsWindow::DrawCard(const Row& r, D2D1_RECT_F c, bool hover) {
    Pal p = P();
    auto rr = D2D1::RoundedRect(c, 8, 8);
    br->SetColor(hover && (r.type == Row::Toggle) ? p.cardHover : p.card);
    rt->FillRoundedRectangle(rr, br.Get());
    br->SetColor(p.cardStroke);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 0.5f, c.top + 0.5f, c.right - 0.5f, c.bottom - 0.5f), 7.5f, 7.5f),
                             br.Get(), 1.0f);
}

void SettingsWindow::Paint() {
    if (!Resources()) return;
    Pal p = P();
    D2D1_SIZE_F sz = rt->GetSize();
    LayoutRows(sz.width);
    viewH = sz.height;
    scroll = std::clamp(scroll, 0.0f, std::max(0.0f, contentH - viewH));

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));   // Mica shows through

    // ---- navigation pane
    {
        D2D1_RECT_F logo = D2D1::RectF(20, 16, 36, 32);
        br->SetColor(p.accent);
        for (int yy = 0; yy < 2; ++yy)
            for (int xx = 0; xx < 2; ++xx) {
                float l = logo.left + xx * 8.5f, t = logo.top + yy * 8.5f;
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, l + 7, t + 7), 1.5f, 1.5f), br.Get());
            }
        DrawText(L"Skyline", fApp.Get(), D2D1::RectF(46, 12, kNavW, 36), p.text);

        // the search field
        {
            D2D1_RECT_F f = D2D1::RectF(12, kSearchTop, kNavW - 12, kSearchTop + kSearchH);
            br->SetColor(p.ctl);
            rt->FillRoundedRectangle(D2D1::RoundedRect(f, 4, 4), br.Get());
            br->SetColor(p.ctlStroke);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(f.left + 0.5f, f.top + 0.5f, f.right - 0.5f, f.bottom - 0.5f), 4, 4), br.Get(), 1);
            if (!query_.empty()) {   // the accent line under the field, like WinUI's focused text box
                br->SetColor(p.accent);
                rt->FillRectangle(D2D1::RectF(f.left + 1, f.bottom - 2, f.right - 1, f.bottom), br.Get());
            }
            wchar_t g = 0xE721;
            br->SetColor(p.text2);
            rt->DrawTextW(&g, 1, fIconSmall.Get(), D2D1::RectF(f.right - 30, f.top, f.right - 8, f.bottom), br.Get());
            if (query_.empty()) DrawText(TR(L"Søk i innstillinger", L"Search settings"), fBody.Get(), D2D1::RectF(f.left + 10, f.top, f.right - 34, f.bottom), p.text3);
            else {
                DrawText(query_, fBody.Get(), D2D1::RectF(f.left + 10, f.top, f.right - 34, f.bottom), p.text);
                float cx = f.left + 10 + std::min(TextWidth(query_, fBody.Get()), f.right - f.left - 46) + 1;
                br->SetColor(p.text);
                rt->FillRectangle(D2D1::RectF(cx, f.top + 8, cx + 1, f.bottom - 8), br.Get());
            }
        }

        float y = kNavTop;
        for (int i = 0; i < (int)(sizeof kNav / sizeof kNav[0]); ++i) {
            D2D1_RECT_F r = D2D1::RectF(8, y, kNavW - 8, y + 36);
            bool sel = query_.empty() && kNav[i].page == page;
            if (sel || i == hoverNav) {
                br->SetColor(sel ? p.sel : p.ctl);
                rt->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br.Get());
            }
            if (sel) {
                br->SetColor(p.accent);
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 10, r.left + 3, r.bottom - 10), 1.5f, 1.5f),
                                         br.Get());
            }
            wchar_t g = kNav[i].glyph;
            br->SetColor(p.text);
            rt->DrawTextW(&g, 1, fIcon.Get(), D2D1::RectF(r.left + 12, r.top, r.left + 36, r.bottom), br.Get());
            DrawText(kNav[i].label(), fBody.Get(), D2D1::RectF(r.left + 48, r.top, r.right - 8, r.bottom), p.text);
            y += 40;
        }
    }

    // ---- content
    rt->PushAxisAlignedClip(D2D1::RectF(kNavW, 0, sz.width, sz.height), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -scroll));

    const wchar_t* title = L"";
    for (auto& n : kNav) if (n.page == page) title = n.label();
    if (!query_.empty()) title = TR(L"Søk", L"Search");
    DrawText(title, fTitle.Get(), D2D1::RectF(kNavW + 8, kTitleTop, sz.width - kContentPad, kTitleTop + 44), p.text);

    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.card.bottom - scroll < 0 || r.card.top - scroll > sz.height) continue;
        if (r.type == Row::Header) {
            if (!r.title.empty()) DrawText(r.title, fSection.Get(), D2D1::RectF(r.card.left + 2, r.card.top + 12, r.card.right, r.card.bottom), p.text);
            continue;
        }
        bool hover = i == hoverRow;
        DrawCard(r, r.card, hover);
        float textRight = r.textRight;
        if (r.desc.empty()) {
            DrawText(r.title, fBody.Get(), D2D1::RectF(r.card.left + 16, r.card.top, textRight, r.card.bottom), p.text);
        } else {
            float top = r.card.top + 13;   // 66 high card: title 13..33, desc from 33; taller cards keep the title on top
            DrawText(r.title, fBody.Get(), D2D1::RectF(r.card.left + 16, top, textRight, top + 20), p.text);
            DrawText(r.desc, fCaption.Get(), D2D1::RectF(r.card.left + 16, top + 20, textRight, top + 20 + r.descH), p.text2);
        }
        switch (r.type) {
        case Row::Toggle: DrawToggle(r.ctl, r.getB(), hover, r.enabled); break;
        case Row::Slider: DrawSlider(r, r.ctl, dragRow == i ? dragValue : r.getI(), hover); break;
        case Row::Choice: {
            int v = std::clamp(r.getI(), 0, (int)r.choices.size() - 1);
            DrawButtonish(r.ctl, r.choices.empty() ? L"" : r.choices[v], false, hover, true, r.enabled);
            break;
        }
        case Row::Button: DrawButtonish(r.ctl, r.buttonText, r.accent, hover, false, true); break;
        default: break;
        }
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->PopAxisAlignedClip();
    DrawTooltip(sz);

    if (FAILED(rt->EndDraw())) Discard();
}

void SettingsWindow::DrawTooltip(D2D1_SIZE_F sz) {
    if (mouse_.x < 0 || dragRow >= 0) return;
    std::wstring tip;
    if (hoverNav >= 0 && hoverNav < (int)(sizeof kNav / sizeof kNav[0])) {
        const wchar_t* l = kNav[hoverNav].label();
        if (TextWidth(l, fBody.Get()) > kNavW - 8 - 8 - 48) tip = l;
    } else if (hoverRow >= 0 && hoverRow < (int)rows.size()) {
        const Row& r = rows[hoverRow];
        float y = mouse_.y + scroll;
        bool onCtl = r.ctl.right > r.ctl.left && mouse_.x >= r.ctl.left && mouse_.x <= r.ctl.right && y >= r.ctl.top && y <= r.ctl.bottom;
        if (onCtl) {
            std::wstring t;
            if (r.type == Row::Choice && !r.choices.empty()) t = r.choices[std::clamp(r.getI(), 0, (int)r.choices.size() - 1)];
            else if (r.type == Row::Button) t = r.buttonText;
            float avail = r.ctl.right - r.ctl.left - (r.type == Row::Choice ? 41 : 0);
            if (!t.empty() && TextWidth(t, fBody.Get()) > avail) tip = t;
        } else if (TextWidth(r.title, fBody.Get()) > r.textRight - (r.card.left + 16)) {
            tip = r.title;
            if (!r.desc.empty()) tip += L"\n" + r.desc;
        }
    }
    if (tip.empty()) return;
    Pal p = P();
    const float maxW = 360, pad = 8;
    float w = std::min(maxW, TextWidth(tip, fBody.Get()) + 2);
    float h = TextHeight(tip, fBody.Get(), w);
    float x = mouse_.x + 14, y = mouse_.y + 20;
    if (x + w + 2 * pad > sz.width - 4) x = sz.width - 4 - w - 2 * pad;
    if (y + h + 2 * pad > sz.height - 4) y = mouse_.y - 8 - h - 2 * pad;
    x = std::max(x, 4.0f); y = std::max(y, 4.0f);
    D2D1_RECT_F box = D2D1::RectF(x, y, x + w + 2 * pad, y + h + 2 * pad);
    br->SetColor(p.card);
    rt->FillRoundedRectangle(D2D1::RoundedRect(box, 4, 4), br.Get());
    br->SetColor(p.ctlStroke);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), 4, 4), br.Get(), 1);
    DrawText(tip, fBody.Get(), D2D1::RectF(box.left + pad, box.top + pad, box.right - pad, box.bottom - pad + 2), p.text);
}

// ---------------------------------------------------------------------------
int SettingsWindow::HitNav(D2D1_POINT_2F p) const {
    if (p.x < 8 || p.x > kNavW - 8 || p.y < kNavTop) return -1;
    int i = (int)((p.y - kNavTop) / 40);
    if (i >= (int)(sizeof kNav / sizeof kNav[0]) || p.y > kNavTop + i * 40 + 36) return -1;
    return i;
}

int SettingsWindow::HitRow(D2D1_POINT_2F p, bool controlOnly) const {
    if (p.x < kNavW) return -1;
    float y = p.y + scroll;
    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row& r = rows[i];
        if (r.type == Row::Header || r.type == Row::Info || !r.enabled) continue;   // greyed-out rows don't react
        const D2D1_RECT_F& b = controlOnly ? r.ctl : r.card;
        if (p.x >= b.left && p.x <= b.right && y >= b.top && y <= b.bottom) return i;
    }
    return -1;
}

void SettingsWindow::SetSliderFromX(int i, float x) {
    Row& r = rows[i];
    float frac = std::clamp((x - (r.ctl.left + 10)) / (r.ctl.right - r.ctl.left - 20), 0.0f, 1.0f);
    int v = r.min + (int)std::lround(frac * (r.max - r.min) / r.step) * r.step;
    dragValue = std::clamp(v, r.min, r.max);
    Invalidate();
}

// A little menu of checkboxes: click displays on and off, then Done
bool SettingsWindow::PickDockScreens(int count) {
    Settings& s = g_app.settings;
    std::vector<int> pick = s.dockScreens;
    if (pick.empty()) pick.push_back(0);
    POINT pt;
    GetCursorPos(&pt);
    for (;;) {
        HMENU m = CreatePopupMenu();
        auto mons = MonitorsLeftToRight();
        HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        for (int i = 0; i < std::max(count, (int)mons.size()); ++i) {
            wchar_t t[96];
            if (i < (int)mons.size()) {
                MONITORINFO mi{sizeof mi};
                GetMonitorInfoW(mons[i], &mi);
                swprintf(t, 96, TR(L"Skjerm %d  ·  %d×%d%s", L"Display %d  ·  %d×%d%s"), i + 1, (int)(mi.rcMonitor.right - mi.rcMonitor.left),
                         (int)(mi.rcMonitor.bottom - mi.rcMonitor.top), mons[i] == primary ? TR(L"  (hoved)", L"  (main)") : L"");
            } else {
                swprintf(t, 96, TR(L"Skjerm %d  (ikke tilkoblet)", L"Display %d  (not connected)"), i + 1);
            }
            bool on = std::find(pick.begin(), pick.end(), i) != pick.end();
            AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), 100 + i, t);
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (pick.empty() ? MF_GRAYED : 0), 1, TR(L"Ferdig", L"Done"));
        int cmd;
        {
            MenuGuard guard;
            cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, hwnd, nullptr);
        }
        DestroyMenu(m);
        if (cmd == 0) return false;   // clicked outside / Esc: nothing changes
        if (cmd == 1) break;
        int i = cmd - 100;
        auto it = std::find(pick.begin(), pick.end(), i);
        if (it != pick.end()) pick.erase(it);
        else pick.push_back(i);
    }
    std::sort(pick.begin(), pick.end());
    s.dockScreens = pick;
    std::wstring v;
    for (int i : pick) v += (v.empty() ? L"" : L",") + std::to_wstring(i + 1);
    IniWrite(L"Dock", L"Screens", v);
    s.dockMonitor = kDockChosen;
    IniWrite(L"Dock", L"Monitor", kDockChosen);
    return true;
}

void SettingsWindow::ShowChoice(int i) {
    Row r = rows[i];
    HMENU m = CreatePopupMenu();
    int cur = r.getI();
    for (size_t k = 0; k < r.choices.size(); ++k)
        AppendMenuW(m, MF_STRING | ((int)k == cur ? MF_CHECKED : 0), k + 1, r.choices[k].c_str());
    RECT px{(LONG)(r.ctl.left * dpi / 96), (LONG)((r.ctl.bottom - scroll) * dpi / 96), 0, 0};
    POINT pt{px.left, px.top + 2};
    ClientToScreen(hwnd, &pt);
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y, hwnd, nullptr);
    DestroyMenu(m);
    if (cmd > 0 && cmd - 1 != cur) {
        r.setI(cmd - 1);
        Rebuild();
    }
}

void SettingsWindow::Click(int i, D2D1_POINT_2F p) {
    if (i < 0 || i >= (int)rows.size()) return;
    Row r = rows[i];   // copy: callbacks may rebuild rows
    switch (r.type) {
    case Row::Toggle: r.setB(!r.getB()); Rebuild(); break;
    case Row::Choice: if (HitRow(p, true) == i) ShowChoice(i); break;
    case Row::Button: if (HitRow(p, true) == i && r.onClick) { auto fn = r.onClick; fn(); } break;   // a copy: the rows may be rebuilt while a dialog is open
    default: break;
    }
}

void SettingsWindow::ScheduleGlass() { SetTimer(hwnd, T_APPLY + 2, 60, nullptr); }

void SettingsWindow::ScheduleApply() {
    // Changes that rebuild the bar/dock are batched so dragging a slider
    // or flipping several switches doesn't make everything flash.
    SetTimer(hwnd, T_APPLY, 350, nullptr);
}

// ---------------------------------------------------------------------------
void SettingsWindow::Open(SettingsPage p) {
    page = p;
    scroll = 0;
    query_.clear();
    if (hwnd) {
        BuildRows();
        Invalidate();
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_app.hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(g_app.hinst, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int w = MulDiv(920, dx, 96), h = MulDiv(660, dx, 96);
    int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;

    hwnd = CreateWindowExW(0, kClass, TR(L"Skyline – Innstillinger", L"Skyline – Settings"), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
                           g_app.hinst, this);
    if (!hwnd) return;
    dpi = GetDpiForWindow(hwnd);

    BOOL dark = g_app.theme.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    DWORD mica = 2; // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &mica, sizeof mica);
    MARGINS mg{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &mg);

    BuildRows();
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = h;
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT SettingsWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (rt) rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        Invalidate();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {MulDiv(700, dpi, 96), MulDiv(560, dpi, 96)};   // room for all ten pages in the navigation
        return 0;
    }
    case WM_DPICHANGED: {
        dpi = HIWORD(wp);
        auto* r = reinterpret_cast<RECT*>(lp);
        Discard();
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet")) {
            BOOL dark = LoadTheme().dark;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof dark);
            Invalidate();
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            tracking = true;
        }
        D2D1_POINT_2F p = Dip(lp);
        if (dragRow >= 0) { SetSliderFromX(dragRow, p.x); return 0; }
        int r = HitRow(p, false), n = HitNav(p);
        mouse_ = p;
        if (r != hoverRow || n != hoverNav) { hoverRow = r; hoverNav = n; Invalidate(); }
        else if (r >= 0 || n >= 0) Invalidate();   // the tooltip follows the cursor
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        mouse_ = {-1, -1};
        if (dragRow < 0 && (hoverRow != -1 || hoverNav != -1)) { hoverRow = hoverNav = -1; Invalidate(); }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = Dip(lp);
        int n = HitNav(p);
        if (n >= 0) {
            page = kNav[n].page;
            query_.clear();
            scroll = 0;
            hoverRow = -1;
            BuildRows();
            Invalidate();
            return 0;
        }
        int r = HitRow(p, false);
        if (r >= 0 && rows[r].type == Row::Slider) {
            const auto& c = rows[r].ctl;
            if (p.x >= c.left - 4 && p.x <= c.right + 4) {
                dragRow = r;
                dragValue = rows[r].getI();
                SetCapture(hwnd);
                SetSliderFromX(r, p.x);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        D2D1_POINT_2F p = Dip(lp);
        if (dragRow >= 0) {
            int r = dragRow, v = dragValue;
            dragRow = -1;
            ReleaseCapture();
            if (r < (int)rows.size() && rows[r].type == Row::Slider) {
                Row row = rows[r];   // a copy: the setter may rebuild the rows
                if (v != row.getI()) row.setI(v);
            }
            Rebuild();
            return 0;
        }
        int r = HitRow(p, false);
        if (r >= 0) Click(r, p);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragRow >= 0 && (HWND)lp != hwnd) { dragRow = -1; Invalidate(); }
        return 0;
    case WM_MOUSEWHEEL: {
        scroll -= GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA * 48;
        scroll = std::clamp(scroll, 0.0f, std::max(0.0f, contentH - viewH));
        Invalidate();
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (capturing_) { CaptureKey((UINT)wp); return 0; }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            if (!query_.empty()) { query_.clear(); scroll = 0; hoverRow = -1; BuildRows(); Invalidate(); }   // Esc clears the search first
            else DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
        if (capturing_ || eatChar_) { eatChar_ = false; return 0; }   // the keys of a hotkey being recorded
        if (msg == WM_CHAR && (wp == 8 || (wp >= 32 && wp != 127))) {   // typing anywhere searches
            if (wp == 8) { if (!query_.empty()) query_.pop_back(); }
            else if (query_.size() < 40) query_ += (wchar_t)wp;
            scroll = 0;
            hoverRow = -1;
            BuildRows();
            Invalidate();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == T_APPLY) {
            KillTimer(hwnd, T_APPLY);
            FullReload();
            SetTimer(hwnd, T_APPLY + 1, 400, nullptr);   // refresh once the reload is done
        } else if (wp == T_APPLY + 1) {
            KillTimer(hwnd, T_APPLY + 1);
            SetWindowTextW(hwnd, TR(L"Skyline – Innstillinger", L"Skyline – Settings"));   // a restore may change the language
            Rebuild();
        } else if (wp == T_APPLY + 2) {
            KillTimer(hwnd, T_APPLY + 2);
            PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 4, 0);   // same path as a light/dark switch
        }
        return 0;
    case WM_DESTROY:
        // closed right after a change: apply it now instead of losing it with the timer
        if (KillTimer(hwnd, T_APPLY)) FullReload();
        if (KillTimer(hwnd, T_APPLY + 2)) PostMessageW(g_app.ctlWnd, WM_SKY_STATUS, 4, 0);
        if (capturing_) {   // closed while recording: keep the old hotkey
            capturing_ = false;
            if (captureWhat_ == 1) WritingTools::ApplyHotkey();
            else StartMenus::ApplySpotlightHotkey();
        }
        Discard();
        rows.clear();
        hwnd = nullptr;
        // Give memory back: the settings window is rarely open
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void SettingsWindow::CaptureKey(UINT vk) {
    switch (vk) {   // a modifier on its own: wait for the real key
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        return;
    }
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    Settings& s = g_app.settings;
    if (vk == VK_ESCAPE && !mods) {   // cancel: keep the old one
        capturing_ = false;
        eatChar_ = true;
        if (captureWhat_ == 1) WritingTools::ApplyHotkey();
        else StartMenus::ApplySpotlightHotkey();
        Rebuild();
        return;
    }
    if (!mods) return;   // a hotkey needs Ctrl, Alt, Shift or Win
    if (captureWhat_ == 1) {   // SkyWritingTools
        // Shift + a key would steal capital letters; Ctrl + C/V/X/A/Z/Y are everyone's (and we send Ctrl+C/V ourselves)
        bool bad = mods == MOD_SHIFT || (mods == MOD_CONTROL && (vk == 'C' || vk == 'V' || vk == 'X' || vk == 'A' || vk == 'Z' || vk == 'Y'));
        if (bad) {
            capturing_ = false;
            eatChar_ = true;
            WritingTools::ApplyHotkey();
            MessageBoxW(hwnd, TR(L"Den kombinasjonen brukes til vanlig skriving og kopiering. Velg en annen, for eksempel Ctrl + Mellomrom eller Ctrl + Alt + W.",
                                 L"That combination is used for normal typing and copying. Pick another, for example Ctrl + Space or Ctrl + Alt + W."),
                        L"Skyline", MB_ICONINFORMATION);
            Rebuild();
            return;
        }
        UINT oldMods = s.writingMods, oldVk = s.writingVk;
        s.writingMods = mods;
        s.writingVk = vk;
        capturing_ = false;
        eatChar_ = true;
        bool clash = mods == s.spotlightMods && vk == s.spotlightVk && s.spotlightEnabled;
        if (!clash && WritingTools::ApplyHotkey()) {
            IniWrite(L"SkyWritingTools", L"Modifiers", (int)mods);
            IniWrite(L"SkyWritingTools", L"Key", (int)vk);
        } else {
            std::wstring taken = StartMenus::HotkeyName(mods, vk);
            s.writingMods = oldMods;
            s.writingVk = oldVk;
            WritingTools::ApplyHotkey();
            std::wstring msg = taken + TR(L" er allerede i bruk (av Windows, Spotlight eller et annet program). Prøv en annen kombinasjon.",
                                          L" is already used (by Windows, Spotlight or another app). Try another combination.");
            MessageBoxW(hwnd, msg.c_str(), L"Skyline", MB_ICONINFORMATION);
        }
        Rebuild();
        return;
    }
    UINT oldMods = s.spotlightMods, oldVk = s.spotlightVk;
    s.spotlightMods = mods;
    s.spotlightVk = vk;
    capturing_ = false;
    eatChar_ = true;
    if (StartMenus::ApplySpotlightHotkey()) {
        IniWrite(L"Spotlight", L"Modifiers", (int)mods);
        IniWrite(L"Spotlight", L"Key", (int)vk);
    } else {
        std::wstring taken = StartMenus::HotkeyName(mods, vk);
        s.spotlightMods = oldMods;
        s.spotlightVk = oldVk;
        StartMenus::ApplySpotlightHotkey();
        std::wstring msg = taken + TR(L" er allerede i bruk av Windows eller et annet program. Prøv en annen kombinasjon.",
                                      L" is already used by Windows or another app. Try another combination.");
        MessageBoxW(hwnd, msg.c_str(), L"Skyline", MB_ICONINFORMATION);
    }
    Rebuild();
}

void ShowSettings(SettingsPage page) { g_win.Open(page); }

void SettingsChanged() {
    if (g_win.hwnd) g_win.Rebuild();
}
