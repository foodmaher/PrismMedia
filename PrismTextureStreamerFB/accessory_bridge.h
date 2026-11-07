#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct screen_t;

namespace accessory_bridge
{
    struct accessory_t
    {
        std::string name;
        std::string dataPath;
        std::string source;
        std::string definition;
        std::string interiorModel;
        std::string exteriorModel;
        std::string uiTexture;
    };

    void update();
    void shutdown();
    bool start_scan();
    bool start_generate(const screen_t& screen, size_t accessoryIndex);
    bool busy();
    const std::vector<accessory_t>& accessories();
    const std::string& status();
}
