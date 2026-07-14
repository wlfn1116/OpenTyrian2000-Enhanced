# Generate the PS Vita LiveArea / VPK image assets as 8-bit INDEXED (palette) PNGs.
#
# WHY INDEXED: the Vita VPK promoter rejects 24-bit truecolor LiveArea PNGs (install fails
# at ~98% with 0x8010113D); it requires 8-bit palette PNGs. GDI+ cannot Clone() straight to
# an indexed format reliably, so we round-trip through the GIF encoder (which quantizes to a
# 256-colour adaptive palette) and re-save the palettized bitmap as PNG -- the PNG encoder
# then writes color-type 3 (indexed), which is exactly what the promoter wants.
#
# Source art is the Tyrian 2000 box art shared with the Switch build (switch/icon.jpg); if it
# is missing, a solid dark background is used. Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File vita/make_livearea.ps1
#
# Outputs (under vita/sce_sys): icon0.png 128x128, pic0.png 960x544,
#   livearea/contents/{bg0.png 840x500, startup.png 280x158}
Add-Type -AssemblyName System.Drawing

$root   = $PSScriptRoot
$srcPath = Join-Path $root "..\switch\icon.jpg"
$src = $null
if (Test-Path $srcPath) {
    try { $src = [System.Drawing.Image]::FromFile((Resolve-Path $srcPath)) } catch { $src = $null }
}

function Save-IndexedPng([int]$w, [int]$h, [string]$path) {
    $canvas = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::FromArgb(255, 8, 10, 24))
    if ($script:src -ne $null) {
        # Scale the source to COVER the target (fill, preserve aspect, centre-crop).
        $sr = $script:src.Width / $script:src.Height
        $tr = $w / $h
        if ($sr -gt $tr) { $dh = $h; $dw = [int][math]::Ceiling($h * $sr) }
        else             { $dw = $w; $dh = [int][math]::Ceiling($w / $sr) }
        $dx = [int](($w - $dw) / 2)
        $dy = [int](($h - $dh) / 2)
        $g.DrawImage($script:src, $dx, $dy, $dw, $dh)
    }
    $g.Dispose()

    # 32bpp -> GIF (adaptive 8bpp palette) -> reload as Format8bppIndexed -> save as indexed PNG.
    $ms = New-Object System.IO.MemoryStream
    $canvas.Save($ms, [System.Drawing.Imaging.ImageFormat]::Gif)
    $canvas.Dispose()
    $ms.Position = 0
    $indexed = New-Object System.Drawing.Bitmap($ms)

    $dir = Split-Path $path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $indexed.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)

    $fmt = $indexed.PixelFormat
    $indexed.Dispose()
    $ms.Dispose()
    Write-Host ("wrote {0}  ({1}x{2}, {3})" -f $path, $w, $h, $fmt)
}

Save-IndexedPng 128 128 (Join-Path $root "sce_sys\icon0.png")
Save-IndexedPng 960 544 (Join-Path $root "sce_sys\pic0.png")
Save-IndexedPng 840 500 (Join-Path $root "sce_sys\livearea\contents\bg0.png")
Save-IndexedPng 280 158 (Join-Path $root "sce_sys\livearea\contents\startup.png")

if ($src -ne $null) { $src.Dispose() }
Write-Host "LiveArea assets generated."
