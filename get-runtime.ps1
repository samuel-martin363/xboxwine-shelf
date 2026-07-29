[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$uwpDir = Join-Path $RepoRoot "project\msvc\BoxedWine\uwp"
if (-not (Test-Path $uwpDir)) {
    throw "UWP project directory not found: $uwpDir"
}

$runtimeName = "Debian10-Wine-5.0.zip"
$destination = Join-Path $uwpDir $runtimeName
$url = "https://sourceforge.net/projects/boxedwine/files/FileSystems/Full/v5/Debian10-Wine-5.0.zip/download"

if (Test-Path $destination) {
    Write-Host "Wine filesystem already exists: $destination"
    exit 0
}

Write-Host "Downloading the official BoxedWine starter Wine filesystem..."
Write-Host "This download is roughly 109 MB."
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -Uri $url -OutFile $destination -UseBasicParsing

if ((Get-Item $destination).Length -lt 50000000) {
    Remove-Item -Force $destination
    throw "The downloaded file is unexpectedly small. SourceForge may have returned an HTML page instead of the ZIP."
}

Write-Host "Runtime downloaded:" -ForegroundColor Green
Write-Host $destination -ForegroundColor Cyan
