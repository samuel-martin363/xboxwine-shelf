/*
 * XboxWine Shelf
 * Local-storage library, LAN folder import, executable discovery metadata,
 * and per-game controller mapping editor.
 *
 * GPL-2.0-or-later
 */

#include "xbox_shelf.h"
#include "transfer_server.h"

#include "SDL2/SDL.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.ApplicationModel.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace xboxwine {
namespace {

using namespace winrt;
using namespace Windows::Storage;
using namespace Windows::ApplicationModel;
using namespace Windows::UI::Core;

const std::vector<std::pair<std::string, std::string>> kControllerInputs{
    {"A", "A BUTTON"}, {"B", "B BUTTON"},
    {"X", "X BUTTON"}, {"Y", "Y BUTTON"},
    {"LB", "LEFT BUMPER"}, {"RB", "RIGHT BUMPER"},
    {"LT", "LEFT TRIGGER"}, {"RT", "RIGHT TRIGGER"},
    {"VIEW", "VIEW BUTTON"}, {"MENU", "MENU BUTTON"},
    {"LS_CLICK", "LEFT STICK CLICK"}, {"RS_CLICK", "RIGHT STICK CLICK"},
    {"DPAD_UP", "D-PAD UP"}, {"DPAD_DOWN", "D-PAD DOWN"},
    {"DPAD_LEFT", "D-PAD LEFT"}, {"DPAD_RIGHT", "D-PAD RIGHT"},
    {"LS_UP", "LEFT STICK UP"}, {"LS_DOWN", "LEFT STICK DOWN"},
    {"LS_LEFT", "LEFT STICK LEFT"}, {"LS_RIGHT", "LEFT STICK RIGHT"},
    {"RS_UP", "RIGHT STICK UP"}, {"RS_DOWN", "RIGHT STICK DOWN"},
    {"RS_LEFT", "RIGHT STICK LEFT"}, {"RS_RIGHT", "RIGHT STICK RIGHT"}
};

std::atomic<bool> gSystemBackRequested{false};
std::atomic<Uint32> gIgnoreBackUntil{0};
SystemNavigationManager gSystemNavigationManager{nullptr};
winrt::event_token gSystemBackToken{};
bool gSystemBackHandlerInstalled = false;

void InstallSystemBackHandler() {
    if (gSystemBackHandlerInstalled) {
        return;
    }
    try {
        gSystemNavigationManager =
            SystemNavigationManager::GetForCurrentView();
        gSystemBackToken = gSystemNavigationManager.BackRequested(
            [](
                winrt::Windows::Foundation::IInspectable const&,
                winrt::Windows::UI::Core::BackRequestedEventArgs const& arguments
            ) {
                arguments.Handled(true);
                gSystemBackRequested.store(true);
            }
        );
        gSystemBackHandlerInstalled = true;
    } catch (...) {
        // SDL keyboard/controller events still provide a fallback.
    }
}

bool BackInputAllowed() {
    return SDL_GetTicks() >= gIgnoreBackUntil.load();
}

bool ConsumeSystemBackRequest() {
    const bool requested = gSystemBackRequested.exchange(false);
    return requested && BackInputAllowed();
}

bool IsBackEvent(const SDL_Event& event) {
    if (!BackInputAllowed()) {
        return false;
    }

    return (
        event.type == SDL_KEYDOWN &&
        event.key.keysym.sym == SDLK_ESCAPE
    ) || (
        event.type == SDL_CONTROLLERBUTTONDOWN &&
        event.cbutton.button == SDL_CONTROLLER_BUTTON_B
    );
}

void ClearBackInput() {
    // Xbox can deliver the same B press through SDL and UWP BackRequested.
    // Ignore the delayed duplicate after leaving a submenu.
    gIgnoreBackUntil.store(SDL_GetTicks() + 650);
    gSystemBackRequested.store(false);
    SDL_FlushEvent(SDL_KEYDOWN);
    SDL_FlushEvent(SDL_KEYUP);
    SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
    SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
}

std::string Trim(const std::string& input) {
    const auto first = std::find_if_not(
        input.begin(), input.end(),
        [](unsigned char c) { return std::isspace(c) != 0; }
    );
    if (first == input.end()) {
        return {};
    }
    const auto last = std::find_if_not(
        input.rbegin(), input.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }
    ).base();
    return std::string(first, last);
}

std::string Lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
    );
    return value;
}

std::string Upper(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );
    return value;
}

bool ParseBool(const std::string& value, bool fallback) {
    const std::string lowered = Lower(Trim(value));
    if (lowered == "1" || lowered == "true" || lowered == "yes" ||
        lowered == "on" || lowered == "keyboard_mouse") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" ||
        lowered == "off") {
        return false;
    }
    return fallback;
}

int ParseInt(const std::string& value, int fallback) {
    try {
        return std::stoi(Trim(value));
    } catch (...) {
        return fallback;
    }
}

std::map<std::string, std::string> ParseManifest(const std::string& text) {
    std::map<std::string, std::string> values;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        values[Lower(Trim(line.substr(0, equals)))] =
            Trim(line.substr(equals + 1));
    }
    return values;
}

std::vector<std::string> SplitList(
    const std::string& text,
    char delimiter
) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        item = Trim(item);
        if (item.empty()) {
            continue;
        }
        const std::string normalized = Upper(item);
        if (seen.insert(normalized).second) {
            result.push_back(item);
        }
    }
    return result;
}

std::vector<std::string> SplitOrderedList(
    const std::string& text,
    char delimiter
) {
    std::vector<std::string> result;
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, delimiter)) {
        result.push_back(Trim(item));
    }
    return result;
}

std::string JoinList(const std::vector<std::string>& values, char delimiter) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << delimiter;
        }
        output << values[index];
    }
    return output.str();
}

std::string JoinNative(const std::string& folder, const std::string& leaf) {
    if (folder.empty()) {
        return leaf;
    }
    const char last = folder.back();
    if (last == '\\' || last == '/') {
        return folder + leaf;
    }
    return folder + "\\" + leaf;
}


std::string PackagedRuntimePath() {
    static std::string cached;
    static bool searched = false;
    if (searched) {
        return cached;
    }
    searched = true;
    try {
        const StorageFolder installed = Package::Current().InstalledLocation();
        const auto files = installed.GetFilesAsync().get();
        for (const StorageFile& file : files) {
            const std::string name = Lower(to_string(file.Name()));
            if (name.size() >= 4 &&
                name.substr(name.size() - 4) == ".zip" &&
                name.find("wine") != std::string::npos) {
                cached = to_string(file.Path());
                break;
            }
        }
    } catch (...) {
    }
    return cached;
}

std::string BindingKey(const std::string& input) {
    return "bind_" + Lower(input);
}

