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
 * XboxWine minimal software-surface boot probe
 * Temporary diagnostic build
 * GPL-2.0-or-later
 */

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

extern "C" int SDL_main(int, char**) {
    // Prevent SDL from selecting the crashing OpenGL renderer.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Delay(120000);
        return 10;
    }

    SDL_Window* window = SDL_CreateWindow(
        "XboxWine Software Probe",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_FULLSCREEN_DESKTOP
    );

    if (!window) {
        SDL_Delay(120000);
        SDL_Quit();
        return 20;
    }

    // Do not call SDL_CreateRenderer here. The crash dump proved that the
    // bundled SDL library enters a broken OpenGL context path from there.
    SDL_Surface* surface = SDL_GetWindowSurface(window);

    if (surface) {
        const Uint32 green = SDL_MapRGB(
            surface->format,
            20,
            180,
            70
        );

        SDL_FillRect(surface, nullptr, green);
        SDL_UpdateWindowSurface(window);
    }

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

        if (surface) {
            const Uint32 green = SDL_MapRGB(
                surface->format,
                20,
                180,
                70
            );

            SDL_FillRect(surface, nullptr, green);
            SDL_UpdateWindowSurface(window);
        }

        SDL_Delay(16);
    }

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

$patcher = [System.Text.RegularExpressions.Regex]::Replace(
    $patcher,
    '0\.2\.\d+\.0',
    '0.2.5.0'
)

$patcher = [System.Text.RegularExpressions.Regex]::Replace(
    $patcher,
    'v0\.2\.\d+',
    'v0.2.5'
)

[System.IO.File]::WriteAllText(
    $patcherPath,
    $patcher,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host "Applied XboxWine v0.2.5 software-surface probe." -ForegroundColor Green
Write-Host "This probe never calls SDL_CreateRenderer or OpenGL."
