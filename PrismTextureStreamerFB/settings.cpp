#include "settings.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "screens.h"
#include "diagnostic_log.h"
#include "hotkeys.h"
#include "override_assets.h"
#include "scs_logging.h"
#include "sources/media_client.h"
#include "sources/native_media.h"
#include "sources/window.h"
#include "sources/wgc_window.h"
#include "telemetry_state.h"
#include "environment_audio.h"

using namespace scs_logging;

namespace {
    std::string config_path()
    {
        char localAppData[MAX_PATH]{};
        const auto localAppDataLength = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
        if (localAppDataLength > 0 && localAppDataLength < MAX_PATH)
        {
            std::string directory(localAppData);
            directory += "\\PrismTextureStreamerFB";
            CreateDirectoryA(directory.c_str(), nullptr);
            return directory + "\\config.ini";
        }

        // Fallback for unusual environments without LOCALAPPDATA.
        static int moduleAnchor{};
        HMODULE module{};
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&moduleAnchor),
            &module);

        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        std::string result(path);
        const auto slash = result.find_last_of("\\/");
        if (slash != std::string::npos)
            result.resize(slash + 1);
        else
            result.clear();
        result += "PrismTextureStreamerFB.ini";
        return result;
    }

    constexpr size_t kConfigurationBackupCount = 3;

    bool regular_file_exists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    std::string backup_path(size_t index)
    {
        return config_path() + ".backup" +
            std::to_string(index + 1) + ".ini";
    }

    bool files_equal(
        const std::string& firstPath,
        const std::string& secondPath)
    {
        WIN32_FILE_ATTRIBUTE_DATA firstAttributes{};
        WIN32_FILE_ATTRIBUTE_DATA secondAttributes{};
        if (!GetFileAttributesExA(
                firstPath.c_str(), GetFileExInfoStandard,
                &firstAttributes) ||
            !GetFileAttributesExA(
                secondPath.c_str(), GetFileExInfoStandard,
                &secondAttributes))
            return false;
        if (firstAttributes.nFileSizeHigh != secondAttributes.nFileSizeHigh ||
            firstAttributes.nFileSizeLow != secondAttributes.nFileSizeLow)
            return false;

        std::ifstream first(firstPath, std::ios::binary);
        std::ifstream second(secondPath, std::ios::binary);
        if (!first || !second)
            return false;
        std::array<char, 4096> firstBuffer{};
        std::array<char, 4096> secondBuffer{};
        do
        {
            first.read(firstBuffer.data(), firstBuffer.size());
            second.read(secondBuffer.data(), secondBuffer.size());
            const std::streamsize firstCount = first.gcount();
            const std::streamsize secondCount = second.gcount();
            if (firstCount != secondCount ||
                !std::equal(
                    firstBuffer.begin(),
                    firstBuffer.begin() +
                        static_cast<size_t>(firstCount),
                    secondBuffer.begin()))
                return false;
        } while (first);
        return true;
    }

    bool copy_if_present(
        const std::string& source,
        const std::string& destination)
    {
        if (!regular_file_exists(source))
            return true;
        if (CopyFileA(
            source.c_str(), destination.c_str(), FALSE))
            return true;
        diagnostic_log::writef(
            "settings", "Could not copy configuration backup %s "
            "(Win32 error %lu).", source.c_str(), GetLastError());
        return false;
    }

    bool rotate_configuration_backups(const std::string& activePath)
    {
        if (!regular_file_exists(activePath))
            return true;
        for (size_t index = kConfigurationBackupCount - 1;
            index > 0; --index)
        {
            if (!copy_if_present(
                backup_path(index - 1), backup_path(index)))
                return false;
        }
        if (!CopyFileA(
            activePath.c_str(), backup_path(0).c_str(), FALSE))
        {
            diagnostic_log::writef(
                "settings", "Could not create newest configuration "
                "backup (Win32 error %lu).", GetLastError());
            return false;
        }
        return true;
    }

    std::string backup_description(size_t index)
    {
        const std::string path = backup_path(index);
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExA(
            path.c_str(), GetFileExInfoStandard, &attributes))
            return "Backup " + std::to_string(index + 1) + " - empty";

        FILETIME localTime{};
        SYSTEMTIME systemTime{};
        if (!FileTimeToLocalFileTime(
                &attributes.ftLastWriteTime, &localTime) ||
            !FileTimeToSystemTime(&localTime, &systemTime))
            return "Backup " + std::to_string(index + 1);
        char text[96]{};
        std::snprintf(
            text, sizeof(text),
            "Backup %zu - %04u-%02u-%02u %02u:%02u:%02u",
            index + 1,
            systemTime.wYear, systemTime.wMonth, systemTime.wDay,
            systemTime.wHour, systemTime.wMinute, systemTime.wSecond);
        return text;
    }

    std::string read_string(const std::string& path, const char* section, const char* key)
    {
        char value[2048]{};
        GetPrivateProfileStringA(section, key, "", value, static_cast<DWORD>(sizeof(value)), path.c_str());
        return value;
    }

    void write_number(const std::string& path, const char* section, const char* key, uint32_t value)
    {
        const auto text = std::to_string(value);
        WritePrivateProfileStringA(section, key, text.c_str(), path.c_str());
    }

    float read_float(
        const std::string& path,
        const char* section,
        const char* key,
        float fallback)
    {
        const std::string text = read_string(path, section, key);
        if (text.empty())
            return fallback;

        char* end{};
        const float value = std::strtof(text.c_str(), &end);
        return end == text.c_str() ? fallback : value;
    }

    void write_float(
        const std::string& path,
        const char* section,
        const char* key,
        float value)
    {
        char text[64]{};
        std::snprintf(text, sizeof(text), "%.4f", value);
        WritePrivateProfileStringA(section, key, text, path.c_str());
    }

    std::vector<std::string> read_url_list(
        const std::string& path,
        const std::string& section,
        const char* prefix)
    {
        std::vector<std::string> result;
        const std::string countKey = std::string(prefix) + "Count";
        const UINT count = (std::min)(
            GetPrivateProfileIntA(
                section.c_str(), countKey.c_str(), 0, path.c_str()),
            100U);
        result.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            const std::string key =
                std::string(prefix) + std::to_string(index);
            std::string value =
                read_string(path, section.c_str(), key.c_str());
            if (!value.empty())
                result.push_back(std::move(value));
        }
        return result;
    }

    void write_url_list(
        const std::string& path,
        const std::string& section,
        const char* prefix,
        const std::vector<std::string>& values)
    {
        const std::string countKey = std::string(prefix) + "Count";
        const size_t count = (std::min)(values.size(), size_t{ 100 });
        write_number(
            path, section.c_str(), countKey.c_str(),
            static_cast<uint32_t>(count));
        for (size_t index = 0; index < count; ++index)
        {
            const std::string key =
                std::string(prefix) + std::to_string(index);
            WritePrivateProfileStringA(
                section.c_str(), key.c_str(), values[index].c_str(),
                path.c_str());
        }
    }

}

