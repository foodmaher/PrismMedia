#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct screen_t;

namespace override_assets
{
    bool ensure(
        screen_t& screen,
        const std::vector<std::pair<uint32_t, uint32_t>>& used_dimensions,
        bool require_generated_identity,
        std::string& status);
}
