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

$project = Get-Content -Raw -Path $projectPath

$oldCompileGroup = @'
  <ItemGroup>
    <ClCompile Include="main.cpp" />
  </ItemGroup>
'@

$newCompileGroup = @'
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="shelf_entry.cpp" />
    <ClCompile Include="xbox_shelf.cpp" />
    <ClCompile Include="controller_bridge.cpp" />
    <ClCompile Include="transfer_server.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="xbox_shelf.h" />
    <ClInclude Include="controller_bridge.h" />
    <ClInclude Include="transfer_server.h" />
  </ItemGroup>
'@

if ($project.Contains($oldCompileGroup)) {
    $project = $project.Replace($oldCompileGroup, $newCompileGroup)
}
elseif (-not $project.Contains('ClCompile Include="shelf_entry.cpp"')) {
    throw "Could not locate the expected main.cpp ItemGroup in uwp.vcxproj."
}
elseif (-not $project.Contains('ClCompile Include="transfer_server.cpp"')) {
    $project = $project.Replace(
        '    <ClCompile Include="controller_bridge.cpp" />',
        "    <ClCompile Include=`"controller_bridge.cpp`" />`r`n    <ClCompile Include=`"transfer_server.cpp`" />"
    )
    $project = $project.Replace(
        '    <ClInclude Include="controller_bridge.h" />',
        "    <ClInclude Include=`"controller_bridge.h`" />`r`n    <ClInclude Include=`"transfer_server.h`" />"
    )
}

if (-not $project.Contains('$(ProjectDir)*Wine*.zip')) {
    $project = $project.Replace(
        '  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />',
        @'
  <ItemGroup>
    <Content Include="$(ProjectDir)*Wine*.zip">
      <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
    </Content>
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
'@
    )
}

Set-Content -Path $projectPath -Value $project -Encoding UTF8

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
    'Version="0.2.0.0"'
)
$manifest = $manifest.Replace(
    'Version="0.1.0.0"',
    'Version="0.2.0.0"'
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

Write-Host "Patched XboxWine Shelf v0.2 source and Visual Studio project." -ForegroundColor Green
Write-Host "Original files saved in: $backupDir"
