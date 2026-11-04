# Checks an installed kpp release as a user meets it (docs/build.md, "Continuous integration": the
# nightly release check, .github/workflows/release-check.yml): `kpp --version` names the release's
# version and commit and the bundled toolchain; `kpp new hello` and `kpp hello --run`; a project with
# a C library and a build.kpp setting (--set); each in debug and release with --toolchain bundled; and
# that the programs import only Windows' own DLLs; `kpp --target freebsd` with the release's sysroot; a project using httpclient and mariadb from the release's libraries\. PATH is cut to the installation, System32 and the
# Windows directory, and a Visual Studio environment (VSINSTALLDIR, INCLUDE, LIB...) or KPP_CLANG is a
# failure, not something to clear. -Summary appends the results to a Markdown file (the run's summary).
# Exit code 1 when anything failed; the last line says ALL PASSED or how many failed.
param([Parameter(Mandatory)][string]$Install, [Parameter(Mandatory)][string]$Version,
      [Parameter(Mandatory)][string]$Commit, [Parameter(Mandatory)][string]$Work, [string]$Summary = '')
$ErrorActionPreference = 'Stop'
$failed = 0
$lines = New-Object System.Collections.Generic.List[string]
function Note([string]$line) { Write-Host $line; $script:lines.Add($line) }
function Fail([string]$what) { Note "- FAIL: $what"; $script:failed++ }
function Pass([string]$what) { Note "- ok: $what" }

