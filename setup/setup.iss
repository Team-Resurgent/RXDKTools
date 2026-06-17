#define MyAppName "Xbox Neighborhood"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Team Resurgent"
#define MyAppURL "https://github.com/Team-Resurgent/XboxNeighborhood"
#define MyAppId "F3A8C2E1-9B4D-4F6A-8E2C-1D5B7A9C0E3F"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
OutputDir=..\out\bin\x64\Release
OutputBaseFilename=XboxNeighborhood-Setup
SetupIconFile=Icon.ico
WizardImageFile=WizardImage.bmp
WizardSmallImageFile=WizardSmallImage.bmp
UninstallDisplayIcon={app}\xbshlext.dll,13
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=
UsePreviousPrivileges=no
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\out\bin\x64\Release\xbshlext.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{win}\explorer.exe"; Parameters: "shell:::{{DB15FEDD-96B8-4DA9-97E0-7E5CCA05CC44}}"; IconFilename: "{app}\xbshlext.dll"; IconIndex: 13
Name: "{commondesktop}\{#MyAppName}"; Filename: "{win}\explorer.exe"; Parameters: "shell:::{{DB15FEDD-96B8-4DA9-97E0-7E5CCA05CC44}}"; IconFilename: "{app}\xbshlext.dll"; IconIndex: 13

[Code]
var
  NeedExplorerRestart: Boolean;
const
  UninstallRegRoot = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\';
  UninstallRegKey = '{#MyAppId}_is1';
  ShellExtClsidKey = 'SOFTWARE\Classes\CLSID\{{DB15FEDD-96B8-4DA9-97E0-7E5CCA05CC44}}\InprocServer32';

function UninstallRegPath(const SubKey: String): String;
begin
  Result := UninstallRegRoot + SubKey;
end;

function QueryInstallPath(const SubKey: String; var Path: String): Boolean;
begin
  Result :=
    RegQueryStringValue(HKLM, UninstallRegPath(SubKey), 'InstallLocation', Path) or
    RegQueryStringValue(HKLM, UninstallRegPath(SubKey), 'Inno Setup: App Path', Path);
  if Result then
    Path := RemoveBackslashUnlessRoot(Path);
end;

function ExistingInstallPath(): String;
var
  Path: String;
  LegacySubKeys: array[0..1] of String;
  I: Integer;
begin
  if QueryInstallPath(UninstallRegKey, Path) then
  begin
    Result := Path;
    Exit;
  end;

  LegacySubKeys[0] := '{{F3A8C2E1-9B4D-4F6A-8E2C-1D5B7A9C0E3F}}_is1';
  LegacySubKeys[1] := '{{F3A8C2E1-9B4D-4F6A-8E2C-1D5B7A9C0E3F}}}_is1';
  for I := 0 to 1 do
  begin
    if QueryInstallPath(LegacySubKeys[I], Path) then
    begin
      Result := Path;
      Exit;
    end;
  end;

  Result := '';
end;

procedure RequireAdminInstallMode;
begin
  if not IsAdminInstallMode then
    RaiseException('Administrator privileges are required.');
end;

procedure StopExplorer;
var
  ResultCode: Integer;
