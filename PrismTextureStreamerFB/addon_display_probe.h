#pragma once

#include <cstdint>
#include <string>

namespace addon_display_probe
{
    // Runtime-only, version-locked capture of the addon display creation call.
    // No hook is installed unless the live call and destination validate.
    std::string command(const std::string& arguments);
    // Persistent owner-mapper console. Unlike the older isolation test this
    // never suppresses native addon processing or changes routing.
    std::string owner_command(const std::string& arguments);
    // Called for validated 1024x1024 slot-6 final draws. Correlations recorded
    // while a watched addon call is on the stack are exact; short same-thread
    // proximity is diagnostic only and is never used for automatic routing.
    void observe_final_draw(uintptr_t resourceIdentity);
    void tick();
    bool active();
    bool isolating();
    void shutdown();
}
