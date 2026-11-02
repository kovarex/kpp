; The Kpp installer (docs/build.md, "Installing"), built by package.ps1 with Inno Setup 6:
; ISCC /DVersion=X /DSource=<build\release\kpp> /O<output> kpp.iss
; Installs kpp.exe, stdlib\ and projectlib\ into Program Files\Kpp and puts that directory on the
; system PATH; the uninstaller takes it off again. When VS Code is found (`code` on the PATH), it
; offers to install the extension kpp.vsix into it, as the user who started the installer. It warns
; when no Visual Studio with its Clang component is found, which kpp compiles and links with.
; ExecAndCaptureOutput (the Visual Studio check) is Inno Setup 6.4's.
#if Ver < EncodeVer(6, 4, 0)
  #error Inno Setup 6.4 or later is needed
#endif
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

// vswhere's output lines (trimmed, empty ones dropped) for Params, through ExecAndCaptureOutput:
// no cmd, no redirection into a file. False when vswhere could not be run or failed; Reason then
// says how.
function Vswhere(const Locator, Params: string; var Lines: TArrayOfString; var Reason: string): Boolean;
var
  Code, I, N: Integer;
  Output: TExecOutput;
begin
  SetArrayLength(Lines, 0);
  Result := False;
  if not ExecAndCaptureOutput(Locator, Params, '', SW_HIDE, ewWaitUntilTerminated, Code, Output) then
    Reason := 'it could not be run: ' + SysErrorMessage(Code)
  else if Code <> 0 then
    Reason := 'it failed with exit code ' + IntToStr(Code)
  else if Output.Error then
    Reason := 'its output could not be read'
  else
    Result := True;
  if not Result then
    exit;
  N := 0;
  for I := 0 to GetArrayLength(Output.StdOut) - 1 do
    if Trim(Output.StdOut[I]) <> '' then
    begin
      SetArrayLength(Lines, N + 1);
      Lines[N] := Trim(Output.StdOut[I]);
      N := N + 1;
    end;
end;

// kpp's own search (clangPath in compiler/src/source.cpp): the newest Visual Studio, any edition,
// with the component Microsoft.VisualStudio.Component.VC.Llvm.Clang, and its
// VC\Tools\Llvm\x64\bin\clang.exe. When there is none, the warning names what was looked for and
// what was found.
function InitializeSetup(): Boolean;
var
  Locator, Found, Reason, Clang: string;
  Lines, All: TArrayOfString;
  I: Integer;
begin
  Result := True;
  Locator := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(Locator) then
    Found := 'Visual Studio''s locator ' + Locator + ' does not exist: no Visual Studio is installed.'
  else if not Vswhere(Locator, '-latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath', Lines, Reason) then
    Found := 'Asking ' + Locator + ' failed: ' + Reason + '.'
  else
  begin
    if GetArrayLength(Lines) > 0 then
    begin
      Clang := AddBackslash(Lines[0]) + 'VC\Tools\Llvm\x64\bin\clang.exe';
      if FileExists(Clang) then
        exit;
      Found := 'Visual Studio in ' + Lines[0] + ' has the component, but ' + Clang + ' does not exist.';
    end
    else
    begin
      Found := 'No Visual Studio has the component.';
      if Vswhere(Locator, '-products * -property installationPath', All, Reason) then
      begin
        if GetArrayLength(All) = 0 then
          Found := Found + ' Visual Studio''s locator lists no installation.'
        else
        begin
          Found := Found + ' The installations are:';
          for I := 0 to GetArrayLength(All) - 1 do
            Found := Found + #13#10 + '  ' + All[I];
        end;
      end;
    end;
  end;
  // Suppressible: a /SUPPRESSMSGBOXES install answers Yes and goes on (its log has the text).
  Result := SuppressibleMsgBox('Kpp compiles and links with the clang of a Visual Studio that has the component ' +
                   '"C++ Clang Compiler for Windows" (Microsoft.VisualStudio.Component.VC.Llvm.Clang). ' +
                   'Setup looked for one with ' + Locator + ':' + #13#10#13#10 + Found + #13#10#13#10 +
                   'Add the component with the Visual Studio Installer (Modify, Individual components) to ' +
                   'Visual Studio 2022 or its Build Tools. Install Kpp anyway?', mbConfirmation, MB_YESNO, IDYES) = IDYES;
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