# Runs a command: its stdout and stderr together (as written: not PowerShell's error records), exit code.
function Run([string]$exe, [string[]]$arguments)
{
  $info = New-Object System.Diagnostics.ProcessStartInfo
  $info.FileName = $exe
  $info.Arguments = ($arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $info.UseShellExecute = $false
  $info.WorkingDirectory = (Get-Location).Path
  $info.RedirectStandardOutput = $true
  $info.RedirectStandardError = $true
  $p = [System.Diagnostics.Process]::Start($info)
  $errTask = $p.StandardError.ReadToEndAsync()
  $out = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  $text = (($out + $errTask.Result) -replace "`r", '').TrimEnd()
  Write-Host "> $exe $($info.Arguments) (exit $($p.ExitCode))"
  if ($text) { Write-Host $text }
  return [pscustomobject]@{ exit = $p.ExitCode; text = $text }
}

# The DLLs a PE32+ executable imports (its import directory, read from the file).
function Imports([string]$exe)
{
  $b = [IO.File]::ReadAllBytes($exe)
  $pe = [BitConverter]::ToInt32($b, 0x3C)
  $optional = $pe + 24
  if ([BitConverter]::ToUInt16($b, $optional) -ne 0x20B) { throw "$exe is not a PE32+ executable" }
  $sections = [BitConverter]::ToUInt16($b, $pe + 6)
  $table = $optional + [BitConverter]::ToUInt16($b, $pe + 20)
  $offset = {
    param([uint32]$rva)
    for ($i = 0; $i -lt $sections; $i++)
    {
      $s = $table + 40 * $i
      $va = [BitConverter]::ToUInt32($b, $s + 12)
      $size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
      if ($rva -ge $va -and $rva -lt $va + $size) { return [int]($rva - $va + [BitConverter]::ToUInt32($b, $s + 20)) }
    }
    throw "$exe has no section holding RVA $rva"
  }
  $names = @()
  $d = & $offset ([BitConverter]::ToUInt32($b, $optional + 120))
  while (($name = [BitConverter]::ToUInt32($b, $d + 12)) -ne 0)
  {
    $o = & $offset $name
    $e = $o
    while ($b[$e]) { $e++ }
    $names += [Text.Encoding]::ASCII.GetString($b, $o, $e - $o)
    $d += 20
  }
  return $names
}

# Runs `kpp ARGS` in the current directory, checks its exit code, its last line and the program the
# cell made (build\<configuration>\<name>.exe): it exists and imports only Windows' own DLLs.
function Build([string]$what, [string[]]$arguments, [string]$expected, [string]$program)
{
  $program = Join-Path (Get-Location).Path $program # (.NET's current directory is not PowerShell's)
  $r = Run 'kpp' $arguments
  $last = ($r.text -split "`n")[-1].Trim()
  if ($r.exit -ne 0 -or $last -ne $expected) { Fail "$what (kpp $($arguments -join ' ')): exit $($r.exit), last line '$last', not '$expected'"; return }
  if (-not (Test-Path $program)) { Fail "$what made no $program"; return }
  $foreign = @(Imports $program | Where-Object { $_ -notmatch '^(ucrtbase|api-ms-win-crt-[a-z0-9-]+|kernel32|user32|advapi32|shell32|ole32|ws2_32|ntdll)\.dll$' })
  if ($foreign.Count) { Fail "$what`: $program imports $($foreign -join ', '), which Windows does not have"; return }
  Pass "$what (kpp $($arguments -join ' ')): printed '$last'; $program imports $((Imports $program) -join ', ')"
}

Note "### kpp $Version, commit $Commit, installed in $Install"
$Install = $Install.TrimEnd('\')
foreach ($name in 'VSINSTALLDIR', 'VCINSTALLDIR', 'VCToolsInstallDir', 'WindowsSdkDir', 'DevEnvDir', 'INCLUDE', 'LIB', 'LIBPATH', 'KPP_CLANG')
{
  if (Test-Path "Env:$name") { Fail "$name is set ($((Get-Item "Env:$name").Value)): this is to be a machine without Visual Studio's environment" }
}
$env:PATH = "$Install;$env:SystemRoot\System32;$env:SystemRoot"
New-Item -ItemType Directory -Force $Work | Out-Null
Push-Location $Work
try
{
  $found = (Get-Command kpp -CommandType Application | Select-Object -First 1).Source
  if ($found -ne "$Install\kpp.exe") { Fail "kpp on PATH is '$found', not $Install\kpp.exe" }

  $r = Run 'kpp' @('--version')
  $first = ($r.text -split "`n")[0]
  if ($r.exit -ne 0 -or $first -ne "kpp $Version, commit $Commit") { Fail "kpp --version: exit $($r.exit), '$first', not 'kpp $Version, commit $Commit'" }
  else { Pass "kpp --version: $first" }
  if ($r.text -notmatch "(?m)^toolchain: $([regex]::Escape("$Install\bin\clang.exe")) \(bundled") { Fail "kpp --version does not name the bundled toolchain $Install\bin\clang.exe" }
  else { Pass "kpp --version: $(($r.text -split "`n") -match '^toolchain: ')" }

  if (Test-Path hello) { cmd /c rd /s /q hello }
  $r = Run 'kpp' @('new', 'hello')
  if ($r.exit -ne 0 -or -not (Test-Path 'hello\src\main.kpp')) { Fail "kpp new hello: exit $($r.exit)" }
  else
  {
    Pass 'kpp new hello'
    Push-Location hello
    try
    {
      foreach ($configuration in 'debug', 'release')
      {
        Build "hello, $configuration" @('hello', $configuration, '--toolchain', 'bundled', '--run') 'Hello from kpp' "build\$configuration\hello.exe"
      }
      # --verbose: kppc's report of each build, whose toolchain line says which clang it used.
      $r = Run 'kpp' @('hello', '--toolchain', 'bundled', '--verbose')
      $line = ($r.text -split "`n") -match '^toolchain: ' | Select-Object -First 1
      if ($r.exit -ne 0 -or $line -notlike "toolchain: $Install\bin\clang.exe (bundled, windows-gnu: links with lld)") { Fail "kpp hello --verbose: exit $($r.exit), its toolchain line '$line'" }
      else { Pass "kpp hello --verbose: $line" }
      $null = Run 'kpp' @('--stop-server')
    }
    finally { Pop-Location }
  }

  # FreeBSD as installed (2026-11-04): `kpp --target freebsd` with no --sysroot takes the release's
  # sysroots\freebsd-x86_64, which --version names, and links an ELF FreeBSD program (not run here).
  $r = Run 'kpp' @('--version')
  if ($r.text -notmatch "(?m)^sysroot freebsd-x86_64: $([regex]::Escape("$Install\sysroots\freebsd-x86_64")) \(FreeBSD 14\.3-RELEASE") { Fail "kpp --version does not name the release's FreeBSD sysroot $Install\sysroots\freebsd-x86_64" }
  else { Pass "kpp --version: $(($r.text -split "`n") -match '^sysroot freebsd-x86_64: ')" }
  Set-Content -Encoding ascii 'freebsd.kpp' "int main()`n{`n  println(`"Hello from kpp`");`n  return 0;`n}"
  Remove-Item 'freebsd.elf' -ErrorAction SilentlyContinue
  $r = Run 'kpp' @('--target', 'freebsd', '--exe', 'freebsd.elf', 'freebsd.kpp')
  $elf = if (Test-Path 'freebsd.elf') { [IO.File]::ReadAllBytes((Resolve-Path 'freebsd.elf').Path) } else { @() }
  if ($r.exit -ne 0 -or $elf.Count -lt 8 -or $elf[0] -ne 0x7f -or $elf[1] -ne 0x45 -or $elf[7] -ne 9) { Fail "kpp --target freebsd (no --sysroot): exit $($r.exit), no ELF FreeBSD program: $($r.text)" } # (EI_OSABI 9: FreeBSD)
  else { Pass 'kpp --target freebsd (no --sysroot): an ELF FreeBSD program, linked with the release''s sysroot' }

  # The official libraries as installed (2026-11-04): a project whose build.kpp uses httpclient and
  # mariadb by name alone takes them (and net) from the installation's libraries\, which --version names.
  $r = Run 'kpp' @('--version')
  if ($r.text -notmatch "(?m)^libraries: $([regex]::Escape("$Install\libraries")) \(.*\bhttpclient\b.*\bmariadb\b") { Fail "kpp --version does not name the installation's libraries $Install\libraries with httpclient and mariadb" }
  else { Pass "kpp --version: $(($r.text -split "`n") -match '^libraries: ')" }
  if (Test-Path uses-libraries) { cmd /c rd /s /q uses-libraries }
  New-Item -ItemType Directory -Force 'uses-libraries\src' | Out-Null
  Push-Location uses-libraries
  try
  {
    Set-Content -Encoding ascii 'build.kpp' "void build(Project& project)`n{`n  project.executable(`"app`", {`"src`"});`n  project.use(`"httpclient`");`n  project.use(`"mariadb`");`n}"
    Set-Content -Encoding ascii 'src\main.kpp' "int main()`n{`n  try`n  {`n    httpclient::fetch(`"ftp://example.test/`");`n  }`n  catch (const httpclient::HttpClientError& e)`n  {`n    println(`"mariadb `", mariadb::mariaDbQuote(`"it's`"), `" httpclient `", e.message);`n  }`n  return 0;`n}"
    Build 'httpclient and mariadb from the installation, debug' @('app', 'debug', '--toolchain', 'bundled', '--run') "mariadb 'it\'s' httpclient not an http or https address: ftp://example.test/" 'build\debug\app.exe'
    $null = Run 'kpp' @('--stop-server')
  }
  finally { Pop-Location }

  # A C library built from its source, with a define from a build.kpp setting (PLUS, --set PLUS=2).
  if (Test-Path clib) { cmd /c rd /s /q clib }
  New-Item -ItemType Directory -Force 'clib\src', 'clib\include', 'clib\libs\plus' | Out-Null
  Push-Location clib
  try
  {
    Set-Content -Encoding ascii 'build.kpp' @'
void build(Project& project)
{
  CLibrary& plus = project.cLibrary("plus");
  plus.source("libs/plus");
  plus.include("include");
  plus.define("PLUS=" + project.setting("PLUS", "1"));
  Executable& app = project.executable("app", {"src"});
  app.link("plus");
  app.cInclude("include");
}
'@
    Set-Content -Encoding ascii 'include\plus.h' 'int plus(int value);'
    Set-Content -Encoding ascii 'libs\plus\plus.c' "#include `"plus.h`"`nint plus(int value) { return value + PLUS; }"
    Set-Content -Encoding ascii 'src\main.kpp' "extern `"C`" import `"plus.h`";`nint main()`n{`n  println(`"plus `", plus(41));`n  return 0;`n}"
    Build 'a C library, debug' @('app', 'debug', '--toolchain', 'bundled', '--run') 'plus 42' 'build\debug\app.exe'
    Build 'a C library, release, --set PLUS=2' @('app', 'release', '--toolchain', 'bundled', '--set', 'PLUS=2', '--run') 'plus 43' 'build\release\app.exe'
    $null = Run 'kpp' @('--stop-server')
  }
  finally { Pop-Location }
}
catch { Fail "the check stopped: $_" }
finally { Pop-Location }

$result = if ($failed) { "release check: $failed FAILED (kpp $Version)" } else { "release check: ALL PASSED (kpp $Version)" }
if ($Summary) { Add-Content -Encoding utf8 $Summary (@($lines) + @('', "**$result**")) }
$result
exit [int]($failed -ne 0)