namespace settings {
    bool load()
    {
        const auto path = config_path();
        for (size_t hotkeyIndex = 0; hotkeyIndex < g_media_hotkeys.size(); ++hotkeyIndex)
        {
            const std::string section = "Hotkey" + std::to_string(hotkeyIndex);
            auto& binding = g_media_hotkeys[hotkeyIndex];
            binding.virtualKey = GetPrivateProfileIntA(
                section.c_str(), "Key", binding.virtualKey, path.c_str());
            binding.control = GetPrivateProfileIntA(
                section.c_str(), "Control", binding.control ? 1 : 0, path.c_str()) != 0;
            binding.alt = GetPrivateProfileIntA(
                section.c_str(), "Alt", binding.alt ? 1 : 0, path.c_str()) != 0;
            binding.shift = GetPrivateProfileIntA(
                section.c_str(), "Shift", binding.shift ? 1 : 0, path.c_str()) != 0;
        }

        g_gamepad_hotkeys_enabled = GetPrivateProfileIntA(
            "Gamepad", "Enabled", g_gamepad_hotkeys_enabled ? 1 : 0,
            path.c_str()) != 0;
        g_gamepad_controller_index = static_cast<int>((std::clamp)(
            GetPrivateProfileIntA(
                "Gamepad", "Controller",
                static_cast<UINT>(g_gamepad_controller_index + 1),
                path.c_str()),
            0U, 4U)) - 1;
        g_gamepad_axis_threshold = (std::clamp)(
            read_float(
                path, "Gamepad", "AxisThreshold",
                g_gamepad_axis_threshold),
            0.20f, 0.95f);
        g_gamepad_menu_hotkey.modifier =
            static_cast<gamepad_modifier_t>((std::clamp)(
                GetPrivateProfileIntA(
                    "Gamepad", "MenuModifier",
                    static_cast<UINT>(
                        g_gamepad_menu_hotkey.modifier), path.c_str()),
                0U,
                static_cast<UINT>(gamepad_modifier_t::COUNT) - 1));
        g_gamepad_menu_hotkey.input =
            static_cast<gamepad_input_t>((std::clamp)(
                GetPrivateProfileIntA(
                    "Gamepad", "MenuInput",
                    static_cast<UINT>(
                        g_gamepad_menu_hotkey.input), path.c_str()),
                0U,
                static_cast<UINT>(gamepad_input_t::COUNT) - 1));
        if (g_gamepad_menu_hotkey.modifier ==
                gamepad_modifier_t::LEFT_BUMPER &&
            g_gamepad_menu_hotkey.input == gamepad_input_t::START)
        {
            // LB + Start was the old default. ETS2/ATS also consumes Start and
            // can expose its own software cursor, leaving two cursors over the
            // plugin menu. Migrate only that exact legacy default.
            g_gamepad_menu_hotkey.input =
                gamepad_input_t::RIGHT_STICK_CLICK;
            diagnostic_log::write(
                "input", "Migrated legacy LB + Start menu chord to "
                "LB + Right Stick Click to prevent the game cursor opening.");
        }
        for (size_t bindingIndex = 0;
            bindingIndex < g_media_gamepad_hotkeys.size(); ++bindingIndex)
        {
            const std::string section =
                "GamepadHotkey" + std::to_string(bindingIndex);
            auto& binding = g_media_gamepad_hotkeys[bindingIndex];
            binding.modifier = static_cast<gamepad_modifier_t>((std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "Modifier",
                    static_cast<UINT>(binding.modifier), path.c_str()),
                0U,
                static_cast<UINT>(gamepad_modifier_t::COUNT) - 1));
            binding.input = static_cast<gamepad_input_t>((std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "Input",
                    static_cast<UINT>(binding.input), path.c_str()),
                0U,
                static_cast<UINT>(gamepad_input_t::COUNT) - 1));
        }

        {
            std::lock_guard<std::mutex> environmentLock(
                g_environment_audio_settings_mutex);
            g_environment_audio_settings.enabled = GetPrivateProfileIntA(
                "EnvironmentAudio", "Enabled",
                g_environment_audio_settings.enabled ? 1 : 0,
                path.c_str()) != 0;
            g_environment_audio_settings.interiorEffect = (std::clamp)(
                read_float(
                    path, "EnvironmentAudio", "InteriorEffect",
                    g_environment_audio_settings.interiorEffect),
                0.0f, 1.0f);
            g_environment_audio_settings.exteriorEffect = (std::clamp)(
                read_float(
                    path, "EnvironmentAudio", "ExteriorEffect",
                    g_environment_audio_settings.exteriorEffect),
                0.0f, 1.0f);
        }

        const int count = (std::min)(GetPrivateProfileIntA("General", "ScreenCount", 0, path.c_str()), 16U);
        if (count <= 0)
        {
            std::lock_guard<std::mutex> lock(g_screens_mutex);
            g_screens.clear();
            return true;
        }

        std::vector<screen_t> loaded;
        loaded.reserve(count);

        for (int i = 0; i < count; ++i)
        {
            const std::string section = "Screen" + std::to_string(i);
            const int type = GetPrivateProfileIntA(section.c_str(), "Type", 0, path.c_str());
            if (type < static_cast<int>(screen_type_t::GPS) ||
                type > static_cast<int>(screen_type_t::CUSTOM))
                continue;

            screen_t screen;
            screen.type = static_cast<screen_type_t>(type);
            screen.original_texture = read_string(path, section.c_str(), "OriginalTexture");
            screen.override_texture = read_string(path, section.c_str(), "OverrideTexture");
            screen.override_texture_size_w = GetPrivateProfileIntA(section.c_str(), "OverrideWidth", 0, path.c_str());
            screen.override_texture_size_h = GetPrivateProfileIntA(section.c_str(), "OverrideHeight", 0, path.c_str());
            screen.targetLiveTextureWidth = (std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "TargetWidth", 1280, path.c_str()), 64U, 7680U);
            screen.targetLiveTextureHeight = (std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "TargetHeight", 720, path.c_str()), 64U, 4320U);
            screen.framerate = static_cast<uint8_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "Framerate", 60, path.c_str()), 1U, 120U));
            screen.legacyCapture = GetPrivateProfileIntA(section.c_str(), "LegacyCapture", 0, path.c_str()) != 0;
            screen.flipVertical = GetPrivateProfileIntA(section.c_str(), "FlipVertical", 1, path.c_str()) != 0;
            screen.paused = GetPrivateProfileIntA(section.c_str(), "Paused", 0, path.c_str()) != 0;
            screen.scaleMode = static_cast<scale_mode_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ScaleMode", static_cast<UINT>(scale_mode_t::FIT), path.c_str()),
                0U, static_cast<UINT>(scale_mode_t::CROP)));
            screen.brightness = (std::clamp)(
                read_float(path, section.c_str(), "Brightness", 0.30f),
                0.10f, 2.0f);
            screen.autoBrightnessEnabled = GetPrivateProfileIntA(
                section.c_str(), "AutoBrightnessEnabled", 1,
                path.c_str()) != 0;
            screen.autoBrightnessDarkMultiplier = (std::clamp)(
                read_float(
                    path, section.c_str(),
                    "AutoBrightnessDarkMultiplier", 0.65f),
                0.25f, 1.25f);
            screen.autoBrightnessBrightMultiplier = (std::clamp)(
                read_float(
                    path, section.c_str(),
                    "AutoBrightnessBrightMultiplier", 1.15f),
                0.50f, 2.0f);
            screen.effectiveBrightness = screen.brightness;
            screen.edgeBleedGuard = static_cast<uint8_t>((std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "EdgeBleedGuard", 2, path.c_str()),
                0U, 16U));
            screen.performanceProfile = static_cast<performance_profile_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "PerformanceProfile", static_cast<UINT>(performance_profile_t::SMOOTH), path.c_str()),
                0U, static_cast<UINT>(performance_profile_t::SMOOTH)));
            screen.source_application_name = read_string(path, section.c_str(), "SourceApplication");
            screen.source_application_display_name = read_string(path, section.c_str(), "SourceTitle");
            screen.mediaClientId = read_string(
                path, section.c_str(), "MediaClientId");
            if (screen.mediaClientId.empty())
            {
                // The first pre-isolation screen keeps the historical profile
                // folder, preserving its cookies and sign-in. Additional
                // legacy screens receive deterministic independent profiles.
                screen.mediaClientId = i == 0
                    ? "legacy"
                    : sources::MakeMediaClientInstanceId(
                        section + "|" + screen.original_texture + "|" +
                        screen.override_texture);
            }
            screen.mediaUrl = read_string(path, section.c_str(), "MediaUrl");
            screen.mediaService = static_cast<media_service_t>((std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "MediaService",
                    static_cast<UINT>(media_service_t::YOUTUBE),
                    path.c_str()),
                0U, static_cast<UINT>(media_service_t::SPOTIFY)));
            screen.youtubeUrls =
                read_url_list(path, section, "YouTubeUrl");
            screen.spotifyUrls =
                read_url_list(path, section, "SpotifyUrl");
            screen.selectedYoutubeUrl = (std::min)(
                GetPrivateProfileIntA(
                    section.c_str(), "SelectedYouTubeUrl", 0,
                    path.c_str()),
                screen.youtubeUrls.empty()
                    ? 0U
                    : static_cast<UINT>(screen.youtubeUrls.size() - 1));
            screen.selectedSpotifyUrl = (std::min)(
                GetPrivateProfileIntA(
                    section.c_str(), "SelectedSpotifyUrl", 0,
                    path.c_str()),
                screen.spotifyUrls.empty()
                    ? 0U
                    : static_cast<UINT>(screen.spotifyUrls.size() - 1));
            // Migrate a single 4.0.0 MediaUrl into the restored library once.
            if (screen.youtubeUrls.empty() && screen.spotifyUrls.empty() &&
                !screen.mediaUrl.empty())
            {
                if (screen.mediaService == media_service_t::SPOTIFY)
                    screen.spotifyUrls.push_back(screen.mediaUrl);
                else
                    screen.youtubeUrls.push_back(screen.mediaUrl);
            }
            screen.contentMode = static_cast<content_mode_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ContentMode",
                    static_cast<UINT>(content_mode_t::INTEGRATED_MEDIA), path.c_str()),
                0U, static_cast<UINT>(content_mode_t::NATIVE_DIRECT_MEDIA)));
            screen.hotkeyTarget = GetPrivateProfileIntA(
                section.c_str(), "HotkeyTarget", 0, path.c_str()) != 0;
            screen.followTruckEngine = GetPrivateProfileIntA(
                section.c_str(), "FollowTruckEngine", 1, path.c_str()) != 0;
            screen.engineOffBrightness = (std::clamp)(
                read_float(
                    path, section.c_str(), "EngineOffBrightness", 0.35f),
                0.05f, 1.0f);
            screen.adaptiveAudioEnabled = GetPrivateProfileIntA(
                section.c_str(), "AdaptiveAudioEnabled", 1, path.c_str()) != 0;
            screen.adaptiveAudioInteriorVolume = (std::clamp)(
                read_float(
                    path, section.c_str(),
                    "AdaptiveAudioInteriorVolume", 1.0f),
                0.0f, 1.0f);
            screen.adaptiveAudioStrength = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioStrength", 0.85f),
                0.0f, 1.0f);
            screen.adaptiveAudioSpeakerAzimuth = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioSpeakerAzimuth", 0.0f),
                -180.0f, 180.0f);
            screen.adaptiveAudioFacingAwayVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioFacingAwayVolume", 0.0f),
                0.0f, 1.0f);
            screen.adaptiveAudioOutsideDistance = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioOutsideDistance", 0.25f),
                0.25f, 5.0f);
            screen.adaptiveAudioOutsideVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioOutsideVolume", 0.0f),
                0.0f, 1.0f);
            screen.adaptiveAudioMenuVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioMenuVolume", 0.15f),
                0.0f, 1.0f);
            screen.adaptiveAudioExternalDistanceEnabled =
                GetPrivateProfileIntA(
                    section.c_str(),
                    "AdaptiveAudioExternalDistanceEnabled",
                    1, path.c_str()) != 0;
            screen.adaptiveAudioExternalNearVolume =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalNearVolume",
                        1.0f),
                    0.0f, 1.0f);
            screen.adaptiveAudioExternalNearCutoff =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalNearCutoff",
                        144.0f),
                    20.0f, 20000.0f);
            screen.adaptiveAudioExternalFullVolumeDistance =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalFullVolumeDistance",
                        0.0f),
                    0.0f, 25.0f);
            screen.adaptiveAudioExternalMuteDistance =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalMuteDistance",
                        20.0f),
                    1.0f, 100.0f);
            screen.adaptiveAudioExternalLowPassEnabled =
                GetPrivateProfileIntA(
                    section.c_str(),
                    "AdaptiveAudioExternalLowPassEnabled",
                    1, path.c_str()) != 0;
            screen.adaptiveAudioExternalMinimumCutoff =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalMinimumCutoff",
                        20.0f),
                    20.0f, 8000.0f);

            std::vector<std::pair<uint32_t, uint32_t>> usedDimensions;
            std::vector<std::string> usedOverridePaths;
            bool identityConflict{};
            bool originalTextureConflict{};
            usedDimensions.reserve(loaded.size());
            usedOverridePaths.reserve(loaded.size());
            for (const auto& existing : loaded)
            {
                usedDimensions.emplace_back(
                    existing.override_texture_size_w,
                    existing.override_texture_size_h);
                usedOverridePaths.push_back(existing.override_texture);
                identityConflict = identityConflict ||
                    existing.override_texture == screen.override_texture ||
                    (existing.override_texture_size_w ==
                        screen.override_texture_size_w &&
                     existing.override_texture_size_h ==
                        screen.override_texture_size_h);
                originalTextureConflict = originalTextureConflict ||
                    (!screen.original_texture.empty() &&
                     existing.original_texture == screen.original_texture);
            }
            std::string overrideStatus;
            override_assets::ensure(
                screen, usedDimensions, usedOverridePaths,
                identityConflict, overrideStatus);

            if (originalTextureConflict)
            {
                diagnostic_log::writef(
                    "error",
                    "Screen %s was not started because another configured "
                    "display already owns game texture %s. Select a unique "
                    "accessory TOBJ; one Prism3D texture cannot carry two "
                    "independent media streams.",
                    screen.mediaClientId.c_str(),
                    screen.original_texture.c_str());
            }
            else if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA &&
                !screen.mediaUrl.empty())
            {
                g_screen_source_creation_in_progress = true;
                screen.source = sources::CreateMediaClientSource(
                    screen.mediaClientId,
                    screen.override_texture,
                    screen.mediaUrl, screen.framerate,
                    screen.targetLiveTextureWidth,
                    screen.targetLiveTextureHeight,
                    screen.mediaService == media_service_t::SPOTIFY);
                g_screen_source_creation_in_progress = false;
            }
            else if (screen.contentMode == content_mode_t::NATIVE_DIRECT_MEDIA &&
                !screen.mediaUrl.empty())
            {
                g_screen_source_creation_in_progress = true;
                screen.source = sources::CreateNativeMediaSource(
                    screen.mediaUrl, screen.framerate,
                    screen.targetLiveTextureWidth,
                    screen.targetLiveTextureHeight);
                g_screen_source_creation_in_progress = false;
            }
            else if (screen.contentMode == content_mode_t::WINDOW_CAPTURE &&
                !screen.source_application_name.empty())
            {
                g_screen_source_creation_in_progress = true;
                if (screen.legacyCapture)
                    screen.source = sources::CreateWindowSource(
                        screen.source_application_name.c_str(),
                        screen.source_application_display_name.empty() ? nullptr : screen.source_application_display_name.c_str(),
                        screen.framerate,
                        screen.targetLiveTextureWidth,
                        screen.targetLiveTextureHeight);
                else
                    screen.source = sources::CreateWgcWindowSource(
                        screen.source_application_name.c_str(),
                        screen.source_application_display_name.empty() ? nullptr : screen.source_application_display_name.c_str(),
                        screen.framerate,
                        screen.targetLiveTextureWidth,
                        screen.targetLiveTextureHeight);
                g_screen_source_creation_in_progress = false;
            }

            if (screen.source)
            {
                screen.sourceCreatedTick = GetTickCount64();
                screen.source->SetPaused(screen.paused);
                screen.source->SetSourceBrightness(
                    screen.effectiveBrightness);
            }

            loaded.push_back(std::move(screen));
        }

        bool mediaTargetFound{};
        for (auto& screen : loaded)
        {
            if (!screen.hotkeyTarget)
                continue;
            if (!mediaTargetFound)
                mediaTargetFound = true;
            else
                screen.hotkeyTarget = false;
        }
        if (!mediaTargetFound && !loaded.empty())
            loaded.front().hotkeyTarget = true;

        const auto loadedCount = loaded.size();
        {
            std::lock_guard<std::mutex> lock(g_screens_mutex);
            g_screens = std::move(loaded);
        }
        scs_log(0, "[Settings] Loaded %u saved screen(s)", static_cast<unsigned>(loadedCount));
        return true;
    }

    bool save()
    {
        const auto path = config_path();
        const auto temporaryPath = path + ".tmp";
        DeleteFileA(temporaryPath.c_str());

        std::lock_guard<std::mutex> lock(g_screens_mutex);
        write_number(temporaryPath, "General", "Version", 40);
        write_number(temporaryPath, "General", "ScreenCount", static_cast<uint32_t>(g_screens.size()));

        for (size_t hotkeyIndex = 0; hotkeyIndex < g_media_hotkeys.size(); ++hotkeyIndex)
        {
            const std::string section = "Hotkey" + std::to_string(hotkeyIndex);
            const auto& binding = g_media_hotkeys[hotkeyIndex];
            write_number(temporaryPath, section.c_str(), "Key", binding.virtualKey);
            write_number(temporaryPath, section.c_str(), "Control", binding.control ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "Alt", binding.alt ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "Shift", binding.shift ? 1 : 0);
        }

        write_number(
            temporaryPath, "Gamepad", "Enabled",
            g_gamepad_hotkeys_enabled ? 1 : 0);
        write_number(
            temporaryPath, "Gamepad", "Controller",
            static_cast<uint32_t>(g_gamepad_controller_index + 1));
        write_float(
            temporaryPath, "Gamepad", "AxisThreshold",
            g_gamepad_axis_threshold);
        write_number(
            temporaryPath, "Gamepad", "MenuModifier",
            static_cast<uint32_t>(g_gamepad_menu_hotkey.modifier));
        write_number(
            temporaryPath, "Gamepad", "MenuInput",
            static_cast<uint32_t>(g_gamepad_menu_hotkey.input));
        for (size_t bindingIndex = 0;
            bindingIndex < g_media_gamepad_hotkeys.size(); ++bindingIndex)
        {
            const std::string section =
                "GamepadHotkey" + std::to_string(bindingIndex);
            const auto& binding = g_media_gamepad_hotkeys[bindingIndex];
            write_number(
                temporaryPath, section.c_str(), "Modifier",
                static_cast<uint32_t>(binding.modifier));
            write_number(
                temporaryPath, section.c_str(), "Input",
                static_cast<uint32_t>(binding.input));
        }

		environment_audio_settings_t environmentSettings;
		{
			std::lock_guard<std::mutex> environmentLock(
				g_environment_audio_settings_mutex);
			environmentSettings = g_environment_audio_settings;
		}
        write_number(
            temporaryPath, "EnvironmentAudio", "Enabled",
            environmentSettings.enabled ? 1 : 0);
        write_float(
            temporaryPath, "EnvironmentAudio", "InteriorEffect",
            environmentSettings.interiorEffect);
        write_float(
            temporaryPath, "EnvironmentAudio", "ExteriorEffect",
            environmentSettings.exteriorEffect);

        for (size_t i = 0; i < g_screens.size(); ++i)
        {
            const auto& screen = g_screens[i];
            const std::string section = "Screen" + std::to_string(i);
            write_number(temporaryPath, section.c_str(), "Type", static_cast<uint32_t>(screen.type));
            WritePrivateProfileStringA(section.c_str(), "OriginalTexture", screen.original_texture.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "OverrideTexture", screen.override_texture.c_str(), temporaryPath.c_str());
            write_number(temporaryPath, section.c_str(), "OverrideWidth", screen.override_texture_size_w);
            write_number(temporaryPath, section.c_str(), "OverrideHeight", screen.override_texture_size_h);
            write_number(temporaryPath, section.c_str(), "TargetWidth", screen.targetLiveTextureWidth);
            write_number(temporaryPath, section.c_str(), "TargetHeight", screen.targetLiveTextureHeight);
            write_number(temporaryPath, section.c_str(), "Framerate", screen.framerate);
            write_number(temporaryPath, section.c_str(), "LegacyCapture", screen.legacyCapture ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "FlipVertical", screen.flipVertical ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "Paused", screen.paused ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "ScaleMode", static_cast<uint32_t>(screen.scaleMode));
            write_float(temporaryPath, section.c_str(), "Brightness", screen.brightness);
            write_number(temporaryPath, section.c_str(), "AutoBrightnessEnabled", screen.autoBrightnessEnabled ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "AutoBrightnessDarkMultiplier", screen.autoBrightnessDarkMultiplier);
            write_float(temporaryPath, section.c_str(), "AutoBrightnessBrightMultiplier", screen.autoBrightnessBrightMultiplier);
            write_number(temporaryPath, section.c_str(), "EdgeBleedGuard", screen.edgeBleedGuard);
            write_number(temporaryPath, section.c_str(), "PerformanceProfile", static_cast<uint32_t>(screen.performanceProfile));
            write_number(temporaryPath, section.c_str(), "ContentMode", static_cast<uint32_t>(screen.contentMode));
            write_number(temporaryPath, section.c_str(), "HotkeyTarget", screen.hotkeyTarget ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "FollowTruckEngine", screen.followTruckEngine ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "EngineOffBrightness", screen.engineOffBrightness);
            write_number(temporaryPath, section.c_str(), "AdaptiveAudioEnabled", screen.adaptiveAudioEnabled ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioInteriorVolume", screen.adaptiveAudioInteriorVolume);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioStrength", screen.adaptiveAudioStrength);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioSpeakerAzimuth", screen.adaptiveAudioSpeakerAzimuth);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioFacingAwayVolume", screen.adaptiveAudioFacingAwayVolume);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioOutsideDistance", screen.adaptiveAudioOutsideDistance);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioOutsideVolume", screen.adaptiveAudioOutsideVolume);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioMenuVolume", screen.adaptiveAudioMenuVolume);
            write_number(temporaryPath, section.c_str(), "AdaptiveAudioExternalDistanceEnabled", screen.adaptiveAudioExternalDistanceEnabled ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioExternalNearVolume", screen.adaptiveAudioExternalNearVolume);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioExternalNearCutoff", screen.adaptiveAudioExternalNearCutoff);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioExternalFullVolumeDistance", screen.adaptiveAudioExternalFullVolumeDistance);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioExternalMuteDistance", screen.adaptiveAudioExternalMuteDistance);
            write_number(temporaryPath, section.c_str(), "AdaptiveAudioExternalLowPassEnabled", screen.adaptiveAudioExternalLowPassEnabled ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioExternalMinimumCutoff", screen.adaptiveAudioExternalMinimumCutoff);
            WritePrivateProfileStringA(section.c_str(), "SourceApplication", screen.source_application_name.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "SourceTitle", screen.source_application_display_name.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "MediaClientId", screen.mediaClientId.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "MediaUrl", screen.mediaUrl.c_str(), temporaryPath.c_str());
            write_number(
                temporaryPath, section.c_str(), "MediaService",
                static_cast<uint32_t>(screen.mediaService));
            write_number(
                temporaryPath, section.c_str(), "SelectedYouTubeUrl",
                screen.selectedYoutubeUrl);
            write_number(
                temporaryPath, section.c_str(), "SelectedSpotifyUrl",
                screen.selectedSpotifyUrl);
            write_url_list(
                temporaryPath, section, "YouTubeUrl", screen.youtubeUrls);
            write_url_list(
                temporaryPath, section, "SpotifyUrl", screen.spotifyUrls);
        }

        WritePrivateProfileStringA(nullptr, nullptr, nullptr, temporaryPath.c_str());
        if (regular_file_exists(path) && files_equal(temporaryPath, path))
        {
            DeleteFileA(temporaryPath.c_str());
            return true;
        }
        if (!rotate_configuration_backups(path))
        {
            DeleteFileA(temporaryPath.c_str());
            scs_log(2, "[Settings] Backup rotation failed; active configuration was not replaced");
            return false;
        }
        if (!MoveFileExA(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            scs_log(2, "[Settings] Failed to save configuration, err=%lu", GetLastError());
            return false;
        }
        scs_log(0, "[Settings] Configuration saved");
        diagnostic_log::write(
            "settings", "Configuration saved; rolling backup history updated.");
        return true;
    }

    std::array<configuration_backup_info_t, 3> backup_history()
    {
        std::array<configuration_backup_info_t, 3> result{};
        for (size_t index = 0; index < result.size(); ++index)
        {
            result[index].available =
                regular_file_exists(backup_path(index));
            result[index].description = backup_description(index);
        }
        return result;
    }

    bool restore_backup(size_t index)
    {
        if (index >= kConfigurationBackupCount)
            return false;
        const std::string selectedPath = backup_path(index);
        if (!regular_file_exists(selectedPath))
            return false;

        const std::string activePath = config_path();
        const std::string temporaryPath = activePath + ".restore.tmp";
        DeleteFileA(temporaryPath.c_str());
        if (!CopyFileA(
            selectedPath.c_str(), temporaryPath.c_str(), FALSE))
        {
            diagnostic_log::writef(
                "settings", "Could not stage configuration backup %zu "
                "(Win32 error %lu).", index + 1, GetLastError());
            return false;
        }

        if (!rotate_configuration_backups(activePath))
        {
            diagnostic_log::write(
                "settings", "Backup rotation failed; restore was cancelled "
                "and the active configuration was not replaced.");
            DeleteFileA(temporaryPath.c_str());
            return false;
        }
        if (!MoveFileExA(
            temporaryPath.c_str(), activePath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            diagnostic_log::writef(
                "settings", "Could not restore configuration backup %zu "
                "(Win32 error %lu).", index + 1, GetLastError());
            DeleteFileA(temporaryPath.c_str());
            return false;
        }

        diagnostic_log::writef(
            "settings", "Restored configuration backup %zu; the previous "
            "active configuration was preserved as the newest backup.",
            index + 1);
        return load();
    }
}
