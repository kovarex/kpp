# kpp's colors in a real Visual Studio (docs/editors.md, "Colors"), against the table they are meant to have:
# opens colors-test in the experimental instance (kpp-visualstudio.vsix installed by test.ps1 before), and in the
# Light and the Dark theme takes a picture of the window, and for each line of main.kpp compares the colors drawn,
# pixel by pixel, with the colors the line's names and words should have. Prints one line per check and per
# theme, keeps the pictures (colors-light.png, colors-dark.png); the last line says whether all passed, and the
# exit code agrees. Not part of run_tests, which has no Visual Studio session.
#   colors-check.ps1 [-Suffix Exp] [-KppExe PATH] [-Work DIR]
param([string]$Suffix = 'Exp', [string]$KppExe, [string]$Work)
$ErrorActionPreference = 'Stop'
$test = [IO.File]::ReadAllText("$PSScriptRoot\test.ps1")
$a = $test.IndexOf('Add-Type @"'); $b = $test.IndexOf('[KppVsMessageFilter]::Register()')
Invoke-Expression $test.Substring($a, $b - $a)
[KppVsMessageFilter]::Register()
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Collections.Generic; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class ColorRows {
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public static Bitmap Capture(IntPtr window, int width, int height) {
    var b = new Bitmap(width, height, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(window, dc, 2); g.ReleaseHdc(dc); }
    return b;
  }
  // The lines of text in a part of the picture, top to bottom: each line's pixels by exact color (0xRRGGBB). Only the
  // colors asked for count (the block guides run between the lines; a stroke's edges are blends). The lines are
  // evenly spaced, count of them from the first scan line of the color measure to its last (the program begins and
  // ends with a comment; the text's color is also the caret's). background: the part's commonest color.
  public static List<Dictionary<int, int>> Rows(Bitmap b, Rectangle r, int[] ink, int measure, int count, out int background) {
    var wanted = new HashSet<int>(ink);
    var data = b.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    var pixels = new int[r.Width * r.Height];
    for (int y = 0; y < r.Height; ++y) Marshal.Copy(data.Scan0 + y * data.Stride, pixels, y * r.Width, r.Width);
    b.UnlockBits(data);
    var all = new Dictionary<int, int>();
    foreach (int p in pixels) { int c = p & 0xFFFFFF; int n; all.TryGetValue(c, out n); all[c] = n + 1; }
    background = 0; int best = -1;
    foreach (var kv in all) if (kv.Value > best) { best = kv.Value; background = kv.Key; }
    int first = -1, last = -1;
    for (int i = 0; i < pixels.Length; ++i) if ((pixels[i] & 0xFFFFFF) == measure) { int y = i / r.Width; if (first < 0) first = y; last = y; }
    var rows = new List<Dictionary<int, int>>();
    for (int i = 0; i < count; ++i) rows.Add(new Dictionary<int, int>());
    if (first < 0) return rows;
    // (A comment's ink ends about four fifths of a line below its top.) The grid that fits: the fewest colored
    // pixels at the seam between two lines.
    var inkPerLine = new int[r.Height];
    for (int i = 0; i < pixels.Length; ++i) if (wanted.Contains(pixels[i] & 0xFFFFFF)) ++inkPerLine[i / r.Width];
    double estimate = (last - first) / (count - 1 + 0.8), pitch = estimate, top = first;
    long fewest = long.MaxValue;
    for (double p = estimate - 1.5; p <= estimate + 1.5; p += 0.05)
      for (double t = first - 0.5 * p; t <= first + 0.2 * p; t += 0.5) {
        long seam = 0;
        for (int y = 0; y < r.Height; ++y) {
          if (inkPerLine[y] == 0) continue;
          double f = (y - t) / p; f -= Math.Floor(f);
          if (f < 0.04 || f > 0.96 || y < t || y >= t + count * p) seam += inkPerLine[y];
        }
        if (seam < fewest) { fewest = seam; pitch = p; top = t; }
      }
    for (int i = 0; i < pixels.Length; ++i) {
      int c = pixels[i] & 0xFFFFFF;
      if (!wanted.Contains(c)) continue;
      int line = Math.Max(0, Math.Min(count - 1, (int)((i / r.Width - top) / pitch)));
      int n; rows[line].TryGetValue(c, out n); rows[line][c] = n + 1;
    }
    return rows;
  }
}
'@

