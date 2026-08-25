#pragma once

#include "content_source.h"
#include <memory>
#include <string>

namespace sources {
    constexpr const char* kMediaClientExecutable = "PrismMediaClient.exe";
    constexpr const char* kMediaClientFolder = "PrismMedia\\";
    constexpr const char* kLegacyMediaClientFolder =
        "PrismTextureStreamerFB\\";
    constexpr const char* kMediaClientWindowTitlePrefix =
        "Prism Media Client - ";

    bool IsMediaClientInstalled();
    bool SetMediaClientDucking(float gain);
    void ShutdownMediaClient();
    std::string MakeMediaClientInstanceId(
        const std::string& stable_hint = {});
    std::unique_ptr<IContentSource> CreateMediaClientSource(
        const std::string& instance_id,
        const std::string& media_url,
        uint8_t framerate = 30,
        uint32_t output_width = 1280,
        uint32_t output_height = 720,
        bool full_spotify_web = false);
}