bool TryLoadGame(
    const StorageFolder& folder,
    GameEntry& entry,
    std::string& reason
) {
    try {
        const auto manifestItem = folder.TryGetItemAsync(L"game.xwgame").get();
        if (!manifestItem || !manifestItem.IsOfType(StorageItemTypes::File)) {
            reason = "MISSING GAME.XWGAME IN " + Upper(to_string(folder.Name()));
            return false;
        }

        const auto manifestFile = manifestItem.as<StorageFile>();
        const std::string manifestText = to_string(
            FileIO::ReadTextAsync(manifestFile).get()
        );
        const auto values = ParseManifest(manifestText);
        const auto read = [&](const char* key, const std::string& fallback) {
            const auto found = values.find(key);
            return found == values.end() ? fallback : found->second;
        };

        entry.title = read("title", to_string(folder.Name()));
        entry.nativeFolder = to_string(folder.Path());
        entry.manifestPath = to_string(manifestFile.Path());

        const std::string zipName = read("zip", "game.zip");
        const auto zipItem = folder.TryGetItemAsync(to_hstring(zipName)).get();
        if (!zipItem || !zipItem.IsOfType(StorageItemTypes::File)) {
            reason = "MISSING " + Upper(zipName) + " FOR " + Upper(entry.title);
            return false;
        }
        entry.nativeZip = to_string(zipItem.Path());
        const StorageFolder writableRoot = folder.CreateFolderAsync(
            L"root",
            CreationCollisionOption::OpenIfExists
        ).get();
        entry.nativeRoot = to_string(writableRoot.Path());
        entry.wineExecutable = read("exe", "");
        entry.architecture = Lower(read("architecture", "unknown"));
        if (entry.wineExecutable.empty()) {
            reason = "NO EXECUTABLE WAS SELECTED FOR " + Upper(entry.title);
            return false;
        }

        entry.executableCandidates = SplitList(
            read("candidate_exes", ""), '|'
        );
        entry.executableArchitectures = SplitOrderedList(
            read("candidate_architectures", ""), '|'
        );
        if (entry.executableArchitectures.size() !=
            entry.executableCandidates.size()) {
            entry.executableArchitectures.assign(
                entry.executableCandidates.size(),
                "unknown"
            );
        }

        const auto addRuntime = [&](
            const char* key,
            const char* title
        ) {
            const std::string executable = read(key, "");
            if (!executable.empty()) {
                entry.runtimeInstallers.emplace_back(title, executable);
            }
        };
        addRuntime("runtime_vc14_x86", "VC++ 2015-2022 X86");
        addRuntime("runtime_directx", "DIRECTX LEGACY");
        addRuntime("runtime_dotnet", ".NET FRAMEWORK");
        addRuntime("runtime_openal", "OPENAL");

        entry.detectedKeys = SplitList(read("detected_keys", ""), ',');
        entry.arguments = read("arguments", "");
        entry.fullscreen = Lower(read("fullscreen", "aspect"));
        entry.resolution = read("resolution", "1280x720");
        entry.extraBoxedWineArgs = read("extra_boxedwine_args", "");

        const std::string runtime = read("runtime_zip", "");
        if (!runtime.empty()) {
            entry.runtimeZip = JoinNative(entry.nativeFolder, runtime);
        } else {
            entry.runtimeZip = PackagedRuntimePath();
        }
        if (entry.runtimeZip.empty()) {
            reason = "PACKAGED WINE FILESYSTEM ZIP WAS NOT FOUND";
            return false;
        }

        entry.controller.enabled = ParseBool(
            read("controller_bridge", "keyboard_mouse"), true
        );
        entry.controller.rightStickMouse = ParseBool(
            read("right_stick_mouse", "true"), true
        );
        entry.controller.mouseSpeed = std::clamp(
            ParseInt(read("mouse_speed", "14"), 14), 1, 80
        );
        entry.controller.stickDeadzone = std::clamp(
            ParseInt(read("stick_deadzone", "9000"), 9000), 1000, 30000
        );
        entry.controller.bindings = DefaultControllerBindings();
        for (const auto& input : kControllerInputs) {
            const std::string key = BindingKey(input.first);
            const auto found = values.find(key);
            if (found != values.end()) {
                entry.controller.bindings[input.first] = Upper(found->second);
            }
        }
        return true;
    } catch (const hresult_error& error) {
        reason = "STORAGE ERROR: " + Upper(to_string(error.message()));
        return false;
    } catch (const std::exception& error) {
        reason = Upper(error.what());
        return false;
    }
}

void ScanGameFolder(
    const StorageFolder& gamesFolder,
    std::vector<GameEntry>& games,
    std::string& diagnostic
) {
    const auto candidateFolders = gamesFolder.GetFoldersAsync().get();
    for (const StorageFolder& candidate : candidateFolders) {
        GameEntry entry;
        std::string reason;
        if (TryLoadGame(candidate, entry, reason)) {
            games.push_back(std::move(entry));
        } else if (diagnostic.empty()) {
            diagnostic = reason;
        }
    }
}

std::vector<GameEntry> ScanGames(std::string& diagnostic) {
    std::vector<GameEntry> games;
    diagnostic.clear();
    try {
        try {
            init_apartment(apartment_type::multi_threaded);
        } catch (...) {
        }

        const StorageFolder local = ApplicationData::Current().LocalFolder();
        const StorageFolder localGames = local.CreateFolderAsync(
            L"Games",
            CreationCollisionOption::OpenIfExists
        ).get();
        ScanGameFolder(localGames, games, diagnostic);

        // Keep removable storage support as an optional legacy path.
        try {
            const auto removableRoots =
                KnownFolders::RemovableDevices().GetFoldersAsync().get();
            for (const StorageFolder& drive : removableRoots) {
                const auto xboxWineItem = drive.TryGetItemAsync(L"XboxWine").get();
                if (!xboxWineItem ||
                    !xboxWineItem.IsOfType(StorageItemTypes::Folder)) {
                    continue;
                }
                const auto gamesItem = xboxWineItem.as<StorageFolder>()
                    .TryGetItemAsync(L"Games").get();
                if (gamesItem && gamesItem.IsOfType(StorageItemTypes::Folder)) {
                    ScanGameFolder(gamesItem.as<StorageFolder>(), games, diagnostic);
                }
            }
        } catch (...) {
        }

        std::sort(
            games.begin(), games.end(),
            [](const GameEntry& left, const GameEntry& right) {
                return Lower(left.title) < Lower(right.title);
            }
        );
        if (games.empty() && diagnostic.empty()) {
            diagnostic = "NO GAMES YET - PRESS Y TO SEND A FOLDER FROM YOUR PC";
        }
    } catch (const hresult_error& error) {
        diagnostic = "LIBRARY SCAN FAILED: " + Upper(to_string(error.message()));
    } catch (const std::exception& error) {
        diagnostic = "LIBRARY SCAN FAILED: " + Upper(error.what());
    }
    return games;
}

