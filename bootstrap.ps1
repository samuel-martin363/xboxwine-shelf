[CmdletBinding()]
param(
    [string]$WorkDirectory = (Join-Path $PSScriptRoot "work"),
    [switch]$SkipRuntime
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$archiveUrl = "https://github.com/worleydl/Boxedwine-uwp/archive/refs/heads/uwp-compat.zip"
$archivePath = Join-Path $WorkDirectory "Boxedwine-uwp-compat.zip"

New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null

Write-Host "Downloading the experimental BoxedWine UWP fork..."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath -UseBasicParsing

Write-Host "Extracting..."
$extractPath = Join-Path $WorkDirectory "source"
if (Test-Path $extractPath) {
    Remove-Item -Recurse -Force $extractPath
}
Expand-Archive -Path $archivePath -DestinationPath $extractPath -Force

$repo = Get-ChildItem -Path $extractPath -Directory |
    Where-Object { $_.Name -like "Boxedwine-uwp-*" } |
    Select-Object -First 1

if (-not $repo) {
    throw "Could not find the extracted Boxedwine-uwp source directory."
}

$finalPath = Join-Path $WorkDirectory $repo.Name
if (Test-Path $finalPath) {
    Remove-Item -Recurse -Force $finalPath
}
Move-Item -Path $repo.FullName -Destination $finalPath

Write-Host "Applying XboxWine Shelf patch..."
& (Join-Path $PSScriptRoot "apply-patch.ps1") -RepoRoot $finalPath

if (-not $SkipRuntime) {
    & (Join-Path $PSScriptRoot "get-runtime.ps1") -RepoRoot $finalPath
}

$solution = Join-Path $finalPath "project\msvc\BoxedWine\BoxedWine.sln"
Write-Host ""
Write-Host "Patch complete." -ForegroundColor Green
Write-Host "Open this solution in Visual Studio 2022:"
Write-Host $solution -ForegroundColor Cyan
Write-Host ""
Write-Host "Build configuration: Release | x64"
