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

# The BoxedWine static library supplies its own desktop-style main().
# SDL rewrites that symbol to SDL_main, which conflicts with our custom
# controller-first SDL_main in shelf_entry.cpp. Rename the unused standalone
# entry point for this Xbox/UWP build so shelf_entry.cpp is the sole SDL_main.
$nativeSystemPath = Join-Path $RepoRoot "platform\sdl\knativesystem.cpp"
if (-not (Test-Path $nativeSystemPath)) {
    throw "Could not locate BoxedWine platform entry source: $nativeSystemPath"
}

$nativeSystem = [System.IO.File]::ReadAllText($nativeSystemPath)
$mainPattern = 'int\s+main\s*\(\s*int\s+argc\s*,\s*char\s*\*\*\s*argv\s*\)\s*\{'

if ($nativeSystem -match $mainPattern) {
    $nativeSystem = [System.Text.RegularExpressions.Regex]::Replace(
        $nativeSystem,
        $mainPattern,
        'int boxedwine_standalone_main_disabled(int argc, char** argv) {',
        1
    )

    [System.IO.File]::WriteAllText(
        $nativeSystemPath,
        $nativeSystem,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Host "Disabled BoxedWine's duplicate standalone SDL_main." -ForegroundColor Green
}
elseif (-not $nativeSystem.Contains("boxedwine_standalone_main_disabled")) {
    throw "Could not locate BoxedWine's standalone main() in knativesystem.cpp."
}


# XboxWine v0.2.9.3 stability fix.
#
# The matching 0.2.9.1 and 0.2.9.3 dumps have an identical crash:
# ImGui::Shutdown(nullptr) <- resetContext() <- recreateMainWindow().
#
# Do not depend on BOXEDWINE_UWP being defined in the BoxedWine static-library
# project. Guard the resetContext implementation itself instead.

$mainUiPath = Join-Path $RepoRoot "source\ui\mainui.cpp"
if (-not (Test-Path $mainUiPath)) {
    throw "Could not locate BoxedWine UI source: $mainUiPath"
}

$mainUi = [System.IO.File]::ReadAllText($mainUiPath)
$safeResetMarker = "XBOXWINE_SAFE_RESET_CONTEXT_V3"

if (-not $mainUi.Contains($safeResetMarker)) {
    $resetFunctionPattern = '(?s)void\s+resetContext\s*\(\s*\)\s*\{\s*ImGui_ImplOpenGL3_Shutdown\s*\(\s*\)\s*;\s*SDL_GL_DeleteContext\s*\(\s*gl_context\s*\)\s*;\s*ImGui_ImplSDL2_Shutdown\s*\(\s*\)\s*;\s*ImGui::DestroyContext\s*\(\s*\)\s*;\s*appRunning\s*=\s*true\s*;\s*\}'

    $safeResetFunction = @'
void resetContext() {
    // XBOXWINE_SAFE_RESET_CONTEXT_V3
    //
    // XboxWine enters boxedmain() directly, without first creating
    // BoxedWine's ImGui launcher UI. The old code unconditionally called
    // ImGui::DestroyContext(), which dereferenced a null ImGuiContext.
    ImGuiContext* context = ImGui::GetCurrentContext();

    if (context != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();

        if (gl_context != nullptr) {
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
        }

        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext(context);
    } else {
        klog("XBOXWINE_SAFE_RESET_CONTEXT_V3: no ImGui context; skipping shutdown");
    }

    appRunning = true;
}
'@

    $mainUiAfter = [System.Text.RegularExpressions.Regex]::Replace(
        $mainUi,
        $resetFunctionPattern,
        $safeResetFunction,
        1
    )

    if ($mainUiAfter -eq $mainUi) {
        throw "Could not replace BoxedWine resetContext()."
    }

    $mainUi = $mainUiAfter
}

if (-not $mainUi.Contains($safeResetMarker)) {
    throw "The guarded resetContext implementation was not installed."
}

[System.IO.File]::WriteAllText(
    $mainUiPath,
    $mainUi,
    [System.Text.UTF8Encoding]::new($false)
)

# Replace the ineffective v0.2.9.3 call-site preprocessor wrapper, when it is
# present in a restored cache, with one normal call to the now-safe function.
$nativeScreenPath = Join-Path $RepoRoot "platform\sdl\knativescreenSDL.cpp"
if (-not (Test-Path $nativeScreenPath)) {
    throw "Could not locate BoxedWine screen source: $nativeScreenPath"
}

$nativeScreen = [System.IO.File]::ReadAllText($nativeScreenPath)

$oldCallSitePattern = '(?s)#if\s+!defined\s*\(\s*BOXEDWINE_UWP\s*\)\s*resetContext\s*\(\s*\)\s*;\s*#else\s*//\s*XBOXWINE:\s*skip\s*resetContext\s*without\s*BoxedWine\s*ImGui\s*UI\s*#endif'
$nativeScreen = [System.Text.RegularExpressions.Regex]::Replace(
    $nativeScreen,
    $oldCallSitePattern,
    'resetContext(); // XBOXWINE: guarded by XBOXWINE_SAFE_RESET_CONTEXT_V3',
    1
)

# Correct SDL destruction order and clear stale pointers.
$destroyFunctionPattern = '(?s)void\s+KNativeScreenSDL::destroyMainWindow\s*\(\s*\)\s*\{.*?\n\}'
$safeDestroyFunction = @'
void KNativeScreenSDL::destroyMainWindow() {
    destroyTextureCache();

#ifdef BOXEDWINE_UWP
    if (this->cursorTexture) {
        SDL_DestroyTexture(this->cursorTexture);
        this->cursorTexture = nullptr;
    }
#endif

    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }

    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}
