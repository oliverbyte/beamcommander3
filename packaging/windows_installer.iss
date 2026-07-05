; Inno Setup script for BeamCommander3's Windows installer, producing
; BeamCommander3-Setup.exe. Built by .github/workflows/release.yml using
; ISCC.exe (preinstalled on GitHub's windows-latest runners) via:
;
;   iscc /DMyAppVersion=v2026.07.05-1 /DStageDir=C:\path\to\staged\files ^
;        /DIconFile=C:\path\to\AppIcon.ico packaging\windows_installer.iss
;
; StageDir must contain laser_daemon.exe, libusb-1.0.dll, midi_map.json,
; run.bat, and a frontend_dist\ folder (see the release workflow's
; "Assemble release package" step for how that's put together).

#ifndef MyAppVersion
  #define MyAppVersion "dev"
#endif
#ifndef StageDir
  #define StageDir "win_stage"
#endif
#ifndef IconFile
  #define IconFile "AppIcon.ico"
#endif

#define MyAppName "BeamCommander3"
#define MyAppPublisher "oliverbyte"
#define MyAppURL "https://github.com/oliverbyte/beamcommander3"
#define MyAppExeName "run.bat"

[Setup]
AppId={{6B8F1A3E-6D8C-4E63-9A7B-6E7B5C9C5D02}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=.
OutputBaseFilename=BeamCommander3-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\AppIcon.ico

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#IconFile}"; DestDir: "{app}"; DestName: "AppIcon.ico"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\AppIcon.ico"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\AppIcon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent shellexec