bool SaveGame(GameEntry& game, std::string& diagnostic) {
    try {
        std::ostringstream manifest;
        manifest << "# XboxWine Shelf game manifest\n";
        manifest << "title=" << game.title << "\n";
        manifest << "zip=game.zip\n";
        manifest << "exe=" << game.wineExecutable << "\n";
        manifest << "architecture=" << game.architecture << "\n";
        manifest << "candidate_exes="
                 << JoinList(game.executableCandidates, '|') << "\n";
        manifest << "candidate_architectures="
                 << JoinList(game.executableArchitectures, '|') << "\n";
        for (const auto& runtime : game.runtimeInstallers) {
            const std::string label = Upper(runtime.first);
            if (label.find("VC++") != std::string::npos) {
                manifest << "runtime_vc14_x86=" << runtime.second << "\n";
            } else if (label.find("DIRECTX") != std::string::npos) {
                manifest << "runtime_directx=" << runtime.second << "\n";
            } else if (label.find(".NET") != std::string::npos) {
                manifest << "runtime_dotnet=" << runtime.second << "\n";
            } else if (label.find("OPENAL") != std::string::npos) {
                manifest << "runtime_openal=" << runtime.second << "\n";
            }
        }
        manifest << "detected_keys=" << JoinList(game.detectedKeys, ',') << "\n";
        manifest << "arguments=" << game.arguments << "\n";
        manifest << "fullscreen=" << game.fullscreen << "\n";
        manifest << "resolution=" << game.resolution << "\n";
        manifest << "controller_bridge="
                 << (game.controller.enabled ? "keyboard_mouse" : "off") << "\n";
        manifest << "right_stick_mouse="
                 << (game.controller.rightStickMouse ? "true" : "false") << "\n";
        manifest << "mouse_speed=" << game.controller.mouseSpeed << "\n";
        manifest << "stick_deadzone=" << game.controller.stickDeadzone << "\n";
        manifest << "extra_boxedwine_args=" << game.extraBoxedWineArgs << "\n";
        for (const auto& input : kControllerInputs) {
            const auto found = game.controller.bindings.find(input.first);
            manifest << BindingKey(input.first) << "="
                     << (found == game.controller.bindings.end()
                         ? "NONE" : Upper(found->second))
                     << "\n";
        }

        const StorageFile file = StorageFile::GetFileFromPathAsync(
            to_hstring(game.manifestPath)
        ).get();
        FileIO::WriteTextAsync(
            file,
            to_hstring(manifest.str())
        ).get();
        diagnostic = "CONTROL PROFILE SAVED";
        return true;
    } catch (const hresult_error& error) {
        diagnostic = "SAVE FAILED: " + Upper(to_string(error.message()));
    } catch (const std::exception& error) {
        diagnostic = "SAVE FAILED: " + Upper(error.what());
    }
    return false;
}

std::array<unsigned char, 7> Glyph(char c) {
    switch (c) {
        case 'A': return {14,17,17,31,17,17,17};
        case 'B': return {30,17,17,30,17,17,30};
        case 'C': return {14,17,16,16,16,17,14};
        case 'D': return {30,17,17,17,17,17,30};
        case 'E': return {31,16,16,30,16,16,31};
        case 'F': return {31,16,16,30,16,16,16};
        case 'G': return {14,17,16,23,17,17,15};
        case 'H': return {17,17,17,31,17,17,17};
        case 'I': return {31,4,4,4,4,4,31};
        case 'J': return {7,2,2,2,18,18,12};
        case 'K': return {17,18,20,24,20,18,17};
        case 'L': return {16,16,16,16,16,16,31};
        case 'M': return {17,27,21,21,17,17,17};
        case 'N': return {17,25,21,19,17,17,17};
        case 'O': return {14,17,17,17,17,17,14};
        case 'P': return {30,17,17,30,16,16,16};
        case 'Q': return {14,17,17,17,21,18,13};
        case 'R': return {30,17,17,30,20,18,17};
        case 'S': return {15,16,16,14,1,1,30};
        case 'T': return {31,4,4,4,4,4,4};
        case 'U': return {17,17,17,17,17,17,14};
        case 'V': return {17,17,17,17,17,10,4};
        case 'W': return {17,17,17,21,21,21,10};
        case 'X': return {17,17,10,4,10,17,17};
        case 'Y': return {17,17,10,4,4,4,4};
        case 'Z': return {31,1,2,4,8,16,31};
        case '0': return {14,17,19,21,25,17,14};
        case '1': return {4,12,4,4,4,4,14};
        case '2': return {14,17,1,2,4,8,31};
        case '3': return {30,1,1,14,1,1,30};
        case '4': return {2,6,10,18,31,2,2};
        case '5': return {31,16,16,30,1,1,30};
        case '6': return {14,16,16,30,17,17,14};
        case '7': return {31,1,2,4,8,8,8};
        case '8': return {14,17,17,14,17,17,14};
        case '9': return {14,17,17,15,1,1,14};
        case ':': return {0,4,4,0,4,4,0};
        case '.': return {0,0,0,0,0,6,6};
        case '-': return {0,0,0,31,0,0,0};
        case '_': return {0,0,0,0,0,0,31};
        case '/': return {1,2,2,4,8,8,16};
        case '\\': return {16,8,8,4,2,2,1};
        case '[': return {14,8,8,8,8,8,14};
        case ']': return {14,2,2,2,2,2,14};
        case '(': return {2,4,8,8,8,4,2};
        case ')': return {8,4,2,2,2,4,8};
        case '?': return {14,17,1,2,4,0,4};
        case '!': return {4,4,4,4,4,0,4};
        case '+': return {0,4,4,31,4,4,0};
        case '=': return {0,31,0,31,0,0,0};
        case ' ': return {0,0,0,0,0,0,0};
        default: return {14,17,1,2,4,0,4};
    }
}

void DrawText(
    SDL_Renderer* renderer,
    int x,
    int y,
    const std::string& text,
    int scale,
    SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int cursorX = x;
    for (char raw : text) {
        const char c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(raw))
        );
        const auto glyph = Glyph(c);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((glyph[row] & (1 << (4 - column))) == 0) {
                    continue;
                }
                SDL_Rect pixel{
                    cursorX + column * scale,
                    y + row * scale,
                    scale,
                    scale
                };
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
        cursorX += 6 * scale;
    }
}

std::string Shorten(const std::string& value, std::size_t maximum) {
    if (value.size() <= maximum) {
        return value;
    }
    if (maximum <= 3) {
        return value.substr(0, maximum);
    }
    return value.substr(0, maximum - 3) + "...";
}

bool ControllerPressed(
    const SDL_Event& event,
    SDL_GameControllerButton button
) {
    return event.type == SDL_CONTROLLERBUTTONDOWN &&
           event.cbutton.button == button;
}

