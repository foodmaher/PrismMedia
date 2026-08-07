#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "camera_bridge_client.h"
#include "telemetry_state.h"
#include "../Shared/PrismCameraBridgeShared.h"

#include <algorithm>
#include <cstring>

namespace
{
    HANDLE g_mapping{};
    const prism_camera_bridge::SharedState* g_shared{};
    uint64_t g_lastOpenAttempt{};

    void close_mapping()
    {
        if (g_shared)
        {
            UnmapViewOfFile(g_shared);
            g_shared = nullptr;
        }
        if (g_mapping)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
        g_camera_bridge_connected = false;
        g_camera_bridge_mapping_present = false;
        g_camera_bridge_activated = false;
        g_camera_bridge_telemetry_registered = false;
        g_camera_bridge_trailer_valid = false;
        g_camera_bridge_truck_valid = false;
        g_camera_bridge_trailer_count = 0;
        {
            std::lock_guard<std::mutex> lock(g_ai_traffic_mutex);
            g_ai_traffic = {};
        }
    }

    bool try_open_mapping(uint64_t now)
    {
        if (g_shared)
            return true;
        if (g_lastOpenAttempt != 0 && now >= g_lastOpenAttempt &&
            now - g_lastOpenAttempt < 2000)
            return false;

        g_lastOpenAttempt = now;
        g_mapping = OpenFileMappingW(
            FILE_MAP_READ, FALSE, prism_camera_bridge::kMappingName);
        if (!g_mapping)
            return false;

        g_shared = static_cast<const prism_camera_bridge::SharedState*>(
            MapViewOfFile(
                g_mapping, FILE_MAP_READ, 0, 0,
                sizeof(prism_camera_bridge::SharedState)));
        if (!g_shared)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
            return false;
        }
        return true;
    }
}

namespace camera_bridge
{
    void poll()
    {
        const uint64_t now = GetTickCount64();
        if (!try_open_mapping(now))
        {
            g_camera_bridge_connected = false;
            g_camera_bridge_mapping_present = false;
            return;
        }
        g_camera_bridge_mapping_present = true;

        prism_camera_bridge::SharedState snapshot{};
        bool copied{};
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const LONG before = g_shared->sequence;
            if ((before & 1) != 0)
                continue;

            MemoryBarrier();
            std::memcpy(&snapshot, g_shared, sizeof(snapshot));
            MemoryBarrier();

            const LONG after = g_shared->sequence;
            if (before == after && (after & 1) == 0)
            {
                copied = true;
                break;
            }
        }

        const bool fresh =
            copied &&
            snapshot.magic == prism_camera_bridge::kMagic &&
            snapshot.version == prism_camera_bridge::kVersion &&
            now >= snapshot.updatedTick &&
            now - snapshot.updatedTick <= 1000;
        if (!fresh)
        {
            g_camera_bridge_connected = false;
            g_camera_bridge_activated = false;
            g_camera_bridge_telemetry_registered = false;
            g_camera_bridge_trailer_valid = false;
            g_camera_bridge_truck_valid = false;
            g_camera_bridge_trailer_count = 0;
            {
                std::lock_guard<std::mutex> lock(g_ai_traffic_mutex);
                g_ai_traffic = {};
            }
            return;
        }

        const bool cameraValid =
            (snapshot.flags &
                prism_camera_bridge::kCameraValid) != 0;
        g_camera_bridge_connected = cameraValid;
        g_camera_bridge_activated =
            (snapshot.flags &
                prism_camera_bridge::kActivated) != 0;
        g_camera_bridge_telemetry_registered =
            (snapshot.flags &
                prism_camera_bridge::kTelemetryRegistered) != 0;
        const bool trailerValid =
            (snapshot.flags &
                prism_camera_bridge::kTrailerValid) != 0;
        g_camera_bridge_trailer_valid = trailerValid;
        const bool truckValid =
            (snapshot.flags &
                prism_camera_bridge::kTruckValid) != 0;
        g_camera_bridge_truck_valid = truckValid;
        g_camera_bridge_trailer_count = snapshot.trailerCount;

        if (cameraValid)
        {
            g_camera_world_x = snapshot.cameraX;
            g_camera_world_y = snapshot.cameraY;
            g_camera_world_z = snapshot.cameraZ;
            g_camera_type = snapshot.cameraType;
        }
        if (trailerValid)
        {
            g_last_trailer_world_x = snapshot.trailerX;
            g_last_trailer_world_y = snapshot.trailerY;
            g_last_trailer_world_z = snapshot.trailerZ;
            g_last_trailer_heading = snapshot.trailerHeading;
            g_last_trailer_pitch = snapshot.trailerPitch;
            g_last_trailer_roll = snapshot.trailerRoll;
        }
        if (truckValid)
        {
            g_bridge_truck_world_x = snapshot.truckX;
            g_bridge_truck_world_y = snapshot.truckY;
            g_bridge_truck_world_z = snapshot.truckZ;
            g_bridge_truck_heading = snapshot.truckHeading;
            g_bridge_truck_pitch = snapshot.truckPitch;
            g_bridge_truck_roll = snapshot.truckRoll;
        }
        {
            std::lock_guard<std::mutex> lock(g_ai_traffic_mutex);
            g_ai_traffic = {};
            g_ai_traffic.available =
                (snapshot.flags &
                    prism_camera_bridge::kTrafficValid) != 0;
            g_ai_traffic.updatedTick = snapshot.updatedTick;
            g_ai_traffic.count = (std::min)(
                snapshot.trafficCount,
                prism_camera_bridge::kMaxTrafficVehicles);
            for (uint32_t index = 0;
                index < g_ai_traffic.count; ++index)
            {
                const auto& source = snapshot.traffic[index];
                auto& destination = g_ai_traffic.vehicles[index];
                destination.id = source.id;
                destination.x = source.x;
                destination.y = source.y;
                destination.z = source.z;
                destination.speed = source.speed;
                destination.acceleration = source.acceleration;
            }
        }
        g_last_camera_bridge_tick = snapshot.updatedTick;
    }

    void shutdown()
    {
        close_mapping();
    }
}
