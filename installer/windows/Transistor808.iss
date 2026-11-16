; Inno Setup script for the Windows installer.
; Build:  ISCC /DAppVersion=0.1.0 installer\windows\Transistor808.iss
; (paths are relative to this file; expects a Release build in ..\..\build)

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

#define Artefacts "..\..\build\Transistor808_artefacts\Release"

[Setup]
AppId={{4F7A2C61-8E3B-4B9D-A6C2-7D1E808C7808}
AppName=Transistor 808
AppVersion={#AppVersion}
AppVerName=Transistor 808 {#AppVersion}
AppPublisher=Transistor Labs
DefaultDirName={autopf}\Transistor Labs\Transistor 808
DefaultGroupName=Transistor Labs
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\..\dist
OutputBaseFilename=Transistor-808-{#AppVersion}-Windows-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Transistor 808

[Types]
Name: "full";   Description: "VST3 plug-in and standalone app"
Name: "custom"; Description: "Choose components"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plug-in (Ableton Live, Bitwig, Reaper, Cubase...)"; Types: full custom
Name: "app";  Description: "Standalone app";                                         Types: full custom

[Files]
Source: "{#Artefacts}\VST3\Transistor 808.vst3\*"; DestDir: "{commoncf64}\VST3\Transistor 808.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Artefacts}\Standalone\Transistor 808.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Transistor 808"; Filename: "{app}\Transistor 808.exe"; Components: app

[Messages]
FinishedLabel=Transistor 808 is installed.%n%nRescan plug-ins in your DAW (Ableton Live: Settings > Plug-Ins > Rescan).
