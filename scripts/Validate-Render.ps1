[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$CaptureDirectory = '',
    [switch]$SkipCapture
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path $PSScriptRoot -Parent
if (!$Executable) { $Executable = Join-Path $repositoryRoot 'bin/windows-x86_64/Debug/RestArtEngine.exe' }
if (!$CaptureDirectory) { $CaptureDirectory = Join-Path $repositoryRoot 'artifacts/validation' }
$Executable = [IO.Path]::GetFullPath($Executable)
$CaptureDirectory = [IO.Path]::GetFullPath($CaptureDirectory)
[IO.Directory]::CreateDirectory($CaptureDirectory) | Out-Null

if (!$SkipCapture) {
    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Build the sample first: $Executable" }
    $captureStarted = [DateTime]::UtcNow
    & $Executable --self-test --capture-dir $CaptureDirectory
    if ($LASTEXITCODE -ne 0) { throw "GPU self-test exited with code $LASTEXITCODE" }
}

# Read the BMP explicitly: screenshot captures are uncompressed BGRA32. Ignore alpha,
# honor the data offset and signed height, and normalize both orientations to top down.
# No Python, image-processing package, or other external dependency is required.
if (!('RAEngineValidation.Image' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;

namespace RAEngineValidation {
    public sealed class Image {
        public readonly int Width, Height;
        readonly byte[] pixels;
        public Image(string path) {
            byte[] bytes = File.ReadAllBytes(path);
            if (bytes.Length < 54 || bytes[0] != 66 || bytes[1] != 77 ||
                BitConverter.ToInt32(bytes, 14) < 40 || BitConverter.ToInt16(bytes, 26) != 1 ||
                BitConverter.ToInt16(bytes, 28) != 32 || BitConverter.ToInt32(bytes, 30) != 0)
                throw new InvalidDataException("Expected an uncompressed 32-bit BMP: " + path);
            int offset = BitConverter.ToInt32(bytes, 10);
            Width = BitConverter.ToInt32(bytes, 18);
            int signedHeight = BitConverter.ToInt32(bytes, 22);
            Height = Math.Abs(signedHeight);
            if (Width <= 0 || Height <= 0 || (long)offset + (long)Width * Height * 4 > bytes.Length)
                throw new InvalidDataException("Invalid BMP dimensions or payload: " + path);
            pixels = new byte[checked(Width * Height * 4)];
            for (int y = 0; y < Height; ++y) {
                int sourceY = signedHeight < 0 ? y : Height - y - 1;
                Buffer.BlockCopy(bytes, offset + sourceY * Width * 4, pixels, y * Width * 4, Width * 4);
            }
        }
        public double[] Statistics(double left, double top, double right, double bottom) {
            double sum = 0, squared = 0;
            long count = 0;
            var colors = new HashSet<int>();
            for (int y = (int)(top * Height); y < (int)(bottom * Height); ++y)
                for (int x = (int)(left * Width); x < (int)(right * Width); ++x) {
                    int p = (y * Width + x) * 4;
                    colors.Add((pixels[p] >> 3) | ((pixels[p+1] >> 3) << 5) | ((pixels[p+2] >> 3) << 10));
                    for (int c = 0; c < 3; ++c) { double v = pixels[p+c]; sum += v; squared += v*v; ++count; }
                }
            double mean = sum / count;
            return new double[] { mean, Math.Sqrt(Math.Max(0, squared / count - mean * mean)), colors.Count };
        }
        public double[] Difference(Image other) {
            return Difference(other, 0, 0, 1, 1);
        }
        public double[] Difference(Image other, double left, double top, double right, double bottom) {
            if (Width != other.Width || Height != other.Height) throw new InvalidDataException("Comparison dimensions differ");
            double absolute = 0;
            long changed3 = 0, changed12 = 0;
            int maximum = 0;
            int x0 = (int)(left * Width), x1 = (int)(right * Width);
            int y0 = (int)(top * Height), y1 = (int)(bottom * Height);
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                int p = (y * Width + x) * 4;
                int pixelMaximum = 0;
                for (int c = 0; c < 3; ++c) {
                    int delta = Math.Abs((int)pixels[p+c] - other.pixels[p+c]);
                    absolute += delta;
                    pixelMaximum = Math.Max(pixelMaximum, delta);
                }
                maximum = Math.Max(maximum, pixelMaximum);
                if (pixelMaximum > 3) ++changed3;
                if (pixelMaximum > 12) ++changed12;
            }
            double count = (double)(x1 - x0) * (y1 - y0);
            return new double[] { absolute / (count * 3), changed3 / count, changed12 / count, maximum };
        }
    }
}
'@
}

$names = @('forward_off', 'forward_on', 'deferred_off', 'deferred_on',
    'resized_forward_off', 'resized_forward_on', 'resized_deferred_off', 'resized_deferred_on', 'normals', 'editor',
    'unlit_forward', 'unlit_forward_changed', 'unlit_deferred', 'unlit_deferred_changed')
$images = @{}
$results = [Collections.Generic.List[object]]::new()
$failures = [Collections.Generic.List[string]]::new()
Add-Type -AssemblyName System.Drawing
foreach ($name in $names) {
    $file = Join-Path $CaptureDirectory "$name.bmp"
    if (!(Test-Path -LiteralPath $file)) { throw "Missing required capture: $file" }
    if (!$SkipCapture -and (Get-Item -LiteralPath $file).LastWriteTimeUtc -lt $captureStarted) {
        throw "Capture was not refreshed by this run: $file"
    }
    $image = [RAEngineValidation.Image]::new($file)
    $images[$name] = $image
    $expectedWidth = if ($name.StartsWith('resized_')) { 1024 } else { 1440 }
    $expectedHeight = if ($name.StartsWith('resized_')) { 640 } else { 900 }
    if ($image.Width -ne $expectedWidth -or $image.Height -ne $expectedHeight) {
        $failures.Add("Unexpected dimensions: $name = $($image.Width)x$($image.Height)")
    }
    $stats = $image.Statistics(0, 0, 1, 1)
    # Unlit deliberately removes shading gradients: a sphere and checker floor may
    # use only a few colors. Retain brightness/contrast checks without requiring lit variation.
    $minimumColors = if ($name.StartsWith('unlit_')) { 4 } else { 128 }
    if ($stats[0] -lt 10 -or $stats[0] -gt 245 -or $stats[1] -lt 15 -or $stats[2] -lt $minimumColors) {
        $failures.Add("Blank or insufficiently varied capture: $name")
    }
    $results.Add([pscustomobject]@{Kind='Image'; Name=$name; Width=$image.Width; Height=$image.Height;
        Mean=$stats[0]; StandardDeviation=$stats[1]; QuantizedColors=$stats[2]})
    $bitmap = [Drawing.Bitmap]::FromFile($file)
    try { $bitmap.Save((Join-Path $CaptureDirectory "$name.png"), [Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
}

function Compare-Captures([string]$First, [string]$Second, [bool]$ExpectSimilar) {
    $delta = $images[$First].Difference($images[$Second])
    # Half-float G-buffer storage may shift highlights, intersections and edge pixels.
    # Allow isolated large errors while bounding both total error and affected area.
    if ($ExpectSimilar) {
        $passed = $delta[0] -le 1.25 -and $delta[2] -le 0.01
    } else {
        $passed = $delta[0] -ge 0.15 -and $delta[1] -ge 0.005
    }
    $results.Add([pscustomobject]@{Kind='Comparison'; First=$First; Second=$Second; ExpectSimilar=$ExpectSimilar;
        MeanAbsoluteError=$delta[0]; FractionOver3=$delta[1]; FractionOver12=$delta[2]; MaxChannelError=$delta[3]; Passed=$passed})
    Write-Host ("{0} vs {1}: MAE={2:F4}/255, changed>3={3:P2}, PASS={4}" -f $First,$Second,$delta[0],$delta[1],$passed)
    if (!$passed) { $failures.Add("Image comparison failed: $First vs $Second") }
}

foreach ($prefix in @('', 'resized_')) {
    Compare-Captures "${prefix}forward_off" "${prefix}deferred_off" $true
    Compare-Captures "${prefix}forward_on" "${prefix}deferred_on" $true
    Compare-Captures "${prefix}forward_off" "${prefix}forward_on" $false
    Compare-Captures "${prefix}deferred_off" "${prefix}deferred_on" $false
}
Compare-Captures 'forward_on' 'editor' $false
Compare-Captures 'forward_off' 'normals' $false
Compare-Captures 'unlit_forward' 'unlit_deferred' $true
Compare-Captures 'unlit_forward_changed' 'unlit_deferred_changed' $true
Compare-Captures 'forward_off' 'unlit_forward' $false
Compare-Captures 'deferred_off' 'unlit_deferred' $false
# These pairs change only light intensity and the ray tracing toggle. Unlit must
# be pixel-identical within each pipeline, not merely within the G-buffer tolerance.
foreach ($pipeline in @('forward', 'deferred')) {
    $first = "unlit_$pipeline"
    $second = "${first}_changed"
    $delta = $images[$first].Difference($images[$second])
    $passed = $delta[3] -eq 0
    $results.Add([pscustomobject]@{Kind='UnlitLightingIndependence'; First=$first; Second=$second;
        MeanAbsoluteError=$delta[0]; MaxChannelError=$delta[3]; Passed=$passed})
    Write-Host ("{0} vs {1}: max channel error={2}, PASS={3}" -f $first,$second,$delta[3],$passed)
    if (!$passed) { $failures.Add("Unlit changed with lighting or ray tracing: $pipeline") }
}
# This center crop lies inside the sphere in the deterministic self-test camera.
# A missing mesh or constant-valued normal target cannot satisfy its color variation.
$normalStats = $images['normals'].Statistics(0.43, 0.4, 0.57, 0.6)
if ($normalStats[2] -lt 128 -or $normalStats[1] -lt 20) { $failures.Add('Sphere normals are missing or constant') }
$results.Add([pscustomobject]@{Kind='NormalCrop'; QuantizedColors=$normalStats[2]; StandardDeviation=$normalStats[1]})
# Check the controls panel itself, so the header/footer alone cannot mask a hidden panel.
$panelDelta = $images['forward_on'].Difference($images['editor'], 0.03, 0.12, 0.22, 0.78)
if ($panelDelta[0] -lt 10 -or $panelDelta[1] -lt 0.5) { $failures.Add('Render controls panel is missing from the editor capture') }
$results.Add([pscustomobject]@{Kind='ControlsPanel'; MeanAbsoluteError=$panelDelta[0]; FractionOver3=$panelDelta[1]})
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'validation-results.json') -Encoding UTF8
if ($failures.Count) { throw ($failures -join [Environment]::NewLine) }
Write-Host "PASS: GPU captures, forward/deferred agreement, unlit lighting independence, ray effects, resize, normals and UI. PNGs and validation-results.json: $CaptureDirectory"
