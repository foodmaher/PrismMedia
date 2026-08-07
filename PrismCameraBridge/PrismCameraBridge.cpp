#include <Windows.h>

#include <SPF/SPF_API/SPF_Camera_API.h>
#include <SPF/SPF_API/SPF_Manifest_API.h>
#include <SPF/SPF_API/SPF_Plugin.h>
#include <SPF/SPF_API/SPF_Telemetry_API.h>
#include <SPF/SPF_API/SPF_Vehicle_API.h>

#include "../Shared/PrismCameraBridgeShared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace
{
    const SPF_Core_API* g_core{};
    SPF_Telemetry_Handle* g_telemetryHandle{};
    SPF_Telemetry_Callback_Handle* g_trailerCallback{};
    SPF_Telemetry_Callback_Handle* g_truckCallback{};
    HANDLE g_mapping{};
    prism_camera_bridge::SharedState* g_shared{};
    std::mutex g_trailerMutex;

    struct TrailerSnapshot
    {
        bool valid{};
        uint32_t count{};
        SPF_DPlacement placement{};
    };

    TrailerSnapshot g_trailer{};
    TrailerSnapshot g_truck{};

#pragma pack(push, 1)
    struct TrafficPlacement
    {
        float x;
        float y;
        float z;
        int16_t sectorX;
        int16_t sectorZ;
        float rotationW;
        float rotationX;
        float rotationY;
        float rotationZ;
    };
#pragma pack(pop)
    static_assert(sizeof(TrafficPlacement) == 0x20);

    struct TrafficCandidate
    {
        prism_camera_bridge::TrafficVehicle vehicle{};
        double distanceSquared{};
    };

    bool TryReadTrafficPlacement(
        uintptr_t actorAddress,
        TrafficPlacement& placement)
    {
        if (actorAddress < 0x10000)
            return false;

        // SPF's handle is the live traffic_actor_t. In Prism3D 1.60 the
        // actor's placement_t is the stable base-class field at +0x28.
        // Guard the raw read because an actor may despawn between enumeration
        // and this frame's copy.
        __try
        {
            std::memcpy(
                &placement,
                reinterpret_cast<const void*>(actorAddress + 0x28),
                sizeof(placement));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    uint32_t ReadTraffic(
        prism_camera_bridge::TrafficVehicle* output,
        uint32_t capacity,
        const TrailerSnapshot& truck)
    {
        if (!output || capacity == 0 || !g_core || !g_core->vehicle)
            return 0;

        const SPF_Vehicle_API* api = g_core->vehicle;
        if (!api->Veh_IsReady || !api->Veh_GetCount ||
            !api->Veh_GetAllHandles || !api->Veh_GetId ||
            !api->Veh_GetRawAddress || !api->Veh_IsReady())
            return 0;

        constexpr uint32_t kMaximumEnumeratedVehicles = 128;
        std::array<SPF_VehicleHandle, kMaximumEnumeratedVehicles> handles{};
        const uint32_t requested = (std::min)(
            api->Veh_GetCount(), kMaximumEnumeratedVehicles);
        if (requested == 0)
            return 0;
        const uint32_t count = (std::min)(
            api->Veh_GetAllHandles(handles.data(), requested), requested);

        std::array<
            TrafficCandidate, kMaximumEnumeratedVehicles> candidates{};
        uint32_t candidateCount{};
        for (uint32_t index = 0; index < count; ++index)
        {
            const SPF_VehicleHandle handle = handles[index];
            if (!handle)
                continue;

            const int32_t id = api->Veh_GetId(handle);
            if (id < 0)
                continue; // SPF reserves negative IDs for the player vehicle.

            TrafficPlacement placement{};
            const uintptr_t address = api->Veh_GetRawAddress(handle);
            if (!TryReadTrafficPlacement(address, placement) ||
                !std::isfinite(placement.x) ||
                !std::isfinite(placement.y) ||
                !std::isfinite(placement.z) ||
                std::fabs(placement.x) > 4096.0f ||
                std::fabs(placement.y) > 4096.0f ||
                std::fabs(placement.z) > 4096.0f)
                continue;

            TrafficCandidate candidate{};
            candidate.vehicle.id = id;
            candidate.vehicle.x =
                static_cast<double>(placement.sectorX) * 512.0 + placement.x;
            candidate.vehicle.y = placement.y;
            candidate.vehicle.z =
                static_cast<double>(placement.sectorZ) * 512.0 + placement.z;
            if (truck.valid)
            {
                const double dx =
                    candidate.vehicle.x - truck.placement.position.x;
                const double dy =
                    candidate.vehicle.y - truck.placement.position.y;
                const double dz =
                    candidate.vehicle.z - truck.placement.position.z;
                candidate.distanceSquared = dx * dx + dy * dy + dz * dz;
            }
            candidates[candidateCount++] = candidate;
        }

        if (truck.valid)
        {
            std::sort(
                candidates.begin(), candidates.begin() + candidateCount,
                [](const TrafficCandidate& first,
                   const TrafficCandidate& second)
                {
                    return first.distanceSquared < second.distanceSquared;
                });
        }

        const uint32_t written = (std::min)(
            capacity, candidateCount);
        for (uint32_t index = 0; index < written; ++index)
            output[index] = candidates[index].vehicle;
        return written;
    }

    void BuildManifest(
        SPF_Manifest_Builder_Handle* handle,
        const SPF_Manifest_Builder_API* api)
    {
        if (!handle || !api)
            return;

        api->Info_SetName(handle, "PrismCameraBridge");
        api->Info_SetVersion(handle, "1.3.0");
        api->Info_SetMinFrameworkVersion(handle, "1.2.0");
        api->Info_SetAuthor(handle, "PrismTextureStreamerFB");
        api->Info_SetDescriptionKey(handle, "");
        api->Info_SetDescriptionLiteral(
            handle,
            "Publishes camera, truck, trailer and nearby AI traffic "
            "placements to PrismTextureStreamerFB.");
        api->Policy_SetAllowUserConfig(handle, false);
    }

    bool CreateSharedState()
    {
        if (g_shared)
            return true;

        g_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(prism_camera_bridge::SharedState),
            prism_camera_bridge::kMappingName);
        if (!g_mapping)
            return false;

        g_shared = static_cast<prism_camera_bridge::SharedState*>(
            MapViewOfFile(
                g_mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                sizeof(prism_camera_bridge::SharedState)));
        if (!g_shared)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
            return false;
        }

        std::memset(g_shared, 0, sizeof(*g_shared));
        g_shared->magic = prism_camera_bridge::kMagic;
        g_shared->version = prism_camera_bridge::kVersion;
        g_shared->flags = prism_camera_bridge::kLoaded;
        return true;
    }

    void PublishInvalid()
    {
        if (!g_shared)
            return;

        InterlockedIncrement(&g_shared->sequence);
        MemoryBarrier();
        g_shared->flags = 0;
        g_shared->updatedTick = GetTickCount64();
        MemoryBarrier();
        InterlockedIncrement(&g_shared->sequence);
    }

    void OnTrailers(
        const SPF_Trailer* trailers,
        uint32_t count,
        void*)
    {
        TrailerSnapshot next{};
        next.count = count;
        if (trailers && count > 0)
        {
            // SPF returns the active trailer chain in order. The final
            // connected entry is the physical tail of singles, doubles,
            // triples and articulated combinations.
            for (uint32_t index = 0; index < count; ++index)
            {
                if (!trailers[index].data.connected)
                    continue;
                next.valid = true;
                next.placement =
                    trailers[index].data.world_placement;
            }
        }

        std::lock_guard<std::mutex> lock(g_trailerMutex);
        g_trailer = next;
    }

    void OnTruck(const SPF_TruckData* data, void*)
    {
        TrailerSnapshot next{};
        if (data)
        {
            next.valid = true;
            next.count = 1;
            next.placement = data->world_placement;
        }
        std::lock_guard<std::mutex> lock(g_trailerMutex);
        g_truck = next;
    }

    void OnLoad(const SPF_Load_API*)
    {
        CreateSharedState();
    }

    void OnActivated(const SPF_Core_API* core)
    {
        g_core = core;
        CreateSharedState();
        if (g_shared)
            g_shared->flags =
                prism_camera_bridge::kLoaded |
                prism_camera_bridge::kActivated;

        if (core && core->telemetry &&
            core->telemetry->Tel_GetContext &&
            core->telemetry->Tel_RegisterForTrailers &&
            core->telemetry->Tel_RegisterForTruckData)
        {
            g_telemetryHandle =
                core->telemetry->Tel_GetContext(
                    "PrismCameraBridge");
            if (g_telemetryHandle)
            {
                g_trailerCallback =
                    core->telemetry->Tel_RegisterForTrailers(
                        g_telemetryHandle, OnTrailers, nullptr);
                g_truckCallback =
                    core->telemetry->Tel_RegisterForTruckData(
                        g_telemetryHandle, OnTruck, nullptr);
            }
        }
    }

    void OnUpdate()
    {
        if (!CreateSharedState())
            return;

        float x{};
        float y{};
        float z{};
        SPF_CameraType cameraType{};
        const bool cameraValid =
            g_core && g_core->camera &&
            g_core->camera->Cam_GetCameraWorldCoordinates &&
            g_core->camera->Cam_GetCurrentCamera &&
            g_core->camera->Cam_GetCameraWorldCoordinates(&x, &y, &z) &&
            g_core->camera->Cam_GetCurrentCamera(&cameraType);

        TrailerSnapshot trailer{};
        TrailerSnapshot truck{};
        {
            std::lock_guard<std::mutex> lock(g_trailerMutex);
            trailer = g_trailer;
            truck = g_truck;
        }

        uint32_t flags = prism_camera_bridge::kLoaded;
        if (g_core)
            flags |= prism_camera_bridge::kActivated;
        if (cameraValid)
            flags |= prism_camera_bridge::kCameraValid;
        if (g_trailerCallback)
            flags |= prism_camera_bridge::kTelemetryRegistered;
        if (trailer.valid)
            flags |= prism_camera_bridge::kTrailerValid;
        if (truck.valid)
            flags |= prism_camera_bridge::kTruckValid;

        static std::array<
            prism_camera_bridge::TrafficVehicle,
            prism_camera_bridge::kMaxTrafficVehicles> traffic{};
        static uint32_t trafficCount{};
        static uint64_t lastTrafficRead{};
        const uint64_t now = GetTickCount64();
        if (lastTrafficRead == 0 || now < lastTrafficRead ||
            now - lastTrafficRead >= 50)
        {
            lastTrafficRead = now;
            trafficCount = ReadTraffic(
                traffic.data(),
                prism_camera_bridge::kMaxTrafficVehicles,
                truck);
        }
        const bool trafficValid =
            g_core && g_core->vehicle &&
            g_core->vehicle->Veh_IsReady &&
            g_core->vehicle->Veh_IsReady();
        if (trafficValid)
            flags |= prism_camera_bridge::kTrafficValid;

        InterlockedIncrement(&g_shared->sequence);
        MemoryBarrier();
        g_shared->magic = prism_camera_bridge::kMagic;
        g_shared->version = prism_camera_bridge::kVersion;
        g_shared->flags = flags;
        g_shared->updatedTick = now;
        if (cameraValid)
        {
            g_shared->cameraX = x;
            g_shared->cameraY = y;
            g_shared->cameraZ = z;
            g_shared->cameraType = static_cast<int32_t>(cameraType);
        }
        g_shared->trailerCount = trailer.count;
        if (trailer.valid)
        {
            g_shared->trailerX = trailer.placement.position.x;
            g_shared->trailerY = trailer.placement.position.y;
            g_shared->trailerZ = trailer.placement.position.z;
            g_shared->trailerHeading =
                trailer.placement.orientation.heading;
            g_shared->trailerPitch =
                trailer.placement.orientation.pitch;
            g_shared->trailerRoll =
                trailer.placement.orientation.roll;
        }
        if (truck.valid)
        {
            g_shared->truckX = truck.placement.position.x;
            g_shared->truckY = truck.placement.position.y;
            g_shared->truckZ = truck.placement.position.z;
            g_shared->truckHeading =
                truck.placement.orientation.heading;
            g_shared->truckPitch =
                truck.placement.orientation.pitch;
            g_shared->truckRoll =
                truck.placement.orientation.roll;
        }
        g_shared->trafficCount = trafficCount;
        g_shared->trafficCapacity =
            prism_camera_bridge::kMaxTrafficVehicles;
        if (trafficCount > 0)
        {
            std::memcpy(
                g_shared->traffic,
                traffic.data(),
                sizeof(traffic[0]) * trafficCount);
        }
        if (trafficCount < prism_camera_bridge::kMaxTrafficVehicles)
        {
            std::memset(
                g_shared->traffic + trafficCount,
                0,
                sizeof(traffic[0]) *
                    (prism_camera_bridge::kMaxTrafficVehicles -
                        trafficCount));
        }
        MemoryBarrier();
        InterlockedIncrement(&g_shared->sequence);
    }

    void OnUnload()
    {
        PublishInvalid();
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
        g_core = nullptr;
        g_telemetryHandle = nullptr;
        g_trailerCallback = nullptr;
        g_truckCallback = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_trailerMutex);
            g_trailer = {};
            g_truck = {};
        }
    }
}

extern "C"
{
    SPF_PLUGIN_EXPORT bool SPF_GetManifestAPI(SPF_Manifest_API* api)
    {
        if (!api)
            return false;
        api->BuildManifest = BuildManifest;
        return true;
    }

    SPF_PLUGIN_EXPORT bool SPF_GetPlugin(SPF_Plugin_Exports* exports)
    {
        if (!exports)
            return false;

        std::memset(exports, 0, sizeof(*exports));
        exports->OnLoad = OnLoad;
        exports->OnUnload = OnUnload;
        exports->OnUpdate = OnUpdate;
        exports->OnActivated = OnActivated;
        return true;
    }
}
