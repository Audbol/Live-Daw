; Inno Setup 6 script for Sidecar's Windows installer.
; Built by packaging/package-windows.ps1, which passes:
;   /DAppVersion=0.1.1-beta.2     the full version (shown in the installer and Apps list)
;   /DAppVersionNumeric=0.1.1     the numeric part (Windows file version info)
;   /DStageDir=<dir>              Sidecar.exe plus the docs and licences to install
;   /DOutputDir=<dir> /DOutputBaseName=Sidecar-<version>-Windows-x64-Setup

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is not defined
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef OutputBaseName
  #define OutputBaseName "Sidecar-Setup"
#endif

[Setup]
; Keep this AppId: upgrades and the uninstaller find earlier installs by it.
AppId={{6F0C5B7E-3D2A-4E8B-9C41-5A7D2E9B1F30}
AppName=Sidecar
AppVersion={#AppVersion}
AppVerName=Sidecar {#AppVersion}
AppPublisher=Sidecar contributors
AppCopyright=Copyright (C) Sidecar contributors. Licensed under the GNU AGPLv3.
VersionInfoVersion={#AppVersionNumeric}
VersionInfoProductName=Sidecar
VersionInfoProductTextVersion={#AppVersion}
DefaultDirName={autopf}\Sidecar
DefaultGroupName=Sidecar
DisableProgramGroupPage=yes
; Per-user install without admin rights is offered as an option.
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile={#SourcePath}..\..\resources\icon\sidecar.ico
UninstallDisplayIcon={app}\Sidecar.exe
UninstallDisplayName=Sidecar
#if FileExists(AddBackslash(StageDir) + "LICENSE")
LicenseFile={#AddBackslash(StageDir)}LICENSE
#endif
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#AddBackslash(StageDir)}*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{autoprograms}\Sidecar"; Filename: "{app}\Sidecar.exe"
Name: "{autodesktop}\Sidecar"; Filename: "{app}\Sidecar.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Sidecar.exe"; Description: "{cm:LaunchProgram,Sidecar}"; Flags: nowait postinstall skipifsilent