'@

$nativeScreenAfter = [System.Text.RegularExpressions.Regex]::Replace(
    $nativeScreen,
    $destroyFunctionPattern,
    $safeDestroyFunction,
    1
)

if ($nativeScreenAfter -eq $nativeScreen -and
    -not $nativeScreen.Contains("renderer = nullptr;")) {
    throw "Could not replace KNativeScreenSDL::destroyMainWindow()."
}

$nativeScreen = $nativeScreenAfter

# Make the UWP renderer choice explicit and stop immediately when SDL cannot
# create the first window or renderer.
$windowCreateLine = 'window = SDL_CreateWindow("BoxedWine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, cx, cy, flags);'
if ($nativeScreen.Contains($windowCreateLine) -and
    -not $nativeScreen.Contains("XBOXWINE_DIRECT3D11_RENDERER_V3")) {
    $windowReplacement = @'
        // XBOXWINE_DIRECT3D11_RENDERER_V3
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
        window = SDL_CreateWindow("BoxedWine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, cx, cy, flags);
'@
    $nativeScreen = $nativeScreen.Replace(
        "        $windowCreateLine",
        $windowReplacement
    )
}

$oldWindowFailure = @'
        if (!window) {
            klog("SDL_CreateWindow failed: %s", SDL_GetError());
        }
'@
$newWindowFailure = @'
        if (!window) {
            klog("SDL_CreateWindow failed: %s", SDL_GetError());
            return;
        }
'@
$nativeScreen = $nativeScreen.Replace(
    $oldWindowFailure,
    $newWindowFailure
)

$oldRendererFallback = @'
        renderer = SDL_CreateRenderer(window, -1, flags);
        if (!renderer) {
            klog("Failed to create SDL accelerated renderer, will try software");
            flags &= ~SDL_RENDERER_ACCELERATED;
            flags |= SDL_RENDERER_SOFTWARE;
            renderer = SDL_CreateRenderer(window, -1, flags);
        }
'@
$newRendererFallback = @'
        renderer = SDL_CreateRenderer(window, -1, flags);
        if (!renderer) {
            klog("Failed to create SDL accelerated renderer, will try software");
            flags &= ~SDL_RENDERER_ACCELERATED;
            flags |= SDL_RENDERER_SOFTWARE;
            renderer = SDL_CreateRenderer(window, -1, flags);
        }
        if (!renderer) {
            klog("SDL_CreateRenderer failed: %s", SDL_GetError());
            SDL_DestroyWindow(window);
            window = nullptr;
            return;
        }
'@
$nativeScreen = $nativeScreen.Replace(
    $oldRendererFallback,
    $newRendererFallback
)

# Never create a cursor texture using a null renderer or null image data.
$cursorBlockPattern = '(?s)#ifdef\s+BOXEDWINE_UWP\s*// UWP needs to render it''s own cursor\s*if\s*\(!this->cursorTexture\)\s*\{.*?\}\s*#endif'
$safeCursorBlock = @'
#ifdef BOXEDWINE_UWP
    // UWP needs to render its own cursor.
    if (renderer && !this->cursorTexture) {
        int cursorWidth = 0;
        int cursorHeight = 0;
        int cursorChannels = 0;
        unsigned char* cursorData = stbi_load(
            "pointer_arrow.png",
            &cursorWidth,
            &cursorHeight,
            &cursorChannels,
            0
        );

        if (cursorData && cursorWidth > 0 && cursorHeight > 0) {
            this->cursorWidth = cursorWidth;
            this->cursorHeight = cursorHeight;
            this->cursorTexture = SDL_CreateTexture(
                renderer,
                SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC,
                cursorWidth,
                cursorHeight
            );

            if (this->cursorTexture) {
                SDL_UpdateTexture(
                    this->cursorTexture,
                    nullptr,
                    cursorData,
                    cursorWidth * cursorChannels
                );
                SDL_SetTextureBlendMode(
                    this->cursorTexture,
                    SDL_BLENDMODE_BLEND
                );
            }
        }

        if (cursorData) {
            stbi_image_free(cursorData);
        }
    }
#endif
'@

$nativeScreenAfter = [System.Text.RegularExpressions.Regex]::Replace(
    $nativeScreen,
    $cursorBlockPattern,
    $safeCursorBlock,
    1
)
if ($nativeScreenAfter -ne $nativeScreen) {
    $nativeScreen = $nativeScreenAfter
}

if (-not $nativeScreen.Contains("XBOXWINE_DIRECT3D11_RENDERER_V3")) {
    throw "The Direct3D 11 BoxedWine renderer marker was not installed."
}

[System.IO.File]::WriteAllText(
    $nativeScreenPath,
    $nativeScreen,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Installed XboxWine v0.2.9.3 core stability fixes." -ForegroundColor Green

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
$manifest = [System.Text.RegularExpressions.Regex]::Replace(
    $manifest,
    '(<Identity\b[^>]*\bVersion=")[^"]+(")',
    '${1}0.2.9.3${2}',
    1
)

if (-not $manifest.Contains('Version="0.2.9.3"')) {
    throw "Failed to set Package.appxmanifest version to 0.2.9.3."
}

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

Write-Host "Patched XboxWine Shelf v0.2.9.3 launch-fix source and Visual Studio project." -ForegroundColor Green
Write-Host "Original files saved in: $backupDir"
