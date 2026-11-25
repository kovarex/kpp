# settle.ps1 against a fake GitHub (publish-test.ps1's: an HttpListener on localhost answering from a
# script of responses): the calls GitHub's side fails are tried again (-RetryWait 0), an old release it
# never deletes is left to the next run without failing this one (run 45644587269, 2026-11-25: one
# "Server Error" to that DELETE in an outage of GitHub's made master's run red), and a delete it
# refuses fails it. Run by run_tests (tools\release\settle-test.ps1).
#   powershell -File tools\release\settle-test.ps1
$ErrorActionPreference = 'Stop'
$settle = Join-Path $PSScriptRoot 'settle.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("kpp-settle-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
$failed = 0

# The fake forge: each rule is METHOD, a regex over the path and query, and its responses in order
# (the last one repeats); every request is logged as "METHOD path?query STATUS".
$server = {
  param([int]$port, [object[]]$rules, [string]$log, [string]$ready)
  $listener = [Net.HttpListener]::new()
  $listener.Prefixes.Add("http://localhost:$port/")
  $listener.Start()
  Set-Content $ready 'ready'
  $used = @{}
  while ($listener.IsListening) {
    $context = $listener.GetContext()
    $request = $context.Request
    $path = $request.Url.PathAndQuery
    if ($path -eq '/stop') { $context.Response.Close(); break }
    if ($path -eq '/ping') { $context.Response.StatusCode = 200; $context.Response.Close(); continue }
    $status = 599; $body = '{"message":"no rule"}'
    for ($i = 0; $i -lt $rules.Count; ++$i) {
      $rule = $rules[$i]
      if ($request.HttpMethod -eq $rule[0] -and $path -match $rule[1]) {
        $n = [int]$used[$i]; $used[$i] = $n + 1
        $answer = $rule[2][[Math]::Min($n, $rule[2].Count - 1)]
        $status = $answer[0]; $body = $answer[1]
        break
      }
    }
    [IO.StreamReader]::new($request.InputStream).ReadToEnd() | Out-Null
    Add-Content $log "$($request.HttpMethod) $path $status"
    $bytes = [Text.Encoding]::UTF8.GetBytes($body)
    $context.Response.StatusCode = $status
    $context.Response.ContentType = 'application/json'
    $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    $context.Response.Close()
  }
  $listener.Stop()
}

# Three push releases, the newest Latest already: with -Keep 2, v0.2.1 (id 1) is the one to delete.
$releases = '[{"id":3,"tag_name":"v0.2.3","name":"kpp 0.2.3","draft":false,"prerelease":false},' +
            '{"id":2,"tag_name":"v0.2.2","name":"kpp 0.2.2","draft":false,"prerelease":false},' +
            '{"id":1,"tag_name":"v0.2.1","name":"kpp 0.2.1","draft":false,"prerelease":false}]'
$serverError = @(500, '{"message":"Server Error"}')

