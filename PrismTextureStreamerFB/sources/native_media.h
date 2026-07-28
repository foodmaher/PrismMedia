#pragma once

#include "content_source.h"
#include <memory>
#include <string>

namespace sources {
    // Plays local files and direct media URLs through Windows Media Foundation.
    // YouTube page URLs must use the integrated media client because YouTube
    // does not expose its media streams as public direct URLs.
    std::unique_ptr<IContentSource> CreateNativeMediaSource(
        const std::string& media_url,
        uint8_t framerate = 30,
        uint32_t output_width = 1280,
        uint32_t output_height = 720);
}
