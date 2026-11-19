[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Executable,
    [string]$EngineRoot = '',
    [string]$ProjectPath = '',
    [string]$OutputDirectory = '',
    [ValidateRange(10, 1800)]
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$loadingRepository = Split-Path $PSScriptRoot -Parent
$Executable = [IO.Path]::GetFullPath($Executable)
if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Build the engine first: $Executable" }
if (!$EngineRoot) { $EngineRoot = $loadingRepository }
$EngineRoot = [IO.Path]::GetFullPath($EngineRoot)
if (!(Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine.raeengine') -PathType Leaf)) {
    throw "The engine root is missing Engine.raeengine: $EngineRoot"
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $loadingRepository 'artifacts/loading-progress' }
$loadingRun = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('run-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($loadingRun) | Out-Null

# Use committed sample assets by default: the live Sandbox can contain another
# task's edits. A caller-supplied project is copied and never run in place.
if ($ProjectPath) {
    $loadingProjectItem = Get-Item -LiteralPath ([IO.Path]::GetFullPath($ProjectPath))
    if ($loadingProjectItem.PSIsContainer) {
        $loadingManifests = @(Get-ChildItem -LiteralPath $loadingProjectItem.FullName -Filter '*.raproject' -File)
        if ($loadingManifests.Count -ne 1) { throw 'ProjectPath must contain exactly one .raproject file, or name that file.' }
        $loadingSourceManifest = $loadingManifests[0]
    } else {
        if ($loadingProjectItem.Extension -ne '.raproject') { throw 'ProjectPath must name a .raproject file or its folder.' }
        $loadingSourceManifest = $loadingProjectItem
    }
    $loadingTemplate = Join-Path $loadingRun 'Template'
    Copy-Item -LiteralPath $loadingSourceManifest.DirectoryName -Destination $loadingTemplate -Recurse
    $loadingManifestName = $loadingSourceManifest.Name
} else {
    $loadingArchive = Join-Path $loadingRun 'head-sandbox.zip'
    & git -C $loadingRepository archive --format=zip "--output=$loadingArchive" HEAD Projects/Sandbox
    if ($LASTEXITCODE -ne 0) { throw 'Cannot export HEAD Sandbox for isolated loading validation.' }
    $loadingTemplateRoot = Join-Path $loadingRun 'Template'
    Expand-Archive -LiteralPath $loadingArchive -DestinationPath $loadingTemplateRoot
    $loadingTemplate = Join-Path $loadingTemplateRoot 'Projects/Sandbox'
    $loadingManifestName = 'Sandbox.raproject'
}

function ConvertTo-LoadingArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    # Start-Process joins ArgumentList on Windows. Quote according to the CRT
    # rules, including trailing backslashes before the closing quote.
    $loadingQuoted = [Text.StringBuilder]::new()
    [void]$loadingQuoted.Append('"')
    $loadingSlashes = 0
    foreach ($loadingCharacter in $Value.ToCharArray()) {
        if ($loadingCharacter -eq '\') { $loadingSlashes++; continue }
        if ($loadingCharacter -eq '"') {
            [void]$loadingQuoted.Append(('\' * ($loadingSlashes * 2 + 1)))
        } else {
            [void]$loadingQuoted.Append(('\' * $loadingSlashes))
        }
        [void]$loadingQuoted.Append($loadingCharacter)
        $loadingSlashes = 0
    }
    [void]$loadingQuoted.Append(('\' * ($loadingSlashes * 2)))
    [void]$loadingQuoted.Append('"')
    return $loadingQuoted.ToString()
}

function Invoke-LoadingProcess([string]$CaseDirectory, [string[]]$Arguments) {
    $loadingStdout = Join-Path $CaseDirectory 'stdout.log'
    $loadingStderr = Join-Path $CaseDirectory 'stderr.log'
    $loadingCommand = ($Arguments | ForEach-Object { ConvertTo-LoadingArgument $_ }) -join ' '
    $loadingCommand | Set-Content -LiteralPath (Join-Path $CaseDirectory 'arguments.txt') -Encoding utf8
    # This test explicitly exercises the normal visible loading/editor path.
    # Self-test mode would skip the splash and cannot prove UI submissions.
    $loadingProcess = Start-Process -FilePath $Executable -ArgumentList $loadingCommand `
        -WorkingDirectory $CaseDirectory -WindowStyle Normal -PassThru `
        -RedirectStandardOutput $loadingStdout -RedirectStandardError $loadingStderr
    $null = $loadingProcess.Handle
    $loadingWatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        while (!$loadingProcess.WaitForExit(100)) {
            if ($loadingWatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                throw "Loading validation timed out after $TimeoutSeconds seconds (owned PID $($loadingProcess.Id))."
            }
        }
        $loadingProcess.Refresh()
        $loadingExitCode = $loadingProcess.ExitCode
        if ($null -eq $loadingExitCode) { throw 'Cannot read the owned loading process exit code.' }
        $loadingText = (Get-Content -LiteralPath $loadingStdout -Raw -Encoding utf8) + "`n" +
            (Get-Content -LiteralPath $loadingStderr -Raw -Encoding utf8)
        return [pscustomobject]@{
            ExitCode = [int]$loadingExitCode
            Seconds = $loadingWatch.Elapsed.TotalSeconds
            Text = $loadingText
        }
    } finally {
        # Never enumerate or close the user's editor processes. This handle is
        # exclusively the process launched above, including a stalled error UI.
        if (!$loadingProcess.HasExited) {
            $loadingProcess.Kill()
            $null = $loadingProcess.WaitForExit(10000)
        }
        $loadingProcess.Dispose()
    }
}

function Read-LoadingEvents([string]$Report) {
    if (!(Test-Path -LiteralPath $Report -PathType Leaf)) { throw "Missing startup report: $Report" }
    $loadingLines = @(Get-Content -LiteralPath $Report -Encoding utf8 | Where-Object { $_.Trim().Length -gt 0 })
    if ($loadingLines.Count -eq 0) { throw 'Startup did not report any stages.' }
    return @($loadingLines | ForEach-Object { $_ | ConvertFrom-Json })
}

function Assert-LoadingSequence([object[]]$Events) {
    $loadingPrevious = -1.0
    $loadingPreviousTime = -1.0
    $loadingMaximumJump = 0.0
    foreach ($loadingEvent in $Events) {
        foreach ($loadingProperty in @('progress', 'stage', 'detail', 'elapsedMs', 'presented')) {
            if (!$loadingEvent.PSObject.Properties[$loadingProperty]) { throw "Startup report lacks '$loadingProperty'." }
        }
        $loadingFraction = [double]$loadingEvent.progress
        $loadingTime = [double]$loadingEvent.elapsedMs
        if ([double]::IsNaN($loadingFraction) -or [double]::IsInfinity($loadingFraction) -or
            $loadingFraction -lt 0 -or $loadingFraction -gt 1) { throw 'Startup reported an invalid fraction.' }
        if ([double]::IsNaN($loadingTime) -or [double]::IsInfinity($loadingTime) -or $loadingTime -lt 0) {
            throw 'Startup reported an invalid elapsed time.'
        }
        if ($loadingFraction + 0.000001 -lt $loadingPrevious -or $loadingTime -lt $loadingPreviousTime) {
            throw 'Loading progress or elapsed time moved backwards.'
        }
        if ($loadingPrevious -ge 0) {
            $loadingMaximumJump = [Math]::Max($loadingMaximumJump, $loadingFraction - $loadingPrevious)
        }
        if ([string]::IsNullOrWhiteSpace([string]$loadingEvent.stage)) { throw 'Startup reported an empty stage.' }
        $loadingPrevious = $loadingFraction
        $loadingPreviousTime = $loadingTime
    }
    if ([Math]::Abs([double]$Events[0].progress) -gt 0.000001) { throw 'Loading report did not begin at zero.' }
    return $loadingMaximumJump
}

# The loading observer also works with the D3D11 renderer predating explicit
# backend selection. Test only the modes the supplied executable advertises.
$loadingHelpText = (& $Executable --help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the supplied executable --help.' }
$loadingHasBackendSelection = $loadingHelpText -match '(?m)^\s*--backend\s'
if ($loadingHasBackendSelection) {
    $loadingCases = @(
        @{ Name = 'strict-d3d11'; Backend = 'd3d11'; Effective = 'd3d11'; Device11 = 'yes'; Attempt12 = 'no'; Failure = $false },
        @{ Name = 'native-d3d12'; Backend = 'd3d12'; Effective = 'd3d12'; Device11 = 'no'; Attempt12 = 'yes'; Failure = $false },
        @{ Name = 'hybrid-d3d11Dxr'; Backend = 'd3d11Dxr'; Effective = 'd3d11'; Device11 = 'yes'; Attempt12 = 'yes'; Failure = $false }
    )
} else {
    $loadingCases = @(@{ Name = 'legacy-d3d11Dxr'; Backend = ''; Failure = $false })
}
$loadingCases += @{ Name = 'missing-material-shader'; Backend = 'd3d11'; Failure = $true }
$loadingResults = [Collections.Generic.List[object]]::new()
foreach ($loadingCase in $loadingCases) {
    $loadingCaseDirectory = Join-Path $loadingRun $loadingCase.Name
    [IO.Directory]::CreateDirectory($loadingCaseDirectory) | Out-Null
    $loadingFixture = Join-Path $loadingCaseDirectory 'Project'
    Copy-Item -LiteralPath $loadingTemplate -Destination $loadingFixture -Recurse
    $loadingManifest = Join-Path $loadingFixture $loadingManifestName
    $loadingProject = Get-Content -LiteralPath $loadingManifest -Raw -Encoding utf8 | ConvertFrom-Json
    $loadingProject | Add-Member -NotePropertyName engineRoot -NotePropertyValue $EngineRoot -Force
    if ($loadingCase.Failure) {
        # Redirect only this fixture to an empty shader directory. Do not
        # remove an existing shader: a custom project may use external paths.
        $loadingProject | Add-Member -NotePropertyName shaderRoot -NotePropertyValue 'MissingShaders' -Force
        [IO.Directory]::CreateDirectory((Join-Path $loadingFixture 'MissingShaders')) | Out-Null
    }
    $loadingProject | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $loadingManifest -Encoding utf8
    $loadingManifestHash = (Get-FileHash -LiteralPath $loadingManifest -Algorithm SHA256).Hash
    $loadingReport = Join-Path $loadingCaseDirectory 'loading.jsonl'
    $loadingArguments = @('--project', $loadingManifest, '--engine', $EngineRoot,
        '--loading-report', $loadingReport)
    if ($loadingHasBackendSelection) {
        $loadingArguments += @('--backend', $loadingCase.Backend, '--no-backend-fallback')
    }
    if ($loadingCase.Failure) {
        # The missing shader must fail during startup, before the editor suite.
        # This mode also suppresses the interactive MessageBox on failure.
        $loadingArguments += @('--editor-self-test', '--capture-dir', (Join-Path $loadingCaseDirectory 'unused-captures'))
    } else {
        $loadingArguments += @('--frames', '2')
    }
    try {
        $loadingExecution = Invoke-LoadingProcess $loadingCaseDirectory $loadingArguments
        $loadingEvents = @(Read-LoadingEvents $loadingReport)
        $loadingMaximumJump = Assert-LoadingSequence $loadingEvents
        if ((Get-FileHash -LiteralPath $loadingManifest -Algorithm SHA256).Hash -ne $loadingManifestHash) {
            throw 'A startup session rewrote its project manifest.'
        }
        if ($loadingCase.Failure) {
            if ($loadingExecution.ExitCode -eq 0) { throw 'Missing material shader unexpectedly passed startup.' }
            if (@($loadingEvents | Where-Object { $_.stage -eq 'Ready' -or [double]$_.progress -ge 1 }).Count -gt 0) {
                throw 'Failed startup declared Ready or 100%.'
            }
            if (!($loadingExecution.Text.Contains('Standard.hlsli')) -or
                @($loadingEvents | Where-Object { $_.stage -eq 'Reading material shader' }).Count -eq 0) {
                throw 'The failure did not reach the intended missing material shader.'
            }
            if ($loadingExecution.Text.Contains('PASS:') -or (Test-Path -LiteralPath (Join-Path $loadingCaseDirectory 'unused-captures'))) {
                throw 'Missing-shader startup continued into the editor self-test.'
            }
            $loadingResults.Add([pscustomobject]@{
                case = $loadingCase.Name; passed = $true; exitCode = $loadingExecution.ExitCode
                events = $loadingEvents.Count; finalProgress = $loadingEvents[-1].progress
                ready = $false; presented = @($loadingEvents | Where-Object { $_.presented }).Count
            })
        } else {
            if ($loadingExecution.ExitCode -ne 0) { throw "Startup exited with code $($loadingExecution.ExitCode)." }
            $loadingMarkers = @('Completed 2 frames.', 'validation errors: 0')
            if ($loadingHasBackendSelection) {
                $loadingMarkers += @(
                    "Main backend requested: $($loadingCase.Backend); effective: $($loadingCase.Effective); fallback: no",
                    "D3D11 device created: $($loadingCase.Device11)",
                    "D3D12 initialization attempted: $($loadingCase.Attempt12)")
            } else {
                $loadingMarkers += 'RestArtEngine / D3D11 /'
            }
            foreach ($loadingMarker in $loadingMarkers) {
                if (!$loadingExecution.Text.Contains($loadingMarker)) { throw "Missing normal-startup marker: $loadingMarker" }
            }
            if ([double]$loadingEvents[-1].progress -ne 1 -or $loadingEvents[-1].stage -ne 'Ready') {
                throw 'Successful startup did not finish at Ready / 100%.'
            }
            if (@($loadingEvents | Where-Object { [double]$_.progress -ge 1 -and $_.stage -ne 'Ready' }).Count -gt 0) {
                throw 'Startup reached 100% before Ready.'
            }
            $loadingFractions = @($loadingEvents | ForEach-Object { [Math]::Round([double]$_.progress, 6) } | Sort-Object -Unique)
            $loadingStages = @($loadingEvents | ForEach-Object { $_.stage } | Sort-Object -Unique)
            $loadingPresented = @($loadingEvents | Where-Object { $_.presented }).Count
            if ($loadingFractions.Count -lt 30 -or $loadingStages.Count -lt 15 -or $loadingPresented -le 15) {
                throw "Insufficient actual feedback: $($loadingFractions.Count) values / $($loadingStages.Count) stages / $loadingPresented UI submissions."
            }
            $loadingSurfaces = @($loadingEvents | Where-Object { $_.presented })
            $loadingFirstSurface = $loadingSurfaces[0]
            foreach ($loadingSurface in $loadingSurfaces) {
                if ($loadingSurface.surfaceWidth -ne $loadingFirstSurface.surfaceWidth -or
                    $loadingSurface.surfaceHeight -ne $loadingFirstSurface.surfaceHeight -or !$loadingSurface.borderless) {
                    throw 'The loading page changed size or gained an editor frame before completion.'
                }
            }
            if (@($loadingSurfaces | Where-Object { $_.stage -eq 'Preparing initial scene and workspace' }).Count -ne 1) {
                throw 'The final scene preparation stage did not retain the original small loading surface.'
            }
            if ($loadingMaximumJump -gt 0.25) { throw "Loading still contains a large progress jump: $loadingMaximumJump" }
            foreach ($loadingEntry in @('VSMain', 'VSFullscreen', 'PSForward', 'PSGBuffer', 'PSDeferred',
                'PSBackground', 'PSOutlineMask', 'PSOutlineComposite', 'PSDepthOfField',
                'PSFXAA', 'PSTAA', 'PSAntiAliasingPresent')) {
                $loadingEntryPattern = '^' + [regex]::Escape($loadingEntry) + ' \((?:[1-9]|1[0-2])/12\)$'
                if (@($loadingEvents | Where-Object { $_.detail -match $loadingEntryPattern }).Count -lt 2) {
                    throw "Missing shader begin/completion feedback: $loadingEntry"
                }
            }
            foreach ($loadingStage in @('Loading engine materials', 'Loading engine meshes', 'Loading project materials',
                'Loading project meshes', 'Loading scene', 'Indexing project Content', 'Indexing engine Content',
                'Preparing editor', 'Preparing initial scene and workspace')) {
                if ($loadingStages -notcontains $loadingStage) { throw "Missing resource/workspace stage: $loadingStage" }
            }
            foreach ($loadingResourceStage in @('Loading engine materials', 'Loading engine meshes', 'Loading project materials',
                'Indexing project Content', 'Indexing engine Content')) {
                if (@($loadingEvents | Where-Object { $_.stage -eq $loadingResourceStage -and $_.detail -match '^\d+/\d+\s+\S' }).Count -eq 0) {
                    throw "No per-file detail in resource stage: $loadingResourceStage"
                }
            }
            if ($loadingHasBackendSelection) {
                if (@($loadingEvents | Where-Object { $_.detail -eq '227 graphics pipelines' }).Count -eq 0) {
                    throw 'The graphics pipeline preparation did not report its completed work.'
                }
            } else {
                if (@($loadingEvents | Where-Object {
                    $_.stage -eq 'Preparing material render states' -and $_.detail -eq '6 rasterizer and 18 depth states'
                }).Count -eq 0 -or $loadingStages -notcontains 'Raster pipelines ready') {
                    throw 'The legacy material render-state preparation did not report its actual work.'
                }
            }
            if (($loadingCase.Backend -eq 'd3d11Dxr' -or !$loadingHasBackendSelection) -and
                $loadingStages -notcontains 'Initializing hardware ray tracing') {
                throw 'The hybrid startup did not report its hardware ray tracing initialization.'
            }
            $loadingResults.Add([pscustomobject]@{
                case = $loadingCase.Name; passed = $true; exitCode = $loadingExecution.ExitCode
                events = $loadingEvents.Count; distinctProgress = $loadingFractions.Count
                stages = $loadingStages.Count; presented = $loadingPresented; maximumJump = $loadingMaximumJump
                loadingWidth = $loadingFirstSurface.surfaceWidth; loadingHeight = $loadingFirstSurface.surfaceHeight
                seconds = $loadingExecution.Seconds; ready = $true
            })
        }
        Write-Host "$($loadingCase.Name): PASS"
    } catch {
        $loadingResults.Add([pscustomobject]@{ case = $loadingCase.Name; passed = $false; error = $_.Exception.Message })
        Write-Warning "$($loadingCase.Name): $($_.Exception.Message) See $loadingCaseDirectory"
    }
}
$loadingPassed = @($loadingResults | Where-Object { !$_.passed }).Count -eq 0
$loadingUnavailableModes = @()
if (!$loadingHasBackendSelection) {
    $loadingUnavailableModes = @('strict-d3d11', 'native-d3d12', 'explicit-hybrid-d3d11Dxr')
}
$loadingResult = [pscustomobject]@{
    passed = $loadingPassed
    executable = $Executable
    executableSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
    engineRoot = $EngineRoot
    backendSelectionAvailable = $loadingHasBackendSelection
    unavailableModes = $loadingUnavailableModes
    cases = $loadingResults.ToArray()
    visualCapture = 'Not performed; presented verifies successful loading UI submissions, not captured window pixels.'
}
$loadingResult | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $loadingRun 'results.json') -Encoding utf8
if (!$loadingPassed) { throw "Loading progress validation failed; see $loadingRun/results.json" }
Write-Host "Loading progress startup validation passed: $loadingRun"
