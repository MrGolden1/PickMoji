; PickMoji per-user installer (Inno Setup).
;
; Installs into %LOCALAPPDATA%\Programs\PickMoji — no admin, no UAC prompt, and a
; user-writable location so the in-app self-updater's exe-swap keeps working.
; Built by build-installer.ps1, which supplies AppVersion / StagingDir / OutputDir
; via ISCC /D switches; the defaults below let the script also be opened directly.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StagingDir
  #define StagingDir "..\..\build-portable\PickMoji"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\build-portable"
#endif

[Setup]
; A fixed AppId so future versions upgrade in place rather than installing twice.
AppId={{7C9E6A2F-3B4D-4C8A-9E1B-2F5A8D3C6B10}
AppName=PickMoji
AppVersion={#AppVersion}
AppPublisher=MrGolden1
AppPublisherURL=https://github.com/MrGolden1/PickMoji
DefaultDirName={localappdata}\Programs\PickMoji
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; PickMoji is a tray app that ignores window-close messages, so Restart Manager
; can't shut it down. We terminate it ourselves in [Code] before touching files.
CloseApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=PickMoji-{#AppVersion}-Setup
SetupIconFile=..\..\assets\PickMoji.ico
UninstallDisplayIcon={app}\PickMoji.exe
UninstallDisplayName=PickMoji
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#StagingDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\PickMoji"; Filename: "{app}\PickMoji.exe"
Name: "{autodesktop}\PickMoji"; Filename: "{app}\PickMoji.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
Filename: "{app}\PickMoji.exe"; Parameters: "--background"; \
    Description: "Start PickMoji now"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Leftovers a self-update may have created in the install folder.
Type: files; Name: "{app}\PickMoji.exe.old"
Type: files; Name: "{app}\PickMoji.update-download.exe"

[Code]
{ A running PickMoji holds its own exe and Qt/CRT DLLs open, so neither an
  update-reinstall nor an uninstall can remove them. It runs from the tray and
  does not quit on a close message, so terminate it explicitly first. }
procedure StopPickMoji;
var
  rc: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM PickMoji.exe', '',
       SW_HIDE, ewWaitUntilTerminated, rc);
  Sleep(600); { let the OS release the file handles before we touch the files }
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopPickMoji;
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopPickMoji;
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { PickMoji manages its own "start with Windows" entry (default on); it cannot
    remove that on uninstall, so clear it here to avoid a stale autostart. }
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKEY_CURRENT_USER,
                   'Software\Microsoft\Windows\CurrentVersion\Run', 'PickMoji');
end;