std::vector<std::string> Tokenize(const std::string& commandLine) {
    std::vector<std::string> result;
    std::string current;
    bool quoted = false;
    char quote = 0;
    for (std::size_t index = 0; index < commandLine.size(); ++index) {
        const char c = commandLine[index];
        if (quoted) {
            if (c == quote) {
                quoted = false;
                continue;
            }
            if (c == '\\' && index + 1 < commandLine.size() &&
                commandLine[index + 1] == quote) {
                current.push_back(quote);
                ++index;
                continue;
            }
            current.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'') {
            quoted = true;
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!current.empty()) {
                result.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

std::vector<std::string> ControlOptions(const GameEntry& game) {
    std::vector<std::string> options{
        "NONE", "MOUSE_LEFT", "MOUSE_RIGHT", "MOUSE_MIDDLE",
        "MOUSE_WHEEL_UP", "MOUSE_WHEEL_DOWN"
    };
    std::set<std::string> seen(options.begin(), options.end());
    auto add = [&](const std::string& raw) {
        const std::string value = Upper(Trim(raw));
        if (!value.empty() && seen.insert(value).second) {
            options.push_back(value);
        }
    };
    for (const std::string& key : game.detectedKeys) {
        add(key);
    }
    for (char c = 'A'; c <= 'Z'; ++c) {
        add(std::string(1, c));
    }
    for (char c = '0'; c <= '9'; ++c) {
        add(std::string(1, c));
    }
    for (const std::string& key : std::vector<std::string>{
        "SPACE", "ENTER", "ESCAPE", "TAB", "BACKSPACE",
        "SHIFT", "LSHIFT", "RSHIFT", "CTRL", "LCTRL", "RCTRL",
        "ALT", "LALT", "RALT", "UP", "DOWN", "LEFT", "RIGHT",
        "HOME", "END", "PAGEUP", "PAGEDOWN", "INSERT", "DELETE",
        "CAPSLOCK", "NUMLOCK", "SCROLLLOCK", "MINUS", "EQUALS",
        "COMMA", "PERIOD", "SLASH", "SEMICOLON", "QUOTE",
        "LEFTBRACKET", "RIGHTBRACKET", "BACKSLASH", "GRAVE",
        "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8",
        "F9", "F10", "F11", "F12", "F13", "F14", "F15", "F16",
        "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
        "NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3", "NUMPAD4",
        "NUMPAD5", "NUMPAD6", "NUMPAD7", "NUMPAD8", "NUMPAD9",
        "NUMPAD_PLUS", "NUMPAD_MINUS", "NUMPAD_MULTIPLY", "NUMPAD_DIVIDE",
        "NUMPAD_ENTER", "NUMPAD_PERIOD"
    }) {
        add(key);
    }
    return options;
}

void CycleBinding(
    GameEntry& game,
    const std::string& input,
    const std::vector<std::string>& options,
    int direction
) {
    std::string& current = game.controller.bindings[input];
    const std::string normalized = Upper(current);
    auto found = std::find(options.begin(), options.end(), normalized);
    int index = found == options.end()
        ? 0
        : static_cast<int>(std::distance(options.begin(), found));
    index = (index + direction + static_cast<int>(options.size())) %
            static_cast<int>(options.size());
    current = options[index];
}

bool EditControls(
    SDL_Renderer* renderer,
    GameEntry& game,
    std::string& diagnostic
) {
    const ControllerProfile original = game.controller;
    std::vector<std::string> options = ControlOptions(game);
    int selection = 0;
    bool running = true;
    bool saved = false;
    Uint32 lastMove = 0;

    while (running) {
        if (ConsumeSystemBackRequest()) {
            game.controller = original;
            running = false;
        }
        SDL_Event event{};
        while (running && SDL_PollEvent(&event)) {
            const Uint32 now = SDL_GetTicks();
            const bool canMove = now - lastMove > 120;
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_UP) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
                if (canMove) {
                    selection = (selection - 1 +
                        static_cast<int>(kControllerInputs.size())) %
                        static_cast<int>(kControllerInputs.size());
                    lastMove = now;
                }
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_DOWN) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
                if (canMove) {
                    selection = (selection + 1) %
                        static_cast<int>(kControllerInputs.size());
                    lastMove = now;
                }
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_LEFT) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) {
                if (canMove) {
                    CycleBinding(
                        game,
                        kControllerInputs[selection].first,
                        options,
                        -1
                    );
                    lastMove = now;
                }
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_RIGHT) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_A)) {
                if (canMove) {
                    CycleBinding(
                        game,
                        kControllerInputs[selection].first,
                        options,
                        1
                    );
                    lastMove = now;
                }
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_m) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_START)) {
                game.controller.rightStickMouse = !game.controller.rightStickMouse;
                diagnostic = game.controller.rightStickMouse
                    ? "RIGHT STICK MODE: MOUSE"
                    : "RIGHT STICK MODE: FOUR CUSTOM BINDINGS";
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_X)) {
                game.controller.bindings = DefaultControllerBindings();
                game.controller.rightStickMouse = true;
                diagnostic = "DEFAULT BINDINGS RESTORED";
            }
            if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_s) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_Y)) {
                saved = SaveGame(game, diagnostic);
                if (saved) {
                    running = false;
                }
            }
            if (IsBackEvent(event)) {
                game.controller = original;
                running = false;
            }
        }

        SDL_SetRenderDrawColor(renderer, 10, 14, 22, 255);
        SDL_RenderClear(renderer);
        SDL_Color white{235, 240, 248, 255};
        SDL_Color muted{145, 158, 178, 255};
        SDL_Color accent{54, 166, 255, 255};
        SDL_Color dark{20, 28, 42, 255};

        DrawText(renderer, 48, 35, "CONTROLLER CUSTOMIZATION", 4, white);
        DrawText(renderer, 50, 82, Shorten(game.title, 52), 2, muted);
        DrawText(
            renderer,
            50,
            110,
            game.controller.rightStickMouse
                ? "RIGHT STICK: MOUSE - PRESS MENU TO USE FOUR CUSTOM DIRECTIONS"
                : "RIGHT STICK: CUSTOM DIRECTIONS - PRESS MENU TO USE MOUSE",
            2,
            muted
        );

        const int visible = 9;
        const int first = std::max(0, std::min(
            selection - visible + 1,
            static_cast<int>(kControllerInputs.size()) - visible
        ));
        for (int row = 0; row < visible; ++row) {
            const int index = first + row;
            if (index >= static_cast<int>(kControllerInputs.size())) {
                break;
            }
            SDL_Rect item{48, 155 + row * 50, 1135, 41};
            if (index == selection) {
                SDL_SetRenderDrawColor(
                    renderer, accent.r, accent.g, accent.b, 255
                );
            } else {
                SDL_SetRenderDrawColor(renderer, dark.r, dark.g, dark.b, 255);
            }
            SDL_RenderFillRect(renderer, &item);
            const SDL_Color textColor = index == selection
                ? SDL_Color{5, 14, 24, 255} : white;
            DrawText(
                renderer,
                item.x + 15,
                item.y + 12,
                Shorten(kControllerInputs[index].second, 30),
                2,
                textColor
            );
            DrawText(
                renderer,
                item.x + 650,
                item.y + 12,
                Shorten(
                    game.controller.bindings[kControllerInputs[index].first],
                    28
                ),
                2,
                textColor
            );
        }

        DrawText(
            renderer,
            50,
            630,
            "A / LEFT / RIGHT CHANGE   MENU STICK MODE   X DEFAULTS   Y SAVE   B CANCEL",
            2,
            white
        );
        SDL_RenderPresent(renderer);
    }
    ClearBackInput();
    return saved;
}

