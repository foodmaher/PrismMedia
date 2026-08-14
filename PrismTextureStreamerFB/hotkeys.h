#pragma once

#include "sources/content_source.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <string>

struct screen_t;

struct hotkey_binding_t
{
    UINT virtualKey{};
    bool control{};
    bool alt{};
    bool shift{};
};

enum class gamepad_modifier_t : uint8_t
{
    NONE = 0,
    LEFT_BUMPER,
    RIGHT_BUMPER,
    LEFT_TRIGGER,
    RIGHT_TRIGGER,
    COUNT
};

enum class gamepad_input_t : uint8_t
{
    NONE = 0,
    A,
    B,
    X,
    Y,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    LEFT_STICK_CLICK,
    RIGHT_STICK_CLICK,
    LEFT_STICK_UP,
    LEFT_STICK_DOWN,
    LEFT_STICK_LEFT,
    LEFT_STICK_RIGHT,
    RIGHT_STICK_UP,
    RIGHT_STICK_DOWN,
    RIGHT_STICK_LEFT,
    RIGHT_STICK_RIGHT,
    LEFT_TRIGGER,
    RIGHT_TRIGGER,
    LEFT_BUMPER,
    RIGHT_BUMPER,
    START,
    BACK,
    COUNT
};

struct gamepad_binding_t
{
    gamepad_modifier_t modifier{ gamepad_modifier_t::RIGHT_BUMPER };
    gamepad_input_t input{ gamepad_input_t::NONE };
};

inline std::array<hotkey_binding_t, 6> g_media_hotkeys{ {
    { VK_OEM_6, false, false, false },
    { VK_OEM_4, false, false, false },
    { VK_OEM_5, false, false, false },
    { VK_OEM_MINUS, false, false, false },
    { VK_OEM_2, false, false, false },
    { VK_OEM_PERIOD, false, false, false }
} };

inline bool g_is_binding_hotkey{};

inline bool g_gamepad_hotkeys_enabled{ true };
// -1 automatically uses the first connected controller; 0-3 pins one.
inline int g_gamepad_controller_index = -1;
inline float g_gamepad_axis_threshold = 0.65f;
inline gamepad_binding_t g_gamepad_menu_hotkey{
    gamepad_modifier_t::LEFT_BUMPER,
    gamepad_input_t::RIGHT_STICK_CLICK
};
inline std::array<gamepad_binding_t, 6> g_media_gamepad_hotkeys{ {
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::A },
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::RIGHT_STICK_RIGHT },
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::RIGHT_STICK_LEFT },
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::B },
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::RIGHT_STICK_UP },
    { gamepad_modifier_t::RIGHT_BUMPER, gamepad_input_t::RIGHT_STICK_DOWN }
} };

size_t media_command_index(media_command_t command);
const char* media_command_name(media_command_t command);
std::string hotkey_name(const hotkey_binding_t& binding);
const char* gamepad_modifier_name(gamepad_modifier_t modifier);
const char* gamepad_input_name(gamepad_input_t input);
bool dispatch_media_command(
    screen_t& screen,
    media_command_t command);
void process_media_hotkeys(bool menu_visible);
bool consume_gamepad_menu_toggle_request();
