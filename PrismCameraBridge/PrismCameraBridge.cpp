#include <Windows.h>

#include <SPF/SPF_API/SPF_Camera_API.h>
#include <SPF/SPF_API/SPF_Manifest_API.h>
#include <SPF/SPF_API/SPF_Plugin.h>
#include <SPF/SPF_API/SPF_Telemetry_API.h>

#include "../Shared/PrismCameraBridgeShared.h"

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

    void BuildManifest(
        SPF_Manifest_Builder_Handle* handle,
        const SPF_Manifest_Builder_API* api)
    {
        if (!handle || !api)
            return;

        api->Info_SetName(handle, "PrismCameraBridge");
        api->Info_SetVersion(handle, "2.0.0");
        api->Info_SetMinFrameworkVersion(handle, "1.2.0");
        api->Info_SetAuthor(handle, "PrismTextureStreamerFB");
        api->Info_SetDescriptionKey(handle, "");
        api->Info_SetDescriptionLiteral(
            handle,
            "Publishes camera, truck and trailer placement data to "
            "PrismTextureStreamerFB adaptive audio.");
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

        const uint64_t now = GetTickCount64();

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
