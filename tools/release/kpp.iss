; The Kpp installer (docs/build.md, "Installing"), built by package.ps1 with Inno Setup 6:
; ISCC /DVersion=X /DSource=<build\release\kpp> /O<output> kpp.iss
; Installs kpp.exe, stdlib\ and projectlib\, and the bundled toolchain (bin\, include\, lib\,
; x86_64-w64-mingw32\: llvm-mingw, design.md 17) into Program Files\Kpp and puts that directory on
; the system PATH; the uninstaller takes it off again. When VS Code is found (`code` on the PATH), it
; offers to install the extension kpp.vsix into it, as the user who started the installer. Nothing
; of Visual Studio's is needed, so nothing is checked for.
#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppId={{6B3D2A6E-58C1-4F0B-9E1D-4A7C2B9F0E31}
AppName=Kpp
AppVersion={#Version}
AppPublisher=Kpp
AppPublisherURL=https://github.com/kovarex/kpp
DefaultDirName={autopf}\Kpp
DisableProgramGroupPage=yes
OutputBaseFilename=kpp-{#Version}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesEnvironment=yes
UninstallDisplayName=Kpp {#Version}

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Tasks]
Name: vscode; Description: "Install the Kpp extension into VS Code (colouring and IntelliSense)"; Check: VSCodeFound

[Run]
Filename: "{cmd}"; Parameters: "/c code --install-extension ""{app}\kpp.vsix"" --force"; Flags: runasoriginaluser runhidden waituntilterminated; Tasks: vscode; StatusMsg: "Installing the VS Code extension..."

[Registry]
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Check: NotOnPath(ExpandConstant('{app}'))

[Code]
function NotOnPath(Directory: string): Boolean;
var
  Path: string;
begin
  if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path) then
    Result := True
  else
    Result := Pos(';' + Uppercase(Directory) + ';', ';' + Uppercase(Path) + ';') = 0;
end;

function VSCodeFound(): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/c where code.cmd', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure RemoveFromPath(Directory: string);
var
  Path: string;
  At: Integer;
begin
  if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path) then
    exit;
  At := Pos(';' + Uppercase(Directory) + ';', ';' + Uppercase(Path) + ';');
  if At = 0 then
    exit;
  // At is where ';Directory;' starts in ';' + Path + ';': the entry and one separator go.
  if At = 1 then
    Delete(Path, 1, Length(Directory) + 1)
  else
    Delete(Path, At - 1, Length(Directory) + 1);
  RegWriteExpandStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Path);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
