<#
.SYNOPSIS
Checks the standard 1281x721 Cornell Box camera capture from --raster-gi-self-test.
.EXAMPLE
./scripts/Test-DdgiImageQuality.ps1 -CaptureDirectory artifacts/cornell-dxr
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$CaptureDirectory)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Measure-Plane([Drawing.Bitmap]$Image, [int]$Left, [int]$Top) {
    # Each half-open 432x152 region contains 54x19 complete 8x8 tiles.
    $columns = 54
    $rows = 19
    $tileSize = 8
    $count = $rows * $columns
    $tiles = [double[]]::new($count)
    $dot = [double[]]::new(6)
    $norm = [double[]]::new(6)
    $squared = 0.0
    for ($row = 0; $row -lt $rows; ++$row) {
        for ($col = 0; $col -lt $columns; ++$col) {
            $tile = 0.0
            for ($dy = 0; $dy -lt $tileSize; ++$dy) {
                for ($dx = 0; $dx -lt $tileSize; ++$dx) {
                    $p = $Image.GetPixel($Left + $col * $tileSize + $dx, $Top + $row * $tileSize + $dy)
                    $tile += 0.2126 * $p.R + 0.7152 * $p.G + 0.0722 * $p.B
                }
            }
            $tile /= $tileSize * $tileSize
            $tiles[$row * $columns + $col] = $tile
            # On this centered regular grid the six quadratic basis columns are
            # orthogonal. Project out the smooth gradient before measuring blotches.
            $x = $col - ($columns - 1) / 2.0
            $y = $row - ($rows - 1) / 2.0
            $xx = $x * $x - ($columns * $columns - 1) / 12.0
            $yy = $y * $y - ($rows * $rows - 1) / 12.0
            $basis = @(1.0, $x, $y, $xx, ($x * $y), $yy)
            for ($i = 0; $i -lt $basis.Count; ++$i) {
                $dot[$i] += $tile * $basis[$i]
                $norm[$i] += $basis[$i] * $basis[$i]
            }
            $squared += $tile * $tile
        }
    }
    $mean = $dot[0] / $count
    for ($i = 0; $i -lt $norm.Length; ++$i) {
        $squared -= $dot[$i] * $dot[$i] / $norm[$i]
    }
    $percent = if ($mean -gt 0) { 100.0 * [Math]::Sqrt([Math]::Max(0.0, $squared) / $count) / $mean }
               else { [double]::PositiveInfinity }
    # At 16 px (two tile centers) a quadratic's second difference is 2 * 2^2
    # times its quadratic coefficient. Remove that smooth curvature on each axis.
    $step = 2
    $smoothX = 2 * $step * $step * $dot[3] / $norm[3]
    $smoothY = 2 * $step * $step * $dot[5] / $norm[5]
    $curvatures = [double[]]::new(($rows - 2 * $step) * ($columns - 2 * $step))
    $i = 0
    for ($row = $step; $row -lt $rows - $step; ++$row) {
        for ($col = $step; $col -lt $columns - $step; ++$col) {
            $at = $row * $columns + $col
            $horizontal = $tiles[$at - $step] + $tiles[$at + $step] - 2 * $tiles[$at] - $smoothX
            $vertical = $tiles[$at - $step * $columns] + $tiles[$at + $step * $columns] - 2 * $tiles[$at] - $smoothY
            $curvatures[$i++] = [Math]::Max([Math]::Abs($horizontal), [Math]::Abs($vertical))
        }
    }
    [Array]::Sort($curvatures)
    $p99 = $curvatures[[int][Math]::Ceiling(0.99 * $curvatures.Length) - 1]
    $creasePercent = if ($mean -gt 0) { 100.0 * $p99 / $mean } else { [double]::PositiveInfinity }
    [pscustomobject]@{ Mean = $mean; ResidualPercent = $percent; CreaseP99Percent = $creasePercent }
}

function Measure-Sphere([Drawing.Bitmap]$Image) {
    # Interior of the left diffuse sphere; exclude its silhouette.
    $count = 0
    $nearBlack = 0
    $sum = 0.0
    for ($dy = -68; $dy -le 68; ++$dy) {
        for ($dx = -68; $dx -le 68; ++$dx) {
            if ($dx * $dx + $dy * $dy -ge 68 * 68) { continue }
            $p = $Image.GetPixel(480 + $dx, 360 + $dy)
            if ($p.R -le 2 -and $p.G -le 2 -and $p.B -le 2) { ++$nearBlack }
            $sum += 0.2126 * $p.R + 0.7152 * $p.G + 0.0722 * $p.B
            ++$count
        }
    }
    [pscustomobject]@{ Mean = $sum / $count; NearBlackPercent = 100.0 * $nearBlack / $count }
}

$path = Join-Path $CaptureDirectory 'ddgi-dxr-medium-indirect-ev3.bmp'
$image = [Drawing.Bitmap]::new((Get-Item -LiteralPath $path).FullName)
try {
    if ($image.Width -ne 1281 -or $image.Height -ne 721) {
        throw "The Cornell Box regions require a 1281x721 capture. Got $($image.Width)x$($image.Height) in $path"
    }
    $ceiling = Measure-Plane $image 425 13
    $floor = Measure-Plane $image 425 557
    $sphere = Measure-Sphere $image
} finally {
    $image.Dispose()
}

# Luma is measured in 8-bit display codes. Reject a dark, flat capture even
# when its normalized high-frequency residual is small.
$failed = @()
"Ceiling residual/mean {0:F2}% (limit <9%), mean luma {1:F2}" -f $ceiling.ResidualPercent, $ceiling.Mean
"Floor residual/mean {0:F2}% (limit <12%), mean luma {1:F2}" -f $floor.ResidualPercent, $floor.Mean
"Ceiling 16 px crease P99 {0:F2}% (limit <9%)" -f $ceiling.CreaseP99Percent
"Floor 16 px crease P99 {0:F2}% (limit <14%)" -f $floor.CreaseP99Percent
"Sphere near-black {0:F2}% (limit <1%), mean luma {1:F2}" -f $sphere.NearBlackPercent, $sphere.Mean
if ($ceiling.ResidualPercent -ge 9.0 -or $ceiling.Mean -le 2.0) { $failed += 'ceiling' }
if ($floor.ResidualPercent -ge 12.0 -or $floor.Mean -le 2.0) { $failed += 'floor' }
if ($ceiling.CreaseP99Percent -ge 9.0) { $failed += 'ceiling creases' }
if ($floor.CreaseP99Percent -ge 14.0) { $failed += 'floor creases' }
if ($sphere.NearBlackPercent -ge 1.0 -or $sphere.Mean -le 2.0) { $failed += 'sphere' }
if ($failed.Count) { throw "Cornell Box DDGI image quality failed for $($failed -join ', ') in $path" }
"Cornell Box DDGI image quality passed."
