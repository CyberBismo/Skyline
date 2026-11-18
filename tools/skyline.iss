; Skyline installer (Inno Setup 6). Built by build.bat -> "Bygg setup".
; Installs per user (no admin needed) so Skyline's updater can replace the exe by itself.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6B1C7E58-5A0B-4B0B-9D0E-5B7A5E1D2C11}
AppName=Skyline
AppVersion={#AppVersion}
AppPublisher=Yen-Zen & Cyber
DefaultDirName={localappdata}\Programs\Skyline
DefaultGroupName=Skyline
PrivilegesRequired=lowest
OutputDir=..\out
OutputBaseFilename=Skyline-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE.txt
UninstallDisplayIcon={app}\Skyline.exe
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no

[Tasks]
Name: "autostart"; Description: "Start Skyline when I sign in"; Flags: unchecked
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "..\out\Skyline.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Skyline"; Filename: "{app}\Skyline.exe"
Name: "{userdesktop}\Skyline"; Filename: "{app}\Skyline.exe"; Tasks: desktopicon
Name: "{userstartup}\Skyline"; Filename: "{app}\Skyline.exe"; Tasks: autostart

[Run]
Filename: "{app}\Skyline.exe"; Description: "Start Skyline"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im Skyline.exe"; Flags: runhidden; RunOnceId: "KillSkyline"
