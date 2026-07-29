[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameFolder,

    [Parameter(Mandatory = $true)]
    [string]$ExeRelativePath,

    [Parameter(Mandatory = $true)]
    [string]$Title,

    [Parameter(Mandatory = $true)]
    [string]$UsbRoot,

    [string]$Arguments = "",
    [ValidateSet("aspect", "stretch", "window")]
    [string]$Fullscreen = "aspect",
    [string]$Resolution = "1280x720",
    [ValidateSet("keyboard_mouse", "off")]
    [string]$ControllerBridge = "keyboard_mouse",
    [int]$MouseSpeed = 14
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$gameFolderResolved = (Resolve-Path $GameFolder).Path
$exeHostPath = Join-Path $gameFolderResolved $ExeRelativePath
if (-not (Test-Path $exeHostPath -PathType Leaf)) {
    throw "Executable not found: $exeHostPath"
}

$usbResolved = (Resolve-Path $UsbRoot).Path
$safeTitle = ($Title -replace '[<>:"/\\|?*]', '_').Trim()
if ([string]::IsNullOrWhiteSpace($safeTitle)) {
    throw "The title does not produce a usable folder name."
}

$destination = Join-Path $usbResolved ("XboxWine\Games\" + $safeTitle)
New-Item -ItemType Directory -Force -Path $destination | Out-Null

$zipPath = Join-Path $destination "game.zip"
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $gameFolderResolved,
    $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $false
)

$wineExe = "d:\" + ($ExeRelativePath -replace '/', '\')
$manifest = @"
# XboxWine Shelf game manifest
title=$Title
zip=game.zip
exe=$wineExe
arguments=$Arguments
fullscreen=$Fullscreen
resolution=$Resolution
controller_bridge=$ControllerBridge
mouse_speed=$MouseSpeed
extra_boxedwine_args=
"@

$manifestPath = Join-Path $destination "game.xwgame"
Set-Content -Path $manifestPath -Value $manifest -Encoding UTF8

Write-Host "Prepared game:" -ForegroundColor Green
Write-Host $destination -ForegroundColor Cyan
Write-Host ""
Write-Host "Executable inside Wine: $wineExe"
Write-Host "You can edit game.xwgame in Notepad before moving the USB to Xbox."
