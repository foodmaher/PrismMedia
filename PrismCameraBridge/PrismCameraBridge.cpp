#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <SPF/SPF_API/SPF_Camera_API.h>
#include <SPF/SPF_API/SPF_Manifest_API.h>
#include <SPF/SPF_API/SPF_Plugin.h>

#include "../Shared/PrismCameraBridgeShared.h"

#include <cstring>

namespace
{
    const SPF_Core_API* g_core{};
    HANDLE g_mapping{};
    prism_camera_bridge::SharedState* g_shared{};

    void BuildManifest(
        SPF_Manifest_Builder_Handle* handle,
        const SPF_Manifest_Builder_API* api)
    {
        if (!handle || !api)
            return;

        api->Info_SetName(handle, "PrismCameraBridge");
        api->Info_SetVersion(handle, "1.0.0");
        api->Info_SetMinFrameworkVersion(handle, "1.2.0");
        api->Info_SetAuthor(handle, "PrismTextureStreamerFB");
        api->Info_SetDescriptionKey(handle, "");
        api->Info_SetDescriptionLiteral(
            handle,
            "Publishes the active ETS2/ATS camera position to "
            "PrismTextureStreamerFB for distance-aware media audio.");
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
        return true;
    }

    void PublishInvalid()
    {
        if (!g_shared)
            return;

        InterlockedIncrement(&g_shared->sequence);
        MemoryBarrier();
        g_shared->valid = 0;
        g_shared->updatedTick = GetTickCount64();
        MemoryBarrier();
        InterlockedIncrement(&g_shared->sequence);
    }

    void OnLoad(const SPF_Load_API*)
    {
        CreateSharedState();
    }

    void OnActivated(const SPF_Core_API* core)
    {
        g_core = core;
        CreateSharedState();
    }

    void OnUpdate()
    {
        if (!g_core || !g_core->camera || !CreateSharedState())
            return;

        float x{};
        float y{};
        float z{};
        SPF_CameraType cameraType{};
        const bool valid =
            g_core->camera->Cam_GetCameraWorldCoordinates &&
            g_core->camera->Cam_GetCurrentCamera &&
            g_core->camera->Cam_GetCameraWorldCoordinates(&x, &y, &z) &&
            g_core->camera->Cam_GetCurrentCamera(&cameraType);

        InterlockedIncrement(&g_shared->sequence);
        MemoryBarrier();
        g_shared->magic = prism_camera_bridge::kMagic;
        g_shared->version = prism_camera_bridge::kVersion;
        g_shared->valid = valid ? 1U : 0U;
        g_shared->updatedTick = GetTickCount64();
        if (valid)
        {
            g_shared->cameraX = x;
            g_shared->cameraY = y;
            g_shared->cameraZ = z;
            g_shared->cameraType = static_cast<int32_t>(cameraType);
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
