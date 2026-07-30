#define NOMINMAX
#include "hotkeys.h"

#include "screens.h"

#include <algorithm>
#include <array>
#include <mutex>

size_t media_command_index(media_command_t command)
{
    return static_cast<size_t>(command);
}

const char* media_command_name(media_command_t command)
{
    switch (command)
    {
    case media_command_t::PLAY_PAUSE: return "Play / Pause";
    case media_command_t::NEXT: return "Next";
    case media_command_t::PREVIOUS: return "Previous";
    case media_command_t::MUTE: return "Mute";
    case media_command_t::VOLUME_UP: return "Volume Up";
    case media_command_t::VOLUME_DOWN: return "Volume Down";
    }
    return "Unknown";
}

std::string hotkey_name(const hotkey_binding_t& binding)
{
    if (binding.virtualKey == 0)
        return "Unassigned";

    std::string result;
    if (binding.control) result += "Ctrl + ";
    if (binding.alt) result += "Alt + ";
    if (binding.shift) result += "Shift + ";

    switch (binding.virtualKey)
    {
    case VK_MEDIA_PLAY_PAUSE: return result + "Media Play/Pause";
    case VK_MEDIA_NEXT_TRACK: return result + "Media Next";
    case VK_MEDIA_PREV_TRACK: return result + "Media Previous";
    case VK_VOLUME_MUTE: return result + "Volume Mute";
    case VK_VOLUME_UP: return result + "Volume Up";
    case VK_VOLUME_DOWN: return result + "Volume Down";
    case VK_SPACE: return result + "Space";
    case VK_ESCAPE: return result + "Escape";
    case VK_RETURN: return result + "Enter";
    case VK_BACK: return result + "Backspace";
    case VK_DELETE: return result + "Delete";
    case VK_UP: return result + "Up";
    case VK_DOWN: return result + "Down";
    case VK_LEFT: return result + "Left";
    case VK_RIGHT: return result + "Right";
    }

    const UINT scanCode = MapVirtualKeyA(binding.virtualKey, MAPVK_VK_TO_VSC);
    char name[64]{};
    if (GetKeyNameTextA(static_cast<LONG>(scanCode << 16), name, sizeof(name)) > 0)
        return result + name;
    return result + "VK " + std::to_string(binding.virtualKey);
}

bool dispatch_media_command(
    screen_t& screen,
    media_command_t command)
{
    if (!screen.source ||
        !screen.source->SupportsMediaControls())
        return false;

    const bool changeSpotifyLink =
        screen.contentMode == content_mode_t::INTEGRATED_MEDIA &&
        screen.mediaService == media_service_t::SPOTIFY &&
        (command == media_command_t::NEXT ||
            command == media_command_t::PREVIOUS);
    if (!changeSpotifyLink)
        return screen.source->SendMediaCommand(command);

    if (screen.spotifyUrls.empty())
        return false;

    const size_t count = screen.spotifyUrls.size();
    size_t selected = (std::min)(
        static_cast<size_t>(screen.selectedSpotifyUrl),
        count - 1);
    if (command == media_command_t::NEXT)
        selected = (selected + 1) % count;
    else
        selected = selected == 0 ? count - 1 : selected - 1;

    screen.selectedSpotifyUrl =
        static_cast<uint32_t>(selected);
    screen.mediaUrl = screen.spotifyUrls[selected];
    return screen.source->LoadMedia(screen.mediaUrl);
}

namespace {
    bool binding_pressed(const hotkey_binding_t& binding)
    {
        if (binding.virtualKey == 0)
            return false;
        if ((GetAsyncKeyState(static_cast<int>(binding.virtualKey)) & 1) == 0)
            return false;

        const bool control =
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt =
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool shift =
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        return binding.control == control &&
            binding.alt == alt &&
            binding.shift == shift;
    }

    void dispatch(media_command_t command)
    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        IContentSource* fallback{};
        for (auto& screen : g_screens)
        {
            if (!screen.source || !screen.source->SupportsMediaControls())
                continue;
            if (!fallback)
                fallback = screen.source.get();
            if (screen.hotkeyTarget)
            {
                dispatch_media_command(screen, command);
                return;
            }
        }
        if (fallback)
        {
            for (auto& screen : g_screens)
            {
                if (screen.source.get() == fallback)
                {
                    dispatch_media_command(screen, command);
                    break;
                }
            }
        }
    }
}

void process_media_hotkeys(bool menu_visible)
{
    if (g_is_binding_hotkey)
        return;

    // Normal bindings remain available while the menu is open, except simple
    // unmodified keyboard keys which would interfere with text entry.
    for (size_t i = 0; i < g_media_hotkeys.size(); ++i)
    {
        const auto& binding = g_media_hotkeys[i];
        if (menu_visible && !binding.control && !binding.alt && !binding.shift &&
            binding.virtualKey < VK_BROWSER_BACK)
            continue;
        if (binding_pressed(binding))
            dispatch(static_cast<media_command_t>(i));
    }
}
