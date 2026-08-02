#pragma once

#include "content_source.h"
#include <memory>
#include <string>
#include <vector>

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
    bool SetMediaClientWindLibrary(
        const std::vector<std::string>& stationary_files,
        const std::vector<std::string>& city_files,
        const std::vector<std::string>& highway_files);
    bool SetMediaClientWindState(
        bool enabled,
        float stationary_volume,
        float city_volume,
        float highway_volume,
        float pan,
        float media_gain);
}
