# What native\build.cmd builds from, written to build\inputs.new: the Factorio checkout's commit (the
# library headers, the manifest and the icon the sources and factorio.rc name under C:\Factorio) and
# the hash of every file here. build.cmd skips the build when it equals the last build's
# build\inputs.stamp: rebuilt, the library differed by a few bytes each time, which made every other
# incremental link of the game a full one (docs/compile-speed.md, "The link in the edit loop").
$here = $PSScriptRoot
$lines = @("factorio " + (git -C C:\Factorio rev-parse HEAD))
if ($LASTEXITCODE -ne 0) { throw "git -C C:\Factorio rev-parse HEAD failed" }
# The tree the headers come from when FACTORIO names another (a snapshot of an older commit).
if ($env:FACTORIO) { $lines += "tree " + $env:FACTORIO }
$files = Get-ChildItem -Path $here -Recurse -File | Where-Object { $_.FullName -notlike "$here\build\*" } | Sort-Object FullName
foreach ($f in $files) { $lines += (Get-FileHash -Algorithm SHA1 -LiteralPath $f.FullName).Hash + " " + $f.FullName.Substring($here.Length + 1) }
$lines | Set-Content -Encoding ascii (Join-Path $here 'build\inputs.new')
