#pragma once

namespace diagnostic_console
{
    // Starts a local-only named-pipe command server. Commands are executed by
    // update() on the telemetry thread, never on the pipe worker.
    void start();
    void update(bool driving, bool customDisplayActive);
    void stop();
}
