#define NOMINMAX
#include "hotkeys.h"

#include "screens.h"
#include "diagnostic_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <mmsystem.h>
#include <Xinput.h>

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

const char* gamepad_modifier_name(gamepad_modifier_t modifier)
{
    switch (modifier)
    {
    case gamepad_modifier_t::NONE: return "None";
    case gamepad_modifier_t::LEFT_BUMPER: return "LB";
    case gamepad_modifier_t::RIGHT_BUMPER: return "RB";
    case gamepad_modifier_t::LEFT_TRIGGER: return "LT";
    case gamepad_modifier_t::RIGHT_TRIGGER: return "RT";
    case gamepad_modifier_t::COUNT: break;
    }
    return "Unknown";
}

const char* gamepad_input_name(gamepad_input_t input)
{
    switch (input)
    {
    case gamepad_input_t::NONE: return "Unassigned";
    case gamepad_input_t::A: return "A";
    case gamepad_input_t::B: return "B";
    case gamepad_input_t::X: return "X";
    case gamepad_input_t::Y: return "Y";
    case gamepad_input_t::DPAD_UP: return "D-pad Up";
    case gamepad_input_t::DPAD_DOWN: return "D-pad Down";
    case gamepad_input_t::DPAD_LEFT: return "D-pad Left";
    case gamepad_input_t::DPAD_RIGHT: return "D-pad Right";
    case gamepad_input_t::LEFT_STICK_CLICK: return "Left Stick Click";
    case gamepad_input_t::RIGHT_STICK_CLICK: return "Right Stick Click";
    case gamepad_input_t::LEFT_STICK_UP: return "Left Stick Up";
    case gamepad_input_t::LEFT_STICK_DOWN: return "Left Stick Down";
    case gamepad_input_t::LEFT_STICK_LEFT: return "Left Stick Left";
    case gamepad_input_t::LEFT_STICK_RIGHT: return "Left Stick Right";
    case gamepad_input_t::RIGHT_STICK_UP: return "Right Stick Up";
    case gamepad_input_t::RIGHT_STICK_DOWN: return "Right Stick Down";
    case gamepad_input_t::RIGHT_STICK_LEFT: return "Right Stick Left";
    case gamepad_input_t::RIGHT_STICK_RIGHT: return "Right Stick Right";
    case gamepad_input_t::LEFT_TRIGGER: return "LT";
    case gamepad_input_t::RIGHT_TRIGGER: return "RT";
    case gamepad_input_t::LEFT_BUMPER: return "LB";
    case gamepad_input_t::RIGHT_BUMPER: return "RB";
    case gamepad_input_t::COUNT: break;
    }
    return "Unknown";
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
        screen.spotifyPlaybackMode == spotify_playback_mode_t::EMBED &&
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
    using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    using JoyGetPosEx_t = MMRESULT(WINAPI*)(UINT, LPJOYINFOEX);
    using JoyGetDevCapsW_t = MMRESULT(WINAPI*)(UINT_PTR, LPJOYCAPSW, UINT);

    enum class gamepad_backend_t
    {
        NONE,
        XINPUT,
        WINDOWS_JOYSTICK
    };

    XInputGetState_t load_xinput_get_state()
    {
        static XInputGetState_t function = []() -> XInputGetState_t
        {
            const char* libraries[] = {
                "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"
            };
            for (const char* library : libraries)
            {
                const HMODULE module = LoadLibraryA(library);
                if (!module)
                    continue;
                const auto result = reinterpret_cast<XInputGetState_t>(
                    GetProcAddress(module, "XInputGetState"));
                if (result)
                    return result;
            }
            return nullptr;
        }();
        return function;
    }

    struct winmm_functions_t
    {
        JoyGetPosEx_t getPosition{};
        JoyGetDevCapsW_t getCapabilities{};
    };

    const winmm_functions_t& load_winmm_functions()
    {
        static const winmm_functions_t functions = []
        {
            winmm_functions_t result{};
            const HMODULE module = LoadLibraryW(L"winmm.dll");
            if (!module)
                return result;
            result.getPosition = reinterpret_cast<JoyGetPosEx_t>(
                GetProcAddress(module, "joyGetPosEx"));
            result.getCapabilities = reinterpret_cast<JoyGetDevCapsW_t>(
                GetProcAddress(module, "joyGetDevCapsW"));
            return result;
        }();
        return functions;
    }

    SHORT normalize_joystick_axis(
        DWORD value, UINT minimum, UINT maximum, bool invert = false)
    {
        if (maximum <= minimum)
            return 0;
        const double unit = (std::clamp)(
            (static_cast<double>(value) - minimum) /
                (static_cast<double>(maximum) - minimum),
            0.0, 1.0);
        double normalized = unit * 2.0 - 1.0;
        if (invert)
            normalized = -normalized;
        return static_cast<SHORT>(std::lround(
            normalized * 32767.0));
    }

    BYTE normalize_joystick_trigger(
        DWORD value, UINT minimum, UINT maximum)
    {
        if (maximum <= minimum)
            return 0;
        const double unit = (std::clamp)(
            (static_cast<double>(value) - minimum) /
                (static_cast<double>(maximum) - minimum),
            0.0, 1.0);
        return static_cast<BYTE>(std::lround(unit * 255.0));
    }

    void map_winmm_buttons(DWORD buttons, WORD& mapped)
    {
        struct button_map_t { DWORD source; WORD target; };
        static constexpr button_map_t map[] = {
            { 1u << 0, XINPUT_GAMEPAD_A },
            { 1u << 1, XINPUT_GAMEPAD_B },
            { 1u << 2, XINPUT_GAMEPAD_X },
            { 1u << 3, XINPUT_GAMEPAD_Y },
            { 1u << 4, XINPUT_GAMEPAD_LEFT_SHOULDER },
            { 1u << 5, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { 1u << 6, XINPUT_GAMEPAD_BACK },
            { 1u << 7, XINPUT_GAMEPAD_START },
            { 1u << 8, XINPUT_GAMEPAD_LEFT_THUMB },
            { 1u << 9, XINPUT_GAMEPAD_RIGHT_THUMB }
        };
        for (const auto& entry : map)
            if ((buttons & entry.source) != 0)
                mapped |= entry.target;
    }

    void map_winmm_pov(DWORD pov, WORD& mapped)
    {
        if (pov == JOY_POVCENTERED || pov > 35999)
            return;
        if (pov >= 31500 || pov <= 4500)
            mapped |= XINPUT_GAMEPAD_DPAD_UP;
        if (pov >= 4500 && pov <= 13500)
            mapped |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (pov >= 13500 && pov <= 22500)
            mapped |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (pov >= 22500 && pov <= 31500)
            mapped |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    bool read_winmm_controller(
        UINT controller, XINPUT_STATE& state)
    {
        static std::array<JOYCAPSW, 16> cachedCapabilities{};
        static std::array<bool, 16> capabilitiesReady{};
        const auto& functions = load_winmm_functions();
        if (controller >= cachedCapabilities.size() ||
            !functions.getPosition || !functions.getCapabilities)
            return false;

        JOYINFOEX position{};
        position.dwSize = sizeof(position);
        position.dwFlags = JOY_RETURNALL;
        if (functions.getPosition(controller, &position) != JOYERR_NOERROR)
        {
            capabilitiesReady[controller] = false;
            return false;
        }

        JOYCAPSW& capabilities = cachedCapabilities[controller];
        if (!capabilitiesReady[controller])
        {
            capabilities = {};
            if (functions.getCapabilities(
                controller, &capabilities,
                sizeof(capabilities)) != JOYERR_NOERROR)
                return false;
            capabilitiesReady[controller] = true;
        }

        XINPUT_GAMEPAD& pad = state.Gamepad;
        map_winmm_buttons(position.dwButtons, pad.wButtons);
        if ((capabilities.wCaps & JOYCAPS_HASPOV) != 0)
            map_winmm_pov(position.dwPOV, pad.wButtons);
        pad.sThumbLX = normalize_joystick_axis(
            position.dwXpos, capabilities.wXmin, capabilities.wXmax);
        pad.sThumbLY = normalize_joystick_axis(
            position.dwYpos, capabilities.wYmin, capabilities.wYmax, true);

        // Most DirectInput-style pads expose the right stick as Z/R.
        if ((capabilities.wCaps & JOYCAPS_HASZ) != 0)
            pad.sThumbRX = normalize_joystick_axis(
                position.dwZpos, capabilities.wZmin, capabilities.wZmax);
        if ((capabilities.wCaps & JOYCAPS_HASR) != 0)
            pad.sThumbRY = normalize_joystick_axis(
                position.dwRpos, capabilities.wRmin, capabilities.wRmax, true);

        // Separate U/V axes, when present, are treated as analog triggers.
        if ((capabilities.wCaps & JOYCAPS_HASU) != 0)
            pad.bLeftTrigger = normalize_joystick_trigger(
                position.dwUpos, capabilities.wUmin, capabilities.wUmax);
        if ((capabilities.wCaps & JOYCAPS_HASV) != 0)
            pad.bRightTrigger = normalize_joystick_trigger(
                position.dwVpos, capabilities.wVmin, capabilities.wVmax);
        return true;
    }

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

    bool button_down(const XINPUT_GAMEPAD& pad, WORD button)
    {
        return (pad.wButtons & button) != 0;
    }

    bool trigger_down(BYTE value)
    {
        return value >= static_cast<BYTE>((std::clamp)(
            g_gamepad_axis_threshold, 0.20f, 0.95f) * 255.0f);
    }

    bool positive_axis(SHORT value)
    {
        return value >= static_cast<SHORT>((std::clamp)(
            g_gamepad_axis_threshold, 0.20f, 0.95f) * 32767.0f);
    }

    bool negative_axis(SHORT value)
    {
        return value <= static_cast<SHORT>(-(std::clamp)(
            g_gamepad_axis_threshold, 0.20f, 0.95f) * 32767.0f);
    }

    bool modifier_down(
        const XINPUT_GAMEPAD& pad,
        gamepad_modifier_t modifier)
    {
        switch (modifier)
        {
        case gamepad_modifier_t::NONE: return true;
        case gamepad_modifier_t::LEFT_BUMPER:
            return button_down(pad, XINPUT_GAMEPAD_LEFT_SHOULDER);
        case gamepad_modifier_t::RIGHT_BUMPER:
            return button_down(pad, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        case gamepad_modifier_t::LEFT_TRIGGER:
            return trigger_down(pad.bLeftTrigger);
        case gamepad_modifier_t::RIGHT_TRIGGER:
            return trigger_down(pad.bRightTrigger);
        case gamepad_modifier_t::COUNT: break;
        }
        return false;
    }

    bool input_down(const XINPUT_GAMEPAD& pad, gamepad_input_t input)
    {
        switch (input)
        {
        case gamepad_input_t::NONE: return false;
        case gamepad_input_t::A:
            return button_down(pad, XINPUT_GAMEPAD_A);
        case gamepad_input_t::B:
            return button_down(pad, XINPUT_GAMEPAD_B);
        case gamepad_input_t::X:
            return button_down(pad, XINPUT_GAMEPAD_X);
        case gamepad_input_t::Y:
            return button_down(pad, XINPUT_GAMEPAD_Y);
        case gamepad_input_t::DPAD_UP:
            return button_down(pad, XINPUT_GAMEPAD_DPAD_UP);
        case gamepad_input_t::DPAD_DOWN:
            return button_down(pad, XINPUT_GAMEPAD_DPAD_DOWN);
        case gamepad_input_t::DPAD_LEFT:
            return button_down(pad, XINPUT_GAMEPAD_DPAD_LEFT);
        case gamepad_input_t::DPAD_RIGHT:
            return button_down(pad, XINPUT_GAMEPAD_DPAD_RIGHT);
        case gamepad_input_t::LEFT_STICK_CLICK:
            return button_down(pad, XINPUT_GAMEPAD_LEFT_THUMB);
        case gamepad_input_t::RIGHT_STICK_CLICK:
            return button_down(pad, XINPUT_GAMEPAD_RIGHT_THUMB);
        case gamepad_input_t::LEFT_STICK_UP:
            return positive_axis(pad.sThumbLY);
        case gamepad_input_t::LEFT_STICK_DOWN:
            return negative_axis(pad.sThumbLY);
        case gamepad_input_t::LEFT_STICK_LEFT:
            return negative_axis(pad.sThumbLX);
        case gamepad_input_t::LEFT_STICK_RIGHT:
            return positive_axis(pad.sThumbLX);
        case gamepad_input_t::RIGHT_STICK_UP:
            return positive_axis(pad.sThumbRY);
        case gamepad_input_t::RIGHT_STICK_DOWN:
            return negative_axis(pad.sThumbRY);
        case gamepad_input_t::RIGHT_STICK_LEFT:
            return negative_axis(pad.sThumbRX);
        case gamepad_input_t::RIGHT_STICK_RIGHT:
            return positive_axis(pad.sThumbRX);
        case gamepad_input_t::LEFT_TRIGGER:
            return trigger_down(pad.bLeftTrigger);
        case gamepad_input_t::RIGHT_TRIGGER:
            return trigger_down(pad.bRightTrigger);
        case gamepad_input_t::LEFT_BUMPER:
            return button_down(pad, XINPUT_GAMEPAD_LEFT_SHOULDER);
        case gamepad_input_t::RIGHT_BUMPER:
            return button_down(pad, XINPUT_GAMEPAD_RIGHT_SHOULDER);
        case gamepad_input_t::COUNT: break;
        }
        return false;
    }

    bool read_gamepad(
        XINPUT_STATE& state,
        DWORD& controller,
        gamepad_backend_t& backend,
        bool& usedAutomaticFallback)
    {
        const XInputGetState_t getState = load_xinput_get_state();
        usedAutomaticFallback = false;

        if (getState && g_gamepad_controller_index >= 0 &&
            g_gamepad_controller_index < XUSER_MAX_COUNT)
        {
            controller = static_cast<DWORD>(g_gamepad_controller_index);
            if (getState(controller, &state) == ERROR_SUCCESS)
            {
                backend = gamepad_backend_t::XINPUT;
                return true;
            }
            usedAutomaticFallback = true;
        }

        if (getState)
        {
            for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
            {
                if (g_gamepad_controller_index >= 0 &&
                    index == static_cast<DWORD>(g_gamepad_controller_index))
                    continue;
                XINPUT_STATE candidate{};
                if (getState(index, &candidate) != ERROR_SUCCESS)
                    continue;
                state = candidate;
                controller = index;
                backend = gamepad_backend_t::XINPUT;
                return true;
            }
        }

        // Steam Input and many PlayStation/generic controllers are visible to
        // the legacy Windows joystick API even when XInput exposes no slot.
        const UINT preferred = g_gamepad_controller_index >= 0
            ? static_cast<UINT>(g_gamepad_controller_index)
            : static_cast<UINT>(-1);
        if (preferred != static_cast<UINT>(-1) &&
            read_winmm_controller(preferred, state))
        {
            controller = preferred;
            backend = gamepad_backend_t::WINDOWS_JOYSTICK;
            return true;
        }
        for (UINT index = 0; index < 16; ++index)
        {
            if (index == preferred)
                continue;
            XINPUT_STATE candidate{};
            if (!read_winmm_controller(index, candidate))
                continue;
            state = candidate;
            controller = index;
            backend = gamepad_backend_t::WINDOWS_JOYSTICK;
            usedAutomaticFallback = preferred != static_cast<UINT>(-1);
            return true;
        }
        backend = gamepad_backend_t::NONE;
        return false;
    }

    void process_gamepad_bindings()
    {
        static std::array<bool, 6> wasDown{};
        static std::array<uint64_t, 6> pressedAt{};
        static std::array<uint64_t, 6> repeatedAt{};
        static int lastController = -2;
        static gamepad_backend_t lastBackend = gamepad_backend_t::NONE;
        static uint64_t lastPollTick{};

        if (!g_gamepad_hotkeys_enabled)
        {
            wasDown.fill(false);
            return;
        }

        const uint64_t now = GetTickCount64();
        const uint64_t pollInterval = lastController == -1 ? 500 : 16;
        if (lastPollTick != 0 && now - lastPollTick < pollInterval)
            return;
        lastPollTick = now;

        XINPUT_STATE state{};
        DWORD controller{};
        gamepad_backend_t backend{};
        bool usedAutomaticFallback{};
        if (!read_gamepad(
            state, controller, backend, usedAutomaticFallback))
        {
            wasDown.fill(false);
            if (lastController != -1)
            {
                diagnostic_log::write(
                    "input", "No XInput or Windows joystick controller is connected.");
                lastController = -1;
                lastBackend = gamepad_backend_t::NONE;
            }
            return;
        }
        if (lastController != static_cast<int>(controller) ||
            lastBackend != backend)
        {
            if (backend == gamepad_backend_t::XINPUT)
                diagnostic_log::writef(
                    "input", "%sXInput controller %lu for media chords.",
                    usedAutomaticFallback ?
                        "Selected slot unavailable; using " : "Using ",
                    static_cast<unsigned long>(controller + 1));
            else
                diagnostic_log::writef(
                    "input", "%sWindows joystick controller %lu for media chords.",
                    usedAutomaticFallback ?
                        "Selected slot unavailable; using " : "Using ",
                    static_cast<unsigned long>(controller + 1));
            lastController = static_cast<int>(controller);
            lastBackend = backend;
        }

        for (size_t index = 0; index < g_media_gamepad_hotkeys.size(); ++index)
        {
            const auto& binding = g_media_gamepad_hotkeys[index];
            const bool down = modifier_down(state.Gamepad, binding.modifier) &&
                input_down(state.Gamepad, binding.input);
            const bool volumeCommand =
                index == media_command_index(media_command_t::VOLUME_UP) ||
                index == media_command_index(media_command_t::VOLUME_DOWN);
            bool fire = down && !wasDown[index];
            if (fire)
            {
                pressedAt[index] = now;
                repeatedAt[index] = now;
            }
            else if (down && volumeCommand &&
                now >= pressedAt[index] + 450 &&
                now >= repeatedAt[index] + 150)
            {
                fire = true;
                repeatedAt[index] = now;
            }
            if (fire)
            {
                const auto command = static_cast<media_command_t>(index);
                dispatch(command);
                diagnostic_log::writef(
                    "input", "Gamepad media command: %s.",
                    media_command_name(command));
            }
            wasDown[index] = down;
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

    process_gamepad_bindings();
}
