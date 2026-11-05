# The kpp extension in a real VS Code (docs/editors.md, "VS Code"): installs kpp-vscode.vsix into a profile of
# its own, copies a project, opens it in VS Code with this directory as the test runner, and prints the
# checks NAME.js logged. Opens a window; the screenshot NAME.png is of that window only.
#   run.ps1 -Name cells -Project compiler\tests\lsp\cells -Target game
#   run.ps1 -Name game -Project C:\factorio-in-kpp -Target factorio
#   run.ps1 -Name libraries -Project C:\gorating (a project that uses kpp libraries)
param(
  [Parameter(Mandatory)][ValidateSet('cells', 'game', 'inactive', 'libraries')][string]$Name,
  [Parameter(Mandatory)][string]$Project,
  [string]$Target = $null,
  [string]$Configuration = 'debug',
  [string]$Kppc, # default: the checkout's compiler\build\kppc.exe
  [string]$Vsix, # default: tools\vscode\kpp-vscode.vsix (npm run package)
  [string]$Work, # default: compiler\build\vscode: the profile, the project's copy, the log
  [string]$Code = "$env:LOCALAPPDATA\Programs\Microsoft VS Code"
)
$ErrorActionPreference = 'Stop'
$checkout = "$PSScriptRoot\..\..\..\.."
if (-not $Kppc) { $Kppc = "$checkout\compiler\build\kppc.exe" }
if (-not $Vsix) { $Vsix = "$PSScriptRoot\..\..\kpp-vscode.vsix" }
if (-not $Work) { $Work = "$checkout\compiler\build\vscode" }
New-Item -ItemType Directory -Force "$Work\user", "$Work\ext" | Out-Null
$Work = (Resolve-Path $Work).Path
$Kppc = (Resolve-Path $Kppc).Path

cmd /c "`"$Code\bin\code.cmd`" --user-data-dir `"$Work\user`" --extensions-dir `"$Work\ext`" --install-extension `"$((Resolve-Path $Vsix).Path)`" --force >nul 2>nul"
if ($LASTEXITCODE) { throw "run.ps1: code --install-extension $Vsix failed ($LASTEXITCODE)" }

# A copy, without .git and build outputs: the checks write build\ into the project.
$copy = "$Work\$Name"
if (Test-Path $copy) { cmd /c "rd /s /q `"$copy`"" }
robocopy $Project $copy /E /XD .git build bin /XJ /NFL /NDL /NJH /NJS /NP /MT:16 | Out-Null
New-Item -ItemType Directory -Force "$copy\.vscode" | Out-Null
@{ 'kpp.path' = $Kppc; 'kpp.target' = $(if ($Target) { $Target } else { $null }); 'kpp.configuration' = $Configuration } |
  ConvertTo-Json | Set-Content "$copy\.vscode\settings.json" -Encoding utf8

$log = "$Work\$Name.log"
Remove-Item $log, "$log.screenshot" -ErrorAction SilentlyContinue
$env:KPP_HARNESS_LOG = $log
$p = Start-Process "$Code\Code.exe" -PassThru -ArgumentList @(
  '--user-data-dir', "`"$Work\user`"", '--extensions-dir', "`"$Work\ext`"",
  "--extensionDevelopmentPath=`"$PSScriptRoot`"", "--extensionTestsPath=`"$PSScriptRoot\$Name.js`"",
  '--disable-workspace-trust', '--skip-welcome', '--new-window', "`"$copy`"")

# NAME.js writes NAME.log.screenshot when the window shows what is worth a look. The window is drawn
# by itself (PrintWindow), never copied from the screen, where other windows may cover it.
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace KppHarness -Name User32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr dc, uint flags);
public struct RECT { public int L, T, R, B; }
'@
while (-not $p.HasExited)
{
  if (Test-Path "$log.screenshot")
  {
    Start-Sleep 2
    $w = (Get-Process Code | Where-Object { $_.MainWindowTitle -like '*Extension Development Host*' } | Select-Object -First 1).MainWindowHandle
    $r = New-Object KppHarness.User32+RECT
    if ($w -and [KppHarness.User32]::GetWindowRect($w, [ref]$r))
    {
      $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      $dc = $g.GetHdc()
      [KppHarness.User32]::PrintWindow($w, $dc, 2) | Out-Null
      $g.ReleaseHdc($dc)
      $g.Dispose()
      $bmp.Save("$Work\$Name.png")
    }
    Remove-Item "$log.screenshot"
  }
  Start-Sleep -Milliseconds 300
}
Get-Content $log
if (-not (Select-String -Path $log -Pattern '^(\[[0-9.]+s\] )?done, 0 failed' -Quiet)) { exit 1 }
exit 0
