@echo off
setlocal
cd /d "%~dp0"
title Skyline - build
set "OUTEXE=out\Skyline.exe"

rem ===== 1. find the C++ compiler (Visual Studio or Build Tools) =====
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_cpp
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto no_env
goto have_cl

:no_vs
echo Visual Studio not found. Install "Build Tools for Visual Studio 2022" with "Desktop development with C++".
goto fail
:no_cpp
echo The C++ tools are missing. Add "Desktop development with C++" in the Visual Studio Installer.
goto fail
:no_env
echo Could not start the Visual Studio build environment.
goto fail

:have_cl
rem ===== 2. version number =====
set "CURVER="
for /f "tokens=2,3" %%a in ('findstr /c:"#define SKYLINE_VERSION_STR" src\version.h') do set "CURVER=%%~b"
echo.
echo  Skyline - current version: %CURVER%
set "NEWVER="
set /p "NEWVER=  New version (e.g. 2.39.0), or press Enter to keep it: "
if not defined NEWVER set "NEWVER=%CURVER%"
if "%NEWVER%"=="%CURVER%" goto ask_mode

set "PS1=%TEMP%\skyline_setver.ps1"
> "%PS1%" echo param([string]$v)
>> "%PS1%" echo if ($v -notmatch '^^\d+\.\d+(\.\d+)?$') { exit 2 }
>> "%PS1%" echo $p = $v.Split('.'); if ($p.Count -lt 3) { $p += '0' }
>> "%PS1%" echo $s = $p -join '.'; $q = [char]34
>> "%PS1%" echo $t = Get-Content 'src\version.h' -Raw
>> "%PS1%" echo $t = $t -replace '(?m)^#define SKYLINE_VERSION_MAJOR [^^\r\n]*', ('#define SKYLINE_VERSION_MAJOR ' + $p[0])
>> "%PS1%" echo $t = $t -replace '(?m)^#define SKYLINE_VERSION_MINOR [^^\r\n]*', ('#define SKYLINE_VERSION_MINOR ' + $p[1])
>> "%PS1%" echo $t = $t -replace '(?m)^#define SKYLINE_VERSION_PATCH [^^\r\n]*', ('#define SKYLINE_VERSION_PATCH ' + $p[2])
>> "%PS1%" echo $t = $t -replace '(?m)^#define SKYLINE_VERSION_STR [^^\r\n]*', ('#define SKYLINE_VERSION_STR ' + $q + $s + $q)
>> "%PS1%" echo $t = $t -replace '(?m)^#define SKYLINE_VERSION_W [^^\r\n]*', ('#define SKYLINE_VERSION_W L' + $q + $s + $q)
>> "%PS1%" echo [IO.File]::WriteAllText('src\version.h', $t, (New-Object Text.UTF8Encoding $false))
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" "%NEWVER%"
if errorlevel 1 goto bad_ver
del "%PS1%" >nul 2>nul
for /f "tokens=2,3" %%a in ('findstr /c:"#define SKYLINE_VERSION_STR" src\version.h') do set "NEWVER=%%~b"
goto ask_mode

:bad_ver
echo That is not a valid version number. Use something like 2.39.0
goto fail

rem ===== 3. what to build =====
:ask_mode
echo.
echo  Version %NEWVER% - what do you want to build?
echo    1  Bygg vanlig  (Skyline.exe)
echo    2  Bygg setup   (Skyline.exe + installer)
choice /c 12 /n /m "  Press 1 or 2: "
set "MODE=%errorlevel%"

call :compile
if errorlevel 1 goto fail
echo.
echo Done: %OUTEXE%  (version %NEWVER%)
if "%MODE%"=="1" goto done

rem ===== 4. installer (Inno Setup 6) =====
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LocalAppData%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LocalAppData%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC goto no_inno
"%ISCC%" /Qp /DAppVersion=%NEWVER% tools\skyline.iss
if errorlevel 1 goto fail
echo Done: out\Skyline-Setup-%NEWVER%.exe
goto done

:no_inno
echo.
echo Inno Setup 6 was not found, so the installer was not made. Skyline.exe is ready.
echo Install Inno Setup free:  winget install JRSoftware.InnoSetup   - then run this file again.
goto fail

:done
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1

:compile
if not exist out mkdir out
rc /nologo /fo out\skyline.res src\skyline.rc || exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A00000C /DNOMINMAX ^
  src\main.cpp src\bar.cpp src\tray.cpp src\status.cpp src\menus.cpp src\dock.cpp src\tiling.cpp src\enhancedtiling.cpp src\deskclock.cpp src\settings.cpp src\icons.cpp src\hotcorners.cpp src\preview.cpp src\applist.cpp src\startmenu.cpp src\nowplaying.cpp src\garden.cpp src\update.cpp src\dialog.cpp src\backup.cpp src\filesearch.cpp src\flyouts.cpp src\look.cpp src\mission.cpp src\systheme.cpp src\writing.cpp ^
  out\skyline.res /Fo:out\ /Fe:%OUTEXE% ^
  /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /MANIFEST:NO ^
  d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib shcore.lib shlwapi.lib shell32.lib ole32.lib oleaut32.lib ^
  uuid.lib comctl32.lib wlanapi.lib iphlpapi.lib version.lib propsys.lib psapi.lib user32.lib gdi32.lib advapi32.lib secur32.lib powrprof.lib runtimeobject.lib winhttp.lib bcrypt.lib crypt32.lib
if errorlevel 1 (echo. & echo Build failed. & exit /b 1)
exit /b 0
