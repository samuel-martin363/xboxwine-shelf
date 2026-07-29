param(
    [string]$ProjectRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'

$entryPath = Join-Path $ProjectRoot 'patch\uwp\shelf_entry.cpp'
$shelfPath = Join-Path $ProjectRoot 'patch\uwp\xbox_shelf.cpp'
$patcherPath = Join-Path $ProjectRoot 'apply-patch.ps1'

if (-not (Test-Path $entryPath)) { throw "Missing $entryPath" }
if (-not (Test-Path $shelfPath)) { throw "Missing $shelfPath" }
if (-not (Test-Path $patcherPath)) { throw "Missing $patcherPath" }

$entry = @'
/*
 * XboxWine Shelf entry point
 * Runtime-diagnostic build
 * GPL-2.0-or-later
 */

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include "controller_bridge.h"
#include "xbox_shelf.h"

#include <winrt/base.h>
#include <winrt/Windows.Storage.h>

#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <vector>

extern int boxedmain(int argc, const char** argv);

namespace {

void WriteStartupLog(const std::string& line, bool reset = false) noexcept {
    try {
        using namespace winrt;
        using namespace Windows::Storage;

        const StorageFolder local = ApplicationData::Current().LocalFolder();
        const StorageFile file = local.CreateFileAsync(
            L"xboxwine-startup.log",
            CreationCollisionOption::OpenIfExists
        ).get();

        const std::string text = line + "\r\n";
        if (reset) {
            FileIO::WriteTextAsync(file, to_hstring(text)).get();
        } else {
            FileIO::AppendTextAsync(file, to_hstring(text)).get();
        }
    } catch (...) {
        // Logging must never become another startup failure.
    }
}

void ReportStartupFailure(const std::string& message) noexcept {
    WriteStartupLog("STARTUP FAILURE: " + message);
    SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR,
        "XboxWine Shelf startup failure",
        message.c_str(),
        nullptr
    );
    // Keep the process alive briefly so Device Portal can observe it and so a
    // platform message box has time to appear.
    std::this_thread::sleep_for(std::chrono::seconds(8));
}

} // namespace

extern "C" int SDL_main(int, char**) {
    WriteStartupLog("Entered XboxWine SDL_main", true);

    try {
        xboxwine::GameEntry selected;
        std::string diagnostic;

        WriteStartupLog("Opening Shelf UI");
        if (!xboxwine::PickGame(selected, diagnostic)) {
            if (diagnostic.empty()) {
                diagnostic = "The Shelf UI stopped before returning an SDL error.";
            }
            ReportStartupFailure(diagnostic);
            return 1;
        }

        WriteStartupLog("Selected: " + selected.title);
        xboxwine::InstallControllerBridge(selected.controller);
        WriteStartupLog("Controller bridge installed");

        std::vector<std::string> owned = xboxwine::BuildBoxedWineArguments(selected);
        std::vector<const char*> raw;
        raw.reserve(owned.size());
        for (const std::string& value : owned) {
            raw.push_back(value.c_str());
        }

        WriteStartupLog("Starting BoxedWine");
        const int result = boxedmain(static_cast<int>(raw.size()), raw.data());
        WriteStartupLog("BoxedWine returned " + std::to_string(result));
        return result;
    } catch (const winrt::hresult_error& error) {
        const std::string message =
            "WinRT exception: " + winrt::to_string(error.message());
        ReportStartupFailure(message);
        return 1;
    } catch (const std::exception& error) {
        ReportStartupFailure(std::string("C++ exception: ") + error.what());
        return 1;
    } catch (...) {
        ReportStartupFailure("Unknown exception during XboxWine startup.");
        return 1;
    }
}
'@

[System.IO.File]::WriteAllText(
    $entryPath,
    $entry,
    [System.Text.UTF8Encoding]::new($false)
)

$shelf = [System.IO.File]::ReadAllText($shelfPath)

$oldInit = @'
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        diagnostic = SDL_GetError();
        return false;
    }
'@
$newInit = @'
    // Video and events are required. Controller initialization is optional so
    // a controller-backend problem cannot kill the entire UI before it draws.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        diagnostic = std::string("SDL INIT FAILED: ") + SDL_GetError();
        return false;
    }
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
'@
if (-not $shelf.Contains($oldInit)) {
    throw 'Could not find the expected SDL_Init block.'
}
$shelf = $shelf.Replace($oldInit, $newInit)

$shelf = $shelf.Replace(
    '        SDL_WINDOW_FULLSCREEN_DESKTOP',
    '        SDL_WINDOW_SHOWN'
)

$oldRenderer = @'
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        diagnostic = SDL_GetError();
        SDL_DestroyWindow(window);
        return false;
    }
'@
$newRenderer = @'
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED
    );
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        diagnostic = std::string("SDL RENDERER FAILED: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        return false;
    }

    // Present a visible frame before storage/network initialization. If this
    // stays onscreen, SDL is healthy and a later subsystem is the problem.
    SDL_SetRenderDrawColor(renderer, 10, 14, 22, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
'@
if (-not $shelf.Contains($oldRenderer)) {
    throw 'Could not find the expected renderer block.'
}
$shelf = $shelf.Replace($oldRenderer, $newRenderer)

$oldWindowFailure = @'
    if (!window) {
        diagnostic = SDL_GetError();
        return false;
    }
'@
$newWindowFailure = @'
    if (!window) {
        diagnostic = std::string("SDL WINDOW FAILED: ") + SDL_GetError();
        return false;
    }
'@
if (-not $shelf.Contains($oldWindowFailure)) {
    throw 'Could not find the expected SDL window failure block.'
}
$shelf = $shelf.Replace($oldWindowFailure, $newWindowFailure)

[System.IO.File]::WriteAllText(
    $shelfPath,
    $shelf,
    [System.Text.UTF8Encoding]::new($false)
)

# Bump the package version so Xbox Device Portal installs this as an update.
$patcher = [System.IO.File]::ReadAllText($patcherPath)
$patcher = $patcher.Replace('0.2.2.0', '0.2.3.0')
$patcher = $patcher.Replace('v0.2.2', 'v0.2.3')
[System.IO.File]::WriteAllText(
    $patcherPath,
    $patcher,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'Applied Xbox runtime diagnostics and safer SDL startup.' -ForegroundColor Green
Write-Host 'Changed:'
Write-Host '  patch\uwp\shelf_entry.cpp'
Write-Host '  patch\uwp\xbox_shelf.cpp'
Write-Host '  apply-patch.ps1 (version 0.2.3.0)'
