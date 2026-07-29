/*
 * XboxWine Shelf entry point
 * GPL-2.0-or-later
 */

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include "controller_bridge.h"
#include "xbox_shelf.h"

#include <exception>
#include <string>
#include <vector>

extern int boxedmain(int argc, const char** argv);

extern "C" int SDL_main(int, char**) {
    xboxwine::GameEntry selected;
    std::string diagnostic;

    if (!xboxwine::PickGame(selected, diagnostic)) {
        return 0;
    }

    xboxwine::InstallControllerBridge(selected.controller);

    std::vector<std::string> owned = xboxwine::BuildBoxedWineArguments(selected);
    std::vector<const char*> raw;
    raw.reserve(owned.size());
    for (const std::string& value : owned) {
        raw.push_back(value.c_str());
    }

    try {
        return boxedmain(static_cast<int>(raw.size()), raw.data());
    } catch (const std::exception& error) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "XboxWine Shelf",
            error.what(),
            nullptr
        );
        return 1;
    } catch (...) {
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR,
            "XboxWine Shelf",
            "BoxedWine stopped because of an unknown exception.",
            nullptr
        );
        return 1;
    }
}
