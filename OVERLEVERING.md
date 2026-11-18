# Skyline for Windows 11 – overlevering til ny samtale

> **Til Claude:** Last opp denne fila sammen med `Skyline-11-x.y-backup.zip` (siste versjon) (git-repo med all kildekode).
> Les hele fila før du gjør noe. Svar brukeren på **norsk**. Brukeren er ikke programmerer: bygg ting selv,
> og send ferdige zip-filer (se «Utgivelse»). Forklar kort og enkelt hva som er gjort.

---

## 1. Hva Skyline er

En native macOS-aktig **topplinje + dock** for Windows 11, laget av brukeren («Yen-Zen & Cyber») sammen med Claude.
Ren C++20, Win32 + Direct2D/DirectWrite + DWM. Ingen webview, ingen .NET, ingen installasjon. Én exe på ca. 0,9 MB.
Lisens: **GNU GPL-3.0 eller senere**. Credits i appen: «Made by Yen-Zen & Cyber and a whole bunch of Claude».
Skal legges ut gratis på GitHub. **Siste versjon: 2.42.0** (18. nov 2026; 2.40.5 er siste utgitte).

> **Dette er prosjektet for Windows 11.** Skyline for Windows 10 er et **helt eget prosjekt** med egen kode, egen backup
> (`Skyline-10-x.y-backup.zip`) og egen GitHub-pakke. Brukeren vil ha det slik for å holde det ryddig: ingen delt kode.
> En ny funksjon som skal i begge, lages i hvert prosjekt for seg. Windows 11-utgaven 2.6 er nøyaktig den første 2.6
> (commit a882ec0) – den brukeren har skrevet oppdateringstekst for.

**Funksjoner i 2.3**
- **Topplinje:** appnavn + Mac-aktige appmenyer (`menus.txt` eller speilede Win32-menyer), tray-ikonene (Skyline er tray-vert;
  dra for å sortere, dra ned for å skjule), nett/lyd/batteri, dato/klokke. Stiler: Klassisk, Flytende, Øyer, Separert (tre paneler).
