# kpp's scripts that load the whole machine (gate.cmd, run_tests.cmd, spec.cmd, the differential suite,
# convert.cmd, tests.cmd and the rest under programs\factorio) run through heavy.cmd, which runs this. The
# heavy-job lock itself is not part of kpp (kovarex, 2026-11-24: its own repository,
# github.com/kovarex/heavy-queue): a machine that has one has it at C:\kpp-sandbox\heavy.cmd, and the command
# line goes to it as it came. Without one (CI, another machine) the lock's options are dropped and the
# command runs at once, with KPP_HEAVY_HELD set as the lock sets it, so the scripts it calls do not come
# here again. The exit code is the command's.
$line = $env:KPP_HEAVY_COMMAND
if ($null -eq $line) { $line = '' }
$line = $line.Trim()
$lock = 'C:\kpp-sandbox\heavy.cmd'
$info = New-Object System.Diagnostics.ProcessStartInfo
$info.FileName = $env:ComSpec
$info.UseShellExecute = $false
if ((Test-Path $lock) -and -not $env:GITHUB_ACTIONS)
{
  $info.Arguments = '/d /s /c ""' + $lock + '" ' + $line + '"'
}
else
{
  while ($true)
  {
    if ($line -match '^--status(\s+|$)') { 'no heavy-job lock on this machine'; exit 0 }
    if ($line -match '^--(exclusive|local|travel)(\s+|$)(.*)$') { $line = $matches[3]; continue }
    if ($line -match '^--(name|estimate)\s+"[^"]*"\s*(.*)$') { $line = $matches[2]; continue }
    if ($line -match '^--(name|estimate)\s+\S+\s*(.*)$') { $line = $matches[2]; continue }
    break
  }
  if (-not $line) { 'usage: heavy.cmd [--exclusive] [--name TEXT] [--estimate T] [--local or --travel] COMMAND LINE...'; exit 2 }
  $info.Arguments = '/d /s /c "' + $line + '"'
  $info.EnvironmentVariables['KPP_HEAVY_HELD'] = "$PID"
}
$process = [System.Diagnostics.Process]::Start($info)
$process.WaitForExit()
exit $process.ExitCode
