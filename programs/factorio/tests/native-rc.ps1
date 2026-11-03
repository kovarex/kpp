# libraries\kpp-native\factorio.rc compiles from its own directory alone (convert.cmd's check): its
# files (the manifest and the icons) copied into an empty directory with nothing around it, as a
# thread's copy has no data\ (setup-checkout.cmd), then llvm-rc there, as kpp builds a resource
# (project.cpp: cd to the script's directory, llvm-rc /fo). A file named outside the directory is
# "file not found".
param([Parameter(Mandatory = $true)][string]$Native,
      [string]$LlvmRc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\llvm-rc.exe')
$work = Join-Path $env:TEMP 'kpp-native-rc-test'
if (Test-Path $work) { cmd /c "rd /s /q `"$work`"" | Out-Null }
$alone = Join-Path $work 'a\b\kpp-native'
New-Item -ItemType Directory -Force $alone | Out-Null
Get-ChildItem -File $Native | Where-Object { $_.Extension -in '.rc', '.manifest', '.ico', '.png' } | Copy-Item -Destination $alone
$output = cmd /c "cd /d `"$alone`" && `"$LlvmRc`" /nologo /fo `"$work\factorio.res`" factorio.rc 2>&1"
$code = $LASTEXITCODE
$made = Test-Path (Join-Path $work 'factorio.res')
cmd /c "rd /s /q `"$work`"" | Out-Null
if ($code -ne 0 -or -not $made)
{
  "factorio.rc: llvm-rc failed in a directory of libraries\kpp-native's files alone (exit $code):"
  $output
  exit 1
}
"factorio.rc: compiles from libraries\kpp-native alone"
exit 0