bool CycleExecutable(GameEntry& game, std::string& diagnostic) {
    if (game.executableCandidates.empty()) {
        diagnostic = "NO ALTERNATE EXECUTABLES WERE FOUND";
        return false;
    }
    auto current = std::find(
        game.executableCandidates.begin(),
        game.executableCandidates.end(),
        game.wineExecutable
    );
    std::size_t index = current == game.executableCandidates.end()
        ? 0
        : (static_cast<std::size_t>(std::distance(
            game.executableCandidates.begin(), current
        )) + 1) % game.executableCandidates.size();
    game.wineExecutable = game.executableCandidates[index];
    if (index < game.executableArchitectures.size()) {
        game.architecture = Lower(game.executableArchitectures[index]);
    } else {
        game.architecture = "unknown";
    }
    if (!SaveGame(game, diagnostic)) {
        return false;
    }
    diagnostic = "EXECUTABLE: " + Upper(game.wineExecutable);
    return true;
}

void ShowTransferScreen(SDL_Renderer* renderer) {
    bool running = true;
    while (running) {
        if (ConsumeSystemBackRequest()) {
            running = false;
        }
        SDL_Event event{};
        while (running && SDL_PollEvent(&event)) {
            if (IsBackEvent(event)) {
                running = false;
            }
        }

        const TransferSnapshot transfer = GetTransferSnapshot();
        SDL_SetRenderDrawColor(renderer, 10, 14, 22, 255);
        SDL_RenderClear(renderer);
        SDL_Color white{235, 240, 248, 255};
        SDL_Color muted{145, 158, 178, 255};
        SDL_Color accent{54, 166, 255, 255};
        SDL_Color dark{20, 28, 42, 255};

        DrawText(renderer, 48, 42, "ADD A GAME FOLDER", 5, white);
        DrawText(
            renderer,
            50,
            105,
            "ON YOUR PC OPEN XBOXWINE_UPLOADER.PY AND CHOOSE THE WHOLE GAME FOLDER",
            2,
            muted
        );
        DrawText(
            renderer,
            50,
            135,
            "THE PC IS ONLY NEEDED FOR THIS TRANSFER - AFTERWARD THE GAME STAYS HERE",
            2,
            muted
        );

        SDL_Rect addressBox{48, 205, 1135, 120};
        SDL_SetRenderDrawColor(renderer, dark.r, dark.g, dark.b, 255);
        SDL_RenderFillRect(renderer, &addressBox);
        DrawText(renderer, 75, 230, "XBOX ADDRESS", 2, muted);
        DrawText(
            renderer,
            75,
            270,
            transfer.address.empty() ? "STARTING..." : transfer.address,
            4,
            accent
        );

        DrawText(renderer, 50, 375, Shorten(transfer.status, 82), 2, white);
        if (transfer.totalBytes > 0) {
            const double ratio = std::min(
                1.0,
                static_cast<double>(transfer.receivedBytes) /
                    static_cast<double>(transfer.totalBytes)
            );
            SDL_Rect track{50, 425, 1130, 26};
            SDL_SetRenderDrawColor(renderer, dark.r, dark.g, dark.b, 255);
            SDL_RenderFillRect(renderer, &track);
            SDL_Rect fill{
                track.x,
                track.y,
                static_cast<int>(track.w * ratio),
                track.h
            };
            SDL_SetRenderDrawColor(
                renderer, accent.r, accent.g, accent.b, 255
            );
            SDL_RenderFillRect(renderer, &fill);
            DrawText(
                renderer,
                50,
                470,
                std::to_string(static_cast<int>(ratio * 100.0)) + " PERCENT",
                3,
                white
            );
        }

        DrawText(renderer, 50, 665, "B  RETURN TO LIBRARY", 2, white);
        SDL_RenderPresent(renderer);
    }
}

} // namespace


enum class ShelfScreen {
    Home,
    Library,
    About
};

std::string ArchitectureLabel(const GameEntry& game) {
    const std::string value = Lower(game.architecture);
    if (value == "x86") {
        return "32-BIT X86";
    }
    if (value == "x64" || value == "amd64") {
        return "64-BIT X64";
    }
    if (value == "arm64") {
        return "ARM64";
    }
    return "ARCH UNKNOWN";
}

bool Is64BitGame(const GameEntry& game) {
    const std::string value = Lower(game.architecture);
    return value == "x64" || value == "amd64";
}

bool Is32BitGame(const GameEntry& game) {
    return Lower(game.architecture) == "x86";
}

std::string ArchitectureShortLabel(const GameEntry& game) {
    if (Is32BitGame(game)) {
        return "X86";
    }
    if (Is64BitGame(game)) {
        return "X64";
    }
    return "UNK";
}

std::string RuntimeArguments(const std::string& title) {
    const std::string label = Upper(title);
    if (label.find("VC++") != std::string::npos) {
        return "/install /quiet /norestart";
    }
    if (label.find("DIRECTX") != std::string::npos) {
        return "/silent";
    }
    if (label.find(".NET") != std::string::npos) {
        return "/q /norestart";
    }
    if (label.find("OPENAL") != std::string::npos) {
        return "/S";
    }
    return {};
}

void FillRect(
    SDL_Renderer* renderer,
    const SDL_Rect& rect,
    SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

void StrokeRect(
    SDL_Renderer* renderer,
    const SDL_Rect& rect,
    SDL_Color color,
    int thickness = 2
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int index = 0; index < thickness; ++index) {
        SDL_Rect line{
            rect.x + index,
            rect.y + index,
            rect.w - index * 2,
            rect.h - index * 2
        };
        SDL_RenderDrawRect(renderer, &line);
    }
}

void DrawButtonHint(
    SDL_Renderer* renderer,
    int x,
    int y,
    const std::string& button,
    const std::string& action,
    SDL_Color accent,
    SDL_Color text
) {
    SDL_Rect key{x, y, 38, 30};
    FillRect(renderer, key, accent);
    DrawText(renderer, x + 11, y + 8, button, 2, SDL_Color{7, 17, 28, 255});
    DrawText(renderer, x + 50, y + 8, action, 2, text);
}

std::map<std::string, std::string> DefaultControllerBindings() {
    return {
        {"A", "SPACE"}, {"B", "ESCAPE"},
        {"X", "E"}, {"Y", "Q"},
        {"LB", "SHIFT"}, {"RB", "CTRL"},
        {"LT", "MOUSE_RIGHT"}, {"RT", "MOUSE_LEFT"},
        {"VIEW", "TAB"}, {"MENU", "ENTER"},
        {"LS_CLICK", "NONE"}, {"RS_CLICK", "NONE"},
        {"DPAD_UP", "UP"}, {"DPAD_DOWN", "DOWN"},
        {"DPAD_LEFT", "LEFT"}, {"DPAD_RIGHT", "RIGHT"},
        {"LS_UP", "W"}, {"LS_DOWN", "S"},
        {"LS_LEFT", "A"}, {"LS_RIGHT", "D"},
        {"RS_UP", "NONE"}, {"RS_DOWN", "NONE"},
        {"RS_LEFT", "NONE"}, {"RS_RIGHT", "NONE"}
    };
}

