#pragma once

struct ID3D11Texture2D;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace custom_render_probe
{
    // Arms one bounded diagnostic capture before the game reloads the truck
    // and accessory models. This is deliberately synchronous so the branch
    // cannot execute between the reload command and the next telemetry frame.
    bool prepare_capture(
        const char* displayId,
        const char* originalTexture);

    // Supplies the exact live Direct3D texture after the prepared display's
    // route is matched. Captured pre-match object pointers are correlated at
    // this point, after the game has attached the replacement texture.
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

    // Restores the original game bytes and removes the exception handler.
    void shutdown();
}
