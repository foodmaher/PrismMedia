#pragma once

#include <cstddef>
#include <string>

namespace accessory_bridge
{
    void start_auto();
    void update();
    void shutdown();
    bool busy();
    bool mod_active();
    size_t adapter_count();
    const std::string& source_name();
    const std::string& status();
    bool should_show_toast();
    const std::string& toast_message();
}