bool ShowRuntimeMenu(
    SDL_Renderer* renderer,
    const GameEntry& game,
    GameEntry& selected,
    std::string& diagnostic
) {
    if (game.runtimeInstallers.empty()) {
        diagnostic =
            "NO RUNTIME INSTALLERS FOUND - REUPLOAD WITH VC++ X86 ENABLED";
        ClearBackInput();
        return false;
    }

    int selection = 0;
    bool running = true;
    bool accepted = false;
    Uint32 lastMove = 0;

    while (running) {
        if (ConsumeSystemBackRequest()) {
            running = false;
        }

        SDL_Event event{};
        while (running && SDL_PollEvent(&event)) {
            const Uint32 now = SDL_GetTicks();
            const bool canMove = now - lastMove > 140;

            if ((event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_UP) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_UP)) {
                if (canMove) {
                    selection = (selection - 1 +
                        static_cast<int>(game.runtimeInstallers.size())) %
                        static_cast<int>(game.runtimeInstallers.size());
                    lastMove = now;
                }
            }

            if ((event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_DOWN) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) {
                if (canMove) {
                    selection = (selection + 1) %
                        static_cast<int>(game.runtimeInstallers.size());
                    lastMove = now;
                }
            }

            if ((event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_RETURN) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_A)) {
                selected = game;
                selected.title = game.title + " - RUNTIME SETUP";
                selected.wineExecutable =
                    game.runtimeInstallers[selection].second;
                selected.architecture = "x86";
                selected.arguments = RuntimeArguments(
                    game.runtimeInstallers[selection].first
                );
                accepted = true;
                running = false;
            }

            if (IsBackEvent(event)) {
                running = false;
            }
        }

        SDL_SetRenderDrawColor(renderer, 7, 11, 18, 255);
        SDL_RenderClear(renderer);
        const SDL_Color white{239, 244, 251, 255};
        const SDL_Color muted{144, 159, 180, 255};
        const SDL_Color accent{54, 166, 255, 255};
        const SDL_Color panel{23, 34, 50, 255};

        DrawText(renderer, 48, 38, "RUNTIME SETUP", 5, white);
        DrawText(renderer, 50, 98, Shorten(game.title, 58), 2, muted);
        DrawText(
            renderer,
            50,
            128,
            "INSTALLERS MODIFY ONLY THIS GAME'S PRIVATE WINE ROOT",
            2,
            muted
        );

        for (int index = 0;
             index < static_cast<int>(game.runtimeInstallers.size());
             ++index) {
            SDL_Rect item{48, 190 + index * 74, 1135, 58};
            FillRect(
                renderer,
                item,
                index == selection ? accent : panel
            );
            DrawText(
                renderer,
                item.x + 20,
                item.y + 18,
                game.runtimeInstallers[index].first,
                3,
                index == selection
                    ? SDL_Color{5, 15, 26, 255}
                    : white
            );
        }

        DrawText(
            renderer,
            50,
            665,
            "A INSTALL   B RETURN - REOPEN XBOXWINE AFTER INSTALLER FINISHES",
            2,
            white
        );
        SDL_RenderPresent(renderer);
    }

    ClearBackInput();
    return accepted;
}

