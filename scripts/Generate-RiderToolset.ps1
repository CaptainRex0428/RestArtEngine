$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = $null
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $previousEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        $query = @('-products', '*', '-version', '[17.0,18.0)', '-sort', '-requires',
            'Microsoft.Component.MSBuild', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property', 'installationPath', '-utf8')
        $installations = & $vswhere @query
        if ($LASTEXITCODE -ne 0) { throw "vswhere failed with exit code $LASTEXITCODE." }
    } finally {
        [Console]::OutputEncoding = $previousEncoding
    }
    foreach ($installation in $installations) {
        $candidate = Join-Path $installation 'MSBuild/Current/Bin/amd64/MSBuild.exe'
        $vcTargets = Join-Path $installation 'MSBuild/Microsoft/VC/v170'
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $vcTargets 'Microsoft.Cpp.Default.props') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $vcTargets 'Platforms/x64/PlatformToolsets/v143/Toolset.props') -PathType Leaf)) {
            $msbuild = [IO.Path]::GetFullPath($candidate)
            break
        }
    }
}
if (!$msbuild) {
    Write-Warning 'VS 2022 x64 MSBuild with MSVC v143 was not found. Install VS 2022 or Build Tools 2022 with the C++ workload and Windows SDK, then run GenerateWIN.bat again.'
}

# Rider reads this layer next to the solution. Its separate .user layer can override it.
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root 'RestArtEngine.sln.DotSettings'
$template = [xml][IO.File]::ReadAllText((Join-Path $root '.idea/RestArtEngine.sln.DotSettings.template'))
$settings = [System.Xml.XmlDocument]::new()
$existing = Test-Path -LiteralPath $destination -PathType Leaf
if ($existing) {
    $settings.PreserveWhitespace = $true
    $settings.Load($destination)
} else {
    $settings.LoadXml($template.OuterXml)
}
$original = $settings.OuterXml

$xaml = 'http://schemas.microsoft.com/winfx/2006/xaml'
$namespaces = [System.Xml.XmlNamespaceManager]::new($settings.NameTable)
$namespaces.AddNamespace('x', $xaml)
$keyPrefix = '/Default/Environment/Hierarchy/Build/BuildTool/'
$versionKey = "${keyPrefix}MsbuildVersion/@EntryValue"
$pathKey = "${keyPrefix}CustomBuildToolPath/@EntryValue"
$versionEntry = $template.SelectSingleNode("/*/*[@x:Key='$versionKey']", $namespaces)
if (!$versionEntry) { throw 'MSBuild version entry is missing from the Rider settings template.' }
$currentVersion = $settings.SelectSingleNode("/*/*[@x:Key='$versionKey']", $namespaces)
if ($currentVersion) {
    $currentVersion.InnerText = $versionEntry.InnerText
} else {
    [void]$settings.DocumentElement.AppendChild($settings.ImportNode($versionEntry, $true))
}

$pathEntry = $settings.SelectSingleNode("/*/*[@x:Key='$pathKey']", $namespaces)
if ($msbuild) {
    if (!$pathEntry) {
        $pathEntry = $settings.CreateElement('s', 'String', $versionEntry.NamespaceURI)
        $key = $settings.CreateAttribute('x', 'Key', $xaml)
        $key.Value = $pathKey
        [void]$pathEntry.Attributes.Append($key)
        [void]$settings.DocumentElement.AppendChild($pathEntry)
    }
    $pathEntry.InnerText = $msbuild
} elseif ($pathEntry) {
    [void]$settings.DocumentElement.RemoveChild($pathEntry)
}

if (!$existing -or $settings.OuterXml -cne $original) {
    $temporary = "$destination.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        $settings.Save($temporary)
        if ($existing) {
            [IO.File]::Replace($temporary, $destination, [NullString]::Value)
        } else {
            [IO.File]::Move($temporary, $destination)
        }
    } finally {
        [IO.File]::Delete($temporary)
    }
}