# settle.ps1 -Keep 2 against the rules: its exit code, the calls the forge got and a line of its output.
function Settle([string]$name, [object[]]$rules, [int]$exit, [string[]]$expected, [string]$says) {
  # (the fake forge started and answering before settle.ps1 runs, on one of up to 5 ports: publish-test.ps1's reasons)
  $job = $null
  for ($try = 1; $try -le 5 -and -not $job; ++$try) {
    $port = Get-Random -Minimum 49152 -Maximum 65000
    $log = "$work\$name.log"; $ready = "$work\$name.ready"
    Remove-Item $log, $ready -ErrorAction SilentlyContinue
    $started = Start-Job -ScriptBlock $server -ArgumentList $port, $rules, $log, $ready
    $deadline = (Get-Date).AddSeconds(60)
    $answers = $false
    while (-not $answers -and $started.State -in 'NotStarted', 'Running' -and (Get-Date) -lt $deadline) {
      if (Test-Path $ready) {
        try { Invoke-WebRequest "http://localhost:$port/ping" -UseBasicParsing -TimeoutSec 5 | Out-Null; $answers = $true } catch { Start-Sleep -Milliseconds 200 }
      } else { Start-Sleep -Milliseconds 100 }
    }
    if ($answers) { $job = $started }
    else {
      Write-Output "settle-test ${name}: the fake forge on port $port did not answer (job $($started.State)): $(($started | Receive-Job -ErrorAction SilentlyContinue 2>&1) -join ' ')"
      $started | Stop-Job -PassThru | Remove-Job -Force
    }
  }
  if (-not $job) { Write-Output "FAIL settle-test ${name}: no fake forge answered on 5 ports"; $script:failed++; return }
  $env:KPP_FORGE_TOKEN = 'test'
  $ErrorActionPreference = 'Continue'
  $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $settle -ApiUrl "http://localhost:$port" -Repository o/r -Keep 2 -RetryWait 0 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  try { Invoke-WebRequest "http://localhost:$port/stop" -UseBasicParsing -TimeoutSec 5 | Out-Null } catch { }
  $job | Wait-Job -Timeout 10 | Out-Null
  $job | Remove-Job -Force
  $calls = @(Get-Content $log -ErrorAction SilentlyContinue)
  if ($code -ne $exit -or ($calls -join "`n") -ne ($expected -join "`n") -or -not ($output | Where-Object { $_ -like "*$says*" })) {
    Write-Output "FAIL settle-test ${name}: exit $code, expected $exit and a line saying '$says'`n$($output -join "`n")`n-- calls:`n$($calls -join "`n")`n-- expected:`n$($expected -join "`n")"
    $script:failed++
  }
}

try {
  # The list fails once with a 502 and the old release's delete once with a 500: each tried again, the
  # release and its tag deleted.
  Settle 'again' @(
    @('GET', '^/repos/o/r/releases\?', @(@(502, '{"message":"Bad Gateway"}'), @(200, $releases))),
    @('GET', '^/repos/o/r/releases/latest$', @(, @(200, '{"id":3,"tag_name":"v0.2.3"}'))),
    @('DELETE', '^/repos/o/r/releases/1$', @($serverError, @(204, ''))),
    @('DELETE', '^/repos/o/r/git/refs/tags/v0\.2\.1$', @(, @(204, '')))
  ) 0 @(
    'GET /repos/o/r/releases?per_page=100&page=1&limit=50 502',
    'GET /repos/o/r/releases?per_page=100&page=1&limit=50 200',
    'GET /repos/o/r/releases/latest 200',
    'DELETE /repos/o/r/releases/1 500',
    'DELETE /repos/o/r/releases/1 204',
    'DELETE /repos/o/r/git/refs/tags/v0.2.1 204') 'settle: deleted v0.2.1 (0.2.1)'
  # A delete that failed after it was done: its second attempt's 404 is success.
  Settle 'done' @(
    @('GET', '^/repos/o/r/releases\?', @(, @(200, $releases))),
    @('GET', '^/repos/o/r/releases/latest$', @(, @(200, '{"id":3,"tag_name":"v0.2.3"}'))),
    @('DELETE', '^/repos/o/r/releases/1$', @($serverError, @(404, '{"message":"Not Found"}'))),
    @('DELETE', '^/repos/o/r/git/refs/tags/v0\.2\.1$', @(, @(204, '')))
  ) 0 @(
    'GET /repos/o/r/releases?per_page=100&page=1&limit=50 200',
    'GET /repos/o/r/releases/latest 200',
    'DELETE /repos/o/r/releases/1 500',
    'DELETE /repos/o/r/releases/1 404',
    'DELETE /repos/o/r/git/refs/tags/v0.2.1 204') 'settle: deleted v0.2.1 (0.2.1)'
  # Another release Latest, and the forge's side never deletes the old one (an outage): Latest is set, the
  # old release left to the next run after four attempts, its tag kept, and the run does not fail.
  Settle 'outage' @(
    @('GET', '^/repos/o/r/releases\?', @(, @(200, $releases))),
    @('GET', '^/repos/o/r/releases/latest$', @(, @(200, '{"id":2,"tag_name":"v0.2.2"}'))),
    @('PATCH', '^/repos/o/r/releases/3$', @($serverError, @(200, '{}'))),
    @('DELETE', '^/repos/o/r/releases/1$', @(, $serverError))
  ) 0 @(
    'GET /repos/o/r/releases?per_page=100&page=1&limit=50 200',
    'GET /repos/o/r/releases/latest 200',
    'PATCH /repos/o/r/releases/3 500',
    'PATCH /repos/o/r/releases/3 200',
    'DELETE /repos/o/r/releases/1 500',
    'DELETE /repos/o/r/releases/1 500',
    'DELETE /repos/o/r/releases/1 500',
    'DELETE /repos/o/r/releases/1 500') 'settle: v0.2.1 (0.2.1) not deleted (HTTP 500, 4 attempts): left to the next run'
  # A delete the forge refuses (the token may not): no second attempt, and the run fails.
  Settle 'refused' @(
    @('GET', '^/repos/o/r/releases\?', @(, @(200, $releases))),
    @('GET', '^/repos/o/r/releases/latest$', @(, @(200, '{"id":3,"tag_name":"v0.2.3"}'))),
    @('DELETE', '^/repos/o/r/releases/1$', @(, @(403, '{"message":"Resource not accessible by integration"}')))
  ) 1 @(
    'GET /repos/o/r/releases?per_page=100&page=1&limit=50 200',
    'GET /repos/o/r/releases/latest 200',
    'DELETE /repos/o/r/releases/1 403') '(403) Forbidden'
}
finally {
  cmd /c rd /s /q $work
}
if ($failed -eq 0) { Write-Output 'ok   settle-test: failed calls tried again, an old release the forge never deletes left to the next run, a refused delete a failure' }
exit $failed
