; The kpp installer (docs/build.md, "Installing"), built by package.ps1 with Inno Setup 6:
; ISCC /DVersion=X /DSource=<build\release\kpp> /O<output> kpp.iss
; Installs kpp.exe, stdlib\ and projectlib\, and the bundled toolchain (bin\, include\, lib\,
; x86_64-w64-mingw32\: llvm-mingw, design.md 17) into Program Files\kpp and puts that directory on
; the system PATH; the uninstaller takes it off again. When VS Code is found (CodeCommand), it offers
; to install the VS Code extension kpp-vscode.vsix into it, as the user who started the installer
; (named for its editor: Windows opens every .vsix in Visual Studio's installer, which refuses a VS
; Code extension); when Visual Studio 2022 17.14 or later is found (VSIXInstaller), the Visual Studio
; extension kpp-visualstudio.vsix likewise. kpp needs nothing of Visual Studio's, so nothing is
; required.
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

; Earlier releases' name for the VS Code extension, which opened in Visual Studio's installer.
[InstallDelete]
Type: files; Name: "{app}\kpp.vsix"

; Only a package with kpp-vscode.vsix offers it (package.ps1 leaves it out by hand without Node.js).
; cmd /c ""...\code.cmd" --install-extension "...\kpp-vscode.vsix" --force": cmd takes off the outer
; quotes and keeps the inner ones.
#if FileExists(Source + "\kpp-vscode.vsix")
[Tasks]
Name: vscode; Description: "Install kpp-vscode.vsix, the kpp extension for VS Code (colouring and IntelliSense), into VS Code"; Check: VSCodeFound

[Run]
Filename: "{cmd}"; Parameters: "/c """"{code:CodeCommandOf}"" --install-extension ""{app}\kpp-vscode.vsix"" --force"""; Flags: runasoriginaluser runhidden waituntilterminated; Tasks: vscode; StatusMsg: "Installing the VS Code extension (kpp-vscode.vsix)..."
#endif

; The Visual Studio extension (tools/vs/vsix), for Visual Studio 2022 17.14 or later, through that
; Visual Studio's VSIXInstaller (VSIXInstaller), as the user who started the installer: an extension
; is the user's own.
#if FileExists(Source + "\kpp-visualstudio.vsix")
[Tasks]
Name: visualstudio; Description: "Install kpp-visualstudio.vsix, the kpp extension for Visual Studio (colouring and IntelliSense), into Visual Studio 2022 (restart it afterwards)"; Check: VSIXInstallerFound

[Run]
Filename: "{code:VSIXInstallerOf}"; Parameters: "/quiet ""{app}\kpp-visualstudio.vsix"""; Flags: runasoriginaluser waituntilterminated; Tasks: visualstudio; StatusMsg: "Installing the Visual Studio extension (kpp-visualstudio.vsix)..."
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

// VS Code's command line, bin\code.cmd: on the PATH, else where VS Code's user installer puts it
// (%LOCALAPPDATA%\Programs), else its system installer's (Program Files); '' when there is none.
function CodeCommand(): string;
var
  Path, Dir: string;
  At: Integer;
begin
  Result := '';
  Path := GetEnv('PATH') + ';';
  while (Result = '') and (Path <> '') do
  begin
    At := Pos(';', Path);
    Dir := RemoveQuotes(Trim(Copy(Path, 1, At - 1)));
    Delete(Path, 1, At);
    if (Dir <> '') and FileExists(AddBackslash(Dir) + 'code.cmd') then
      Result := AddBackslash(Dir) + 'code.cmd';
  end;
  if (Result = '') and FileExists(ExpandConstant('{localappdata}\Programs\Microsoft VS Code\bin\code.cmd')) then
    Result := ExpandConstant('{localappdata}\Programs\Microsoft VS Code\bin\code.cmd');
  if (Result = '') and FileExists(ExpandConstant('{commonpf64}\Microsoft VS Code\bin\code.cmd')) then
    Result := ExpandConstant('{commonpf64}\Microsoft VS Code\bin\code.cmd');
end;

// (CodeCommand for {code:...}, which passes a parameter)
function CodeCommandOf(Param: string): string;
begin
  Result := CodeCommand();
end;

function VSCodeFound(): Boolean;
begin
  Result := CodeCommand() <> '';
end;

var
  VSIXInstallerPath: string;
  VSIXInstallerLooked: Boolean;

// Visual Studio 2022's VSIXInstaller.exe, 17.14 or later (the extension's lowest), as vswhere finds
// it; '' when there is none. vswhere runs once.
function VSIXInstaller(): string;
var
  VsWhere, Found: string;
  Lines: TArrayOfString;
  Code: Integer;
begin
  if not VSIXInstallerLooked then
  begin
    VSIXInstallerLooked := True;
    VsWhere := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
    Found := ExpandConstant('{tmp}\vsixinstaller.txt');
    if FileExists(VsWhere) and
       Exec(ExpandConstant('{cmd}'), '/c ""' + VsWhere + '" -latest -version [17.14,18.0) -find Common7\IDE\VSIXInstaller.exe > "' + Found + '""',
            '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) and LoadStringsFromFile(Found, Lines) and (GetArrayLength(Lines) > 0) and
       FileExists(Trim(Lines[0])) then
      VSIXInstallerPath := Trim(Lines[0]);
  end;
  Result := VSIXInstallerPath;
end;

// (VSIXInstaller for {code:...}, which passes a parameter)
function VSIXInstallerOf(Param: string): string;
begin
  Result := VSIXInstaller();
end;

function VSIXInstallerFound(): Boolean;
begin
  Result := VSIXInstaller() <> '';
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