# Each role's color, as make-colors.ps1's table: Light Modern, Dark Modern (keyword: Visual Studio's own Keyword).
$roles = @{
  comment = 'kpp comment'; control = 'kpp control keyword'; namespace = 'kpp namespace'; type = 'kpp type'; enumerator = 'kpp enumerator'
  numberType = 'kpp number type'; global = 'kpp global'; local = 'kpp local'; number = 'kpp number'; member = 'kpp member'
  static = 'kpp static member'; parameter = 'kpp parameter'; method = 'kpp method'; function = 'kpp function'; string = 'kpp string'
  escape = 'kpp string escape'; operator = 'kpp operator'
}
$table = @{}
foreach ($m in [regex]::Matches([IO.File]::ReadAllText("$PSScriptRoot\make-colors.ps1"), "@\('(kpp [a-z ]+)',\s+'([0-9A-F]{6})', '([0-9A-F]{6})'\)")) { $table[$m.Groups[1].Value] = @($m.Groups[2].Value, $m.Groups[3].Value) }
function Palette($column)
{
  $p = @{ keyword = @('0000FF', '569CD6')[$column] }
  foreach ($r in $roles.Keys) { $p[$r] = $table[$roles[$r]][$column] }
  $p
}

# What each line of colors-test\main.kpp draws, beside the text's color. A row is a line that has ink.
$lines = @(
  'comment', 'keyword namespace', '', 'keyword type', '', 'enumerator', 'enumerator', '', 'numberType global number', 'keyword type', '',
  'keyword', 'keyword method numberType parameter member', 'numberType member number', 'keyword numberType static',
  'numberType method parameter', '', 'numberType local parameter', 'control local', 'control local number', 'control keyword method', '',
  'numberType method control type static', '', 'numberType type static number', '',
  'keyword type', 'numberType function type parameter', '', 'control parameter function', '',
  'namespace type global number', 'keyword global string escape', 'keyword global', 'numberType global keyword', 'numberType global operator',
  'namespace type global enumerator', 'keyword global namespace', 'keyword function', '', 'control numberType number global namespace', '', 'comment'
)

