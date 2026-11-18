[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$CaptureDirectory = ''
)

$ErrorActionPreference = 'Stop'
$backendRepository = Split-Path $PSScriptRoot -Parent
if (!$Executable) { $Executable = Join-Path $backendRepository 'build/cmake-vs2022/Debug/RestArtEngine.exe' }
if (!$CaptureDirectory) { $CaptureDirectory = Join-Path $backendRepository 'artifacts/backend-startup' }
$Executable = [IO.Path]::GetFullPath($Executable)
if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Build the engine first: $Executable" }
$backendRun = Join-Path ([IO.Path]::GetFullPath($CaptureDirectory)) ('run-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($backendRun) | Out-Null
$backendFixture = Join-Path $backendRun 'Project'
Copy-Item -LiteralPath (Join-Path $backendRepository 'Projects/RayTracing_Simple') -Destination $backendFixture -Recurse
$backendManifest = Join-Path $backendFixture 'RayTracing_Simple.raproject'
$backendJson = Get-Content -LiteralPath $backendManifest -Raw | ConvertFrom-Json
$backendJson.engineRoot = $backendRepository
# Exercise configuration-driven strict mode, plus command-line overrides that
# must not rewrite the persisted request or the independent hardware/GI intent.
$backendJson | Add-Member -NotePropertyName rendering -NotePropertyValue ([pscustomobject]@{
    backendPreference = 'd3d11'; allowFallbackToD3D11 = $false
    hardwareRayTracing = $true
    globalIllumination = [pscustomobject]@{ method = 'pathTracing' }
}) -Force
$backendJson | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $backendManifest -Encoding utf8
$backendManifestHash = (Get-FileHash -LiteralPath $backendManifest).Hash
$backendCases = @(
    @{ Name = 'configured-strict11'; Arguments = @(); ExitCode = 0; Requested = 'd3d11'; Effective = 'd3d11'; D3D12Attempted = 'no'; D3D11Created = 'yes' },
    @{ Name = 'auto'; Arguments = @('--backend', 'auto'); ExitCode = 0; Requested = 'auto'; Effective = 'd3d11'; D3D12Attempted = 'no'; D3D11Created = 'yes' },
    @{ Name = 'native12'; Arguments = @('--backend', 'd3d12', '--allow-backend-fallback'); ExitCode = 0; Requested = 'd3d12'; Effective = 'd3d12'; D3D12Attempted = 'yes'; D3D11Created = 'no' },
    @{ Name = 'native12-no-fallback'; Arguments = @('--backend', 'd3d12', '--no-backend-fallback'); ExitCode = 0; Requested = 'd3d12'; Effective = 'd3d12'; D3D12Attempted = 'yes'; D3D11Created = 'no' },
    @{ Name = 'hybrid'; Arguments = @('--backend', 'd3d11Dxr', '--no-hardware-rt'); ExitCode = 0; Requested = 'd3d11Dxr'; Effective = 'd3d11'; D3D12Attempted = 'yes'; D3D11Created = 'yes' },
    @{ Name = 'invalid'; Arguments = @('--backend', 'd3d13'); ExitCode = 1 }
)
# The native cases deliberately require successful native D3D12 execution.
# Unsupported hardware or a real initialization fallback is a visible failure
# of this validation run, never evidence that native D3D12 execution passed.
foreach ($backendCase in $backendCases) {
    $backendOutput = Join-Path $backendRun $backendCase.Name
    [IO.Directory]::CreateDirectory($backendOutput) | Out-Null
    $backendLog = Join-Path $backendOutput 'startup.log'
    $backendArguments = @('--self-test', '--project', $backendFixture, '--capture-dir', $backendOutput) + $backendCase.Arguments
    & $Executable @backendArguments *> $backendLog
    if ($LASTEXITCODE -ne $backendCase.ExitCode) { throw "$($backendCase.Name): unexpected exit $LASTEXITCODE; see $backendLog" }
    $backendText = Get-Content -LiteralPath $backendLog -Raw
    if ($backendCase.ExitCode -eq 0) {
        $backendExpected = "Main backend requested: $($backendCase.Requested); effective: $($backendCase.Effective); fallback: no"
        foreach ($backendMarker in @($backendExpected,
            "D3D12 initialization attempted: $($backendCase.D3D12Attempted)",
            "D3D11 device created: $($backendCase.D3D11Created)",
            'validation errors: 0', 'DXR dispatches: 0', 'Completed 14 frames.')) {
            if (!$backendText.Contains($backendMarker)) { throw "$($backendCase.Name): missing '$backendMarker'; see $backendLog" }
        }
        if (!(Test-Path -LiteralPath (Join-Path $backendOutput 'forward_off.bmp'))) { throw 'Missing actual GPU output.' }
    } elseif ($backendText.Contains('Main backend requested:') -or (Get-ChildItem -LiteralPath $backendOutput -Filter '*.bmp')) {
        throw "$($backendCase.Name): rejected startup reached a rendered frame."
    } elseif ($backendCase.Name -eq 'invalid' -and !$backendText.Contains('Invalid --backend')) {
        throw 'Invalid backend was not rejected by argument parsing.'
    }
    if ((Get-FileHash -LiteralPath $backendManifest).Hash -ne $backendManifestHash) { throw 'A session override rewrote project preferences.' }
    Write-Host "$($backendCase.Name): PASS"
}
# Strict D3D11 and Auto run identical raster inputs. Compare actual readback
# bytes, excluding the editor image whose backend status text differs. Native
# D3D12 parity is evaluated separately with its explicit image tolerances.
foreach ($backendImage in (Get-ChildItem -LiteralPath (Join-Path $backendRun 'configured-strict11') -Filter '*.bmp')) {
    if ($backendImage.Name -eq 'editor.bmp') { continue }
    $backendReference = (Get-FileHash -LiteralPath $backendImage.FullName).Hash
    foreach ($backendVariant in @('auto')) {
        if ((Get-FileHash -LiteralPath (Join-Path (Join-Path $backendRun $backendVariant) $backendImage.Name)).Hash -ne $backendReference) {
            throw "Raster output differs: $backendVariant / $($backendImage.Name)"
        }
    }
}
Write-Host "Native D3D12 startup matrix and D3D11/Auto GPU image equivalence passed: $backendRun"
