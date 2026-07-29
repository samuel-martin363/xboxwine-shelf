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
#include <fstream>
#include <string>
#include <thread>
#include <vector>

extern int boxedmain(int argc, const char** argv);

namespace {

void WriteStartupLog(const std::string& line, bool reset = false) noexcept {
    try {
        using namespace winrt::Windows::Storage;

        // Avoid blocking C++/WinRT async .get() calls during UWP startup.
        // The app is allowed to write synchronously inside its own LocalFolder.
        const StorageFolder local = ApplicationData::Current().LocalFolder();

        std::string path = winrt::to_string(local.Path());
        if (!path.empty() && path.back() != '\\') {
            path.push_back('\\');
        }
        path += "xboxwine-startup.log";

        const std::ios::openmode mode =
            std::ios::binary |
            std::ios::out |
            (reset ? std::ios::trunc : std::ios::app);

        std::ofstream file(path, mode);
        if (file) {
            file.write(line.data(), static_cast<std::streamsize>(line.size()));
            file.write("\r\n", 2);
            file.flush();
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