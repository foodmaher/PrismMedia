#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace traffic_audio
{
    struct config_t
    {
        bool enabled{};
        std::vector<std::string> sources;
        float vehicleDensity{ 0.50f };
        float maximumVolume{ 0.20f };
        float fullVolumeDistance{ 2.5f };
        float muteDistance{ 28.0f };
        float nearCutoffHz{ 1400.0f };
        float farCutoffHz{ 260.0f };
    };

    struct status_t
    {
        bool bridgeAvailable{};
        bool helperReady{};
        bool active{};
        uint32_t observedVehicles{};
        uint32_t eligibleVehicles{};
        int32_t emitterId{ -1 };
        float distance{};
        float gain{};
        float pan{};
        float cutoffHz{ 20000.0f };
        std::string currentSource;
    };

    // Called from telemetry with a cheap desired-state update. Helper launch,
    // WebView commands and retries stay on a dedicated below-normal worker.
    void update(const config_t& config);
    status_t status();
    void shutdown();
}
