#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct screen_t;

namespace override_assets
{
    // Writes the TOBJ/DDS pair for an adapter identity that is already fixed
    // in a generated model material. Unlike ensure(), this never renames the
    // virtual path or changes its dimensions.
    bool write_exact(screen_t& screen, std::string& status);

    bool ensure(
        screen_t& screen,
        const std::vector<std::pair<uint32_t, uint32_t>>& used_dimensions,
        const std::vector<std::string>& used_override_paths,
        bool require_generated_identity,
        std::string& status);
}
