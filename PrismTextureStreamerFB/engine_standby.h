#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace engine_standby
{
    // Produces an opaque BGRA standby card at the requested texture size.
    // The caller applies the screen's saved standby brightness while copying
    // this frame to the game texture.
    void render_truck_logo(
        std::vector<uint8_t>& destination,
        uint32_t width,
        uint32_t height,
        const std::string& truckBrand,
        const std::string& truckName);
}
