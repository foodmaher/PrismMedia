#include "settings.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "screens.h"
#include "dx11/internal_render_probe.h"
#include "hotkeys.h"
#include "scs_logging.h"
#include "sources/media_client.h"
#include "sources/native_media.h"
#include "sources/reverse_camera.h"
#include "sources/window.h"
#include "sources/wgc_window.h"
#include "telemetry_state.h"
#include "wind_audio.h"

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
        write_number(
            path, section.c_str(), countKey.c_str(),
            static_cast<uint32_t>((std::min)(values.size(), size_t{100})));
        for (size_t index = 0; index < values.size() && index < 100; ++index)
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

        g_wind_audio_settings.enabled = GetPrivateProfileIntA(
            "WindAudio", "Enabled",
            g_wind_audio_settings.enabled ? 1 : 0,
            path.c_str()) != 0;
        g_wind_audio_settings.stationaryFiles = read_url_list(
            path, "WindAudio", "StationaryFile");
        g_wind_audio_settings.cityFiles = read_url_list(
            path, "WindAudio", "CityFile");
        g_wind_audio_settings.highwayFiles = read_url_list(
            path, "WindAudio", "HighwayFile");
        if (g_wind_audio_settings.stationaryFiles.empty() &&
            g_wind_audio_settings.cityFiles.empty() &&
            g_wind_audio_settings.highwayFiles.empty())
        {
            // Import the v3.4 custom loop once. Procedural noise is no longer
            // generated or used as a fallback.
            const std::string legacyCustom = read_string(
                path, "WindAudio", "CustomSoundPath");
            if (!legacyCustom.empty())
                g_wind_audio_settings.cityFiles.push_back(legacyCustom);
        }
        g_wind_audio_settings.masterVolume = (std::clamp)(
            read_float(path, "WindAudio", "MasterVolume", 0.65f),
            0.0f, 1.0f);
        g_wind_audio_settings.stationaryVolume = (std::clamp)(
            read_float(path, "WindAudio", "StationaryVolume", 0.45f),
            0.0f, 1.0f);
        g_wind_audio_settings.cityVolume = (std::clamp)(
            read_float(path, "WindAudio", "CityVolume", 0.75f),
            0.0f, 1.0f);
        g_wind_audio_settings.highwayVolume = (std::clamp)(
            read_float(path, "WindAudio", "HighwayVolume", 1.0f),
            0.0f, 1.0f);
        g_wind_audio_settings.stationaryFadeKmh = (std::clamp)(
            read_float(path, "WindAudio", "StationaryFadeKmh", 8.0f),
            1.0f, 30.0f);
        g_wind_audio_settings.highwayStartKmh = (std::clamp)(
            read_float(path, "WindAudio", "HighwayStartKmh", 55.0f),
            10.0f, 150.0f);
        g_wind_audio_settings.highwayFullKmh = (std::clamp)(
            read_float(path, "WindAudio", "HighwayFullKmh", 90.0f),
            20.0f, 200.0f);
        g_wind_audio_settings.stereoSeparation = (std::clamp)(
            read_float(path, "WindAudio", "StereoSeparation", 0.85f),
            0.0f, 1.0f);
        g_wind_audio_settings.mediaDucking = (std::clamp)(
            read_float(path, "WindAudio", "MediaDucking", 1.0f),
            0.0f, 1.0f);
        g_wind_audio_settings.windowTravelSeconds = (std::clamp)(
            read_float(path, "WindAudio", "WindowTravelSeconds", 2.8f),
            0.5f, 10.0f);
        g_wind_audio_settings.leftWindowOpen = (std::clamp)(
            read_float(path, "WindAudio", "LeftWindowOpen", 0.0f),
            0.0f, 1.0f);
        g_wind_audio_settings.rightWindowOpen = (std::clamp)(
            read_float(path, "WindAudio", "RightWindowOpen", 0.0f),
            0.0f, 1.0f);
        for (size_t index = 0; index < g_window_hotkeys.size(); ++index)
        {
            const std::string section =
                "WindowHotkey" + std::to_string(index);
            auto& binding = g_window_hotkeys[index];
            binding.virtualKey = GetPrivateProfileIntA(
                section.c_str(), "Key", binding.virtualKey, path.c_str());
            binding.control = GetPrivateProfileIntA(
                section.c_str(), "Control",
                binding.control ? 1 : 0, path.c_str()) != 0;
            binding.alt = GetPrivateProfileIntA(
                section.c_str(), "Alt",
                binding.alt ? 1 : 0, path.c_str()) != 0;
            binding.shift = GetPrivateProfileIntA(
                section.c_str(), "Shift",
                binding.shift ? 1 : 0, path.c_str()) != 0;
        }
        wind_audio::sync_from_settings();

        const int count = (std::min)(GetPrivateProfileIntA("General", "ScreenCount", 0, path.c_str()), 16U);
        if (count <= 0)
            return true;

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
                GetPrivateProfileIntA(section.c_str(), "Framerate", 30, path.c_str()), 1U, 120U));
            screen.legacyCapture = GetPrivateProfileIntA(section.c_str(), "LegacyCapture", 0, path.c_str()) != 0;
            screen.flipVertical = GetPrivateProfileIntA(section.c_str(), "FlipVertical", 1, path.c_str()) != 0;
            screen.paused = GetPrivateProfileIntA(section.c_str(), "Paused", 0, path.c_str()) != 0;
            screen.scaleMode = static_cast<scale_mode_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ScaleMode", static_cast<UINT>(scale_mode_t::FIT), path.c_str()),
                0U, static_cast<UINT>(scale_mode_t::CROP)));
            screen.brightness = (std::clamp)(
                read_float(path, section.c_str(), "Brightness", 1.0f),
                0.10f, 2.0f);
            screen.edgeBleedGuard = static_cast<uint8_t>((std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "EdgeBleedGuard", 2, path.c_str()),
                0U, 16U));
            screen.performanceProfile = static_cast<performance_profile_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "PerformanceProfile", static_cast<UINT>(performance_profile_t::CUSTOM), path.c_str()),
                0U, static_cast<UINT>(performance_profile_t::SMOOTH)));
            screen.source_application_name = read_string(path, section.c_str(), "SourceApplication");
            screen.source_application_display_name = read_string(path, section.c_str(), "SourceTitle");
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
            if (screen.youtubeUrls.empty() &&
                screen.spotifyUrls.empty() &&
                !screen.mediaUrl.empty())
            {
                if (screen.mediaUrl.find("spotify.com") != std::string::npos ||
                    screen.mediaUrl.rfind("spotify:", 0) == 0)
                {
                    screen.mediaService = media_service_t::SPOTIFY;
                    screen.spotifyUrls.push_back(screen.mediaUrl);
                }
                else
                {
                    screen.mediaService = media_service_t::YOUTUBE;
                    screen.youtubeUrls.push_back(screen.mediaUrl);
                }
            }
            screen.contentMode = static_cast<content_mode_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ContentMode",
                    static_cast<UINT>(content_mode_t::WINDOW_CAPTURE), path.c_str()),
                0U, static_cast<UINT>(content_mode_t::NATIVE_DIRECT_MEDIA)));
            screen.hotkeyTarget = GetPrivateProfileIntA(
                section.c_str(), "HotkeyTarget", 0, path.c_str()) != 0;
            screen.followTruckEngine = GetPrivateProfileIntA(
                section.c_str(), "FollowTruckEngine", 1, path.c_str()) != 0;
            screen.adaptiveAudioEnabled = GetPrivateProfileIntA(
                section.c_str(), "AdaptiveAudioEnabled", 0, path.c_str()) != 0;
            screen.adaptiveAudioStrength = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioStrength", 0.85f),
                0.0f, 1.0f);
            screen.adaptiveAudioSpeakerAzimuth = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioSpeakerAzimuth", 0.0f),
                -180.0f, 180.0f);
            screen.adaptiveAudioFacingAwayVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioFacingAwayVolume", 0.05f),
                0.0f, 1.0f);
            screen.adaptiveAudioOutsideDistance = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioOutsideDistance", 0.85f),
                0.25f, 5.0f);
            screen.adaptiveAudioOutsideVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioOutsideVolume", 0.0f),
                0.0f, 1.0f);
            screen.adaptiveAudioMenuVolume = (std::clamp)(
                read_float(path, section.c_str(), "AdaptiveAudioMenuVolume", 0.50f),
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
                        0.35f),
                    0.0f, 1.0f);
            screen.adaptiveAudioExternalNearCutoff =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalNearCutoff",
                        1200.0f),
                    20.0f, 20000.0f);
            screen.adaptiveAudioExternalFullVolumeDistance =
                (std::clamp)(
                    read_float(
                        path, section.c_str(),
                        "AdaptiveAudioExternalFullVolumeDistance",
                        1.5f),
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
                        120.0f),
                    20.0f, 8000.0f);
            screen.reverseCameraEnabled = GetPrivateProfileIntA(
                section.c_str(), "ReverseCameraEnabled", 0, path.c_str()) != 0;
            screen.reverseCameraMethod =
                static_cast<reverse_camera_method_t>((std::clamp)(
                    GetPrivateProfileIntA(
                        section.c_str(), "ReverseCameraMethod",
                        static_cast<UINT>(
                            reverse_camera_method_t::WINDOW_CROP),
                        path.c_str()),
                    0U,
                    static_cast<UINT>(
                        reverse_camera_method_t::
                            INTERNAL_PARK_PROBE)));
            screen.reverseInternalTargetVariant =
                static_cast<uint8_t>((std::clamp)(
                    GetPrivateProfileIntA(
                        section.c_str(),
                        "ReverseInternalTargetVariant",
                        1,
                        path.c_str()),
                    0U,
                    4U));
            screen.reverseCameraKitInstalled =
                GetPrivateProfileIntA(
                    section.c_str(), "ReverseCameraKitInstalled",
                    0, path.c_str()) != 0;
            screen.reverseTrailerAwareMount =
                GetPrivateProfileIntA(
                    section.c_str(), "ReverseTrailerAwareMount",
                    1, path.c_str()) != 0;
            screen.reverseMountLateral = (std::clamp)(
                read_float(path, section.c_str(),
                    "ReverseMountLateral", 0.0f),
                -5.0f, 5.0f);
            screen.reverseMountHeight = (std::clamp)(
                read_float(path, section.c_str(),
                    "ReverseMountHeight", 2.6f),
                -2.0f, 8.0f);
            screen.reverseMountLongitudinal = (std::clamp)(
                read_float(path, section.c_str(),
                    "ReverseMountLongitudinal", -0.35f),
                -8.0f, 8.0f);
            screen.reverseMountYaw = (std::clamp)(
                read_float(path, section.c_str(),
                    "ReverseMountYaw", 180.0f),
                -360.0f, 360.0f);
            screen.reverseMountPitch = (std::clamp)(
                read_float(path, section.c_str(),
                    "ReverseMountPitch", -8.0f),
                -89.0f, 89.0f);
            screen.reverseZeroForwardImpact = GetPrivateProfileIntA(
                section.c_str(), "ReverseZeroForwardImpact", 1, path.c_str()) != 0;
            screen.reversePerformanceProfile =
                static_cast<reverse_performance_profile_t>((std::clamp)(
                    GetPrivateProfileIntA(
                        section.c_str(), "ReversePerformanceProfile",
                        static_cast<UINT>(
                            reverse_performance_profile_t::BALANCED),
                        path.c_str()),
                    0U,
                    static_cast<UINT>(
                        reverse_performance_profile_t::ULTRA)));
            screen.reverseCaptureWidth = (std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "ReverseCaptureWidth",
                    640, path.c_str()),
                256U, 1920U);
            screen.reverseCaptureHeight = (std::clamp)(
                GetPrivateProfileIntA(
                    section.c_str(), "ReverseCaptureHeight",
                    360, path.c_str()),
                144U, 1080U);
            screen.reverseFramerate = static_cast<uint8_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ReverseFramerate", 15, path.c_str()),
                5U, 60U));
            apply_reverse_performance_profile(screen);
            screen.reverseCropLeft = (std::clamp)(
                read_float(path, section.c_str(), "ReverseCropLeft", 0.30f),
                0.0f, 0.98f);
            screen.reverseCropTop = (std::clamp)(
                read_float(path, section.c_str(), "ReverseCropTop", 0.02f),
                0.0f, 0.98f);
            screen.reverseCropWidth = (std::clamp)(
                read_float(path, section.c_str(), "ReverseCropWidth", 0.40f),
                0.02f, 1.0f);
            screen.reverseCropHeight = (std::clamp)(
                read_float(path, section.c_str(), "ReverseCropHeight", 0.22f),
                0.02f, 1.0f);

            if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA &&
                !screen.mediaUrl.empty())
            {
                g_screen_source_creation_in_progress = true;
                screen.source = sources::CreateMediaClientSource(
                    screen.mediaUrl, screen.framerate,
                    screen.targetLiveTextureWidth,
                    screen.targetLiveTextureHeight);
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
                screen.source->SetPaused(screen.paused);
                screen.source->SetSourceBrightness(screen.brightness);
            }

            if (screen.reverseCameraEnabled &&
                screen.reverseCameraMethod ==
                    reverse_camera_method_t::WINDOW_CROP &&
                (!screen.reverseZeroForwardImpact ||
                    g_reverse_active.load()))
            {
                g_screen_source_creation_in_progress = true;
                screen.reverseLastStartAttemptTick = GetTickCount64();
                screen.reverseSource = sources::CreateReverseCameraSource(
                    screen.reverseFramerate,
                    screen.reverseCaptureWidth,
                    screen.reverseCaptureHeight,
                    screen.reverseCropLeft,
                    screen.reverseCropTop,
                    screen.reverseCropWidth,
                    screen.reverseCropHeight);
                g_screen_source_creation_in_progress = false;
                if (screen.reverseSource)
                    screen.reverseSource->SetPaused(
                        !g_reverse_active.load());
            }

            loaded.push_back(std::move(screen));
        }

        const auto loadedCount = loaded.size();
        const bool internalParkRequested =
            std::any_of(
                loaded.begin(), loaded.end(),
                [](const screen_t& screen)
                {
                    return screen.reverseCameraEnabled &&
                        screen.reverseCameraMethod ==
                            reverse_camera_method_t::
                                INTERNAL_PARK_PROBE;
                });
        {
            std::lock_guard<std::mutex> lock(g_screens_mutex);
            g_screens = std::move(loaded);
        }
        dx11::internal_render_probe::
            set_park_activation_requested(
                internalParkRequested);
        dx11::internal_render_probe::
            set_park_render_requested(false);
        scs_log(0, "[Settings] Loaded %u saved screen(s)", static_cast<unsigned>(loadedCount));
        return true;
    }

    bool save()
    {
        const auto path = config_path();
        const auto temporaryPath = path + ".tmp";
        DeleteFileA(temporaryPath.c_str());

        std::lock_guard<std::mutex> lock(g_screens_mutex);
        write_number(temporaryPath, "General", "Version", 13);
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

		wind_audio_settings_t windSettings;
		{
			std::lock_guard<std::recursive_mutex> windLock(
				g_wind_audio_settings_mutex);
			windSettings = g_wind_audio_settings;
		}
        write_number(
            temporaryPath, "WindAudio", "Enabled",
            windSettings.enabled ? 1 : 0);
        write_url_list(
            temporaryPath, "WindAudio", "StationaryFile",
            windSettings.stationaryFiles);
        write_url_list(
            temporaryPath, "WindAudio", "CityFile",
            windSettings.cityFiles);
        write_url_list(
            temporaryPath, "WindAudio", "HighwayFile",
            windSettings.highwayFiles);
        write_float(
            temporaryPath, "WindAudio", "MasterVolume",
            windSettings.masterVolume);
        write_float(
            temporaryPath, "WindAudio", "StationaryVolume",
            windSettings.stationaryVolume);
        write_float(
            temporaryPath, "WindAudio", "CityVolume",
            windSettings.cityVolume);
        write_float(
            temporaryPath, "WindAudio", "HighwayVolume",
            windSettings.highwayVolume);
        write_float(
            temporaryPath, "WindAudio", "StationaryFadeKmh",
            windSettings.stationaryFadeKmh);
        write_float(
            temporaryPath, "WindAudio", "HighwayStartKmh",
            windSettings.highwayStartKmh);
        write_float(
            temporaryPath, "WindAudio", "HighwayFullKmh",
            windSettings.highwayFullKmh);
        write_float(
            temporaryPath, "WindAudio", "StereoSeparation",
            windSettings.stereoSeparation);
        write_float(
            temporaryPath, "WindAudio", "MediaDucking",
            windSettings.mediaDucking);
        write_float(
            temporaryPath, "WindAudio", "WindowTravelSeconds",
            windSettings.windowTravelSeconds);
        write_float(
            temporaryPath, "WindAudio", "LeftWindowOpen",
            g_wind_left_open.load());
        write_float(
            temporaryPath, "WindAudio", "RightWindowOpen",
            g_wind_right_open.load());
        for (size_t index = 0; index < g_window_hotkeys.size(); ++index)
        {
            const std::string section =
                "WindowHotkey" + std::to_string(index);
            const auto& binding = g_window_hotkeys[index];
            write_number(
                temporaryPath, section.c_str(), "Key",
                binding.virtualKey);
            write_number(
                temporaryPath, section.c_str(), "Control",
                binding.control ? 1 : 0);
            write_number(
                temporaryPath, section.c_str(), "Alt",
                binding.alt ? 1 : 0);
            write_number(
                temporaryPath, section.c_str(), "Shift",
                binding.shift ? 1 : 0);
        }

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
            write_number(temporaryPath, section.c_str(), "EdgeBleedGuard", screen.edgeBleedGuard);
            write_number(temporaryPath, section.c_str(), "PerformanceProfile", static_cast<uint32_t>(screen.performanceProfile));
            write_number(temporaryPath, section.c_str(), "ContentMode", static_cast<uint32_t>(screen.contentMode));
            write_number(temporaryPath, section.c_str(), "HotkeyTarget", screen.hotkeyTarget ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "FollowTruckEngine", screen.followTruckEngine ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "AdaptiveAudioEnabled", screen.adaptiveAudioEnabled ? 1 : 0);
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
            write_number(temporaryPath, section.c_str(), "ReverseCameraEnabled", screen.reverseCameraEnabled ? 1 : 0);
            write_number(
                temporaryPath, section.c_str(),
                "ReverseCameraMethod",
                static_cast<uint32_t>(
                    screen.reverseCameraMethod));
            write_number(
                temporaryPath, section.c_str(),
                "ReverseInternalTargetVariant",
                screen.reverseInternalTargetVariant);
            write_number(temporaryPath, section.c_str(),
                "ReverseCameraKitInstalled",
                screen.reverseCameraKitInstalled ? 1 : 0);
            write_number(temporaryPath, section.c_str(),
                "ReverseTrailerAwareMount",
                screen.reverseTrailerAwareMount ? 1 : 0);
            write_float(temporaryPath, section.c_str(),
                "ReverseMountLateral", screen.reverseMountLateral);
            write_float(temporaryPath, section.c_str(),
                "ReverseMountHeight", screen.reverseMountHeight);
            write_float(temporaryPath, section.c_str(),
                "ReverseMountLongitudinal",
                screen.reverseMountLongitudinal);
            write_float(temporaryPath, section.c_str(),
                "ReverseMountYaw", screen.reverseMountYaw);
            write_float(temporaryPath, section.c_str(),
                "ReverseMountPitch", screen.reverseMountPitch);
            write_number(temporaryPath, section.c_str(), "ReverseZeroForwardImpact", screen.reverseZeroForwardImpact ? 1 : 0);
            write_number(
                temporaryPath, section.c_str(),
                "ReversePerformanceProfile",
                static_cast<uint32_t>(
                    screen.reversePerformanceProfile));
            write_number(temporaryPath, section.c_str(), "ReverseCaptureWidth", screen.reverseCaptureWidth);
            write_number(temporaryPath, section.c_str(), "ReverseCaptureHeight", screen.reverseCaptureHeight);
            write_number(temporaryPath, section.c_str(), "ReverseFramerate", screen.reverseFramerate);
            write_float(temporaryPath, section.c_str(), "ReverseCropLeft", screen.reverseCropLeft);
            write_float(temporaryPath, section.c_str(), "ReverseCropTop", screen.reverseCropTop);
            write_float(temporaryPath, section.c_str(), "ReverseCropWidth", screen.reverseCropWidth);
            write_float(temporaryPath, section.c_str(), "ReverseCropHeight", screen.reverseCropHeight);
            WritePrivateProfileStringA(section.c_str(), "SourceApplication", screen.source_application_name.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "SourceTitle", screen.source_application_display_name.c_str(), temporaryPath.c_str());
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
        if (!MoveFileExA(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            scs_log(2, "[Settings] Failed to save configuration, err=%lu", GetLastError());
            return false;
        }
        scs_log(0, "[Settings] Configuration saved");
        return true;
    }
}
