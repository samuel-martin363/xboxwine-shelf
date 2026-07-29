[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$uwpDir = Join-Path $RepoRoot "project\msvc\BoxedWine\uwp"
$projectPath = Join-Path $uwpDir "uwp.vcxproj"
$manifestPath = Join-Path $uwpDir "Package.appxmanifest"
$patchDir = Join-Path $PSScriptRoot "patch\uwp"

foreach ($required in @($uwpDir, $projectPath, $manifestPath, $patchDir)) {
    if (-not (Test-Path $required)) {
        throw "Required path does not exist: $required"
    }
}

$backupDir = Join-Path $uwpDir "xboxwine-shelf-backup"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
Copy-Item $projectPath (Join-Path $backupDir "uwp.vcxproj.original") -Force
Copy-Item $manifestPath (Join-Path $backupDir "Package.appxmanifest.original") -Force
Copy-Item (Join-Path $uwpDir "main.cpp") (Join-Path $backupDir "main.cpp.original") -Force

foreach ($file in @(
    "shelf_entry.cpp",
    "xbox_shelf.cpp",
    "xbox_shelf.h",
    "controller_bridge.cpp",
    "controller_bridge.h",
    "transfer_server.cpp",
    "transfer_server.h"
)) {
    Copy-Item (Join-Path $patchDir $file) $uwpDir -Force
}

# Edit the Visual C++ project as XML instead of matching a multiline string.
# This is resilient to LF/CRLF and harmless whitespace changes in the fork.
[xml]$projectXml = Get-Content -Raw -Path $projectPath
$namespaceUri = $projectXml.Project.NamespaceURI

$namespaceManager = New-Object System.Xml.XmlNamespaceManager($projectXml.NameTable)
$namespaceManager.AddNamespace("msb", $namespaceUri)

$projectNode = $projectXml.SelectSingleNode("/msb:Project", $namespaceManager)
if (-not $projectNode) {
    throw "Could not read the root Project element from uwp.vcxproj."
}

$targetsImport = $projectXml.SelectSingleNode(
    '/msb:Project/msb:Import[@Project="$(VCTargetsPath)\Microsoft.Cpp.targets"]',
    $namespaceManager
)
if (-not $targetsImport) {
    throw "Could not locate Microsoft.Cpp.targets in uwp.vcxproj."
}

function Find-ProjectItem {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ElementName,

        [Parameter(Mandatory = $true)]
        [string]$Include
    )

    foreach ($node in $projectXml.SelectNodes("//msb:$ElementName", $namespaceManager)) {
        if ($node.GetAttribute("Include") -eq $Include) {
            return $node
        }
    }

    return $null
}

function New-ItemGroupBeforeTargets {
    $group = $projectXml.CreateElement("ItemGroup", $namespaceUri)
    [void]$projectNode.InsertBefore($group, $targetsImport)
    return $group
}

$mainCompile = Find-ProjectItem -ElementName "ClCompile" -Include "main.cpp"
if (-not $mainCompile) {
    throw "Could not locate main.cpp in uwp.vcxproj."
}

$compileGroup = $mainCompile.ParentNode
foreach ($file in @(
    "shelf_entry.cpp",
    "xbox_shelf.cpp",
    "controller_bridge.cpp",
    "transfer_server.cpp"
)) {
    if (-not (Find-ProjectItem -ElementName "ClCompile" -Include $file)) {
        $node = $projectXml.CreateElement("ClCompile", $namespaceUri)
        $node.SetAttribute("Include", $file)
        [void]$compileGroup.AppendChild($node)
    }
}

$includeGroup = $null
$existingInclude = $projectXml.SelectSingleNode("//msb:ClInclude", $namespaceManager)
if ($existingInclude) {
    $includeGroup = $existingInclude.ParentNode
} else {
    $includeGroup = New-ItemGroupBeforeTargets
}

foreach ($file in @(
    "xbox_shelf.h",
    "controller_bridge.h",
    "transfer_server.h"
)) {
    if (-not (Find-ProjectItem -ElementName "ClInclude" -Include $file)) {
        $node = $projectXml.CreateElement("ClInclude", $namespaceUri)
        $node.SetAttribute("Include", $file)
        [void]$includeGroup.AppendChild($node)
    }
}

$runtimeInclude = '$(ProjectDir)*Wine*.zip'
if (-not (Find-ProjectItem -ElementName "Content" -Include $runtimeInclude)) {
    $contentGroup = New-ItemGroupBeforeTargets
    $content = $projectXml.CreateElement("Content", $namespaceUri)
    $content.SetAttribute("Include", $runtimeInclude)

    $copy = $projectXml.CreateElement("CopyToOutputDirectory", $namespaceUri)
    $copy.InnerText = "PreserveNewest"
    [void]$content.AppendChild($copy)
    [void]$contentGroup.AppendChild($content)
}

$writerSettings = New-Object System.Xml.XmlWriterSettings
$writerSettings.Indent = $true
$writerSettings.Encoding = New-Object System.Text.UTF8Encoding($false)

$writer = [System.Xml.XmlWriter]::Create($projectPath, $writerSettings)
try {
    $projectXml.Save($writer)
}
finally {
    $writer.Dispose()
}

$manifest = Get-Content -Raw -Path $manifestPath
$manifest = $manifest.Replace(
    "<DisplayName>Boxed Wine UWP</DisplayName>",
    "<DisplayName>XboxWine Shelf</DisplayName>"
)
$manifest = $manifest.Replace(
    'DisplayName="Boxed Wine"',
    'DisplayName="XboxWine Shelf"'
)
$manifest = $manifest.Replace(
    'Description="16/32 bit app support for UWP bia Boxed Wine"',
    'Description="Local 16/32-bit Windows game shelf with network folder transfer"'
)
$manifest = $manifest.Replace(
    'Version="1.0.0.0"',
    'Version="0.2.1.0"'
)
$manifest = $manifest.Replace(
    'Version="0.1.0.0"',
    'Version="0.2.1.0"'
)
$manifest = $manifest.Replace(
    'Version="0.2.0.0"',
    'Version="0.2.1.0"'
)

if (-not $manifest.Contains('Name="privateNetworkClientServer"')) {
    $manifest = $manifest.Replace(
        '<Capabilities>',
        @'
<Capabilities>
    <Capability Name="privateNetworkClientServer" />
    <Capability Name="internetClientServer" />
'@
    )
}

Set-Content -Path $manifestPath -Value $manifest -Encoding UTF8

Write-Host "Patched XboxWine Shelf v0.2.1 source and Visual Studio project." -ForegroundColor Green
Write-Host "Original files saved in: $backupDir"
