<#
.SYNOPSIS
Checks the standard 1281x721 Cornell Box ceiling in --raster-gi-self-test captures.
.DESCRIPTION
Uses the medium-quality GI indirect BMPs at +3 EV. Use the checked-in Cornell
scene and camera framing, with the original scene materials and lighting.
The screen path is required. The DXR path is checked when captured and can be
required explicitly for a hardware-enabled test run.
.EXAMPLE
./scripts/Test-SsgiCeilingImageQuality.ps1 -CaptureDirectory artifacts/gi-d3d12 -RequireDxr
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CaptureDirectory,
    [switch]$RequireDxr
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Measure-ArcDrop([Drawing.Bitmap]$Image) {
    # 4x4 tiles over x=[532,700), y=[36,96); the entire region faces down.
    $columns = 42
    $rows = 15
    $tiles = [double[,]]::new($rows, $columns)
    for ($y = 0; $y -lt $rows; ++$y) {
        for ($x = 0; $x -lt $columns; ++$x) {
            $sum = 0.0
            for ($dy = 0; $dy -lt 4; ++$dy) {
                for ($dx = 0; $dx -lt 4; ++$dx) {
                    $p = $Image.GetPixel(532 + 4 * $x + $dx, 36 + 4 * $y + $dy)
                    $sum += 0.2126 * $p.R + 0.7152 * $p.G + 0.0722 * $p.B
                }
            }
            $tiles[$y,$x] = $sum / 16.0
        }
    }
    # A separable 1-2-1 filter suppresses residual pixel noise. Compare tile
    # centers 16 px apart and look for the largest fall in each ceiling row.
    $weights = @(1, 2, 1)
    $smoothed = [double[,]]::new($rows, $columns)
    $drops = [double[]]::new($rows - 2)
    for ($y = 1; $y -lt $rows - 1; ++$y) {
        for ($x = 1; $x -lt $columns - 1; ++$x) {
            $sum = 0.0
            for ($dy = 0; $dy -lt 3; ++$dy) {
                for ($dx = 0; $dx -lt 3; ++$dx) {
                    $tapY = $y + $dy - 1
                    $tapX = $x + $dx - 1
                    $sum += $weights[$dx] * $weights[$dy] * $tiles[$tapY,$tapX]
                }
            }
            $smoothed[$y,$x] = $sum / 16.0
            if ($x -ge 5) {
                $leftX = $x - 4
                $drop = $smoothed[$y,$leftX] - $smoothed[$y,$x]
                $drops[$y - 1] = [Math]::Max($drops[$y - 1], $drop)
            }
        }
    }
    [Array]::Sort($drops)
    return $drops[[int][Math]::Ceiling(0.90 * $drops.Length) - 1]
}

function Measure-MeanColor([Drawing.Bitmap]$Image, [int]$Left, [int]$Top, [int]$Width, [int]$Height) {
    $red = $green = $blue = 0.0
    for ($y = $Top; $y -lt $Top + $Height; ++$y) {
        for ($x = $Left; $x -lt $Left + $Width; ++$x) {
            $p = $Image.GetPixel($x, $y)
            $red += $p.R
            $green += $p.G
            $blue += $p.B
        }
    }
    $count = $Width * $Height
    [pscustomobject]@{ R = $red / $count; G = $green / $count; B = $blue / $count }
}

$failed = @()
foreach ($mode in 'screen', 'dxr') {
    $path = Join-Path $CaptureDirectory "ssgi-$mode-medium-indirect-ev3.bmp"
    if ($mode -eq 'dxr' -and !$RequireDxr -and !(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $image = [Drawing.Bitmap]::new((Get-Item -LiteralPath $path).FullName)
    try {
        if ($image.Width -ne 1281 -or $image.Height -ne 721) {
            throw "Cornell Box checks require 1281x721. Got $($image.Width)x$($image.Height) in $path"
        }
        $drop = Measure-ArcDrop $image
        $ball = Measure-MeanColor $image 488 56 88 48
        $redWall = Measure-MeanColor $image 420 44 40 44
        $greenWall = Measure-MeanColor $image 836 44 40 44
    } finally {
        $image.Dispose()
    }

    # Display codes at +3 EV. Guard against passing by suppressing the sphere
    # bounce or the indirect color from the neighboring walls.
    $dropLimit = if ($mode -eq 'dxr') { 6.0 } else { 10.0 }
    $luma = 0.2126 * $ball.R + 0.7152 * $ball.G + 0.0722 * $ball.B
    $redTint = $redWall.R - $redWall.G
    $greenTint = $greenWall.G - $greenWall.R
    "SSGI {0}: arc drop P90 {1:F2} (<{2:F0}), sphere region luma {3:F2} (>40), red/green tints {4:F2}/{5:F2} (>12)" -f
        $mode, $drop, $dropLimit, $luma, $redTint, $greenTint
    if ($drop -ge $dropLimit -or $luma -le 40.0 -or $redTint -le 12.0 -or $greenTint -le 12.0) {
        $failed += $mode
    }
}
if ($failed.Count) { throw "Cornell Box SSGI ceiling image quality failed for $($failed -join ', ')." }
'Cornell Box SSGI ceiling image quality passed.'
