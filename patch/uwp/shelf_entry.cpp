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