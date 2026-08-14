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
    bool consume_phase_request(uint32_t& phase);

    void begin_run(const char* detail);
    void publish(
        prism_camera_monitor::Stage stage,
        uint32_t flags,
        const char* stageText,
        const char* detailText,
        int32_t errorCode = 0,
        uint64_t observedRenderJobs = 0,
        uint64_t submittedProbeJobs = 0,
        uint64_t taggedTargets = 0,
        uint64_t readbackFrames = 0);
    void publish_frame(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        const char* detailText);
    void publish_correlation(
        prism_camera_monitor::Stage stage,
        uint32_t flags,
        prism_camera_monitor::CorrelationPhase phase,
        uint32_t completedPhaseMask,
        uint64_t correlationSamples,
        const char* stageText,
        const char* detailText,
        const char* instructionText,
        const prism_camera_monitor::CorrelationCandidate* candidates = nullptr,
        uint32_t candidateCount = 0,
        uint64_t observedRenderJobs = 0,
        uint64_t submittedProbeJobs = 0);
}
