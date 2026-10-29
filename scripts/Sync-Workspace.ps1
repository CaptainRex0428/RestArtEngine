# Public entry point only. Collaboration workflow and data live in the optional local repository.
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [ValidateSet('Status', 'Collect', 'Install')]
    [string]$Mode = 'Status'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$privateRoot = Join-Path $projectRoot 'local'
$implementation = Join-Path $privateRoot 'scripts/Sync-Workspace.ps1'
if (-not (Test-Path -LiteralPath (Join-Path $privateRoot '.git')) -or
    -not (Test-Path -LiteralPath $implementation -PathType Leaf)) {
    throw 'Collaboration workspace is not installed. Clone your collaboration workspace into local/ first. Engine builds do not require it.'
}

# No remote access, installation side effects, or credentials in this public wrapper.
& $implementation -Mode $Mode -ProjectRoot $projectRoot -PrivateRoot $privateRoot -WhatIf:$WhatIfPreference
if ($LASTEXITCODE -ne 0) { throw "Workspace synchronization failed (exit $LASTEXITCODE). See the collaboration tool's diagnostics above." }