begin
  Exec('taskkill.exe', '/F /IM explorer.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(2000);
  NeedExplorerRestart := True;
end;

procedure StartExplorer;
var
  ResultCode: Integer;
begin
  Sleep(1000);
  if Exec(ExpandConstant('{cmd}'), '/c start "" explorer.exe', ExpandConstant('{win}'), SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    NeedExplorerRestart := False
  else
    Log('Failed to restart Explorer.');
end;

procedure EnsureExplorerRunning;
begin
  if NeedExplorerRestart then
    StartExplorer;
end;

procedure RestartExplorer;
begin
  StopExplorer;
  StartExplorer;
end;

function RunRegsvr32(const DllPath: String; Unregister: Boolean): Integer;
var
  ResultCode: Integer;
  Params: String;
begin
  Params := '/s';
  if Unregister then
    Params := Params + ' /u';
  Params := Params + ' "' + DllPath + '"';

  if Exec(ExpandConstant('{sys}\regsvr32.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Result := ResultCode
  else
    Result := 1;
end;

function Regsvr32ErrorMessage(const Action: String; ExitCode: Integer): String;
begin
  if IsAdminInstallMode then
    Result := Format('%s failed (exit %d).', [Action, ExitCode])
  else
    Result := Format('%s failed (exit %d). Setup is not running with administrator privileges.', [Action, ExitCode]);
end;

procedure UnregisterShellExtension(const DllPath: String);
var
  ResultCode: Integer;
  ExplorerStopped: Boolean;
begin
  if (DllPath = '') or (not FileExists(DllPath)) then
    Exit;

  ExplorerStopped := False;
  try
    StopExplorer;
    ExplorerStopped := True;

    ResultCode := RunRegsvr32(DllPath, True);
    if ResultCode <> 0 then
      Log(Regsvr32ErrorMessage('regsvr32 /u', ResultCode));
  finally
    if ExplorerStopped then
      StartExplorer;
  end;
end;

procedure DeleteMatchingFiles(const Directory, Pattern: String);
var
  FindRec: TFindRec;
  Path: String;
begin
  if not FindFirst(Directory + '\' + Pattern, FindRec) then
    Exit;
  try
    repeat
      if ((FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) then
      begin
        Path := Directory + '\' + FindRec.Name;
        DeleteFile(Path);
      end;
    until not FindNext(FindRec);
  finally
    FindClose(FindRec);
  end;
end;

procedure RemoveXboxNeighborhoodShortcuts;
var
  Desktop: String;
begin
  Desktop := ExpandConstant('{commondesktop}');
  if DirExists(Desktop) then
    DeleteMatchingFiles(Desktop, 'Xbox Neighborhood*.lnk');
  Desktop := ExpandConstant('{autodesktop}');
  if DirExists(Desktop) then
    DeleteMatchingFiles(Desktop, 'Xbox Neighborhood*.lnk');
end;

function GetExistingUninstaller(var UninstallExe: String): Boolean;
var
  SubKeys: array[0..2] of String;
  UninstallString: String;
  I: Integer;
begin
  SubKeys[0] := UninstallRegKey;
  SubKeys[1] := '{{F3A8C2E1-9B4D-4F6A-8E2C-1D5B7A9C0E3F}}_is1';
  SubKeys[2] := '{{F3A8C2E1-9B4D-4F6A-8E2C-1D5B7A9C0E3F}}}_is1';

  for I := 0 to 2 do
  begin
    if RegQueryStringValue(HKLM, UninstallRegPath(SubKeys[I]), 'UninstallString', UninstallString) then
    begin
      UninstallExe := RemoveQuotes(UninstallString);
      if FileExists(UninstallExe) then
      begin
        Result := True;
        Exit;
      end;
    end;
  end;

  Result := False;
end;

function GetRegisteredDllPath: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM, ShellExtClsidKey, '', Result) then
  begin
    if not FileExists(Result) then
      Result := '';
  end;
end;

function GetExistingDllPath: String;
var
  InstallPath: String;
begin
  Result := GetRegisteredDllPath();
  if Result <> '' then
    Exit;

  InstallPath := ExistingInstallPath();
  if InstallPath = '' then
    Exit;

  Result := InstallPath + '\xbshlext.dll';
  if not FileExists(Result) then
    Result := '';
end;

function IsExistingInstall: Boolean;
var
  UninstallExe: String;
begin
  Result :=
    GetExistingUninstaller(UninstallExe) or
    (GetRegisteredDllPath() <> '') or
    (ExistingInstallPath() <> '');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExistingDll: String;
begin
  Result := '';
  NeedsRestart := False;

  RequireAdminInstallMode;

  if not IsExistingInstall then
    Exit;

  { Unregister in-process; do not run the old unins*.exe (it may be from a broken build). }
  ExistingDll := GetExistingDllPath();
  if ExistingDll <> '' then
    UnregisterShellExtension(ExistingDll);

  EnsureExplorerRunning;
end;

function RestartElevated: Boolean;
var
  ResultCode: Integer;
begin
  Result := ShellExec('runas', ExpandConstant('{srcexe}'), '', '', SW_SHOW, ewNoWait, ResultCode);
end;

function InitializeUninstall(): Boolean;
begin
  if not IsAdminInstallMode then
  begin
    if RestartElevated then
    begin
      Result := False;
      Exit;
    end;
    MsgBox('Administrator privileges are required to uninstall {#MyAppName}.', mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;

  Result := True;
end;

function InitializeSetup(): Boolean;
begin
  if not IsWin64 then
  begin
    MsgBox('This installer requires 64-bit Windows.', mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;

  if not IsAdminInstallMode then
  begin
    if RestartElevated then
    begin
      Result := False;
      Exit;
    end;
    MsgBox('Administrator privileges are required to install {#MyAppName}.', mbCriticalError, MB_OK);
    Result := False;
    Exit;
  end;

  Result := True;
end;

procedure InitializeWizard;
var
  Path: String;
begin
  if IsExistingInstall then
    WizardForm.WelcomeLabel2.Caption :=
      'Setup will remove the previous version, then install ' + '{#MyAppName}' + '.';

  Path := ExistingInstallPath();
  if Path <> '' then
    WizardForm.DirEdit.Text := Path;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectDir) and IsExistingInstall;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  DllPath: String;
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;

  RequireAdminInstallMode;

  DllPath := ExpandConstant('{app}\xbshlext.dll');
  ResultCode := RunRegsvr32(DllPath, False);
  if ResultCode <> 0 then
    RaiseException(Regsvr32ErrorMessage('regsvr32', ResultCode));

  EnsureExplorerRunning;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DllPath: String;
begin
  if CurUninstallStep <> usUninstall then
    Exit;

  RequireAdminInstallMode;

  DllPath := ExpandConstant('{app}\xbshlext.dll');
  UnregisterShellExtension(DllPath);
  RemoveXboxNeighborhoodShortcuts;
  EnsureExplorerRunning;
end;
