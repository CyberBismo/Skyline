#!/bin/sh
# Cross-compile Skyline with llvm-mingw (Linux/WSL). On Windows use build.bat instead.
set -e
OUT=out/Skyline.exe
mkdir -p out
x86_64-w64-mingw32-windres src/skyline.rc -O coff -o out/skyline.res
x86_64-w64-mingw32-clang++ -std=c++20 -O2 -Wall -Wno-unknown-pragmas -Wno-missing-braces -Wno-unused-lambda-capture -municode -mwindows \
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A00000C \
  src/main.cpp src/bar.cpp src/tray.cpp src/status.cpp src/menus.cpp src/dock.cpp src/tiling.cpp src/settings.cpp src/icons.cpp src/hotcorners.cpp src/preview.cpp src/applist.cpp src/startmenu.cpp src/nowplaying.cpp src/garden.cpp src/update.cpp src/dialog.cpp src/backup.cpp src/filesearch.cpp src/flyouts.cpp src/look.cpp src/mission.cpp src/systheme.cpp src/writing.cpp out/skyline.res -o $OUT \
  -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections \
  -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lshcore -lshlwapi -lshell32 -lole32 -loleaut32 -luuid \
  -lcomctl32 -lwlanapi -liphlpapi -lversion -lpropsys -lpsapi -luser32 -lgdi32 -ladvapi32 -lsecur32 -lpowrprof -lruntimeobject -lwinhttp -lbcrypt -lcrypt32
ls -la $OUT
