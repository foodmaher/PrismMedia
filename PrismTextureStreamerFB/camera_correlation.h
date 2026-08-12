#pragma once

#include <cstdint>

namespace camera_correlation
{
    void begin();
    void stop();
    void observe(
        void* cameraInput,
        void* renderRequest,
        void* renderCommand);
    bool capture_phase(uint32_t requestedPhase);
    void tick(uint64_t observedJobs, uint64_t rejectedSlot7Jobs);
    void finish(bool timedOut);
    bool active();
    bool complete();
    uint32_t candidate_count();
}
