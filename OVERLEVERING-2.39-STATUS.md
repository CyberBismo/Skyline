# Skyline 2.39 – statusnotat (2026-11-16)

Utgangspunkt: 2.38.1 (inkl. vennens oppdaterer). Språk: norsk, kort og enkelt. Brukeren er ikke programmerer; Claude skriver og bygger alt.
Regler: nye funksjoner er av som standard, lavt ressursbruk (hendelsesstyrt, ingen polling), ikke rør Skyline10, behold update.cpp.

## Ferdig (committet i git, bygger uten feil/advarsler)
1. Spillmodus: Skyline holder seg helt i ro under eksklusiv fullskjerm (MW4, BO7 …) og tar igjen etterpå. (`GameMode()`)
2. Steam-ikon i docken: reserveløsninger (mål → exe → vinduets ikon).
3. Dock på flere skjermer (valg i Innstillinger).
4. SkyGarden mappe-bed som speiler en mappe; slipp av filer flytter/kopierer inn i mappen.
5. Fem nye temaer: Midnatt (OLED), Hav, Solnedgang, Skog, Rosé.
6. SkyMenu: bruker/navn + strøm, Anbefalt (nylige filer), mappesnarveier, A–Z-hopp.

## Gjenstår
- Penere utseende: dock, SkyMenu/startmenyer, innstillingsvindu, topplinje (ikke begynt i kode).
- Ny build.bat: spør om versjonsnummer, to valg «bygg vanlig» / «bygg setup».
- Versjon -> 2.39 (`src/version.h`), oppdatere OVERLEVERING.md/README, kodegjennomgang, zip-er (app, GitHub, backup), levere.

## Bygg
`PATH=/tmp/llvm-mingw/bin:$PATH ./build.sh` (llvm-mingw 20261104; last ned på nytt ved ny container).
Ingenting kan kjøres på Windows her – spill, Steam og flere skjermer må testes av brukeren.