bool PickGame(GameEntry& selected, std::string& diagnostic) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");

    if (SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_GAMECONTROLLER |
            SDL_INIT_EVENTS
        ) != 0) {
        diagnostic = SDL_GetError();
        return false;
    }

    SDL_Window* window = SDL_CreateWindow(
        "XboxWine",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        SDL_WINDOW_FULLSCREEN_DESKTOP
    );

    if (!window) {
        diagnostic = SDL_GetError();
        return false;
    }

    InstallSystemBackHandler();

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

    SDL_RenderSetLogicalSize(renderer, 1280, 720);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_GameController* menuController = nullptr;
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            menuController = SDL_GameControllerOpen(index);
            if (menuController) {
                break;
            }
        }
    }

    StartTransferServer();

    std::vector<GameEntry> games = ScanGames(diagnostic);
    ShelfScreen screen = ShelfScreen::Home;
    int homeSelection = 0;
    int gameSelection = 0;
    bool running = true;
    bool accepted = false;
    Uint32 lastMove = 0;

    const std::array<std::string, 3> homeItems{
        "MY LIBRARY",
        "ADD A GAME",
        "SYSTEM STATUS"
    };

    while (running) {
        const bool systemBack = ConsumeSystemBackRequest();
        if (systemBack) {
            if (screen == ShelfScreen::Home) {
                running = false;
            } else {
                screen = ShelfScreen::Home;
                ClearBackInput();
            }
        }

        SDL_Event event{};

        while (running && SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }

            if (event.type == SDL_CONTROLLERDEVICEADDED && !menuController) {
                menuController = SDL_GameControllerOpen(event.cdevice.which);
            }

            const Uint32 now = SDL_GetTicks();
            const bool canMove = now - lastMove > 150;

            const bool up =
                (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_UP) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_UP);

            const bool down =
                (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_DOWN) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_DPAD_DOWN);

            const bool confirm =
                (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_RETURN) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_A);

            const bool back = IsBackEvent(event);

            if (screen == ShelfScreen::Home) {
                if (up && canMove) {
                    homeSelection =
                        (homeSelection - 1 +
                         static_cast<int>(homeItems.size())) %
                        static_cast<int>(homeItems.size());
                    lastMove = now;
                }

                if (down && canMove) {
                    homeSelection =
                        (homeSelection + 1) %
                        static_cast<int>(homeItems.size());
                    lastMove = now;
                }

                if (confirm) {
                    if (homeSelection == 0) {
                        screen = ShelfScreen::Library;
                    } else if (homeSelection == 1) {
                        ShowTransferScreen(renderer);
                        ClearBackInput();
                        games = ScanGames(diagnostic);
                        gameSelection = 0;
                    } else {
                        screen = ShelfScreen::About;
                    }
                }

                if (back) {
                    running = false;
                }

                continue;
            }

            if (screen == ShelfScreen::About) {
                if (back || confirm) {
                    screen = ShelfScreen::Home;
                    if (back) {
                        ClearBackInput();
                    }
                }
                continue;
            }

            if (up && canMove && !games.empty()) {
                gameSelection =
                    (gameSelection - 1 + static_cast<int>(games.size())) %
                    static_cast<int>(games.size());
                lastMove = now;
            }

            if (down && canMove && !games.empty()) {
                gameSelection =
                    (gameSelection + 1) %
                    static_cast<int>(games.size());
                lastMove = now;
            }

            if (confirm && !games.empty()) {
                if (Is64BitGame(games[gameSelection])) {
                    diagnostic =
                        "X64 DETECTED - CURRENT ENGINE IS BOXEDWINE32";
                } else if (!Is32BitGame(games[gameSelection])) {
                    diagnostic =
                        "UNKNOWN ARCHITECTURE - REUPLOAD WITH THE NEW UPLOADER";
                } else {
                    selected = games[gameSelection];
                    accepted = true;
                    running = false;
                }
            }

            if (
                ((event.type == SDL_KEYDOWN &&
                  event.key.keysym.sym == SDLK_x) ||
                 ControllerPressed(event, SDL_CONTROLLER_BUTTON_X)) &&
                !games.empty()
            ) {
                EditControls(renderer, games[gameSelection], diagnostic);
                ClearBackInput();
            }

            if (
                ((event.type == SDL_KEYDOWN &&
                  event.key.keysym.sym == SDLK_F6) ||
                 ControllerPressed(
                     event,
                     SDL_CONTROLLER_BUTTON_RIGHTSHOULDER
                 )) &&
                !games.empty()
            ) {
                GameEntry runtimeSelection;
                if (ShowRuntimeMenu(
                        renderer,
                        games[gameSelection],
                        runtimeSelection,
                        diagnostic
                    )) {
                    selected = runtimeSelection;
                    accepted = true;
                    running = false;
                }
                ClearBackInput();
            }

            if (
                ((event.type == SDL_KEYDOWN &&
                  event.key.keysym.sym == SDLK_TAB) ||
                 ControllerPressed(event, SDL_CONTROLLER_BUTTON_BACK)) &&
                !games.empty()
            ) {
                CycleExecutable(games[gameSelection], diagnostic);
            }

            if (
                (event.type == SDL_KEYDOWN &&
                 event.key.keysym.sym == SDLK_y) ||
                ControllerPressed(event, SDL_CONTROLLER_BUTTON_Y)
            ) {
                ShowTransferScreen(renderer);
                ClearBackInput();
                games = ScanGames(diagnostic);
                gameSelection = 0;
            }

            if (
                event.type == SDL_KEYDOWN &&
                event.key.keysym.sym == SDLK_r
            ) {
                games = ScanGames(diagnostic);
                gameSelection = 0;
            }

            if (back) {
                // B leaves the Library, not the entire application.
                screen = ShelfScreen::Home;
                ClearBackInput();
            }
        }

        if (menuController) {
            const Sint16 axis = SDL_GameControllerGetAxis(
                menuController,
                SDL_CONTROLLER_AXIS_LEFTY
            );
            const Uint32 now = SDL_GetTicks();

            if (now - lastMove > 220) {
                if (screen == ShelfScreen::Home) {
                    if (axis < -16000) {
                        homeSelection =
                            (homeSelection - 1 +
                             static_cast<int>(homeItems.size())) %
                            static_cast<int>(homeItems.size());
                        lastMove = now;
                    } else if (axis > 16000) {
                        homeSelection =
                            (homeSelection + 1) %
                            static_cast<int>(homeItems.size());
                        lastMove = now;
                    }
                } else if (
                    screen == ShelfScreen::Library &&
                    !games.empty()
                ) {
                    if (axis < -16000) {
                        gameSelection =
                            (gameSelection - 1 +
                             static_cast<int>(games.size())) %
                            static_cast<int>(games.size());
                        lastMove = now;
                    } else if (axis > 16000) {
                        gameSelection =
                            (gameSelection + 1) %
                            static_cast<int>(games.size());
                        lastMove = now;
                    }
                }
            }
        }

        const SDL_Color background{7, 11, 18, 255};
        const SDL_Color panel{17, 25, 38, 255};
        const SDL_Color panelSoft{23, 34, 50, 255};
        const SDL_Color white{239, 244, 251, 255};
        const SDL_Color muted{144, 159, 180, 255};
        const SDL_Color accent{54, 166, 255, 255};
        const SDL_Color accentSoft{34, 88, 126, 255};
        const SDL_Color warning{255, 191, 71, 255};
        const SDL_Color good{78, 214, 132, 255};

        SDL_SetRenderDrawColor(
            renderer,
            background.r,
            background.g,
            background.b,
            255
        );
        SDL_RenderClear(renderer);

        // Header bar.
        SDL_Rect header{0, 0, 1280, 104};
        FillRect(renderer, header, panel);
        SDL_Rect accentLine{0, 101, 1280, 3};
        FillRect(renderer, accentLine, accent);

        DrawText(renderer, 42, 28, "XBOXWINE", 5, white);
        DrawText(
            renderer,
            44,
            73,
            "LOCAL WINDOWS APPS ON XBOX",
            2,
            muted
        );

        if (screen == ShelfScreen::Home) {
            DrawText(renderer, 52, 138, "HOME", 3, muted);
            DrawText(
                renderer,
                52,
                180,
                "WHAT DO YOU WANT TO DO?",
                4,
                white
            );

            for (int index = 0; index < 3; ++index) {
                SDL_Rect card{
                    52,
                    252 + index * 106,
                    720,
                    82
                };

                const bool chosen = index == homeSelection;
                FillRect(
                    renderer,
                    card,
                    chosen ? accent : panelSoft
                );

                if (!chosen) {
                    StrokeRect(renderer, card, accentSoft, 2);
                }

                const SDL_Color labelColor = chosen
                    ? SDL_Color{5, 16, 27, 255}
                    : white;

                DrawText(
                    renderer,
                    card.x + 26,
                    card.y + 27,
                    homeItems[index],
                    3,
                    labelColor
                );

                DrawText(
                    renderer,
                    card.x + 620,
                    card.y + 27,
                    index == 0 ? "A" : (index == 1 ? "Y" : "?"),
                    3,
                    labelColor
                );
            }

            SDL_Rect status{820, 180, 400, 390};
            FillRect(renderer, status, panel);
            StrokeRect(renderer, status, accentSoft, 2);

            DrawText(renderer, 850, 212, "QUICK STATUS", 3, white);
            DrawText(renderer, 850, 274, "INSTALLED ITEMS", 2, muted);
            DrawText(
                renderer,
                850,
                310,
                std::to_string(games.size()),
                5,
                accent
            );

            DrawText(renderer, 850, 390, "WINDOWS ENGINE", 2, muted);
            DrawText(renderer, 850, 427, "BOXEDWINE 32", 3, good);

            DrawText(renderer, 850, 486, "64-BIT ENGINE", 2, muted);
            DrawText(renderer, 850, 523, "NOT READY", 3, warning);

            DrawButtonHint(
                renderer,
                52,
                650,
                "A",
                "SELECT",
                accent,
                white
            );
            DrawButtonHint(
                renderer,
                260,
                650,
                "B",
                "EXIT APP",
                panelSoft,
                white
            );
        } else if (screen == ShelfScreen::About) {
            DrawText(renderer, 52, 145, "SYSTEM STATUS", 4, white);

            SDL_Rect status{52, 210, 1170, 370};
            FillRect(renderer, status, panel);
            StrokeRect(renderer, status, accentSoft, 2);

            DrawText(renderer, 84, 246, "CURRENT ENGINE", 2, muted);
            DrawText(renderer, 84, 284, "BOXEDWINE / WINE 32-BIT", 3, white);

            DrawText(renderer, 84, 350, "SUPPORTED NOW", 2, muted);
            DrawText(
                renderer,
                84,
                386,
                "PORTABLE 16-BIT AND 32-BIT WINDOWS PROGRAMS",
                3,
                good
            );

            DrawText(renderer, 84, 452, "EXPERIMENTAL ROADMAP", 2, muted);
            DrawText(
                renderer,
                84,
                488,
                "VC++ X86 RUNTIME - GAME REDISTS - XBOXWINE64 RESEARCH",
                2,
                warning
            );

            DrawButtonHint(
                renderer,
                52,
                650,
                "B",
                "BACK TO HOME",
                panelSoft,
                white
            );
        } else {
            DrawText(renderer, 42, 126, "MY LIBRARY", 4, white);
            DrawText(
                renderer,
                44,
                170,
                "A SELECTED ITEM WILL ONLY LAUNCH WHEN ITS ENGINE IS SUPPORTED",
                2,
                muted
            );

            const int listTop = 218;
            const int itemHeight = 72;
            const int visible = 6;
            int first = std::max(0, gameSelection - visible + 1);

            for (int row = 0; row < visible; ++row) {
                const int gameIndex = first + row;
                if (gameIndex >= static_cast<int>(games.size())) {
                    break;
                }

                const bool chosen = gameIndex == gameSelection;
                SDL_Rect card{
                    42,
                    listTop + row * itemHeight,
                    760,
                    58
                };

                FillRect(
                    renderer,
                    card,
                    chosen ? accent : panelSoft
                );

                if (!chosen) {
                    StrokeRect(renderer, card, accentSoft, 1);
                }

                const SDL_Color itemText = chosen
                    ? SDL_Color{5, 15, 26, 255}
                    : white;

                DrawText(
                    renderer,
                    card.x + 18,
                    card.y + 11,
                    Shorten(games[gameIndex].title, 38),
                    3,
                    itemText
                );

                DrawText(
                    renderer,
                    card.x + 550,
                    card.y + 20,
                    ArchitectureShortLabel(games[gameIndex]),
                    2,
                    chosen
                        ? SDL_Color{5, 15, 26, 255}
                        : (Is64BitGame(games[gameIndex])
                            ? warning
                            : (Is32BitGame(games[gameIndex])
                                ? good
                                : muted))
                );
            }

            SDL_Rect details{836, 218, 402, 390};
            FillRect(renderer, details, panel);
            StrokeRect(renderer, details, accentSoft, 2);

            if (!games.empty()) {
                const GameEntry& game = games[gameSelection];

                DrawText(renderer, 864, 246, "SELECTED ITEM", 2, muted);
                DrawText(
                    renderer,
                    864,
                    280,
                    Shorten(game.title, 25),
                    3,
                    white
                );

                DrawText(renderer, 864, 342, "ARCHITECTURE", 2, muted);
                DrawText(
                    renderer,
                    864,
                    376,
                    ArchitectureLabel(game),
                    3,
                    Is64BitGame(game) ? warning : good
                );

                DrawText(renderer, 864, 438, "EXECUTABLE", 2, muted);
                DrawText(
                    renderer,
                    864,
                    472,
                    Shorten(game.wineExecutable, 28),
                    2,
                    white
                );

                DrawText(renderer, 864, 520, "DETECTED KEYS", 2, muted);
                DrawText(
                    renderer,
                    864,
                    550,
                    std::to_string(game.detectedKeys.size()),
                    3,
                    accent
                );
                DrawText(renderer, 1010, 520, "RUNTIMES", 2, muted);
                DrawText(
                    renderer,
                    1010,
                    550,
                    std::to_string(game.runtimeInstallers.size()),
                    3,
                    accent
                );
            } else {
                DrawText(renderer, 872, 270, "YOUR LIBRARY", 3, white);
                DrawText(renderer, 872, 326, "IS EMPTY", 4, muted);
                DrawText(renderer, 872, 406, "PRESS Y TO", 3, accent);
                DrawText(renderer, 872, 448, "ADD A FOLDER", 3, white);
            }

            if (!diagnostic.empty()) {
                SDL_Rect message{42, 622, 1196, 42};
                FillRect(renderer, message, panelSoft);
                DrawText(
                    renderer,
                    58,
                    636,
                    Shorten(diagnostic, 92),
                    2,
                    Is64BitGame(
                        games.empty()
                            ? GameEntry{}
                            : games[gameSelection]
                    ) ? warning : muted
                );
            }

            DrawButtonHint(renderer, 42, 676, "A", "PLAY", accent, white);
            DrawButtonHint(renderer, 190, 676, "X", "CONTROLS", panelSoft, white);
            DrawButtonHint(renderer, 414, 676, "Y", "ADD", panelSoft, white);
            DrawButtonHint(renderer, 558, 676, "RB", "RUNTIMES", panelSoft, white);
            DrawButtonHint(renderer, 808, 676, "B", "HOME", panelSoft, white);
            DrawText(renderer, 1040, 684, "VIEW EXE", 2, muted);
        }

        SDL_RenderPresent(renderer);
    }

    if (menuController) {
        SDL_GameControllerClose(menuController);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return accepted;
}

