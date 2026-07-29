param(
    [string]$ProjectRoot = (Get-Location).Path
)

$ErrorActionPreference = "Stop"

$entryPath = Join-Path $ProjectRoot "patch\uwp\shelf_entry.cpp"
$patcherPath = Join-Path $ProjectRoot "apply-patch.ps1"

if (-not (Test-Path $entryPath)) {
    throw "Missing $entryPath"
}
if (-not (Test-Path $patcherPath)) {
    throw "Missing $patcherPath"
}

$entry = @'
/*
 * XboxWine minimal Xbox boot probe
 * Temporary diagnostic build
 * GPL-2.0-or-later
 */

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

extern "C" int SDL_main(int, char**) {
    // If SDL initialization returns an error, remain alive long enough for the
    // tester to distinguish a handled failure from an immediate process crash.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Delay(60000);
        return 10;
    }

    SDL_Window* window = SDL_CreateWindow(
        "XboxWine Boot Probe",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        SDL_Delay(60000);
        SDL_Quit();
        return 20;
    }

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
        SDL_Delay(60000);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 30;
    }

    // Bright green means the UWP entry point, SDL video system, window, and
    // renderer all initialized successfully.
    SDL_SetRenderDrawColor(renderer, 20, 180, 70, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    bool running = true;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            if (
                event.type == SDL_KEYDOWN &&
                event.key.keysym.sym == SDLK_ESCAPE
            ) {
                running = false;
            }
        }

        SDL_SetRenderDrawColor(renderer, 20, 180, 70, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
'@

[System.IO.File]::WriteAllText(
    $entryPath,
    $entry,
    [System.Text.UTF8Encoding]::new($false)
)

$patcher = [System.IO.File]::ReadAllText($patcherPath)

# Replace any current 0.2.x package version with the diagnostic version.
$patcher = [System.Text.RegularExpressions.Regex]::Replace(
    $patcher,
    '0\.2\.\d+\.0',
    '0.2.4.0'
)
$patcher = [System.Text.RegularExpressions.Regex]::Replace(
    $patcher,
    'v0\.2\.\d+',
    'v0.2.4'
)

[System.IO.File]::WriteAllText(
    $patcherPath,
    $patcher,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Applied XboxWine v0.2.4 minimal boot probe." -ForegroundColor Green
Write-Host "This build should display a solid green screen and remain open."
