# Skyline for Windows 11 2.39 – full backup (16. november 2026)

Native topplinje, dock, tray-vert og automatisk tiling for Windows 11.
C++20, Win32, Direct2D/DirectWrite. Ingen webview, ingen .NET, ingen eksterne biblioteker.

## Innhold
| Mappe/fil | Hva |
|---|---|
| `ferdig/Skyline.exe` | Ferdig bygget app, klar til bruk |
| `ferdig/LES-MEG.md` | Brukerguide |
| `src/` | All kildekode (.cpp/.h, ikon, manifest, ressursfil) |
| `github/` | Det som legges ut på GitHub: engelsk README.md, LICENSE, .gitignore, .gitattributes |
| `build.bat` | Bygg på Windows med Visual Studio – bare dobbeltklikk |
| `CMakeLists.txt` | Alternativ: åpne mappen i Visual Studio eller bruk CMake |
| `build.sh` | Bygg fra Linux/WSL med llvm-mingw |
| `tools/` | Signeringsnøkkel og signering av nye versjoner (auto-oppdatering) |

## Bygge på nytt (Windows)
1. Installer **Visual Studio 2022 Community** (gratis) med arbeidsmengden
   **«Desktop development with C++»**.
2. Dobbeltklikk `build.bat`. Ferdig fil havner i `out\Skyline.exe`.

Alt som trengs følger med Windows SDK-en som installeres sammen med Visual Studio
(Direct2D, DirectWrite, DWM, WLAN, Core Audio osv.).

## Bygge fra Linux / WSL
Last ned llvm-mingw: https://github.com/mstorsjo/llvm-mingw/releases
(f.eks. `llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz`), legg `bin/` i PATH,
og kjør `./build.sh`.

## Filoversikt (src/)
- `main.cpp` – oppstart, innstillinger (skyline.ini), tema, aktiv app, fullskjermdeteksjon
- `bar.cpp/.h` – topplinjen: appnavn, appmenyer, tray-ikoner, status, klokke, tiling-knapp
- `tray.cpp` – blir Windows' tray-vert så ikonene havner i topplinjen
- `status.cpp` – volum, nettverk, batteri (hendelsesstyrt)
- `menus.cpp/.h` – macOS-aktige appmenyer (menus.txt) og speiling av klassiske menyer
- `dock.cpp/.h` – docken og skjuling av Windows-oppgavelinjen
- `tiling.cpp/.h` – automatisk tiling (BSP, Master, Kolonner, Monocle)
- `settings.cpp/.h` – innstillingsvinduet (Mica, Windows 11-stil)
- `icons.cpp/.h` – skarpe app-ikoner (største ikon fra exe, HQ-skalering)
- `hotcorners.cpp/.h` – hot corners
- `startmenu.cpp/.h` – SkyMenu og SkyLauncher
- `applist.cpp/.h`, `nowplaying.cpp/.h`, `garden.cpp/.h`, `preview.cpp/.h` – appliste, Now Playing, SkyGarden, forhåndsvisninger
- `backup.cpp/.h` – backup, gjenoppretting og fabrikkinnstillinger (.skyline-filer)
- `update.cpp/.h` – innebygd oppdatering (signert manifest, nedlasting, bytte av exe, omstart)
- `dialog.cpp/.h` – spørsmålsvindu i Skyline-stil (Mica, samme farger som Innstillinger) i stedet for MessageBox
- `version.h` – versjonsnummeret (det eneste stedet; brukes av skyline.rc og oppdateringen)
- `update_key.h` – offentlig signeringsnøkkel, lages av `tools/new-signing-key.ps1`
- `look.cpp`, `mission.cpp/.h`, `flyouts.cpp/.h`, `systheme.cpp/.h` – temaer, Mission Control, Native Flyouts, opprydding etter det fjernede systemtemaet
- `writing.cpp/.h` – SkyWritingTools (AI-skriveverktøy, tillegg)
- `common.h`, `app.h` – felles typer og global tilstand

## Dine egne innstillinger
`skyline.ini` (innstillinger, festede apper, tray-rekkefølge) og `menus.txt` (appmenyer)
ligger ved siden av Skyline.exe på PC-en din. Innstillinger → Generelt → Backup lagrer dem (og `garden.ini`) i én `.skyline`-fil.


## Auto-oppdatering
Skyline ser etter ny versjon 45 sek. etter oppstart og så hver 6. time
(`https://skyline.skybyn.com/update/latest.txt`), spør én gang per ny versjon, og oppdaterer
med ett klikk (appnavnet → «Oppdater til Skyline x.y.z…» eller Innstillinger → Om Skyline).
Den nye exe-en lastes ned og sjekkes mot SHA-256 i manifestet. Før noe byttes startes den én gang
i en test-modus (`--update-selftest`), så Smart App Control/antivirus som blokkerer den oppdages mens
den gamle fortsatt ligger urørt. Så skrives den ved siden av som `Skyline.new.exe` og byttes inn med
`ReplaceFile` (gammel blir `Skyline.old.exe`, slettes ved neste oppstart), og Skyline starter på nytt.
Ligger Skyline i Program Files, ber Windows om admin (UAC) bare for selve byttet.

Manifestet er signert med ECDSA P-256. Den private nøkkelen ligger **bare** på PC-ene deres,
aldri på serveren, så selv om webserveren blir hacket kan ingen sende kode til brukerne.

**Første gang (én av dere, én gang):**
`powershell -ExecutionPolicy Bypass -File tools\new-signing-key.ps1`
→ privat nøkkel i `%USERPROFILE%\skyline-signing.key` (hemmelig, ta backup, del privat med den andre),
offentlig nøkkel skrives inn i `src\update_key.h`. Uten nøkkel (bare nuller) oppdaterer ikke exe-en seg.

**Ny versjon:**
1. Øk versjonen i `src/version.h` og bygg.
2. `powershell -ExecutionPolicy Bypass -File tools\sign-release.ps1 -Exe out\Skyline.exe -Notes "Hva er nytt (engelsk)" -NotesNo "Hva er nytt"`
3. Last opp `release\Skyline-x.y.z.exe` og så `release\latest.txt` til `skybyn.com/public_html/skyline/update/` (= `https://skyline.skybyn.com/update/`).

**Eller alt i ett fra serveren:** øk versjonen i `src/version.h`, oppdater dokumentene i `ferdig/`
(første linje må ha det nye versjonsnummeret), og kjør `skyline-release` (valgfritt: `skyline-release "What's new" "Hva er nytt"`,
eller legg notatene i `release-notes.txt`: første linje engelsk, andre norsk). Den bygger, signerer med
`/home/cyberbyte/skyline-signing.key`, legger ut oppdateringen og nedlastingssiden, sjekker alt på nettsiden og committer.
`skyline-release --dry-run` bygger og signerer uten å legge ut noe. Skriptet er `tools/release.py`.

Versjoner før 2.2.0 har ikke oppdatering og må byttes manuelt én gang.
Mister dere nøkkelen, må alle laste ned en ny versjon manuelt.

## Norsk og engelsk
Én app med begge språk. Alle tekster står som `TR(L"norsk", L"English")` og velges mens appen kjører (`g_norsk`). Engelsk er standard; byttes under Settings → General → Language / Språk (lagres som `Language=en|no` i `[General]` i skyline.ini).

## Lisens
Copyright © 2026 Yen-Zen & Cyber. Alle rettigheter forbeholdt. Kildekoden er privat; selve appen er gratis å bruke og dele uendret. Se `LICENSE.txt`.
