#pragma once

struct ID3D11Texture2D;

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

    // Keeps the legacy compatibility branch working outside the short probe
    // window. Call once per telemetry frame with the current custom-display
    // presentation requirement.
    void update(bool customDisplayActive);

    // Restores the original game bytes and removes the exception handler.
    void shutdown();
}
