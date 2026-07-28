#pragma once

#include "sources/content_source.h"
#include <Windows.h>
#include <array>
#include <cstddef>
#include <string>

struct hotkey_binding_t
{
    UINT virtualKey{};
    bool control{};
    bool alt{};
    bool shift{};
};

inline std::array<hotkey_binding_t, 6> g_media_hotkeys{ {
    { VK_MEDIA_PLAY_PAUSE, false, false, false },
    { VK_MEDIA_NEXT_TRACK, false, false, false },
    { VK_MEDIA_PREV_TRACK, false, false, false },
    { VK_VOLUME_MUTE, false, false, false },
    { VK_VOLUME_UP, false, false, false },
    { VK_VOLUME_DOWN, false, false, false }
} };

inline bool g_is_binding_hotkey{};

size_t media_command_index(media_command_t command);
const char* media_command_name(media_command_t command);
std::string hotkey_name(const hotkey_binding_t& binding);
void process_media_hotkeys(bool menu_visible);
