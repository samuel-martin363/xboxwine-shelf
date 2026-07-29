#pragma once

#include <map>
#include <utility>
#include <string>
#include <vector>

namespace xboxwine {

struct ControllerProfile {
    bool enabled = true;
    bool rightStickMouse = true;
    int mouseSpeed = 14;
    int stickDeadzone = 9000;
    std::map<std::string, std::string> bindings;
};

struct GameEntry {
    std::string title;
    std::string nativeFolder;
    std::string nativeZip;
    std::string nativeRoot;
    std::string manifestPath;
    std::string wineExecutable;
    std::string architecture = "unknown";
    std::vector<std::string> executableCandidates;
    std::vector<std::string> executableArchitectures;
    std::vector<std::pair<std::string, std::string>> runtimeInstallers;
    std::vector<std::string> detectedKeys;
    std::string arguments;
    std::string fullscreen = "aspect";
    std::string resolution = "1280x720";
    std::string extraBoxedWineArgs;
    std::string runtimeZip;
    ControllerProfile controller;
};

std::map<std::string, std::string> DefaultControllerBindings();
bool PickGame(GameEntry& selected, std::string& diagnostic);
std::vector<std::string> BuildBoxedWineArguments(const GameEntry& game);

} // namespace xboxwine
