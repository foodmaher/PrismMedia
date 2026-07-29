#pragma once

#include "content_source.h"
#include <memory>

namespace sources {
    std::unique_ptr<IContentSource> CreateReverseCameraSource(
        uint8_t framerate = 15,
        uint32_t output_width = 640,
        uint32_t output_height = 360,
        float crop_left = 0.30f,
        float crop_top = 0.02f,
        float crop_width = 0.40f,
        float crop_height = 0.22f);
}