$ide = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
$root = (Resolve-Path "$PSScriptRoot\..\..\..").Path
if (-not $KppExe) { $KppExe = "$root\compiler\build\kpp.exe" }
if (-not $Work) { $Work = "$root\compiler\build\vs-colors" }
if (Test-Path $Work) { cmd /c rd /s /q "$Work" }
New-Item -ItemType Directory -Force $Work | Out-Null
Copy-Item "$PSScriptRoot\colors-test\main.kpp", "$PSScriptRoot\colors-test\build.kpp" $Work
$env:KPP_LSP_SERVER = $KppExe
$script:failed = 0
function Check($what, $ok, $detail)
{
  if ($ok) { "ok   $what ($detail)" } else { "FAIL $what ($detail)"; $script:failed++ }
}
# The newest log of the extension's (a second Visual Studio has a numbered one).
function LogLines { Get-ChildItem "$env:LOCALAPPDATA\kpp\visual-studio-lsp*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { Get-Content $_.FullName -ErrorAction SilentlyContinue } }

$devenv = Start-Process "$ide\devenv.exe" -ArgumentList '/rootsuffix', $Suffix, "`"$Work`"" -PassThru
$dte = $null; $t = [Diagnostics.Stopwatch]::StartNew()
while (-not $dte -and $t.Elapsed.TotalSeconds -lt 120) { Start-Sleep -Milliseconds 250; $dte = [KppVsRot]::Dte($devenv.Id) }
if (-not $dte) { "colors-check: FAILED: no DTE for devenv $($devenv.Id)"; exit 1 }
try
{
  Start-Sleep -Seconds 3
  $dte.ItemOperations.OpenFile("$Work\main.kpp") | Out-Null
  $dte.MainWindow.WindowState = 2 # (vsWindowStateMaximize)
  # The tokens of the file, the last thing colored.
  $tokens = $null
  $t = [Diagnostics.Stopwatch]::StartNew()
  while (-not $tokens -and $t.Elapsed.TotalSeconds -lt 90)
  {
    Start-Sleep -Milliseconds 500
    $tokens = @(LogLines | Where-Object { $_ -match '^kpp: semantic tokens in .*\\main\.kpp: ([0-9]+)$' -and [int]$Matches[1] -gt 0 }) | Select-Object -Last 1
  }
  Check 'semantic tokens of main.kpp' ([bool]$tokens) $(if ($tokens) { $tokens } else { 'none in the log' })

  # Large enough that a stroke has pixels in the color itself, where the edges' are blends.
  $fonts = $dte.Properties('FontsAndColors', 'TextEditor')
  $size = $fonts.Item('FontSize').Value
  $selection = $dte.ActiveDocument.Selection
  $selection.MoveToLineAndOffset(3, 1) # (on a line of the text's color, between the first and the last: Rows measures them)
  $themes = @(@('light', '{DE3DBBCD-F642-433C-8353-8F1DF4370ABA}', 0), @('dark', '{1DED0138-47CE-435E-84EF-9EC1F439B749}', 1))
  foreach ($theme in $themes)
  {
    # A theme is a setting that can be imported.
    $settings = "$Work\$($theme[0]).vssettings"
    Set-Content -Encoding UTF8 $settings @"
<UserSettings><ApplicationIdentity version="17.0"/><Category name="Environment_Group" RegisteredName="Environment_Group"><Category name="Environment_FontsAndColors" Category="{1EDA5DD4-927A-43a7-810E-7FD247D0DA1D}" Package="{DA9FB551-C724-11d0-AE1F-00A0C90FFFC3}" RegisteredName="Environment_FontsAndColors" PackageName="Visual Studio Environment Package"><PropertyValue name="Version">2</PropertyValue><FontsAndColors Version="2.0"><Theme Id="$($theme[1])"/></FontsAndColors></Category></Category></UserSettings>
"@
    $dte.ExecuteCommand('Tools.ImportandExportSettings', "/import:`"$settings`"")
    Start-Sleep -Seconds 3
    $fonts.Item('FontSize').Value = 18 # (after the import has finished: it sets the fonts back)
    Start-Sleep -Seconds 4
    $devenv.Refresh() # (the handle read before is the splash screen's)
    $handle = $devenv.MainWindowHandle
    $window = [Windows.Automation.AutomationElement]::FromHandle($handle)
    $view = $null
    # (the widest: the Output window has a text view too, and a hidden one has no size)
    foreach ($e in $window.FindAll([Windows.Automation.TreeScope]::Descendants, [Windows.Automation.Condition]::TrueCondition))
    {
      if ($e.Current.ClassName -ne 'WpfTextView' -or $e.Current.BoundingRectangle.IsEmpty) { continue }
      if (-not $view -or $e.Current.BoundingRectangle.Width -gt $view.Current.BoundingRectangle.Width) { $view = $e }
    }
    $outer = $window.Current.BoundingRectangle
    $inner = $view.Current.BoundingRectangle
    $picture = [ColorRows]::Capture($handle, [int]$outer.Width, [int]$outer.Height)
    $picture.Save("$Work\colors-$($theme[0]).png")
    $part = New-Object Drawing.Rectangle ([int]($inner.X - $outer.X)), ([int]($inner.Y - $outer.Y)), ([int]$inner.Width), ([int]$inner.Height)
    $part.Intersect((New-Object Drawing.Rectangle 0, 0, $picture.Width, $picture.Height))
    "$($theme[0]): the window $outer, the text $inner, the part read $part of a picture $($picture.Width) x $($picture.Height)"
    $palette = Palette $theme[2]
    $text = @('000000', 'DCDCDC')[$theme[2]]
    $ink = @($palette.Values + $text | Sort-Object -Unique | ForEach-Object { [Convert]::ToInt32($_, 16) })
    [int]$background = 0
    $rows = [ColorRows]::Rows($picture, $part, [int[]]$ink, [Convert]::ToInt32($palette['comment'], 16), $lines.Count, [ref]$background)
    $picture.Dispose()
    "$($theme[0]): background {0:X6}" -f $background
    # A palette color is drawn in a row when enough pixels have it exactly; the blends at a stroke's edges rarely do.
    $names = @{}
    foreach ($r in $palette.Keys) { $names[$palette[$r]] = (@($names[$palette[$r]], $r) | Where-Object { $_ }) -join '/' }
    for ($i = 0; $i -lt $lines.Count; $i++)
    {
      $want = @($lines[$i] -split ' ' | Where-Object { $_ } | ForEach-Object { $palette[$_] } | Sort-Object -Unique)
      $seen = @()
      # (Any pixel shows a color the line should have; another takes more, an edge's blend can equal a color by chance.)
      foreach ($k in $rows[$i].Keys) { $hex = '{0:X6}' -f $k; if ($names.ContainsKey($hex) -and ($rows[$i][$k] -ge 5 -or $want -contains $hex)) { $seen += $hex } }
      $seen = @($seen | Sort-Object -Unique)
      # (The text's own color may be one of the palette's: Light's operator is black.)
      $missing = @($want | Where-Object { $seen -notcontains $_ -and $_ -ne $text })
      $extra = @($seen | Where-Object { $want -notcontains $_ -and $_ -ne $text })
      $counts = ($rows[$i].Keys | ForEach-Object { "{0:X6}:{1}" -f $_, $rows[$i][$_] }) -join ' '
      Check "$($theme[0]) line $($i + 1)" (-not $missing -and -not $extra) ("want [{0}] seen [{1}]{2}{3} {4}" -f ($want -join ' '), ($seen -join ' '), $(if ($missing) { " missing $($missing -join ' ')" }), $(if ($extra) { " extra $(($extra | ForEach-Object { "$_=$($names[$_])" }) -join ' ')" }), $(if ($missing -or $extra) { "(pixels $counts)" }))
    }
  }
  $fonts.Item('FontSize').Value = $size
}
finally
{
  try { $dte.Documents.CloseAll(2) } catch { }
  $dte.Quit()
  if (-not $devenv.WaitForExit(60000)) { "colors-check: Visual Studio $($devenv.Id) did not quit" }
}
if ($script:failed) { "colors-check: FAILED ($($script:failed) checks)"; exit 1 }
'colors-check: ALL PASSED'
exit 0
