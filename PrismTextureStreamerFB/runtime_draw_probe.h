#pragma once
#include <cstdint>
#include <string>
#include <mutex>

struct ID3D11DeviceContext;

namespace runtime_draw_probe
{
    // Commands and timer run outside the render callback. Results contain
    // copied values only; no D3D pointer is dereferenced after a callback ends.
    std::string command(const std::string& text);
    void tick();
    void shutdown();
    bool active();
    // Serializes start requests from both the in-game UI and the console.
    std::mutex& start_mutex();
    void draw(ID3D11DeviceContext* context, const char* kind,
        uintptr_t caller, unsigned count, unsigned start, int base,
        unsigned instances = 1, unsigned firstInstance = 0,
        uintptr_t indirectBuffer = 0);
}
