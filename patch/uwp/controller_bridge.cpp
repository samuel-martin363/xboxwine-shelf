/*
 * Xbox controller to guest keyboard/mouse bridge.
 * Per-game bindings are supplied by game.xwgame.
 *
 * GPL-2.0-or-later
 */

#include "controller_bridge.h"

#include "SDL2/SDL.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <string>

namespace xboxwine {
namespace {

std::mutex gMutex;
SDL_GameController* gController = nullptr;
ControllerProfile gProfile;
SDL_TimerID gTimer = 0;
std::atomic<bool> gInstalled{false};
std::map<std::string, bool> gStates;

std::string Upper(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); }
    );
    return value;
}

SDL_Keycode KeyFromName(const std::string& raw) {
    const std::string key = Upper(raw);
    if (key.size() == 1) {
        const char c = key[0];
        if (c >= 'A' && c <= 'Z') {
            return static_cast<SDL_Keycode>(SDLK_a + (c - 'A'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<SDL_Keycode>(SDLK_0 + (c - '0'));
        }
    }

    static const std::map<std::string, SDL_Keycode> known{
        {"SPACE", SDLK_SPACE}, {"ENTER", SDLK_RETURN},
        {"RETURN", SDLK_RETURN}, {"ESC", SDLK_ESCAPE},
        {"ESCAPE", SDLK_ESCAPE}, {"TAB", SDLK_TAB},
        {"BACKSPACE", SDLK_BACKSPACE}, {"SHIFT", SDLK_LSHIFT},
        {"LSHIFT", SDLK_LSHIFT}, {"RSHIFT", SDLK_RSHIFT},
        {"CTRL", SDLK_LCTRL}, {"CONTROL", SDLK_LCTRL},
        {"LCTRL", SDLK_LCTRL}, {"RCTRL", SDLK_RCTRL},
        {"ALT", SDLK_LALT}, {"LALT", SDLK_LALT},
        {"RALT", SDLK_RALT}, {"UP", SDLK_UP},
        {"DOWN", SDLK_DOWN}, {"LEFT", SDLK_LEFT},
        {"RIGHT", SDLK_RIGHT}, {"HOME", SDLK_HOME},
        {"END", SDLK_END}, {"PAGEUP", SDLK_PAGEUP},
        {"PAGEDOWN", SDLK_PAGEDOWN}, {"INSERT", SDLK_INSERT},
        {"DELETE", SDLK_DELETE}, {"CAPSLOCK", SDLK_CAPSLOCK},
        {"MINUS", SDLK_MINUS}, {"EQUALS", SDLK_EQUALS},
        {"COMMA", SDLK_COMMA}, {"PERIOD", SDLK_PERIOD},
        {"SLASH", SDLK_SLASH}, {"SEMICOLON", SDLK_SEMICOLON},
        {"QUOTE", SDLK_QUOTE}, {"LEFTBRACKET", SDLK_LEFTBRACKET},
        {"RIGHTBRACKET", SDLK_RIGHTBRACKET},
        {"BACKSLASH", SDLK_BACKSLASH}, {"GRAVE", SDLK_BACKQUOTE},
        {"NUMLOCK", SDLK_NUMLOCKCLEAR}, {"SCROLLLOCK", SDLK_SCROLLLOCK},
        {"F1", SDLK_F1}, {"F2", SDLK_F2}, {"F3", SDLK_F3},
        {"F4", SDLK_F4}, {"F5", SDLK_F5}, {"F6", SDLK_F6},
        {"F7", SDLK_F7}, {"F8", SDLK_F8}, {"F9", SDLK_F9},
        {"F10", SDLK_F10}, {"F11", SDLK_F11}, {"F12", SDLK_F12},
        {"F13", SDLK_F13}, {"F14", SDLK_F14}, {"F15", SDLK_F15},
        {"F16", SDLK_F16}, {"F17", SDLK_F17}, {"F18", SDLK_F18},
        {"F19", SDLK_F19}, {"F20", SDLK_F20}, {"F21", SDLK_F21},
        {"F22", SDLK_F22}, {"F23", SDLK_F23}, {"F24", SDLK_F24},
        {"NUMPAD0", SDLK_KP_0}, {"NUMPAD1", SDLK_KP_1},
        {"NUMPAD2", SDLK_KP_2}, {"NUMPAD3", SDLK_KP_3},
        {"NUMPAD4", SDLK_KP_4}, {"NUMPAD5", SDLK_KP_5},
        {"NUMPAD6", SDLK_KP_6}, {"NUMPAD7", SDLK_KP_7},
        {"NUMPAD8", SDLK_KP_8}, {"NUMPAD9", SDLK_KP_9},
        {"NUMPAD_PLUS", SDLK_KP_PLUS}, {"NUMPAD_MINUS", SDLK_KP_MINUS},
        {"NUMPAD_MULTIPLY", SDLK_KP_MULTIPLY},
        {"NUMPAD_DIVIDE", SDLK_KP_DIVIDE},
        {"NUMPAD_ENTER", SDLK_KP_ENTER}, {"NUMPAD_PERIOD", SDLK_KP_PERIOD}
    };

    const auto found = known.find(key);
    return found == known.end() ? SDLK_UNKNOWN : found->second;
}

void PushKey(SDL_Keycode key, bool down) {
    if (key == SDLK_UNKNOWN) {
        return;
    }
    SDL_Event event{};
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.repeat = 0;
    event.key.keysym.sym = key;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
    SDL_PushEvent(&event);
}

void PushMouseButton(Uint8 button, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    event.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.button.button = button;
    event.button.clicks = 1;
    SDL_PushEvent(&event);
}

void EmitBinding(const std::string& rawBinding, bool down) {
    const std::string binding = Upper(rawBinding);
    if (binding.empty() || binding == "NONE") {
        return;
    }
    if (binding == "MOUSE_LEFT") {
        PushMouseButton(SDL_BUTTON_LEFT, down);
        return;
    }
    if (binding == "MOUSE_RIGHT") {
        PushMouseButton(SDL_BUTTON_RIGHT, down);
        return;
    }
    if (binding == "MOUSE_MIDDLE") {
        PushMouseButton(SDL_BUTTON_MIDDLE, down);
        return;
    }
    if ((binding == "MOUSE_WHEEL_UP" || binding == "MOUSE_WHEEL_DOWN") && down) {
        SDL_Event event{};
        event.type = SDL_MOUSEWHEEL;
        event.wheel.y = binding == "MOUSE_WHEEL_UP" ? 1 : -1;
        event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        SDL_PushEvent(&event);
        return;
    }
    PushKey(KeyFromName(binding), down);
}

std::string BindingFor(const std::string& input) {
    const auto found = gProfile.bindings.find(input);
    return found == gProfile.bindings.end() ? "NONE" : found->second;
}

void SetAction(const std::string& input, bool wanted) {
    bool& current = gStates[input];
    if (current == wanted) {
        return;
    }
    current = wanted;
    EmitBinding(BindingFor(input), wanted);
}

std::string InputForButton(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return "A";
        case SDL_CONTROLLER_BUTTON_B: return "B";
        case SDL_CONTROLLER_BUTTON_X: return "X";
        case SDL_CONTROLLER_BUTTON_Y: return "Y";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
        case SDL_CONTROLLER_BUTTON_START: return "MENU";
        case SDL_CONTROLLER_BUTTON_BACK: return "VIEW";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "DPAD_UP";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "DPAD_DOWN";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "DPAD_LEFT";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "DPAD_RIGHT";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "LS_CLICK";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "RS_CLICK";
        default: return {};
    }
}

void OpenFirstController() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gController) {
        return;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index)) {
            continue;
        }
        gController = SDL_GameControllerOpen(index);
        if (gController) {
            return;
        }
    }
}

