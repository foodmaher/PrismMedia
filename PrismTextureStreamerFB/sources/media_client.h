#pragma once

#include "content_source.h"
#include <memory>
#include <string>

namespace sources {
    constexpr const char* kMediaClientExecutable = "PrismMediaClient.exe";
    constexpr const char* kMediaClientFolder = "PrismTextureStreamerFB\\";
    constexpr const char* kMediaClientWindowTitle = "Prism Media Client";

    bool IsMediaClientInstalled();
    std::unique_ptr<IContentSource> CreateMediaClientSource(
        const std::string& media_url,
        uint8_t framerate = 30,
        uint32_t output_width = 1280,
        uint32_t output_height = 720);
    bool SetMediaClientWindSource(
        bool procedural,
        const std::string& custom_path);
    bool SetMediaClientWindState(
        bool enabled,
        float volume,
        float pan,
        float speed_blend);
}
