# Draws the .kpp file icon in Solution Explorer (docs/editors.md, "The .kpp file icon"): Images\kpp-file-SIZE.png,
# named by kpp.imagemanifest. A page with a folded corner, as Visual Studio's own file icons, and a blue badge
# with a white "k" at its bottom left, where the .cs icon has its "C#". Drawn on a 16-unit grid, scaled to each
# size; Visual Studio's image service inverts the page's grays for its dark themes, as for its own icons.
# Images\kpp-file.ico has the same pictures and a 256-pixel one, for File Explorer: the kpp installer gives it to
# .kpp files when asked (tools/release/kpp.iss).
#   powershell -NoProfile -ExecutionPolicy Bypass -File make-icons.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$out = "$PSScriptRoot\Images"
New-Item -ItemType Directory -Force $out | Out-Null

function Points($scale, [double[]]$xy)
{
  $points = @()
  for ($i = 0; $i -lt $xy.Length; $i += 2) { $points += New-Object System.Drawing.PointF ([float]($xy[$i] * $scale)), ([float]($xy[$i + 1] * $scale)) }
  , $points
}

$pictures = @()
foreach ($size in 16, 32, 48, 256)
{
  $s = $size / 16.0
  $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bitmap)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
  $g.Clear([System.Drawing.Color]::Transparent)
  $outline = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x42, 0x42, 0x42))
  $paper = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0xF6, 0xF6, 0xF6))
  $blue = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 0x00, 0x5F, 0xB8))
  $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)

  # The page: its outline one unit wide, the top right corner folded.
  $g.FillPolygon($outline, (Points $s @(3, 1, 10, 1, 14, 5, 14, 15, 3, 15)))
  $g.FillPolygon($paper, (Points $s @(4, 2, 9, 2, 9, 6, 13, 6, 13, 14, 4, 14)))
  $g.FillPolygon($paper, (Points $s @(10, 2.5, 12.5, 5, 10, 5)))

  # The badge, and its "k": a stem, an arm up to the right and a leg down to the right.
  $g.FillRectangle($blue, [float](0), [float](6 * $s), [float](10 * $s), [float](10 * $s))
  $g.FillRectangle($white, [float](2 * $s), [float](7 * $s), [float](2 * $s), [float](8 * $s))
  $g.FillPolygon($white, (Points $s @(4, 11, 6.6, 9, 9, 9, 5.4, 12.2, 4, 12.6)))
  $g.FillPolygon($white, (Points $s @(5.2, 11.4, 9.2, 15, 6.6, 15, 4, 12.6)))

  $g.Dispose()
  $png = New-Object System.IO.MemoryStream
  $bitmap.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
  $bitmap.Dispose()
  $pictures += , @($size, $png.ToArray())
  if ($size -le 48) { [System.IO.File]::WriteAllBytes("$out\kpp-file-$size.png", $png.ToArray()) }
}

# The .ico: its header, a directory entry per picture (a width and height of 0 is 256), then the pictures as PNGs.
$ico = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $ico
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$pictures.Count)
$offset = 6 + 16 * $pictures.Count
foreach ($picture in $pictures)
{
  $side = [byte]($picture[0] % 256)
  $writer.Write($side); $writer.Write($side); $writer.Write([byte]0); $writer.Write([byte]0)
  $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]$picture[1].Length); $writer.Write([uint32]$offset)
  $offset += $picture[1].Length
}
foreach ($picture in $pictures) { $writer.Write([byte[]]$picture[1]) }
$writer.Flush()
[System.IO.File]::WriteAllBytes("$out\kpp-file.ico", $ico.ToArray())
"make-icons.ps1: wrote $out\kpp-file-16.png, -32, -48 and kpp-file.ico"