int SDLCALL EventWatch(void*, SDL_Event* event) {
    if (!gInstalled.load()) {
        return 1;
    }

    if (event->type == SDL_CONTROLLERDEVICEADDED) {
        OpenFirstController();
        return 1;
    }

    if (event->type == SDL_CONTROLLERDEVICEREMOVED) {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gController) {
            const SDL_JoystickID current = SDL_JoystickInstanceID(
                SDL_GameControllerGetJoystick(gController)
            );
            if (current == event->cdevice.which) {
                SDL_GameControllerClose(gController);
                gController = nullptr;
            }
        }
        return 1;
    }

    if (event->type == SDL_CONTROLLERBUTTONDOWN ||
        event->type == SDL_CONTROLLERBUTTONUP) {
        const std::string input = InputForButton(event->cbutton.button);
        if (!input.empty()) {
            SetAction(input, event->type == SDL_CONTROLLERBUTTONDOWN);
        }
    }

    return 1;
}

Uint32 SDLCALL PollController(Uint32 interval, void*) {
    if (!gInstalled.load()) {
        return interval;
    }

    std::lock_guard<std::mutex> lock(gMutex);
    if (!gController) {
        return interval;
    }

    SDL_GameControllerUpdate();
    const int deadzone = std::max(1000, gProfile.stickDeadzone);

    const Sint16 leftX = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_LEFTX
    );
    const Sint16 leftY = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_LEFTY
    );
    SetAction("LS_LEFT", leftX < -deadzone);
    SetAction("LS_RIGHT", leftX > deadzone);
    SetAction("LS_UP", leftY < -deadzone);
    SetAction("LS_DOWN", leftY > deadzone);

    const Sint16 rightX = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_RIGHTX
    );
    const Sint16 rightY = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_RIGHTY
    );

    if (gProfile.rightStickMouse) {
        auto scaleAxis = [&](Sint16 value) -> int {
            if (std::abs(static_cast<int>(value)) <= deadzone) {
                return 0;
            }
            const float normalized = static_cast<float>(value) / 32767.0f;
            return static_cast<int>(
                normalized * static_cast<float>(gProfile.mouseSpeed)
            );
        };
        const int dx = scaleAxis(rightX);
        const int dy = scaleAxis(rightY);
        if (dx != 0 || dy != 0) {
            SDL_Event motion{};
            motion.type = SDL_MOUSEMOTION;
            motion.motion.xrel = dx;
            motion.motion.yrel = dy;
            SDL_PushEvent(&motion);
        }
    } else {
        SetAction("RS_LEFT", rightX < -deadzone);
        SetAction("RS_RIGHT", rightX > deadzone);
        SetAction("RS_UP", rightY < -deadzone);
        SetAction("RS_DOWN", rightY > deadzone);
    }

    const Sint16 rightTrigger = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT
    );
    const Sint16 leftTrigger = SDL_GameControllerGetAxis(
        gController, SDL_CONTROLLER_AXIS_TRIGGERLEFT
    );
    SetAction("RT", rightTrigger > 12000);
    SetAction("LT", leftTrigger > 12000);

    return interval;
}

} // namespace

void InstallControllerBridge(const ControllerProfile& profile) {
    if (!profile.enabled || gInstalled.exchange(true)) {
        return;
    }

    gProfile = profile;
    if (gProfile.bindings.empty()) {
        gProfile.bindings = DefaultControllerBindings();
    }
    OpenFirstController();
    SDL_AddEventWatch(EventWatch, nullptr);
    gTimer = SDL_AddTimer(16, PollController, nullptr);
    (void)gTimer;
}

} // namespace xboxwine
