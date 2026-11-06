#pragma once

#include <string>

namespace addon_display_probe
{
    // Runtime-only, version-locked capture of the addon display creation call.
    // No hook is installed unless the live call and destination validate.
    std::string command(const std::string& arguments);
    void tick();
    bool active();
    bool isolating();
    void shutdown();
}
