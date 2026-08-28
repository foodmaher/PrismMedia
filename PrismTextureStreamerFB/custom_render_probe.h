#pragma once

#include <cstdint>

struct ID3D11Texture2D;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace custom_render_probe
{
    enum class fallback_mode_t
    {
        automatic,
        forced_on,
        forced_off
    };

    struct status_t
    {
        bool active{};
        bool waitingForTexture{};
        bool completed{};
        bool textureReady{};
        uint32_t branchEvents{};
        uint32_t listEntries{};
        uint32_t exactOldReleases{};
        uint32_t validatedReleases{};
        uint32_t drawSamples{};
        uint64_t releaseScopeWindowMicroseconds{};
        fallback_mode_t fallbackMode{ fallback_mode_t::automatic };
    };

    // Arms one bounded combined test before the game reloads the truck and
    // accessory models. One cycle covers the branch, list entries, the exact
    // old texture's standard COM Release, and the replacement texture's
    // bind/draw path. Private game cleanup functions are not detoured. Only an
    // exact bounded entry-graph match may be skipped.
    bool prepare_capture(
        const char* displayId,
        const char* originalTexture,
        ID3D11Texture2D* currentLiveTexture);

    // Supplies the exact live Direct3D texture after the prepared display's
    // route is matched. Captured pre-match list entries are fingerprinted and
    // searched again after the game has attached the replacement texture.
    void request_capture(
        const char* displayId,
        const char* originalTexture,
        ID3D11Texture2D* liveTexture);

    // Temporary Direct3D correlation hooks call these only while the
    // one-click diagnostic is active. Together they connect the exact game
    // texture to its SRV bind, the following draw, and the nearest captured
    // Prism3D branch event.
    void notify_shader_resource_view(
        ID3D11Resource* resource,
        ID3D11ShaderResourceView* view);
    void notify_pixel_shader_resources(
        unsigned int startSlot,
        unsigned int viewCount,
        ID3D11ShaderResourceView* const* views);
    void notify_draw(const char* drawKind);

    // Keeps the legacy compatibility branch working outside the short probe
    // window. Call once per telemetry frame with the current custom-display
    // presentation requirement.
    void update(bool customDisplayActive);

    // Runtime controls used by the local diagnostic console. They never
    // install arbitrary hooks or call unknown game functions.
    status_t status();
    bool reset_session();
    bool abort_capture(bool customDisplayActive);
    bool set_release_scope_window_microseconds(uint64_t value);
    void set_fallback_mode(fallback_mode_t mode);
    fallback_mode_t fallback_mode();

    // Restores the original game bytes and removes the exception handler.
    void shutdown();
}
