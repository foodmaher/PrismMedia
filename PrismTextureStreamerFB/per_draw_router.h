#pragma once

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace per_draw_router
{
    enum class phase_t
    {
        disabled,
        waiting_texture,
        training,
        active,
        fallback
    };

    struct status_t
    {
        phase_t phase{ phase_t::disabled };
        bool enabled{};
        std::string displayId;
        uint64_t trainedDraws{};
        uint64_t routedDraws{};
        std::string detail;
    };

    using draw_indexed_t = void(__stdcall*)(
        ID3D11DeviceContext*, unsigned int, unsigned int, int);

    // Called from the telemetry thread. With one enabled custom display this
    // automatically learns its exact draw while the legacy compatibility
    // branch is visible, disables that global branch, and retains only the
    // per-draw substitution. More than one custom display safely falls back
    // to the legacy path until multi-route support is implemented.
    void update(bool driving);
    void set_enabled(bool enabled);
    void retrain();
    status_t status();

    // Called only by the bounded D3D hook set.
    void notify_pixel_shader_resources(
        ID3D11DeviceContext* context,
        unsigned int startSlot,
        unsigned int viewCount,
        ID3D11ShaderResourceView* const* views);
    bool draw_indexed(
        ID3D11DeviceContext* context,
        unsigned int indexCount,
        unsigned int startIndexLocation,
        int baseVertexLocation,
        draw_indexed_t original);

    void shutdown();
}