std::vector<std::string> BuildBoxedWineArguments(const GameEntry& game) {
    std::vector<std::string> arguments;
    arguments.push_back("XboxWineShelf");
    arguments.push_back("-title");
    arguments.push_back(game.title);
    if (!game.nativeRoot.empty()) {
        arguments.push_back("-root");
        arguments.push_back(game.nativeRoot);
    }
    if (!game.runtimeZip.empty()) {
        arguments.push_back("-zip");
        arguments.push_back(game.runtimeZip);
    }
    if (game.fullscreen == "stretch") {
        arguments.push_back("-fullscreen");
    } else if (game.fullscreen != "window") {
        arguments.push_back("-fullscreenAspect");
    }
    if (!game.resolution.empty()) {
        arguments.push_back("-resolution");
        arguments.push_back(game.resolution);
    }
    for (const std::string& token : Tokenize(game.extraBoxedWineArgs)) {
        arguments.push_back(token);
    }
    arguments.push_back("-mount");
    arguments.push_back(game.nativeZip);
    arguments.push_back("/home/username/.wine/dosdevices/c:/xboxwine");
    arguments.push_back("-w");
    arguments.push_back("/home/username/.wine/dosdevices/c:/xboxwine");
    arguments.push_back("/bin/wine");
    arguments.push_back(game.wineExecutable);
    for (const std::string& token : Tokenize(game.arguments)) {
        arguments.push_back(token);
    }
    return arguments;
}

} // namespace xboxwine
