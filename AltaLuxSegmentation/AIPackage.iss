; Inno Setup script for the optional AltaLux AI object-selection package.
; Packages the staging directory (AIPackageDir define) holding
; AltaLuxSegmentation.dll, the ONNX Runtime DLLs, the MobileSAM models, and
; the manifest/notices, and installs them directly beside AltaLux.dll in the
; IrfanView Plugins folder. The release workflow compiles this script with:
;   ISCC.exe /DAIPackageVersion=<tag-without-slashes> AIPackage.iss

#ifndef AIPackageVersion
  #error Pass the release tag with /DAIPackageVersion=<version>
#endif

#define RepoRoot RemoveBackslashUnlessRoot(ExtractFilePath(RemoveBackslash(SourcePath))) + "\"
#define AIPackageDir RepoRoot + "ai-package"
#define AppName "AltaLux AI object selection"

[Setup]
AppId={{7C1E9A24-5B3F-4D6A-8E2C-90F1A4B7D5E8}
AppName={#AppName}
AppVersion={#AIPackageVersion}
UninstallDisplayName={#AppName} (IrfanView x64 add-on)
DefaultDirName={code:GetDefaultPluginsDir}
DirExistsWarning=no
DisableProgramGroupPage=yes
WizardStyle=modern
; Admin by default (Program Files); /CURRENTUSER allows per-user IrfanView
; installs without elevation.
PrivilegesRequiredOverridesAllowed=commandline
; The AI files are x64-only and must not land in a 32-bit IrfanView folder.
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
Compression=lzma2/max
SolidCompression=yes
OutputDir={#RepoRoot}
OutputBaseFilename=AltaLux-AI-x64-{#AIPackageVersion}-setup

[Files]
; Flat staging directory; the loader expects every file beside AltaLux.dll,
; so no subfolders may be created.
Source: "{#AIPackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion

[Code]
const
  UninstallKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\';
  AppPathsKey = 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\';

function QueryRegString(const Root: Integer; const Key, Name: String;
  var Value: String): Boolean;
begin
  Result := RegQueryStringValue(Root, Key, Name, Value) and (Trim(Value) <> '');
end;

function FindIrfanView64Dir: String;
var
  value: String;
begin
  Result := '';
  // Official-installer uninstall entries. IrfanView 64-bit is probed in both
  // registry views because its setup may register under WOW6432Node; HKCU
  // covers per-user installs. The 32-bit-only entries are deliberately not
  // used: this package is x64-only.
  if QueryRegString(HKLM, UninstallKey + 'IrfanView64_is1', 'InstallLocation', value) or
     QueryRegString(HKCU, UninstallKey + 'IrfanView64_is1', 'InstallLocation', value) or
     QueryRegString(HKLM32, UninstallKey + 'IrfanView64_is1', 'InstallLocation', value) then
    Result := value;

  // App Paths registration, whose default value holds the exe path; portable
  // installs write no registry at all and fall through to the default folder.
  if (Result = '') and
     (QueryRegString(HKLM, AppPathsKey + 'i_view64.exe', '', value) or
      QueryRegString(HKCU, AppPathsKey + 'i_view64.exe', '', value)) then
  begin
    StringChangeEx(value, '"', '', True);
    Result := ExtractFileDir(value);
  end;

  if Result = '' then
    Result := 'C:\Program Files\IrfanView';
end;

function GetDefaultPluginsDir(Param: String): String;
begin
  Result := AddBackslash(FindIrfanView64Dir) + 'Plugins';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent then
  begin
    if not FileExists(AddBackslash(WizardDirValue) + 'AltaLux.dll') then
      Result := MsgBox(
        'AltaLux.dll was not found in:' + #13#10 +
        WizardDirValue + #13#10#13#10 +
        'The AI object-selection files must be installed into the Plugins ' +
        'folder of a 64-bit IrfanView installation that has the AltaLux ' +
        'plug-in installed.' + #13#10#13#10 +
        'Install into this folder anyway?',
        mbConfirmation, MB_YESNO) = IDYES;
  end;
end;
