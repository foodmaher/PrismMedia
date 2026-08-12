#pragma once

#include <cstdint>

#include "../Shared/PrismCameraMonitorShared.h"

namespace camera_monitor
{
    bool initialize();
    void shutdown();
    void launch_viewer();
    void heartbeat();
    bool consume_run_request();

    void begin_run(const char* detail);
    void publish(
        prism_camera_monitor::Stage stage,
        uint32_t flags,
        const char* stageText,
        const char* detailText,
        int32_t errorCode = 0,
        uint64_t observedRenderJobs = 0,
        uint64_t rejectedSlot7Jobs = 0,
        uint64_t taggedTargets = 0,
        uint64_t readbackFrames = 0);
    void publish_frame(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        const char* detailText);
}
