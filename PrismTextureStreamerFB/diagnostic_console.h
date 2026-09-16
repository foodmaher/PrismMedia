#pragma once

namespace diagnostic_console
{
    // Local named-pipe host: game mutations execute on telemetry; read-only
    // status/results and the live-capture deadline watchdog use the worker.
    void start();
    void update(bool driving, bool customDisplayActive);
    void stop();
}
