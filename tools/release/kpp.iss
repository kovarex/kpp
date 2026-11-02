; The kpp installer (docs/build.md, "Installing"), built by package.ps1 with Inno Setup 6:
; ISCC /DVersion=X /DSource=<build\release\kpp> /O<output> kpp.iss
; Installs kpp.exe, stdlib\ and projectlib\, and the bundled toolchain (bin\, include\, lib\,
; x86_64-w64-mingw32\: llvm-mingw, design.md 17) into Program Files\kpp and puts that directory on
; the system PATH; the uninstaller takes it off again. When VS Code is found (`code` on the PATH), it
; offers to install the extension kpp.vsix into it, as the user who started the installer. Nothing
; of Visual Studio's is needed, so nothing is checked for.
; Its version is kpp's own, which package.ps1 passes (kppc --version; docs/build.md, "Versions").
#ifndef Version
  #error kpp.iss is built by package.ps1, which passes kpp's version (/DVersion)
#endif

[Setup]
AppId={{6B3D2A6E-58C1-4F0B-9E1D-4A7C2B9F0E31}
AppName=kpp
AppVersion={#Version}
AppPublisher=kpp
AppPublisherURL=https://github.com/kovarex/kpp
; Releases before 2026-11-02 installed into a capitalised folder; an upgrade moves it to Program
; Files\kpp: the folder is renamed and its PATH entry replaced (InstallDir, CurStepChanged).
DefaultDirName={code:InstallDir}
UsePreviousAppDir=no
DisableProgramGroupPage=yes
OutputBaseFilename=kpp-{#Version}-setup
; lzma2's best, solid, in 8 threads: 16 to 28 s and 45.2 MB on PC-PRACOVNA against 68 s and 44.7 MB
; in one thread (docs/build.md, "Installing", 2026-11-02; ultra64 runs out of memory in ISCC).
Compression=lzma2
LZMANumBlockThreads=8
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
ChangesEnvironment=yes
UninstallDisplayName=kpp {#Version}

[Files]
Source: "{#Source}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

; Only a package with kpp.vsix offers it (package.ps1 leaves it out by hand without Node.js).
#if FileExists(Source + "\kpp.vsix")
[Tasks]
Name: vscode; Description: "Install the kpp extension into VS Code (colouring and IntelliSense)"; Check: VSCodeFound

[Run]
Filename: "{cmd}"; Parameters: "/c code --install-extension ""{app}\kpp.vsix"" --force"; Flags: runasoriginaluser runhidden waituntilterminated; Tasks: vscode; StatusMsg: "Installing the VS Code extension..."
#endif

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

// The previous install's folder, as UsePreviousAppDir would keep it, except that the default folder
// of earlier releases, capitalised, is Program Files\kpp as written here.
function InstallDir(Param: string): string;
var
  Previous: string;
begin
  Result := ExpandConstant('{autopf}\kpp');
  if RegQueryStringValue(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B3D2A6E-58C1-4F0B-9E1D-4A7C2B9F0E31}_is1',
    'Inno Setup: App Path', Previous) and (CompareText(Previous, Result) <> 0) then
    Result := Previous;
end;

// Windows keeps a folder's case: a folder whose name differs from {app}'s only in case is renamed
// through a temporary name, and its PATH entry (compared without case) is taken off, for [Registry]
// to add {app} as written.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Dir, Old: string;
  FindRec: TFindRec;
begin
  if CurStep <> ssInstall then
    exit;
  Dir := ExpandConstant('{app}');
  Old := '';
  if FindFirst(Dir, FindRec) then
  begin
    if FindRec.Name <> ExtractFileName(Dir) then
      Old := ExtractFilePath(Dir) + FindRec.Name;
    FindClose(FindRec);
  end;
  if Old <> '' then
  begin
    // Halfway (the temporary name taken, the new one refused), the folder goes back to its old name,
    // so that the install updates it rather than a new, empty one.
    if not RenameFile(Old, Dir + '.renaming') then
      Log('Could not rename ' + Old + ' to ' + Dir + ': it keeps its name')
    else if not RenameFile(Dir + '.renaming', Dir) then
    begin
      Log('Could not rename ' + Dir + '.renaming to ' + Dir + ': back to ' + Old);
      RenameFile(Dir + '.renaming', Old);
    end;
    RemoveFromPath(Dir);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
