#include "settings.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "screens.h"
#include "hotkeys.h"
#include "scs_logging.h"
#include "sources/media_client.h"
#include "sources/native_media.h"
#include "sources/reverse_camera.h"
#include "sources/window.h"
#include "sources/wgc_window.h"
#include "telemetry_state.h"

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
            screen.performanceProfile = static_cast<performance_profile_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "PerformanceProfile", static_cast<UINT>(performance_profile_t::CUSTOM), path.c_str()),
                0U, static_cast<UINT>(performance_profile_t::SMOOTH)));
            screen.source_application_name = read_string(path, section.c_str(), "SourceApplication");
            screen.source_application_display_name = read_string(path, section.c_str(), "SourceTitle");
            screen.mediaUrl = read_string(path, section.c_str(), "MediaUrl");
            screen.contentMode = static_cast<content_mode_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ContentMode",
                    static_cast<UINT>(content_mode_t::WINDOW_CAPTURE), path.c_str()),
                0U, static_cast<UINT>(content_mode_t::NATIVE_DIRECT_MEDIA)));
            screen.hotkeyTarget = GetPrivateProfileIntA(
                section.c_str(), "HotkeyTarget", 0, path.c_str()) != 0;
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
            screen.reverseCameraEnabled = GetPrivateProfileIntA(
                section.c_str(), "ReverseCameraEnabled", 0, path.c_str()) != 0;
            screen.reverseZeroForwardImpact = GetPrivateProfileIntA(
                section.c_str(), "ReverseZeroForwardImpact", 1, path.c_str()) != 0;
            screen.reverseFramerate = static_cast<uint8_t>((std::clamp)(
                GetPrivateProfileIntA(section.c_str(), "ReverseFramerate", 20, path.c_str()),
                5U, 60U));
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
                if (screen.source)
                    screen.source->SetPaused(screen.paused);
            }

            if (screen.reverseCameraEnabled &&
                (!screen.reverseZeroForwardImpact ||
                    g_reverse_active.load()))
            {
                g_screen_source_creation_in_progress = true;
                screen.reverseLastStartAttemptTick = GetTickCount64();
                screen.reverseSource = sources::CreateReverseCameraSource(
                    screen.reverseFramerate,
                    screen.targetLiveTextureWidth,
                    screen.targetLiveTextureHeight,
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
        write_number(temporaryPath, "General", "Version", 4);
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
            write_number(temporaryPath, section.c_str(), "PerformanceProfile", static_cast<uint32_t>(screen.performanceProfile));
            write_number(temporaryPath, section.c_str(), "ContentMode", static_cast<uint32_t>(screen.contentMode));
            write_number(temporaryPath, section.c_str(), "HotkeyTarget", screen.hotkeyTarget ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "AdaptiveAudioEnabled", screen.adaptiveAudioEnabled ? 1 : 0);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioStrength", screen.adaptiveAudioStrength);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioSpeakerAzimuth", screen.adaptiveAudioSpeakerAzimuth);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioFacingAwayVolume", screen.adaptiveAudioFacingAwayVolume);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioOutsideDistance", screen.adaptiveAudioOutsideDistance);
            write_float(temporaryPath, section.c_str(), "AdaptiveAudioOutsideVolume", screen.adaptiveAudioOutsideVolume);
            write_number(temporaryPath, section.c_str(), "ReverseCameraEnabled", screen.reverseCameraEnabled ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "ReverseZeroForwardImpact", screen.reverseZeroForwardImpact ? 1 : 0);
            write_number(temporaryPath, section.c_str(), "ReverseFramerate", screen.reverseFramerate);
            write_float(temporaryPath, section.c_str(), "ReverseCropLeft", screen.reverseCropLeft);
            write_float(temporaryPath, section.c_str(), "ReverseCropTop", screen.reverseCropTop);
            write_float(temporaryPath, section.c_str(), "ReverseCropWidth", screen.reverseCropWidth);
            write_float(temporaryPath, section.c_str(), "ReverseCropHeight", screen.reverseCropHeight);
            WritePrivateProfileStringA(section.c_str(), "SourceApplication", screen.source_application_name.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "SourceTitle", screen.source_application_display_name.c_str(), temporaryPath.c_str());
            WritePrivateProfileStringA(section.c_str(), "MediaUrl", screen.mediaUrl.c_str(), temporaryPath.c_str());
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
