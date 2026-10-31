; The Kpp installer (docs/build.md, "Installing"), built by package.ps1 with Inno Setup 6:
; ISCC /DVersion=X /DSource=<build\release\kpp> /O<output> kpp.iss
; Installs kpp.exe, stdlib\ and projectlib\ into Program Files\Kpp and puts that directory on the
; system PATH; the uninstaller takes it off again. It warns when no Visual Studio with its Clang
; component is found, which kpp compiles and links with.
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

function InitializeSetup(): Boolean;
var
  Vswhere, Output: string;
  Code: Integer;
  Lines: TArrayOfString;
begin
  Result := True;
  Vswhere := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  Output := ExpandConstant('{tmp}\kpp-vswhere.txt');
  if FileExists(Vswhere) and
     Exec(ExpandConstant('{cmd}'), '/c ""' + Vswhere + '" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath > "' + Output + '""',
          '', SW_HIDE, ewWaitUntilTerminated, Code) and
     LoadStringsFromFile(Output, Lines) and (GetArrayLength(Lines) > 0) and (Trim(Lines[0]) <> '') then
    exit;
  Result := MsgBox('Kpp compiles and links with Visual Studio 2022''s clang, and no Visual Studio with ' +
                   '"C++ Clang Compiler for Windows" was found. Install it with the Visual Studio Installer ' +
                   '(Individual components). Install Kpp anyway?', mbConfirmation, MB_YESNO) = IDYES;
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
