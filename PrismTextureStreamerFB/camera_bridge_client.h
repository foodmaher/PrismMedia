#pragma once

namespace camera_bridge
{
    // Polls the optional SPF companion shared-memory feed. This is deliberately
    // non-blocking and retries at a low rate when SPF/the bridge is absent.
    void poll();
    void shutdown();
}
