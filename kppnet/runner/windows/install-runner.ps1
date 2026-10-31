<#
Sets up a Windows machine as kpp's CI runner for git.kpp.net (RUNBOOK.md, step 7). Run in an
elevated PowerShell on the runner machine (Windows Server 2022/2025 or Windows 10/11 Pro, 8+ cores,
32 GB, 200 GB SSD):

  .\install-runner.ps1 -Token <registration token> -Name kpp-win-1

The token comes from git.kpp.net > Site administration > Actions > Runners > Create new runner
(or the `kpp` organization's settings, to serve only its repositories); kovarex pastes it here.

What it does:
1. Installs what ci.yml and release.yml expect on GitHub's windows-2022 image: Visual Studio 2022
   Build Tools with the C++ workload and "C++ Clang Compiler for Windows" (compiler\build.cmd and
   kppc find it through vswhere), Git, Python 3 (spec.cmd), Node.js 20 (run_tests' LSP smoke test,
   the VS Code extension, and JavaScript actions such as actions/checkout), PowerShell 7 (pwsh
   steps) and Inno Setup (release.yml).
2. Gets the runner. -Kind forgejo (default) builds Forgejo Runner from source with Go, since its
   releases have Linux binaries only (v13.2.1, 2026-10-29); -Kind gitea downloads Gitea's
   act_runner, which ships a windows-amd64 binary, as the fallback. Which of the two works against
   Forgejo 15 on Windows is what RUNBOOK.md step 7's smoke test settles; record the result there.
3. Registers it with the label windows-2022:host, so ci.yml's `runs-on: windows-2022` runs on it
   unchanged. `host` means no isolation: jobs run as the runner's user directly on this machine.
   Use a machine that does nothing else, and require approval for pull requests from forks.
4. Runs it at startup as the local user kpprunner (not an administrator), via a scheduled task.
   The script asks for that user's password (made up by kovarex, kept in his password manager).
#>
param(
  [Parameter(Mandatory = $true)][string]$Token,
  [string]$Name = $env:COMPUTERNAME,
  [string]$Instance = "https://git.kpp.net",
  [ValidateSet("forgejo", "gitea")][string]$Kind = "forgejo",
  [string]$ForgejoRunnerVersion = "v13.2.1",
  [string]$GiteaRunnerVersion = "5.2.4",
  [string]$Labels = "windows-2022:host",
  [string]$Root = "C:\kpp-runner"
)
$ErrorActionPreference = "Stop"

function Winget([string]$id, [string[]]$extra = @()) {
  winget install --exact --id $id --silent --accept-package-agreements --accept-source-agreements @extra
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { throw "winget install $id failed ($LASTEXITCODE)" }  # -1978335189: already installed
}

# 1. Tools.
Winget "Microsoft.VisualStudio.2022.BuildTools" @("--override", "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended")
Winget "Git.Git"
Winget "Python.Python.3.12"
Winget "OpenJS.NodeJS.20"
Winget "Microsoft.PowerShell"
Winget "JRSoftware.InnoSetup"
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")

New-Item -ItemType Directory -Force $Root, "$Root\work" | Out-Null
Set-Location $Root

# 2. The runner binary.
$exe = "$Root\runner.exe"
if ($Kind -eq "forgejo") {
  Winget "GoLang.Go"
  $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine")
  if (-not (Test-Path "$Root\src")) { git clone --depth 1 --branch $ForgejoRunnerVersion https://code.forgejo.org/forgejo/runner.git "$Root\src" }
  Push-Location "$Root\src"
  $env:CGO_ENABLED = "0"
  go build -o $exe .
  if ($LASTEXITCODE -ne 0) { throw "go build of forgejo runner $ForgejoRunnerVersion failed; try -Kind gitea" }
  Pop-Location
} else {
  $url = "https://gitea.com/gitea/act_runner/releases/download/v$GiteaRunnerVersion/gitea-runner-$GiteaRunnerVersion-windows-amd64.exe"
  Invoke-WebRequest $url -OutFile $exe
  $sha = (Invoke-WebRequest "$url.sha256" -UseBasicParsing).Content.Split(" ")[0].Trim()
  # The .sha256 published next to the .exe may be of the .xz; compare and say so rather than guess.
  $got = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
  if ($sha -and $got -ne $sha.ToLower()) { Write-Warning "sha256 of $exe is $got, published ${sha}: check by hand before going on" }
}

# 3. Configuration and registration.
Copy-Item "$PSScriptRoot\config.yaml" "$Root\config.yaml" -Force   # this kit's settings replace the defaults
& $exe register --no-interactive --instance $Instance --token $Token --name $Name --labels $Labels --config "$Root\config.yaml"
if ($LASTEXITCODE -ne 0) { throw "registration failed" }

# 4. The user and the scheduled task.
$cred = Get-Credential -UserName "kpprunner" -Message "Password for the local user kpprunner that runs CI jobs"
if (-not (Get-LocalUser -Name "kpprunner" -ErrorAction SilentlyContinue)) {
  New-LocalUser -Name "kpprunner" -Password $cred.Password -PasswordNeverExpires -Description "kpp CI runner" | Out-Null
}
icacls $Root /grant "kpprunner:(OI)(CI)M" | Out-Null
$action = New-ScheduledTaskAction -Execute $exe -Argument "daemon --config `"$Root\config.yaml`"" -WorkingDirectory $Root
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) -StartWhenAvailable
Register-ScheduledTask -TaskName "kpp-runner" -Action $action -Trigger $trigger -Settings $settings `
  -User "kpprunner" -Password $cred.GetNetworkCredential().Password -RunLevel Limited -Force | Out-Null
Start-ScheduledTask -TaskName "kpp-runner"
Write-Host "Runner $Name registered with $Instance as $Labels and started. Check git.kpp.net > Site administration > Actions > Runners: it should be Idle."
