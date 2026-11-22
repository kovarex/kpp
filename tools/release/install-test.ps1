# The installer over a kpp in use (CI's package job, on a runner of its own: it installs into Program Files\kpp and
# puts it on the PATH). kovarex, 2026-11-22: with Visual Studio open, installing a new kpp stopped at "DeleteFile
# failed; code 5" on kpp.exe; its extension starts a stopped language server again at once, from the same kpp.exe.
# Here an editor's stand-in does the same: `kpp --lsp` started again whenever it ends with anything but 0. With it and
# a compile server running, the setup is run again over the first install and must succeed; the language server then
# runs the new kpp.exe (started after the second install began), no compile server of the old one runs, and no
# renamed kpp.exe is left. A failure prints the setup's log.
param([Parameter(Mandatory)][string]$Setup, [string]$FirstSetup = $Setup) # (FirstSetup: an earlier release's, an upgrade)
$ErrorActionPreference = 'Stop'
$work = Join-Path $env:RUNNER_TEMP 'install-test'
if (-not $env:RUNNER_TEMP) { throw 'install-test.ps1 installs into Program Files: run it on a CI runner only' }
New-Item -ItemType Directory -Force $work | Out-Null
$app = "$env:ProgramFiles\kpp"
$failed = 0
function Fail([string]$what) { "FAIL install-test: $what"; $script:failed++ }

function Install([string]$setupFile, [string]$log)
{
  $p = Start-Process -FilePath $setupFile -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/TASKS=""', "/LOG=`"$log`"" -PassThru
  if (-not $p.WaitForExit(600000)) { Stop-Process -Id $p.Id -Force; return 'timed out after 600 s' }
  return "exit $($p.ExitCode)"
}

$first = Install $FirstSetup "$work\first.log"
if ($first -ne 'exit 0' -or -not (Test-Path "$app\kpp.exe")) { Fail "the first install: $first"; Get-Content "$work\first.log" -Tail 40; exit 1 }

# The editor's stand-in: starts `kpp --lsp` (its stdin held open), and again at once whenever it ends with anything
# but 0, from the same path; each start's process id and time in starts.txt.
$editor = "$work\editor.ps1"
Set-Content -Encoding ascii $editor @'
param([string]$Exe, [string]$Starts)
while ($true)
{
  $info = New-Object Diagnostics.ProcessStartInfo $Exe, '--lsp'
  $info.UseShellExecute = $false; $info.RedirectStandardInput = $true; $info.RedirectStandardOutput = $true; $info.CreateNoWindow = $true
  try { $p = [Diagnostics.Process]::Start($info) } catch { Add-Content $Starts "failed $((Get-Date).ToString('o')) $_"; Start-Sleep -Milliseconds 200; continue }
  Add-Content $Starts "started $($p.Id) $((Get-Date).ToString('o'))"
  $p.WaitForExit()
  if ($p.ExitCode -eq 0) { break }
}
'@
$starts = "$work\starts.txt"
$editorProcess = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$editor`"", "`"$app\kpp.exe`"", "`"$starts`"" -PassThru -WindowStyle Hidden
for ($i = 0; $i -lt 100 -and -not (Test-Path $starts); $i++) { Start-Sleep -Milliseconds 100 }

# A compile server of the installed kpp.
Set-Content -Encoding ascii "$work\one.kpp" 'int main() { std::println(1); return 0; }'
& "$app\kpp.exe" --server --quiet --exe "$work\one.exe" "$work\one.kpp"
if ($LASTEXITCODE -ne 0) { Fail "a build through the installed kpp's compile server failed ($LASTEXITCODE)" }
function CompileServers() { @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains(' --serve ') -and $_.CommandLine.Contains("`"$app\kpp.exe`"") }) }
if ((CompileServers).Count -eq 0) { Fail 'no compile server of the installed kpp runs before the second install' }
$lsp = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq "$app\kpp.exe" -and $_.CommandLine -match '--lsp' })
if ($lsp.Count -ne 1) { Fail "$($lsp.Count) language servers of the installed kpp before the second install, 1 expected" }

$before = Get-Date
$second = Install $Setup "$work\second.log"
if ($second -ne 'exit 0') { Fail "the second install, kpp.exe in use: $second"; Get-Content "$work\second.log" -Tail 40 }
Start-Sleep 2 # (the editor's stand-in starts its server again)
$now = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match '--lsp' -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$app\", [StringComparison]::OrdinalIgnoreCase) })
if ($now.Count -ne 1 -or $now[0].ExecutablePath -ne "$app\kpp.exe") { Fail "the language servers after the second install: $(($now | ForEach-Object { $_.ExecutablePath }) -join ', '), one of $app\kpp.exe expected" }
elseif ($now[0].CreationDate -lt $before) { Fail "the language server after the second install was started before it ($($now[0].CreationDate)): the old kpp still serves" }
if ((CompileServers).Count -ne 0) { Fail 'a compile server of the old kpp still runs after the second install' }
$left = @(Get-ChildItem $app -Filter 'kpp.exe.*.old')
if ($left.Count -ne 0) { Fail "renamed kpp.exe left: $($left.Name -join ', ')" }
Get-Content $starts
# Visual Studio's extension, restarted when kpp.exe is not there, shows "the language server was not found" and starts
# no other: the old server must stop only once the new kpp.exe is in place (not by Inno Setup's Restart Manager).
$missed = @(Get-Content $starts | Where-Object { $_ -like 'failed *' })
if ($missed.Count -ne 0) { Fail "the language server was started again while kpp.exe was not there ($($missed.Count) times)" }

Stop-Process -Id $editorProcess.Id -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -match '--lsp' -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$app\", [StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
& "$app\kpp.exe" --stop-all-servers | Out-Null
if ($failed -eq 0) { 'ok   install-test: the setup over a kpp in use (a language server its editor starts again at once, a compile server) installs, and the new kpp serves' }
'the second install''s log:'; Get-Content "$work\second.log" -Tail 80
exit $failed
