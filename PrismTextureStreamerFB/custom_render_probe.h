#pragma once

struct ID3D11Texture2D;

namespace custom_render_probe
{
    // Requests one bounded diagnostic capture after an exact custom-display
    // texture route has been matched. Only the first request in a plugin
    // session is recorded to avoid repeated exception-probe overhead.
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
