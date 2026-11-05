# publish.ps1's forge part against a fake GitHub (an HttpListener on localhost answering from a script
# of responses), both ways a release is published, each call failing once the way GitHub's did on
# 2026-11-05 (a 500 creating the release, an upload that failed): publish.ps1 tries them again
# (-RetryWait 0) and the release is whole. Run by run_tests (tools\release\publish-test.ps1).
#   powershell -File tools\release\publish-test.ps1
$ErrorActionPreference = 'Stop'
$publish = Join-Path $PSScriptRoot 'publish.ps1'
$work = Join-Path ([IO.Path]::GetTempPath()) ("kpp-publish-test-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
Set-Content "$work\a.zip" 'a'
Set-Content "$work\b.zip" 'b'
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

function Publish([string]$name, [object[]]$rules, [string[]]$expected) {
  # The fake forge started and listening before publish.ps1 runs: a job's process can take more than
  # the 10 s once waited for on a loaded machine (other threads' gates; publish.ps1 then met no server,
  # "Unable to connect to the remote server", 2026-11-05), and a random port can be taken or in one of
  # Windows' excluded ranges: up to 5 ports, each waited for until it answers a request (/ping: a
  # listener that started is not always one that can be reached, 2 of 20 under 10 runs at once), its
  # job ends, or 60 s pass.
  $job = $null
  for ($try = 1; $try -le 5 -and -not $job; ++$try) {
    $port = Get-Random -Minimum 49152 -Maximum 65000
    $log = "$work\$name.log"; $ready = "$work\$name.ready"
    Remove-Item $log, $ready -ErrorAction SilentlyContinue
    $up = "http://localhost:$port/upload/1/assets{?name,label}"
    $portRules = @($rules | ForEach-Object { , @($_[0], $_[1], @($_[2] | ForEach-Object { , @($_[0], $_[1].Replace('UPLOAD', $up)) })) })
    $started = Start-Job -ScriptBlock $server -ArgumentList $port, $portRules, $log, $ready
    $deadline = (Get-Date).AddSeconds(60)
    $answers = $false
    while (-not $answers -and $started.State -in 'NotStarted', 'Running' -and (Get-Date) -lt $deadline) {
      if (Test-Path $ready) {
        try { Invoke-WebRequest "http://localhost:$port/ping" -UseBasicParsing -TimeoutSec 5 | Out-Null; $answers = $true } catch { Start-Sleep -Milliseconds 200 }
      } else { Start-Sleep -Milliseconds 100 }
    }
    if ($answers) { $job = $started }
    else {
      Write-Output "publish-test ${name}: the fake forge on port $port did not answer (job $($started.State)): $(($started | Receive-Job -ErrorAction SilentlyContinue 2>&1) -join ' ')"
      $started | Stop-Job -PassThru | Remove-Job -Force
    }
  }
  if (-not $job) { Write-Output "FAIL publish-test ${name}: no fake forge answered on 5 ports"; $script:failed++; return }
  $env:KPP_FORGE_TOKEN = 'test'
  $ErrorActionPreference = 'Continue'
  # (-Command, as CI's step calls it: -File would not take two files as one -Files)
  $command = "& '$publish' -Files '$work\a.zip', '$work\b.zip' -Commit abc123 -Version 9.9.9 -ApiUrl http://localhost:$port -Repository o/r " +
             "-Tag v9.9.9 -Title 'kpp 9.9.9' -Notes n -Forge github -RetryWait 0"
  $output = & powershell -NoProfile -ExecutionPolicy Bypass -Command $command 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  try { Invoke-WebRequest "http://localhost:$port/stop" -UseBasicParsing -TimeoutSec 5 | Out-Null } catch { }
  $job | Wait-Job -Timeout 10 | Out-Null
  $job | Remove-Job -Force
  $calls = @(Get-Content $log -ErrorAction SilentlyContinue | ForEach-Object { $_ -replace '\.new-[0-9a-f]{8}', '.new' })
  if ($code -ne 0 -or ($calls -join "`n") -ne ($expected -join "`n")) {
    Write-Output "FAIL publish-test ${name}: exit $code`n$($output -join "`n")`n-- calls:`n$($calls -join "`n")`n-- expected:`n$($expected -join "`n")"
    $script:failed++
  }
}

try {
  # A new release: creating it fails once with a 500 and the first upload of a.zip once (GitHub's
  # upload error, a 403: run 42059133359); the release of the tag is looked up before the second POST, the half-made a.zip
  # deleted before the second upload.
  Publish 'new' @(
    @('GET', '^/repos/o/r/releases/tags/v9\.9\.9$', @(, @(404, '{"message":"Not Found"}'))),
    @('DELETE', '^/repos/o/r/git/refs/tags/v9\.9\.9$', @(, @(422, '{"message":"Reference does not exist"}'))),
    @('POST', '^/repos/o/r/releases$', @(@(500, '{"message":"Server Error"}'), @(201, '{"id":1,"upload_url":"UPLOAD","assets":[]}'))),
    @('POST', '^/upload/1/assets\?name=a\.zip$', @(@(403, '{"message":"Error updating policy"}'), @(201, '{"id":8,"name":"a.zip"}'))),
    @('POST', '^/upload/1/assets\?name=b\.zip$', @(, @(201, '{"id":9,"name":"b.zip"}'))),
    @('GET', '^/repos/o/r/releases/1/assets$', @(, @(200, '[{"id":7,"name":"a.zip"}]'))),
    @('DELETE', '^/repos/o/r/releases/assets/7$', @(, @(204, '')))
  ) @(
    'GET /repos/o/r/releases/tags/v9.9.9 404',
    'DELETE /repos/o/r/git/refs/tags/v9.9.9 422',
    'POST /repos/o/r/releases 500',
    'GET /repos/o/r/releases/tags/v9.9.9 404',
    'POST /repos/o/r/releases 201',
    'POST /upload/1/assets?name=a.zip 403',
    'GET /repos/o/r/releases/1/assets 200',
    'DELETE /repos/o/r/releases/assets/7 204',
    'POST /upload/1/assets?name=a.zip 201',
    'POST /upload/1/assets?name=b.zip 201')
  # A release updated in place: moving the tag fails once with a 503, deleting the old a.zip once
  # with a 500 after it was deleted (its second attempt's 404 is success).
  Publish 'in-place' @(
    @('GET', '^/repos/o/r/releases/tags/v9\.9\.9$', @(, @(200, '{"id":1,"upload_url":"UPLOAD","assets":[{"id":3,"name":"a.zip"}]}'))),
    @('PATCH', '^/repos/o/r/git/refs/tags/v9\.9\.9$', @(@(503, '{"message":"Unavailable"}'), @(200, '{}'))),
    @('POST', '^/upload/1/assets\?name=a\.zip\.new', @(, @(201, '{"id":11,"name":"a.zip.new"}'))),
    @('POST', '^/upload/1/assets\?name=b\.zip\.new', @(, @(201, '{"id":12,"name":"b.zip.new"}'))),
    @('DELETE', '^/repos/o/r/releases/assets/3$', @(@(500, '{"message":"Server Error"}'), @(404, '{"message":"Not Found"}'))),
    @('PATCH', '^/repos/o/r/releases/assets/1[12]$', @(, @(200, '{}'))),
    @('PATCH', '^/repos/o/r/releases/1$', @(, @(200, '{}')))
  ) @(
    'GET /repos/o/r/releases/tags/v9.9.9 200',
    'PATCH /repos/o/r/git/refs/tags/v9.9.9 503',
    'PATCH /repos/o/r/git/refs/tags/v9.9.9 200',
    'POST /upload/1/assets?name=a.zip.new 201',
    'POST /upload/1/assets?name=b.zip.new 201',
    'DELETE /repos/o/r/releases/assets/3 500',
    'DELETE /repos/o/r/releases/assets/3 404',
    'PATCH /repos/o/r/releases/assets/11 200',
    'PATCH /repos/o/r/releases/assets/12 200',
    'PATCH /repos/o/r/releases/1 200')
}
finally {
  cmd /c rd /s /q $work
}
if ($failed -eq 0) { Write-Output 'ok   publish-test: a release created and one updated in place, each failed call tried again' }
exit $failed
