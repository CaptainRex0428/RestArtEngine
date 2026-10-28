param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [string]$SdkVersion
)
$ErrorActionPreference = 'Stop'
$kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$candidates = @()
if ($SdkVersion) { $candidates += Join-Path $kitsRoot "bin/$SdkVersion/x64" }
$candidates += Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory |
    Where-Object Name -Match '^10\.0\.\d+\.\d+$' |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64' }
foreach ($directory in $candidates) {
    $compiler = Join-Path $directory 'dxcompiler.dll'
    $validator = Join-Path $directory 'dxil.dll'
    if ((Test-Path -LiteralPath $compiler) -and (Test-Path -LiteralPath $validator)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
        Copy-Item -LiteralPath $compiler, $validator -Destination $Destination -Force
        Write-Output 'Copied DXC runtime and matching DXIL validator.'
        exit 0
    }
}
Write-Warning 'No matching x64 DXC runtime found in the Windows SDK. Hardware ray tracing will report unavailable.'
