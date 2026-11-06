#pragma once

#include <cstdint>
#include <cstddef>
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

    // Values intentionally match runtime_draw_probe's stable capture schema.
    enum class draw_kind_t : unsigned int
    {
        indexed = 0,
        draw = 1,
        indexed_instanced = 2,
        instanced = 3,
        draw_auto = 4,
        indexed_indirect = 5,
        instanced_indirect = 6,
        none = 7
    };

    struct draw_call_t
    {
        draw_kind_t kind{ draw_kind_t::none };
        unsigned int count{};
        unsigned int start{};
        int base{};
        unsigned int instances{};
        unsigned int firstInstance{};
        uintptr_t indirectBuffer{};
    };

    struct route_scope_t
    {
        unsigned int slot{};
        ID3D11ShaderResourceView* nativeView{};
        bool active{};
        bool preview{};
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

    // Called from the telemetry thread. With one enabled custom display this
    // traces that texture through one UI-compositor render target, learns the
    // exact final screen draw and SRV slot, then leaves only that draw hook
    // enabled. It does not depend on the version-specific global fallback.
    void update(bool driving);
    void set_enabled(bool enabled);
    void retrain();
    status_t status();

    // RenderDoc proved that ETS2 presents each addon GPS through its own
    // 1024x1024 SRV at pixel-shader slot 6. These commands expose the bounded
    // final-draw discovery so a screen can be previewed and selected without
    // rebuilding the DLL or suppressing native addon processing.
    std::string command(const std::string& text);

    // Visual picker bridge. Candidate indices are physical screen groups, not
    // individual shader passes. A temporary probe replaces every pass in one
    // group and is always restored immediately after each draw.
    size_t picker_candidate_count();
    bool picker_set_probe(
        size_t candidate,
        ID3D11ShaderResourceView* probeView);
    void picker_clear_probe();

    // Called only by the bounded D3D hook set.
    void notify_pixel_shader_resources(
        ID3D11DeviceContext* context,
        unsigned int startSlot,
        unsigned int viewCount,
        ID3D11ShaderResourceView* const* views);

    // Training observes every D3D11 draw variant. Active routing returns true
    // after binding the custom SRV; the hook must call the original draw once
    // and then call end_draw to restore the game's native binding.
    bool begin_draw(
        ID3D11DeviceContext* context,
        const draw_call_t& call,
        route_scope_t& scope);
    void end_draw(
        ID3D11DeviceContext* context,
        route_scope_t& scope);

    void shutdown();
}