- **Dock:** erstatter oppgavelinja, festede og kjørende apper, levende forhåndsvisninger (DWM-thumbnails), smart skjul, valg av skjerm.
- **Startmenyer:** Windows' egen (standard), **SkyMenu** (to kolonner) og **SkyLauncher** (Launchpad-rutenett). Win-tasten åpner valgt meny.
- **Tillegg (plugins):** Now Playing (Windows' mediesesjoner, i dock og/eller topplinje), SkyGarden (skrivebordsikoner i glassbed, som Fences),
  Klokke i docken (digital eller analog med live sekundviser).
- **Annet:** automatisk tiling (BSP/Master/Kolonner/Monocle), hot corners, innstillingsvindu i Windows 11-stil, engelsk (standard) + norsk.
- **Glass per del** (Innstillinger → Utseende): topplinje, dock, startmeny, forhåndsvisninger, Now Playing-kort og SkyGarden har hver sitt
  materiale + fargetone. **Akryl** = accent state 4 med fargetonen i GradientColor-alfa (samme oppskrift og farger som 2.0: 32/243,
  alfa = tint×2,55), ingen D2D-overlegg. **Gjennomsiktig** = accent state 2, alfa 0, fargetonen males med D2D (`GlassTint(Part)`).
  Blur (3), Mica og Solid er fjernet fordi de ikke virker godt på rammeløse vinduer i Win11.

---

## 2. Brukerens ønsker (viktig)

- Nye funksjoner skal være **brytere**, og standarden skal gi samme oppførsel som før (brukeren liker appen som den er).
- Skal være **lett på ressurser**: hendelsesstyrt, null CPU når ingenting skjer, ingen polling uten grunn.
- Ved feilsøking ønsket brukeren tidligere å si «kjør på» før endringer; nå har brukeren gitt generell «kjør på».
- Brukeren tester på sin egen Windows 11-PC og rapporterer tilbake; Claude kan ikke kjøre appen selv.

---

## 3. Bygging

Kryss-kompilering med **llvm-mingw** på Linux (Windows: `build.bat` med Visual Studio, eller CMake; `build.bat` spør om versjonsnummer og har valg 1 «Bygg vanlig» / 2 «Bygg setup» – setup bruker Inno Setup 6 og `tools/skyline.iss`, installerer per bruker uten admin):
```
PATH=/tmp/llvm-mingw/bin:$PATH ./build.sh      # -> out/Skyline.exe
```
Mangler llvm-mingw: last ned siste `llvm-mingw-*-ucrt-ubuntu-*-x86_64.tar.xz` fra github.com/mstorsjo/llvm-mingw/releases og pakk ut til `/tmp/llvm-mingw`.
`build.sh`, `build.bat` og `CMakeLists.txt` lister **de samme kildefilene**; legg nye filer inn i alle tre.
Libs: d2d1 dwrite windowscodecs dwmapi shcore shlwapi shell32 ole32 oleaut32 uuid comctl32 wlanapi iphlpapi version propsys psapi user32 gdi32 advapi32 secur32 powrprof runtimeobject.

---

## 4. Kildekode (`src/`, ca. 12 700 linjer)

| Fil | Innhold |
|---|---|
| `common.h` | `Settings`-struct, `Part`/`Material`, `TR(no,en)`-makro (`g_norsk`), ComPtr, `MenuGuard`/`g_menuDepth`, hjelpere |
| `app.h` | `App` (g_app): bars, docks, tray, status, tiler, theme, settings; `WM_SKY_STATUS`-koder |
| `main.cpp` | `LoadSettings` (ini + migrering), standard-ini, glass (`ApplyGlassEffect(HWND, Part, cheap)`, `GlassTint`, `TintPercent`), kontrollvindu, foreground-hook, sesjonsslutt-opprydding, `set_terminate` |
| `bar.cpp/.h` | Topplinja: layout per stil, island-panelvinduer, appmeny, tray-tegning/dra, status, klokke, Now Playing i baren |
| `tray.cpp` | Tray-vert (Shell_TrayWnd-erstatning, `TaskbarCreated`, speiling til Explorer) |
| `status.cpp` | Nett, lyd, batteri (`BatteryFlag & 8` = lader) |
| `menus.cpp/.h` | Appmeny-profiler fra `menus.txt` (NO/EN standardfiler), speiling av Win32-menyer, sending av hurtigtaster |
| `dock.cpp/.h` | Docken: ikoner, dra/feste, appbar-reservasjon, smart skjul, Now Playing-flis, **klokke-flis** (`PaintClock`, `T_CLOCK`) |
| `preview.cpp/.h` | Dock-forhåndsvisninger (DWM thumbnails), × for å lukke |
| `startmenu.cpp/.h` | SkyMenu + SkyLauncher, lavnivå-tastaturhook for Win-tasten på egen tråd, DwmFlush-animasjoner, søk |
| `applist.cpp/.h` | Liste over installerte apper (AppsFolder) + ikonlaster på bakgrunnstråd (resultat via `WM_SKY_STATUS` 21) |
| `nowplaying.cpp/.h` | Windows.Media.Control via håndskrevet WinRT-ABI, kortvindu |
| `garden.cpp/.h` | SkyGarden: bed-vinduer like over skrivebordet, egen flytt/endre størrelse, `garden.ini` |
| `tiling.cpp/.h` | Tiling; LOCATIONCHANGE hookes **per prosess** (`SyncLocationHooks`), ikke globalt |
| `hotcorners.cpp/.h` | Hot corners (7 handlinger, 6 = SkyLauncher) |
| `settings.cpp/.h` | Innstillingsvindu (egen D2D-UI): sider Generelt, Topplinje, Tray-ikoner, Dock, Utseende, Tiling, Hot corners, Startmeny, Tillegg, Om |
| `icons.cpp/.h` | Ikonhenting/skalering (WIC) |
| `skyline.rc` | Versjonsinfo (FILEVERSION 2,3,0,0), ikon, manifest |

**Viktige mønstre**
- **Innstillinger:** `skyline.ini` ved siden av exe. `IniWrite(section, key, value)`. Endring som krever ombygging → `ScheduleApply()`
  (full reload via `WM_SKY_STATUS` 2). Bare glass/tint → `ScheduleGlass()` (status 4 = `ReloadTheme`, ingen ombygging).
- **Rader i innstillinger:** `header`, `toggle`, `choice`, `slider`, `button`, `info`; `rows.back().enabled = false` for å gråe ut.
- **Språk:** all tekst via `TR(L"norsk", L"english")`.
- **Status-koder (`WM_SKY_STATUS`):** 0 endring, 1 lydenhet, 2 last innstillinger, 3 skjermer endret, 4 tema/glass, 5 fullskjerm,
  10–13 hot corner, 20 åpne startmeny, 21 ikon lastet. Status 2/3 utsettes mens en meny er åpen (`g_menuDepth`).
- **D2D:** ved `EndDraw`-feil kalles `DiscardResources()` (også bitmaps som album-cover!) og prøv igjen inntil 3 ganger.
- **Rebuild:** bars/docks flyttes ut av vektoren før de ødelegges (unngår krasj ved innstillingsendring).

---

## 5. Utgivelse (hver versjon)

1. Bump versjon i `src/skyline.rc` (FILEVERSION/PRODUCTVERSION + strenger) og `header(L"Skyline x.y")` i Om-siden i `settings.cpp`.
2. Bygg. Oppdater dokumentasjon: `LES-MEG.md` (norsk, full), venne-mappa sin `LES MEG.txt` (norsk) og `README.txt` (engelsk)
   (UTF-8 med BOM, CRLF), og GitHub `README.md` (engelsk).
3. Lag tre zip-filer og send dem til brukeren:
   - **`Skyline-x.y-Windows-11.zip`** (til venner): mappa `Skyline/` med `Skyline.exe`, `LES MEG.txt`, `README.txt`, `LICENSE.txt`.
   - **`Skyline-11-x.y-backup.zip`**: git-repoet `Skyline-backup/` (src/, build-filer, `ferdig/` med exe + docs).
     Commit som `Yen-Zen & Cyber <skyline@local>`, melding «Skyline x.y: …» på norsk.
   - **`Skyline 11 GitHub.zip`**: `README.md` (engelsk), `LICENSE` (ren GPL-3-tekst), `.gitignore`, `.gitattributes`, `docs/` (LES-MEG.md, screenshots/), `src/`, build-filer.

---

## 5b. Gjort i 2.4 og 2.5 (3. okt 2026)

- **2.4:** Backup/gjenoppretting/fabrikkinnstillinger (`backup.cpp`, `.skyline`-fil, auto-backup i `Backups/`, maks 10).
  Start på nytt / Slå av i SkyLauncher (klikk to ganger). Topplinje og dock vises mens en startmeny er åpen, også over spill
  (`StartMenuOpen()`, `OurStartMenuShown()`, `Bar/Dock::StartMenuChanged`). Tiling-fiks: `KeepBarsOnTop()` etter retile og ved
  forgrunnsbytte; vinduer tileren har plassert regnes aldri som fullskjerm (`Tiler::Manages`).
- **2.5:** SkyMenu: dra festede apper for å sortere. SkyLauncher skrevet om: sider (`[SkyLauncher] Layout`, `<page>`-skille),
  mapper (`<folder>Navn*key*key`), dra til kanten for neste/ny side, dra app til docken for å feste (`DragGhost`-vindu over docken).
  SkyGarden: høyreklikk på tomt skrivebord gir «Nytt bed her» + Windows' egen skrivebordsmeny (lavnivå-musehook på egen tråd,
  bare mens SkyGarden er på og bryteren `[Garden] DesktopMenu` er på; Shift+høyreklikk = Windows' meny).
- **2.6:** SkyGarden: marker flere (rammemarkering, Ctrl/Shift + klikk, Ctrl+A), dra flere samlet, Windows-meny for flere
  (samme mappe), Delete = papirkurv (`IFileOperation`), Enter åpner, Esc fjerner. Bedet tar fokus ved klikk for å få tastene.
- Bygg: llvm-mingw 20260922 krever `<cmath>`/`<cstdlib>` (lagt i `common.h`). GitHub-filene ligger i `github/`.

## 6. Hva vi skal gjøre videre (i denne rekkefølgen)

Automatiske oppdateringer lages av en kompis av brukeren, så **ikke** lag det.
Alt nytt skal være lett: ingen kostnad før det brukes, hendelsesstyrt.

1. ~~**Backup, gjenoppretting og fabrikkinnstillinger**~~ (ferdig i 2.4). Innstillinger → Generelt.
   Én fil (f.eks. `.skyline`) med `skyline.ini`, `menus.txt`, `garden.ini` (festede apper ligger i ini).
   Fabrikkinnstillinger tar automatisk backup først, slik at det kan angres. Gjenopprett → full reload.
2. ~~**Spotlight**~~ (ferdig i 2.7, som **tillegg** etter brukerens ønske, av som standard): `Spotlight`-klassen i `startmenu.cpp`,
   `RegisterHotKey` (id `kSpotlightHotkey`, standard Alt+Mellomrom, `[Spotlight] Modifiers/Key`), hurtigtast tas opp i innstillingene
   (`CaptureKey`), alltid TOPMOST, `Part::Spotlight`. Filsøk i `filesearch.cpp`: OLE DB `Search.CollatorDSO` via `IDataInitialize`,
   `CONTAINS(System.FileName,'"ord*"')` i brukermappa, bakgrunnstråd som avslutter seg etter 60 s uten søk, svar som `WM_SKY_FILES`
   (lParam = `std::vector<AppEntry>*`, mottaker sletter). SkyMenu søker i filer (`[Start] MenuFiles`, på), SkyLauncher valgfritt
   (`[Start] LauncherFiles`, av). `AppEntry.file/folder`.
   Mappe-fiks i 2.7: over en annen flis venter SkyLauncher 300 ms (`T_REORDER`, `pendingSlot_`) før den lager plass, så man rekker
   midten av ikonet (= lag mappe). Før flyttet flisene seg unna med en gang, så mapper var umulige å lage.
2b. ~~**VibeMenu**~~ (ferdig i 2.8: `Vibe`-klassen + `VibeUsage` + `Calc` + `RecentFiles` i `startmenu.cpp`, `Part::Vibe`,
   `[Start] Menu=3`, bruk i `vibe.ini` (6 fire-timers-spor per exe, lagres ved åpning, avslutning og WM_ENDSESSION), med i backup).
   **2.9: VibeMenu gjort mye livligere** (brukeren syntes 2.8 var «kjedelig»): aurora-bakgrunn (tre radiale gradienter i
   tid-på-døgnet-farger + aksent, `T_AURORA` 30 fps bare mens åpen), stor klokke, mini-spiller (NowPlaying) eller dagsring, søkefelt med
   gradientkant, treff med uthevet match (`DrawMatch`) og fargede piller, «Nå»-fliser i ikonets farge (`IconColour`) med inngang
   én etter én og løft ved hover, nylige filer med «for X siden», avatar + navn nederst.
   **2.10:** nylige filer fikset (stier med `%USERPROFILE%` → `ExpandEnvironmentStrings`, .exe holdes utenfor), «Fjern fra VibeMenu»
   (`[HiddenApps]`/`[HiddenFiles]` i vibe.ini; app nullstilles og kommer tilbake ved bruk, fil kommer tilbake når den åpnes på nytt),
   fullskjerm-valg (`[Vibe] Fullscreen`, samme kort skalert og sentrert, `ToCard()` for musen), alle fire av/på-knapper krever to klikk,
   SkyMenu sin Av/på-meny spør (MessageBox) før logg av/start på nytt/slå av.
   **2.11:** materialet **«Vibe»** (`MatVibe`, ini-verdi `vibe`) for topplinje og dock (brukerens navn på stilen): akryl + `PaintVibe()`
   (tre stille radiale gradienter fra `VibePalette()`, delt med VibeMenu). Ingen animasjon i bar/dock (alltid synlige).
   Tiling-fiks: `Tiler::WorkArea` trekker alltid fra `Bar::ReservedBottom()` (klassisk topplinje: tittellinjer havnet under baren).
   Gjaldt også maksimerte vinduer: arbeidsområdet holdt ikke av stripen til baren. `Bar::EnsureWorkArea()` sjekker etter hver
   flytting og hver `SPI_SETWORKAREA`, og setter arbeidsområdet selv om nødvendig (`ReleaseWorkArea()` gir det tilbake).
   **2.12:** fullskjerm-ikoner skarpe (`iconScale_`/`Px()` i Vibe henter ikoner i skalert størrelse; `Surface::Icon`-cachen
   nøkles nå på `key|px`, så samme app i ulike størrelser ikke deler bitmap). Nylige filer: `RecentTrackingOff()` sjekker
   `Start_TrackDocs`=0 og policy `NoRecentDocsHistory` → klikkbar rad (`KTrackDocs`) til `ms-settings:personalization-start`;
   `GetIDList`-reserve når `GetPath` er tom. Duplikater i «Nå»: `AppForExe()` mapper kjørende exe til appen (eksakt exe →
   filbeskrivelse = appnavn → app hvis mappe exe ligger *inni*, ikke Windows-mappen eller `steamapps`), dedupe på key/exe/navn.
   «Alle apper» i VibeMenu (`allMode_`, `KAll`/`KAllBtn`, rulling med hjulet). Dock: `SameAppExe()` (applist) kobler kjørende
   vinduer til festet app ved ny Squirrel-versjon (`app-x.y.z\`), hjelpe-exe i appens mappe, eller AppsFolder-festing via snarveiens exe.
   SkyLauncher: høyreklikk app/tomt sted → «Lag mappe»; mapper beholdes uansett antall apper, oppløses bare når du har dratt apper ut (`draggedOut_`).
   **2.13:** «Fortsett der du slapp» viser bare rader som får plass (kortet klippet før den tredje), og pillen «Vis flere ›»
   (`KFilesBtn`) åpner `filesMode_` med `RecentFiles(40)` (`KMoreFile`, rulling, fjern via høyreklikk). Dock: web-apper (PWA, AUMID
   med `_crx_`, `IsWebAppId`) matches bare på egen AUMID, aldri på nettleserens exe; `Ident::windowId` lar vinduets AUMID komme
   sent (nettleseren setter den etter at vinduet lages), og nye nettleservinduer uten id sjekkes på nytt inntil 4 ganger (700 ms).
   **2.14:** tillegget **Native Flyouts** (`flyouts.cpp/.h`, `[Flyouts] Enabled=0` som standard, `Part::Flyout` med eget glass,
   `[Flyouts] Material/Tint`). Topplinja: klikk på nett-/lydikonet (delt opp innenfor `Kind::Status`: nett 0–24, lyd 24–48 DIP) →
   `Flyouts::Toggle`; batteri = Win+A som før. Lyd: `StatusMonitor::SetVolume`, enheter via `IMMDeviceEnumerator`, standardenhet via
   udokumentert `IPolicyConfig` (faller tilbake til ms-settings:sound), per app via `IAudioSessionManager2` (gruppert per exe, maks 6).
   Nett: egen WLAN-handle, `WlanScan` ved åpning, liste fra `WlanGetAvailableNetworkList` (ERROR_ACCESS_DENIED = posisjon av → knapp),
   tilkobling med profil, åpen/PSK/SAE via `WlanSetProfile`-XML (profil slettes ved tidsavbrudd 15 s), enterprise → ms-settings.
   Radioer: `Windows.Devices.Radios` håndskrevet ABI på kort tråd (`RadioJob`, `WM_FLY_RADIO`). Bluetooth-enheter: `bthprops.cpl`
   lastes dynamisk (ingen ny lenking). Oppdateres via `Flyouts::StatusChanged()` fra main (debounce-timer og lydenhetsbytte).
   Now Playing: topplinja i Øyer/Separert legger kortet under øya med kantene på linje (`align` −1/0/1); docken bruker
   `ShowDrawer()`: stripe like høy som docken som glir ut bak dockens side (z-ordnet rett under docken, 190 ms ease-out, glir inn igjen).
   **2.15:** fullskjerm-fiks (Schedule I, Unity): `CoversMonitor` godtar nå et *maksimert* vindu uten tittellinje (`WS_CAPTION`)
   som fullskjerm, og 1 px slingringsmonn. Etter skjermendring (case 3, også oppløsningsbytte i spill) kjøres `CheckFullscreen` straks
   og igjen etter 300 ms, så nye barer ikke legger seg over spillet. `Bar::SetFullscreen` skjuler også øy-panelene (glasset i Øyer/Separert).
   **2.16:** tiling + nettleser (Vivaldi/Brave) alene: vinduet åpnet maksimert uten tittellinje og ble tatt for fullskjerm (topplinja
   skjult) før tileren ga det en plass. `Tiler::Retile()` kaller nå `RecheckFullscreen()` (ny i main.cpp/common.h, 150 ms) etter plassering.
   SkyLauncher: mapper med flere apper enn det er plass til ruller med musehjulet (`folderScroll_`, `FolderMaxScroll()`, klipp under tittelen).
   **2.17:** VibeMenu fullskjerm er nå et eget oppsett (`PaintFull`), ikke et skalert kort: `Paint()` deler i `PaintCard`/`PaintFull`,
   panelene tegnes med `at(x,y)`-transform og treffene flyttes etterpå (`shift`); `s_`=1 alltid, `iconScale_`=1. Søkefeltet er `PaintSearch()`
   med eget treff (`KSearchBox`), så klikk på tomt sted lukker. Faner «Nå | Festet» (`KTab`, `tab_` lagres i `[Vibe] Tab`), `pins_` = `MenuPins()`
   (delt med SkyMenu), kontekstmenyen har `vibe`-flagg for teksten. `PaintHome` = `PaintTiles(W,y,cols,allPill)` + `PaintRecent`.
   Nylige filer: 10 stk, egen rulling (`recentScroll_`, `recentCol_`). Alle apper har egen rulling (`allScroll_`). Dagsringen viser natta 22–06.
   **2.18:** fullskjerm-regel endret: et vindu er bare «fullskjerm» når det dekker skjermen **uten** `WS_CAPTION` (eller er eksklusiv D3D
   ifølge `SHQueryUserNotificationState` og ikke maksimert). Vivaldi/Brave kunne starte nøyaktig skjermstore med tittelstil → topplinje og dock
   forsvant, og tileren nektet å tile dem (`Manageable` har samme regel nå). VibeMenu fullskjerm ryddet: `PaintRecentCards` (rad med 3–4 kort)
   og `PaintAllGrid` (ikonrutenett 112×96, ruller med `allScroll_`); «Vis flere» vises i én kolonne på 760 DIP.
   **2.19:** søk i innstillingene (`query_`, `BuildRows` bygger alle sider via `BuildPage` og filtrerer; skriving i vinduet går til søket).
   Kalender (`Flyouts::Calendar`, `[Clock] Calendar=0`, også hot corner-handling 7; åpner oppover når ankeret er i nedre halvdel).
   Dobbeltklikk på tom topplinje (`[Bar] DoubleClick=0`, bare stil 0) → `WM_SYSCOMMAND` SC_MAXIMIZE/SC_RESTORE.
   Now Playing i topplinja: `npBarPos` 2 = midten (egen øy), 3 = etter appmenyene. Dock-tillegg: papirkurv (`[Dock] Trash`,
   `SHChangeNotifyRegister` → WM_APP+47, `DockDrop` IDropTarget → WM_APP+48 → `RecycleFiles` med `FOF_WANTNUKEWARNING`; main bruker
   nå `OleInitialize`) og stack (`[Dock] Stack`, `StackFolder`, `Flyouts::Stack`, 60 nyeste filer, docken holder seg oppe mens den er åpen).
   Varsler: Windows' egne varsler kan ikke leses av en app uten pakke-identitet (UserNotificationListener krever MSIX), så vi har ikke
   egen varselsentral; kalenderen fra hot corner + «Varsler»-lenke er løsningen så langt.
   **2.20:** `look.cpp`: temaer (`Look::ApplyTheme`, 0 Standard, 1 Liquid Glass, 2 Vibe, 3 Frostet, 4 Matt) skriver Material/Tint for alle
   9 deler + `[Look] LiquidGlass/Glassiness/Theme`; forrige utseende i `[LookUndo]` (k0..k20, `\x01` = nøkkelen fantes ikke). `PaintLiquid()`
   (main.cpp) tegner skjær + gradientkant på bar (øyer/flytende), dock, flyouts, NP-kort, forhåndsvisninger, SkyMenu, Spotlight, VibeMenu-kort.
   Ytelsesmodus (`[Advanced] Performance`, ny side Avansert): kopierer skyline.ini til `skyline.ini.performance`, skriver enkle verdier,
   setter `EnableTransparency`=0 (HKCU Personalize, kringkaster `ImmersiveColorSet`); av: kopierer tilbake og setter forrige verdi.
   **2.21:** startmeny 4 = Apple-Ish (`AppleIsh` i `launchers.inc`, `[AppleIsh] List/Classic`; Classic åpner SkyLauncher), (5 = GnomeLauncher
   ble fjernet i 2.22 etter ønske fra brukeren; gammel verdi 5 leses som 0). Mission Control-tillegg
   (`[MissionControl] Enabled/WinTab`): `mission.cpp` overlay med DWM-miniatyrer, animasjon via WM_TIMER, Win+Tab fanges i den lavnivå
   tastaturkroken (`g_winTabWanted`, `g_tabSwallowed`, maske-tast så Start ikke åpnes) → WM_SKY_STATUS 23; hot corner 1 → MC når på.
   Startknapp i docken: `[Start] Button` 0 Windows / 1 Skyline (tegnes som vektor i dock.cpp, samme design som skyline.ico).
   **2.22:** tema 5 = **Nordlys** (Claudes eget): `[Look] Aurora/AuroraStrength/AuroraFjord`. `AuroraOn()`, `FjordNow()` (kl. 8–17),
   `AuroraColors()`, `PaintStars()` i main.cpp; `PaintLiquid(rt, r, radius, role, seed)` tegner nordlyset (role 1 bar = bånd, 2 dock = kant,
   4 SkyGarden med seed per bed). Nattblått glass i `ApplyGlassEffect`/`GlassColor`. Skjær bak Start i dock.cpp, stjerner i VibeMenu-fullskjerm.
   Ytelsesmodus setter Aurora=0. Mission Control tar med minimerte vinduer (`Item::minimized/full`: hele miniatyren uten rcSource, stiger
   fra docken, `SW_RESTORE` ved valg). GnomeLauncher fjernet. Materialet `vibe` gjelder nå alle deler unntatt VibeMenu (`PaintVibe(rt, r, part, opacity)`:
   stripe for bar/dock, hjørner for paneler); Vibe-temaet setter det på alle. Skyline-startikonet 0.92 av ikonstørrelsen (var 0.84).
   **2.23:** ringer/streker (banding) i store flater med Nordlys/Vibe: slørene har 5 gradient-stopp med myk utfasing, og
   `PaintGrain()` la et korn over panelene. **2.24:** kornet fjernet igjen (så forferdelig ut over akrylglasset); bare den myke
   utfasingen er igjen. Ikke prøv synlig korn over DWM-glass på nytt.
   **2.25:** store paneler (meny, kort, bed, launchere) tegner Vibe/Nordlys-slørene med `PaintWash()` i main.cpp: regnet ut i
   float på CPU med smoothstep-utfasing og dither under ett trinn (samme verdi på alle fire kanaler, fast per piksel), lagret som
   ID2D1Bitmap (cache 12 stk per render target + utseende-nøkkel, maks ~300k piksler, strekkes lineært). Bar/dock bruker fortsatt D2D-gradienter.
   **2.26:** Apple-Ish fjernet etter ønske fra brukeren (`launchers.inc` slettet). Gammel `[Start] Menu=4` leses som 2 (SkyLauncher).
   Startmenyer nå: 0 Windows, 1 SkyMenu, 2 SkyLauncher, 3 VibeMenu.
   **2.27:** `systheme.cpp` – «Tema på hele systemet» (Avansert → Eksperimentelt, `[Advanced] SystemTheme/SystemThemeBorders/SystemThemeAccent`).
   WinEvent-kroker (OBJECT_SHOW, SYSTEM_FOREGROUND, out-of-context, ingen injeksjon) setter DWMWA_BORDER/CAPTION/TEXT_COLOR på andres vinduer;
   aktivt vindu = temafarge, andre dempet. Aksent (kun Vibe/Nordlys): skriver AccentPalette + AccentColorMenu/StartColorMenu + DWM
   AccentColor/ColorizationColor, kringkaster ImmersiveColorSet bare hvis verdien endres (ellers løkke). Brukerens originaler i
   `[SystemThemeUndo]` (Palette skrives sist = «alt lagret»); `LoadTheme` leser originalpaletten derfra så Skylines egen aksent ikke driver.
   Timer ved hver hel time (Vibe/Fjord). Av, ytelsesmodus eller avslutning → alt tilbake. Windhawk-tillegg for innhold: kanskje senere.
   **2.28:** VibeMenu heter «Vibe coded» i stedet for «Claudes egen» i innstillinger og dokumentasjon (folk ble skeptiske).
   **2.29:** navnene VibeMenu og Nordlys står; bare merkelappen er «Vibe coded» (beskrivelse, info, «Nordlys (vibe coded)»).
   **2.30:** systemtemaet fjernet (fungerte svært dårlig hos brukeren). `systheme.cpp` gjør nå bare `CleanUpLeftovers()` ved oppstart
   (før `LoadTheme`): hvis `[Advanced] SystemTheme=1` eller `[SystemThemeUndo]` finnes → kant/tittel/tekst-farge tilbake til standard
   på alle vinduer, brukerens aksent tilbake, nøklene slettes. Kan fjernes helt i en senere versjon. Ikke prøv systemtema på nytt uten å spørre.
   **2.31:** «Minimer vinduer til docken» (`[Dock] MinimizeToDock`, av). `Dock::Mini`/`minis_` i dock.cpp: WinEvent MINIMIZESTART..END →
   T_MINIS (40 ms) + T_MINIS_LATE (450 ms) → `Layout()` → `RefreshMinis()` (ikoniske, synlige, ikke-cloaked vinduer fra `apps_`; per skjerm når
   flere dock-vinduer finnes). DWM-miniatyr i dockens klient-piksler med `rcSource` = synlig del (marger målt i `OnForeground` via
   EXTENDED_FRAME_BOUNDS, ellers SM_CXSIZEFRAME+PADDEDBORDER). App-merket ligger i et eget eid lag-vindu `badges_` (UpdateLayeredWindow fra
   ID2D1DCRenderTarget) fordi DWM-miniatyrer tegnes over vinduets eget innhold; følger docken i WM_WINDOWPOSCHANGED (flytt, størrelse, z-orden).
   **2.32:** minimerte vinduer får alle samme kort (1,45:1); `rcSource` beskjæres til kortets form (sidene eller bunnen, aldri toppen).
   Tilleggene etter skillelinjen kan dras: `pluginOrder_` (`[Dock] PluginOrder` = media,clock,windows,stack,trash), `PluginAt`,
   `DragPluginTo` (bytter plass når musa passerer midten av naboen), `SavePluginOrder` oppdaterer de andre dockene. Én skillelinje
   tegnes før første tillegg (i Paint), ikke lenger av hvert tillegg.
   **2.33:** SkyWritingTools (`writing.cpp`, `[SkyWritingTools] Enabled/Modifiers/Key/Provider/GeminiModel/OpenAIModel/BaseUrl`, av).
   Hurtigtast (id 0x6101, eget opptak i settings via `captureWhat_`), venter til tastene slippes, `SaveClip` → Ctrl+C →
   `GetClipboardSequenceNumber` → les → `RestoreClip`. Glasskort (Card): meny/busy/resultat/feil. Gemini `generateContent`
   (x-goog-api-key) eller OpenAI-kompatibel `/chat/completions` via WinHTTP i egen tråd → `WM_APP+1`. Erstatt = fokus til målvinduet,
   skjul kort, `PutClip` (ekskludert fra historikk) + Ctrl+V, gjenopprett etter 1,3 s; hvis målvinduet ikke kom fram vises svaret i stedet.
   Nøkler DPAPI-kryptert i `SkyWritingTools-gemini.key` / `-openai.key` ved exe. Kreditert theJayTea (GPL-3.0). Lenker: -lwinhttp -lcrypt32.
   **2.34:** brukeren fikk 503 «high demand» med gemini-flash-latest. Standard nå `gemini-flash-lite-latest`; `CallAI` prøver 3 ganger
   (1,5 s / 4 s) ved 5xx og deretter Flash-Lite som reserve. Modellvalg er en nedtrekksliste: `ModelList` (fra `[SkyWritingTools]
   GeminiModels/OpenAIModels`, hentet av `FetchModels` via GET models / {base}/models, filtrert), `ModelLabel` gir «opptatt»-hint.
   WM_SKY_STATUS 24 → `SettingsChanged()`. Ingen temperature til Gemini (Googles anbefaling).
   **2.35:** «Rask oppstart» (`[General] FastStartup`, `StartupTask`): bar.cpp lager oppgaven «Skyline Fast Startup» med schtasks /XML
   (LogonTrigger for brukeren, InteractiveToken, LeastPrivilege, Priority 4, ExecutionTimeLimit PT0S, ikke stopp på batteri) og fjerner
   Run-nøkkelen; feiler det, brukes Run-nøkkelen. main.cpp venter (maks 30 s) på Shell_TrayWnd før oppstart.
   **2.36:** rask oppstart er standard (`FastStartup` leses med 1 som standard). `MigrateStartup()` (egen tråd ved oppstart) flytter en
   gammel Run-nøkkel over til oppgaven én gang; lar Windows det ikke, settes `FastStartup=0` så det ikke prøves igjen.
   **2.37 (full kodegjennomgang, 6 kontrollører + egen kontroll av rettelsene):**
   - `Backup::MachineState` (backup.h/.cpp): `[Dock] RestoreAutohide`, `[General] RestoreTaskbarAl`, `[General] StartupTask` overlever
     gjenoppretting, fabrikkinnstillinger og ytelsesmodus-av. Ytelsesmodus avsluttes rent før restore/reset (`Look::DropPerformanceMode`).
     Windows' gjennomsiktighet gis tilbake ved avslutning (`Look::LeavingWindows`) og tas igjen ved start (`Look::Starting`).
   - Krasj: Start-plasseringen gis tilbake (`RestoreStartAfterCrash`, `g_crashIni`); krasjhåndterere settes før dokkene lages.
   - VibeMenu-bruk lagres og glemmes i minnet før restore (`StartMenus::UnloadVibeUsage`). PaintWash-cache: maks 8, ubrukt >20 s kastes.
   - Wi-Fi: profil slettes bare hvis tilkoblingen faktisk feilet (`ConnectedTo`); åpen rad følger SSID, ikke indeks.
   - SkyGarden: `Save` skriver via .tmp + MoveFileEx, aldri tomt/stoppet; MenuGuard rundt menyene.
   - Dock: avbrutt ikon-drag nullstilles (WM_CAPTURECHANGED, `dragging_` settes false FØR ReleaseCapture i LBUTTONUP);
     `TaskbarHider::wanted_` (skjuler oppgavelinjen når Explorer kommer senere); Now Playing tegner bare når flisen synes; hoved-dock
     (owner_) styrer oppgavelinjen; stack/papirkurv skarpe etter DPI-bytte. Now Playing `gen` teller alltid oppover.
   - Tiling: lukkede vinduer etterlater ikke hull; ShowWindowAsync; `tries_` nullstilles; dras ikke ut av fullskjerm (`inOurSlot`).
     `Manages()` uendret (topplinja/nettleser-regelen fra 2.18 står).
   - Systemkurv: GUID-ikoner matches riktig; re-annonsering maks 3 per 10 min; overvåking 1 s; trykk + slipp utenfor gir ikke spøkelses-drag.
   - SkyWritingTools: limer ikke inn hvis utklippstavla ikke kunne tas vare på; gjenoppretter ikke over noe nytt brukeren kopierte.
   - Diverse: ukenummer 1.–3. jan, trege stasjoner (nett/USB) sjekkes ikke i VibeMenu, Vibe-animasjon 20 fps og av i ytelsesmodus,
     hot corners 200 ms, Mission Control-font per skjerm, trådkappløp på ini-stien (MigrateStartup, FetchModels).
   **2.38:**
   - **Chris Titus WinUtil lukket seg straks** med Skyline på («ShowDialog … The method or operation is not implemented»).
     Årsak: vår falske `Shell_TrayWnd` (tray.cpp, ligger først så Shell_NotifyIcon kommer til oss) manglet vindusegenskapen
     `TaskbandHWND`. ITaskbarList leser den på første Shell_TrayWnd for å finne Explorers oppgavebånd; uten den svarer den
     E_NOTIMPL, og WPF-apper med TaskbarItemInfo krasjer. Fiks: `TrayHost::LinkTaskband()` kopierer `TaskbandHWND` fra Explorers
     Shell_TrayWnd (ellers barnevinduet `MSTaskSwWClass`) til vårt vindu; kalles i CreateWindows og fra MonitorTimer når den mangler
     eller er død (Explorer-omstart). Fjernes i DestroyWindows. (Samme løsning som ManagedShell/Cairo/RetroBar.) Bekreftet av brukeren:
     feilen forsvant med tray-ikoner av.
   - **SkyGarden: dra filer inn fra hvor som helst.** Hvert bed er et OLE-slippmål (`BedDrop`, `RegisterDragDrop` i `Bed::Create`,
     `RevokeDragDrop` i `~Bed`), med Explorers dra-bilde (`IDropTargetHelper`) og innsettingsmerke (`g_ole`). Regler som Windows'
     skrivebord: samme disk = flytt, annen disk = kopier, Ctrl kopier, Shift flytt, Alt (eller Ctrl+Shift) snarvei; filer som allerede
     ligger på skrivebordet bytter bare bed. `Drop` returnerer DROPEFFECT_NONE (kilden sletter aldri noe selv) og legger jobben i kø
     (`kMsgDropFiles` = WM_APP+7); `DoDrop` flytter/kopierer med `IFileOperation` (angre, kollisjonsspørsmål) til skrivebordet,
     `LandedSink` gir de endelige navnene, og nøklene settes inn i bedet der brukeren slapp (fjernes fra andre bed først).
     Ble bedet bygget om i mellomtiden, tar catch-all-bedet dem.
   **2.40.3:** **Fiks: tiling på skjerm som ligger over hovedskjermen (negativ topp, f.eks. y = -270) startet for langt nede med autoskjult topplinje.**
     `Tiler::WorkArea` tok `max(skjermtopp, ReservedBottom())`, og `ReservedBottom()` er 0 når linja er autoskjult, så en negativ topp ble til 0.
     Funnet via feilsøkingsfila (monitor 3: rcMonitor top -270, tilerArea top 0). Reservasjonen brukes nå bare når den er over 0.
   **2.40.4:** **Fiks: autoskjult topplinje kom ikke fram med musa når et vindu var maksimert.**
     `Bar::KeepOnTop` gikk ut tidlig når linja var skjult, så den tynne hover-stripa (`trigger_`) aldri ble løftet over det maksimerte vinduet.
     Stripa løftes nå først (også når linja er skjult). Linja løftes også til øverst når den glir fram (kom før opp bak vinduene). Ikke testet på Windows.
   **2.40.5:** **Mission Control åpner nå på alle skjermer samtidig** (før bare skjermen med musa).
     `mission.cpp`: ett `Overlay` per skjerm (`g_overlays`); skjermen med musa får tastaturet. Klikk/Esc/høyreklikk lukker alle; å flytte fokus til en annen av våre skjermer lukker ikke. Ikke testet på Windows.
   **2.40.2:** tiling husker størrelsene når et vindu minimeres og kommer tilbake (`Ratios::saved` per gruppe vinduer, `memIndex_` husker plassen i rekkefølgen).
     Feilsøkingsfil: Ctrl+Alt+D eller knapp i Innstillinger > Tiling skriver `skyline-debug.txt` ved siden av skyline.ini (skjermer, arbeidsområde,
     topplinje, vinduer). Autoskjult topplinje gir tilbake en gammel reservasjon øverst i Windows' arbeidsområde én gang (`Bar::EnsureWorkArea`).
   **2.40.1:** tiling: toppen av arbeidsområdet tas fra skjermen, ikke fra Windows' arbeidsområde (`Tiler::WorkArea`). Med topplinja på autoskjuling lå
     en gammel reservasjon igjen i Windows og vinduene startet langt nede (bare skjerm 1). Bare vår egen synlige linje reserverer plass.
   **2.40.0:** **SkyMenu-størrelse.** Innstilling «SkyMenu: størrelse» (Liten 85 % / Middels 100 / Stor 120 / Ekstra stor 145 / Egendefinert), `[Start] MenuSize`
     og `MenuScale` (%). Menyen tegnes i «designstørrelse» 640 x 660 dip og skaleres med `Menu::scale_` (transform i Paint, vindu, museposisjon,
     ikonstørrelse); `FitScale` holder den på skjermen. Valgfri bryter «endre størrelse med musa» (`[Start] MenuResize`, av som standard): dra
     venstre/høyre/øvre kant eller hjørne (6 dip), menyen vokser fra nedre midten, slipp lagrer «Egendefinert».
   **2.39.2:** tiling står helt stille under spill (`GameMode()` i `Tiler::Retile`/`OnEvent`, tatt igjen etterpå via `FlushPending`): et tilet
     spillvindu ble flyttet midt i skjermmodus-byttet og hoppet ut og inn av fullskjerm (CoD m.fl.).
   **2.39.1 (16. nov 2026, tiling):**
   - **Tiling: dra i kanten for å endre størrelse.** Ved slutt på dra (`EVENT_SYSTEM_MOVESIZEEND`) sammenlignes vinduets ramme med sluttet
     det fikk (`targets_`). Endret størrelse -> `Tiler::ApplyResize` finner skillelinjen (`Split`) kanten satt på og flytter den
     (`Ratios` per skjerm; nullstilles når oppsett eller antall vinduer endres); naboene flyter med. Flyttet hele vinduet -> bytter plass (`SwapAt`) som før.
     Gjelder BSP, Master (master-bredde og stabel) og Kolonner. Bare ved slipp, ikke levende under draing.
   - `Place`: en app som flytter seg selv (f.eks. ZenFiles gjenoppretter lagret plassering rett etter oppstart) får nye forsøk (`seen_`).
   - Fullskjerm-sjekk: mens topplinje/dock er skjult for et fullskjermvindu sjekkes det på nytt hvert 2. sekund, så en tapt melding aldri
     kan la dem bli borte til omstart. (Mistenkt årsak til «bugger på skjerm 1 etter ZenFiles»; ikke bekreftet.)
   **2.39 (16. nov 2026, fra brukertilbakemeldinger; ikke testet på Windows, bare krysskompilert):**
   - **Spillmodus:** under eksklusiv fullskjerm (`SHQueryUserNotificationState` = D3D_FULL_SCREEN, `GameMode()` i main.cpp) rører Skyline
     ingenting: ingen ombygging av topplinje/dock ved skjermmodus-bytte (utsatt 250 ms-timer `kTimerDisplay`), ingen endring av
     arbeidsområdet, ABN_POSCHANGED og tray-gjenkunngjøring utsettes, hot corners og oppdateringsvarsler venter. Tar igjen etterpå.
   - **Steam-ikon i docken:** `Dock::LoadIcon` faller tilbake på exe-ikon og deretter vinduets ikon når Start-menyen ikke har oppføring.
     Tray: robust ikon-kopi (`ImageFromIcon` via WIC + GDI-alfa), beholder gammelt ikon om nytt feiler.
   - **Dock på flere skjermer:** `[Dock] Screens`, valg «valgte skjermer» (`kDockChosen`) med velger i Innstillinger.
   - **SkyGarden mappe-bed:** et bed kan speile en mappe hvor som helst (`Bed::folder`, endringsvarsel per mappe, maks 600 elementer);
     slipp inn i et mappe-bed flytter/kopierer filene inn i den mappen (`IFileOperation`, angre virker). Knapp «Nytt mappe-bed».
   - **Fem temaer:** Midnatt (OLED-svart), Hav, Solnedgang, Skog, Rosé (`Look::kThemes = 11`, farget glass via `glassTone`).
   - **SkyMenu:** bruker + strømmeny (dvale, bytt bruker), «Anbefalt» (nylige filer, bryter `[Start] MenuRecent`), mappesnarveier, A–Z-hopp.
   - **Utseende:** dock: fin lyskant øverst og glorie bak aktiv prikk; runde hover-piller i topplinja; runde kort i Innstillinger.
   - **build.bat:** spør etter versjonsnummer (skriver `src/version.h`) og tilbyr «Bygg vanlig» eller «Bygg setup».
   **Neste (avtalt 16. nov 2026, tas en annen dag): «Enhanced Tiling» – en EGEN funksjon ved siden av vanlig tiling.**
   Egen bryter, av som standard, egen kildefil (`enhancedtiling.cpp/.h`), og vanlig tiling (`tiling.cpp`) røres ikke, slik at den gamle kan brukes hvis noe feiler.
   Innhold (brukeren sa «gjør alt»): layout-tre per skjerm (splitter og vinduer) med LEVENDE resizing; egne skillelinjer som kan gripes
   (tynne gjennomsiktige vinduer i gapet, riktig musepeker, markeres ved hover); hurtigtaster for størrelse (f.eks. Ctrl+Alt+Win+piler) og «lik størrelse»;
   hovedvindu-modus (promover til master); arbeidsområder per skjerm (4, bytt/send med Ctrl+Alt+1–4, skjules med SW_HIDE – må vises igjen i Stop());
   regler per app (`[Tiling] Rules`: `exe=float`, `exe=monitor 2`, `exe=space 3`); dra-for-å-snappe med forhåndsvisning (midten = bytt, kant = del);
   valgfri animasjon (~140 ms) og fokus-følger-musa (valgfri 120 ms-timer). Husk: ingenting kan testes her; brukeren tester mellom hver omgang.
   Gjenbruk fra vanlig tiling: `Manageable`, `WorkArea` (toppen fra skjermen, rett håndtering av negativ topp), `Place`, `GameMode()`-stillstand,
   husk størrelser/plass etter minimering (`Ratios::saved`, `memIndex_`). Feilsøking: Ctrl+Alt+D / knapp i Innstillinger skriver `skyline-debug.txt`
   i installasjonsmappen (den installerte Skyline ligger i `%LocalAppData%\Programs\Skyline`, ikke i `out`).
   **Kjent, ikke gjort ennå (større ombygging):** Now Playing og SkyGarden poller (1–2 s) i stedet for hendelser; app-lista leses på
   UI-tråden etter at en meny lukkes; VibeMenu «Fortsett der du slapp» leser .lnk på UI-tråden; LL-tastaturkroken fjernes aldri;
   status-timer på 30 s; `Schtasks` venter på UI-tråden (maks 15 s); Wi-Fi WEP/OWE-profiler.
   **Neste (brukeren laster opp i morgen):** vennens publiserte versjon med `update.cpp` (auto-oppdatering, `g_app.settings.updateCheck`).
   Slå sammen: behold alt hans om oppdatering uendret, legg 2.32 oppå, legg update.cpp i build.sh/build.bat/CMakeLists. Lever både
   ferdig app-zip og zip med hele koden.
   **Neste (avtalt, venter til brukeren sier fra):** lukkeknapp (✕) i Mission Control som vises ved hover, egen bryter under
   tillegget, av som standard (`WM_CLOSE` til vinduet, fjern det fra rutenettet).
   Opprinnelig idé: Claudes helt egen startmeny som fjerde valg under Startmeny. Idé lovet brukeren: hilsen + ett søk
   for alt (apper, innstillinger, filer, `=` regning), «Nå» = apper brukeren pleier å åpne på denne tiden av døgnet (telles lokalt ved
   oppstart av apper, ingen bakgrunnsjobb), «Fortsett der du slapp» = nylige filer fra Windows' Recent, tall 1–9 starter forslag,
   Start på nytt/Slå av, kompakt glasskort over docken med egen `Part`. Brukeren vil se hva Claude lager helt på egen hånd.
3. ~~**Kalender i klokka**~~ (ferdig i 2.19): klikk på klokka i topplinja åpner Skylines eget glasskort (måned, ukenummer, i dag) i stedet for Win+N.
   Bryter for å bruke Windows' panel som før. Avtaler fra Outlook/Google er usikkert (unpackaged app); første versjon uten avtaler.
4. ~~**Stacks i docken**~~ (ferdig i 2.19): en valgt mappe (f.eks. Nedlastinger) som åpner en vifte/rutenett med filene. Leses først ved klikk.
5. ~~**Papirkurv**~~ (ferdig i 2.19) (tillegg): helt til høyre i docken, tom/full via `SHChangeNotifyRegister`. Dra filer dit = slett (`IFileOperation`).
   Høyreklikk = tøm (med bekreftelse).
6. **Utklippstavle** (tillegg): `AddClipboardFormatListener`, siste 20 elementer (tekst + små bilder) kun i minnet.
   Hopp over innhold merket `ExcludeClipboardContentFromMonitorProcessing`.
7. **Systemmonitor** (tillegg): små CPU-, RAM- og GPU-målere, valg: dock, topplinje eller begge. CPU `GetSystemTimes`,
   RAM `GlobalMemoryStatusEx`, GPU via PDH «GPU Engine». Polling 1 s **bare mens synlig**. Ingen temperatur.
8. **Mission Control:** alle vinduer i et rutenett med DWM-thumbnails, glass og animasjon, klikk for å bytte;
   kan legges på hot corner og hurtigtast.

Gjør dem i omganger og send en ny versjon etter hver omgang (2.4, 2.5 …), så brukeren kan teste mellom hver.

---

## 6b. Status og neste økt (18. nov 2026)

**Status:** Skyline 2.40.5 er ferdig og testet av brukeren (siste ting: autoskjult topplinje over maksimerte vinduer, Mission Control på alle skjermer).
Alt er slått sammen i `CyberBismo/Skyline` (main) via pull request #1 fra forken `ZenForgeDev/Skyline`, og det finnes en GitHub-release `v2.40.5`
med `Skyline-Setup-2.40.5.exe` (brukerens egen bygging). «Lars» (CyberBismo) styrer signeringen: den innebygde auto-oppdateringen går via
`skyline.skybyn.com` og trenger en signert `latest.txt` laget med den private nøkkelen `skyline-signing.key` (`tools/release.py` / `sign-release.ps1`).
Claude rører ikke nøkkelen. Brukeren sender Lars en melding når en ny versjon skal ut.

**Arbeidsmapper:** kildekoden ligger i `C:\Dev\Skyline\Skyline-main\Skyline-main` (zip-nedlasting, ikke git) og i den klonede forken
`C:\Users\ZenForge\Documents\GitHub\Skyline` (git, `origin` = ZenForgeDev/Skyline, `upstream` = CyberBismo/Skyline). Den installerte appen ligger i
`%LocalAppData%\Programs\Skyline`. Claudes arbeidskopi: `/home/claude/work/Skyline` (git, bygges med `PATH=/tmp/llvm-mingw/bin:$PATH ./build.sh`; last ned llvm-mingw på nytt i ny container).

**GitHub-rutine (virket 18. nov):** 1) kopier filene fra arbeidskopien til den klonede forken med `device_commit_files` (ny katalog under
`/mnt/user-data/outputs` for hver commit, aldri samme `stagedPath` to ganger – ellers kommer gammelt innhold), 2) GitHub Desktop: ny gren,
commit, Publish branch, Create pull request (nettleseren i Claude fyller ut og sender), 3) etter merge: ny release via
`releases/new?tag=vX.Y.Z&target=main&title=…&body=…`, brukeren drar `Skyline-Setup-X.Y.Z.exe` inn selv (filopplasting går ikke via Claude).
Claude har ingen GitHub-innlogging i skyen; `gh` virker ikke. Filene i arbeidskopien har LF, forken bruker CRLF – git normaliserer, ingen støy i diffen.

**Prioritert rekkefølge (avtalt 18. nov):**
1. **Enhanced Tiling** (første prioritet) – planen står i punkt 6 over («Neste (avtalt 16. nov 2026…)»). Egen bryter, av som standard, egne filer `enhancedtiling.cpp/.h`, `tiling.cpp` røres ikke.
2. **Forhåndsvisning av minimerte vinduer i docken.** Windows tegner ikke live innhold for minimerte vinduer, så: ta et stillbilde (PrintWindow `PW_RENDERFULLCONTENT`)
   i det vinduet minimeres (`EVENT_SYSTEM_MINIMIZESTART`, før det er borte) og vis det i dock-forhåndsvisningen på samme måte som for vanlige ikoner. Siste utseende, ikke live.
   Vinduer som var minimert før Skyline startet får bare ikon og tittel. Gjenbruk `preview.cpp`. Bryter, av som standard.
3. **Skrivebordsklokke (plugin, av som standard):** eget gjennomsiktig vindu som kan flyttes hvor som helst (posisjon huskes per skjerm), flott digital klokke med dato,
   valg for 12/24 t, sekunder, ukenummer og noen stiler (tynn, fet, flip), «lås» så musa går gjennom, oppdatering én gang per sekund (ingen polling ellers).
4. Resten av forslagene under, i den rekkefølgen brukeren velger.

**Flere plugin-forslag (alle brytere, av som standard):**
- Skrivebords-widgets på samme drabare vindu som klokka: måneds-kalender, vær, CPU/RAM/GPU-måler (se punkt 7 over), notatlapper.
- Utklippshistorikk med søk og festede favoritter (se punkt 6 over: `AddClipboardFormatListener`, siste 20, bare i minnet).
- Fokusmodus + pomodoro: timer i topplinja som demper varsler og skjuler dock/topplinje en stund.
- Skjermbilde-verktøy: marker område, tegn på, kopier/lagre; evt. fargevelger og lupe.
- Nattlys: varmere farger om kvelden, styrt av klokka.

**Generelle forslag:**
- Dock: stacks (finnes fra 2.19, kan utvides), varselprikker på ikoner, seksjon for nylige apper.
- Topplinje: virtuelt-skrivebord-bryter og et lite varselsenter.
- Startmeny: søk som også regner og konverterer enheter; egne hurtigtast-snarveier.
- Innstillinger: eksport/import av hele oppsettet som én fil.
- Allerede avtalt fra før: lukkeknapp (✕) i Mission Control ved hover; Claudes egen startmeny (VibeMenu-idéer i punkt 6).

---

## 6c. Enhanced Tiling – status (18. nov 2026, kveld)

- **2.41.0 (omgang 1, testet OK av brukeren):** `enhancedtiling.cpp/.h` = egen motor (laget som `ETiler`, layout-tre per skjerm). Bryter `[Tiling] Enhanced=1` (Innstillinger → Tiling; «Automatisk tiling» må også være på). Levende resizing, hurtigtaster (Ctrl+Alt+Win+piler flytter skillelinje, +E lik størrelse), regler `[Tiling] Rules=exe=float, exe=monitor 2` (`space N` brukes fra 2.43.0). `tiling.cpp` er urørt; `EnhancedApply()` stopper den vanlige motoren mens Enhanced er på. Felles funksjoner for topplinjeknapp/meny: `TilingRunning()`, `TilingIsFloating()`, `TilingToggleFloat()`, `TilingSchedule()`.
- **2.42.0 (omgang 2, ikke testet ennå):** skillelinjevinduer i gapene (hover lyser, dra = levende, dobbeltklikk = 50/50; `[Tiling] Dividers=1`), dra-for-å-snappe med forhåndsvisning (midten = bytt, kant = del; `[Tiling] Snap=1`). Dessuten: **dock på flere skjermer skjules nå også av vinduer som ikke er i fokus** (`AnyWindowOver` i `dock.cpp`, globale hendelser settes opp bare når det finnes mer enn én dock).
- **2.43.0 (omgang 3, ikke testet ennå):** 4 arbeidsområder per skjerm (Ctrl+Alt+1–4 bytter på skjermen med musa, Ctrl+Alt+Shift+1–4 sender vinduet; aktivt tre i `trees_`, øvrige parkert i `parked_` med vinduene skjult via `ShowWindowAsync(SW_HIDE)` og `hidden_`; `ShowAllHidden()` i `Stop()`), hovedvindu (Ctrl+Alt+Enter), `space N`-regler. **2.43.1:** liten «1 2 3 4»-indikator i ett sekund ved bytte (`ShowSpaceToast`), liste over skjulte vinduer i `skyline-hidden.txt` ved siden av ini (vises igjen ved neste start etter krasj: `RecoverHidden`), og skjulte vinduer blir stående i docken (`IsTaskbarWindow` bruker `EnhancedIsHidden`; klikk går til riktig arbeidsområde via `EnhancedReveal`).
- **2.43.2–2.43.3:** forhåndsvisning i docken for vinduer i andre arbeidsområder (stillbilde) og for minimerte vinduer med «minimer til dock» (`Dock::ShowMiniPreview`).
- **2.44.0:** øyeblikksbilder tas nå i en bakgrunnstråd (`KickCapture`, `CaptureRaw`; før frøs synkron `PrintWindow` Skyline = sannsynlig årsak til «arbeidsområder virker plutselig ikke»). Oversikt over alle områder (Ctrl+Alt+O, `ToggleOverview`), knapp i topplinja (`Kind::Spaces` i `bar.cpp`; klikk = oversikt, høyreklikk = innstillinger). Arbeidsområder i Mission Control: rad øverst med områdene (`mission.cpp`, `EnhancedSpacesInfo`, klikk eller tast 1–8 = bytt; byttet skjer først etter at Mission Control er lukket).
- **2.45.0 (omgang 4):** animasjon ~140 ms (`StartAnim`/`OnAnimTimer`, timer 0x7113, kun mens noe beveger seg; ikke ved live-drag/skillelinje, `noAnim_`) og fokus-følger-musa (`OnFocusTimer`, timer 0x7114, 120 ms, bare når bryteren er på og bare mellom flisede vinduer). Brytere `[Tiling] Animate=1`, `FocusFollowsMouse=0`.
- **2.45.1–2.46.0:** pluss (+) i Mission Control legger til områder (4–8, `[Tiling] Spaces`), × på det siste fjerner det (vinduene flyttes til området før, `DropLastSpace`; minst 2). Hurtigtaster Ctrl+Alt+1–8 / +Shift. Ikon i topplinja: `[Tiling] SpacesIcon` 1 = stablede skjermer, 2 = skjerm med tall (valg i innstillinger).
- **2.47.0:** forhåndsvisning av minimerte vinduer med ekte bilde: bilde tas ved `EVENT_SYSTEM_MINIMIZESTART` og når et vindu mister fokus (`EnhancedSnapshotKick`, bare med «minimer til dock»); `preview.cpp` bruker bildet i stedet for ikon. Virker uten Enhanced Tiling.
- **2.48.x:** skrivebordsklokke som tillegg (`deskclock.cpp/.h`, Innstillinger → Tillegg; `[DeskClock] Enabled, Position, Size, Monitor`): gjennomsiktig, klikkgjennom, oppdateres én gang per minutt. Gjenstår for klokka: stiler, sekunder, ukenummer, 12/24 t, flytting med musa, huske posisjon per skjerm.
- **Ikke testet av brukeren ennå:** alt fra 2.44.0 og oppover (bygget uten kjente feil per 2.48.2, men kjørt lite). Husk at Claude ikke kan bygge i skyen: brukeren bygger med `build.bat`. **Viktig:** bruk alltid ny `stagedPath`-katalog per `device_commit_files`, og kontroller etterpå med `device_stage_files` + `cmp` at filene på maskinen er like (samme sti to ganger ga gamle filer 18. nov).
- **Forslag videre:** Ctrl+Alt+O og klokke-valgene i innstillingsinfo, flere klokkestiler, utklippshistorikk, fokusmodus/pomodoro.

---

## 7. Kjente ting å følge med på

- Akryl-uskarpheten er satt tilbake til 2.0-oppskriften, som brukeren syntes var best. Er det ikke nok, kan Windows 11 sitt systemakryl
  (`DWMWA_SYSTEMBACKDROP_TYPE` = 3, transient) prøves, men det er utestet og krever at brukeren tester.
- Klokka i topplinja forsvant av og til (stale album-cover-bitmap i Now Playing). Dette er fikset i 2.3; spør gjerne brukeren om det har holdt seg.
- Win-tasten fanges ikke når et administrator-program er i front (ellers kan Win «henge»).
