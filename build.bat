@echo off
setlocal
rem ==========================================================================
rem  Builds Skyline.exe with Visual Studio 2022 (the free Community edition is fine).
rem  Just double-click this file. It finds Visual Studio by itself.
rem  Needs the "Desktop development with C++" workload.
rem ==========================================================================
cd /d "%~dp0"
set "OUTEXE=out\Skyline.exe"

where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%VSWHERE%" (
    echo Visual Studio not found. Install Visual Studio 2022 with "Desktop development with C++".
    pause & exit /b 1
  )
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
  if not defined VSDIR (
    echo Visual Studio is missing the C++ tools. Add "Desktop development with C++" in the Visual Studio Installer.
    pause & exit /b 1
  )
  call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || (pause & exit /b 1)
)

if not exist out mkdir out
rc /nologo /fo out\skyline.res src\skyline.rc || (pause & exit /b 1)
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000C /DNOMINMAX ^
  src\main.cpp src\bar.cpp src\tray.cpp src\status.cpp src\menus.cpp src\dock.cpp src\tiling.cpp src\settings.cpp src\icons.cpp src\hotcorners.cpp src\preview.cpp src\applist.cpp src\startmenu.cpp src\nowplaying.cpp src\garden.cpp src\update.cpp src\dialog.cpp src\backup.cpp src\filesearch.cpp src\flyouts.cpp src\look.cpp src\mission.cpp src\systheme.cpp src\writing.cpp ^
  out\skyline.res /Fo:out\ /Fe:%OUTEXE% ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO ^
  d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib shcore.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib ^
  uuid.lib comctl32.lib wlanapi.lib iphlpapi.lib version.lib propsys.lib psapi.lib user32.lib gdi32.lib advapi32.lib secur32.lib powrprof.lib runtimeobject.lib winhttp.lib bcrypt.lib crypt32.lib
if errorlevel 1 (echo. & echo Build failed. & pause & exit /b 1)
echo.
echo Done: %OUTEXE%
pause
