; Inno Setup script for the OnlyCam installer.
; Installs the desktop application and registers the DirectShow virtual camera.

#define AppName "OnlyCam"
#define AppVersion "1.0.0"

[Setup]
AppId={{6F1B4C08-6F0B-4D33-9E77-2A4BE4C2E6B1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=OnlyCam
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputBaseFilename=OnlyCam-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\OnlyCam.exe

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\..\..\dist\OnlyCam\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion
Source: "..\..\..\dist\filter\OnlyCamFilter64.dll"; DestDir: "{app}\filter"; Flags: ignoreversion regserver 64bit
Source: "..\..\..\dist\filter\OnlyCamFilter32.dll"; DestDir: "{app}\filter"; Flags: ignoreversion regserver 32bit

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\OnlyCam.exe"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\OnlyCam.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Run]
Filename: "{app}\OnlyCam.exe"; Description: "Запустить OnlyCam"; Flags: nowait postinstall skipifsilent
