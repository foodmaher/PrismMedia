#include <Windows.h>
#include <scs_sdk/scssdk_telemetry.h>
#include <scs_sdk/common/scssdk_telemetry_truck_common_channels.h>
#include "scs_logging.h"

#include "version.h"

#include "sources/wgc_dispatcher.h"
#include <MinHook/MinHook.h>
#include "bmem.h"
#include "screens.h"
#include "win32/win32.h"
#include "dx11/dx11.h"
#include "dinput8/dinput8.h"
#include "prism/prism.h"
#include "menu/menu.h"
#include "settings.h"
#include "telemetry_state.h"
#include "camera_bridge_client.h"
#include "wind_audio.h"

#include <algorithm>
#include <cmath>
using namespace scs_logging;
#pragma comment(lib, "minhook.x64.lib")

#ifdef _DEBUG
#pragma comment(lib, "ImGuiD.lib")
#else
#pragma comment(lib, "ImGuiR.lib")
#endif



SCSAPI_VOID head_offset_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_fplacement)
        return;

    const uint64_t now = GetTickCount64();
    const uint64_t previousUpdate = g_last_head_update_tick.load();
    if (previousUpdate == 0 || now < previousUpdate ||
        now - previousUpdate > 500)
    {
        // A head stream resuming after an outside-camera gap means the
        // player returned to a cab camera, including with controller or
        // custom camera bindings.
        g_camera_interior_hint = true;
    }

    g_head_offset_x = value->value_fplacement.position.x;
    g_head_offset_y = value->value_fplacement.position.y;
    g_head_offset_z = value->value_fplacement.position.z;
    g_head_heading = value->value_fplacement.orientation.heading;
    g_last_head_update_tick = now;
}

SCSAPI_VOID truck_world_placement_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_dplacement)
        return;

    g_truck_world_x = value->value_dplacement.position.x;
    g_truck_world_y = value->value_dplacement.position.y;
    g_truck_world_z = value->value_dplacement.position.z;
    g_truck_heading = value->value_dplacement.orientation.heading;
    g_last_truck_placement_tick = GetTickCount64();
}

SCSAPI_VOID driving_state_changed(
    const scs_event_t event,
    const void* const,
    scs_context_t)
{
    const bool driving = event == SCS_TELEMETRY_EVENT_started;
    g_telemetry_driving = driving;
    if (driving)
    {
        // The normal camera after loading a truck is the interior camera.
        // A fresh head sample or a camera key will refine this immediately.
        g_camera_interior_hint = true;
        g_last_head_update_tick = 0;
    }
	else
	{
		// Frame telemetry can stop immediately after the paused event. Send a
		// final silent mix now while leaving configured loops running.
		wind_audio::silence();
	}
}

SCSAPI_VOID selected_gear_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_s32)
        return;

    g_selected_gear = value->value_s32.value;
    g_reverse_active =
        value->value_s32.value < 0 || g_reverse_light.load();
}

SCSAPI_VOID engine_enabled_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_bool)
        return;

    g_engine_enabled = value->value_bool.value != 0;
}

SCSAPI_VOID reverse_light_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_bool)
        return;

    const bool lightOn = value->value_bool.value != 0;
    g_reverse_light = lightOn;
    g_reverse_active = lightOn || g_selected_gear.load() < 0;
}

SCSAPI_VOID truck_speed_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_float)
        return;

    g_truck_speed_mps = value->value_float.value;
}

SCSAPI_VOID telemetry_tick(const scs_event_t event, const void* const event_info, scs_context_t context)
{
    static bool gps_patched{};
    static bool dash_patched{};
    static bool custom_patched{};

    bool has_gps{};
    bool has_dash{};
    bool has_custom{};
    IContentSource* spatialAudioTarget{};
    IContentSource* spatialAudioFallback{};
    screen_t* spatialAudioScreen{};
    const bool reverseActive = g_reverse_active.load();

    camera_bridge::poll();

    // Calibrate a driver-head anchor in truck-local space whenever the exact
    // SPF feed reports the interior camera. The anchor can then be transformed
    // with the moving truck while an exterior camera is active.
    constexpr int kSpfInteriorCamera = 2;
    constexpr float kPi = 3.14159265358979323846f;
    const uint64_t cameraNow = GetTickCount64();
    const uint64_t lastTruckPlacement =
        g_last_truck_placement_tick.load();
    const bool exactCameraAvailable =
        g_telemetry_driving.load() &&
        g_camera_bridge_connected.load() &&
        lastTruckPlacement != 0 &&
        cameraNow >= lastTruckPlacement &&
        cameraNow - lastTruckPlacement <= 1000;
    if (exactCameraAvailable)
    {
        const bool exactInterior =
            g_camera_type.load() == kSpfInteriorCamera;
        g_camera_interior_hint = exactInterior;

        const double truckX = g_truck_world_x.load();
        const double truckY = g_truck_world_y.load();
        const double truckZ = g_truck_world_z.load();
        const double cameraX = g_camera_world_x.load();
        const double cameraY = g_camera_world_y.load();
        const double cameraZ = g_camera_world_z.load();
        const float yaw = g_truck_heading.load() * 2.0f * kPi;
        const float cosine = std::cos(yaw);
        const float sine = std::sin(yaw);

        if (exactInterior)
        {
            const float deltaX =
                static_cast<float>(cameraX - truckX);
            const float deltaY =
                static_cast<float>(cameraY - truckY);
            const float deltaZ =
                static_cast<float>(cameraZ - truckZ);
            const float localX =
                deltaX * cosine - deltaZ * sine;
            const float localZ =
                deltaX * sine + deltaZ * cosine;
            const float anchorLength = std::sqrt(
                localX * localX + deltaY * deltaY +
                localZ * localZ);

            if (std::isfinite(static_cast<float>(cameraX)) &&
                std::isfinite(static_cast<float>(cameraY)) &&
                std::isfinite(static_cast<float>(cameraZ)))
            {
                // Always keep a reference pair. If SPF's live floating-origin
                // coordinates differ from the absolute SCS world placement,
                // truck displacement from this pair still moves the head
                // reference correctly.
                g_head_reference_camera_x =
                    static_cast<float>(cameraX);
                g_head_reference_camera_y =
                    static_cast<float>(cameraY);
                g_head_reference_camera_z =
                    static_cast<float>(cameraZ);
                g_head_reference_truck_x = truckX;
                g_head_reference_truck_y = truckY;
                g_head_reference_truck_z = truckZ;
                g_head_anchor_calibrated = true;

                // Prefer the more accurate yaw-aware local transform whenever
                // both APIs clearly use the same coordinate origin.
                if (std::isfinite(anchorLength) &&
                    anchorLength < 20.0f)
                {
                    if (!g_head_anchor_uses_truck_local.load())
                    {
                        g_head_anchor_local_x = localX;
                        g_head_anchor_local_y = deltaY;
                        g_head_anchor_local_z = localZ;
                    }
                    else
                    {
                        constexpr float smoothing = 0.08f;
                        g_head_anchor_local_x =
                            g_head_anchor_local_x.load() +
                            (localX - g_head_anchor_local_x.load()) *
                            smoothing;
                        g_head_anchor_local_y =
                            g_head_anchor_local_y.load() +
                            (deltaY - g_head_anchor_local_y.load()) *
                            smoothing;
                        g_head_anchor_local_z =
                            g_head_anchor_local_z.load() +
                            (localZ - g_head_anchor_local_z.load()) *
                            smoothing;
                    }
                    g_head_anchor_uses_truck_local = true;
                }
                else
                    g_head_anchor_uses_truck_local = false;
                g_external_camera_distance = 0.0f;
            }
        }
        else if (g_head_anchor_calibrated.load())
        {
            double headX{};
            double headY{};
            double headZ{};
            if (g_head_anchor_uses_truck_local.load())
            {
                const float localX = g_head_anchor_local_x.load();
                const float localY = g_head_anchor_local_y.load();
                const float localZ = g_head_anchor_local_z.load();
                headX =
                    truckX + localX * cosine + localZ * sine;
                headY = truckY + localY;
                headZ =
                    truckZ - localX * sine + localZ * cosine;
            }
            else
            {
                headX = g_head_reference_camera_x.load() +
                    (truckX - g_head_reference_truck_x.load());
                headY = g_head_reference_camera_y.load() +
                    (truckY - g_head_reference_truck_y.load());
                headZ = g_head_reference_camera_z.load() +
                    (truckZ - g_head_reference_truck_z.load());
            }
            const double deltaX = cameraX - headX;
            const double deltaY = cameraY - headY;
            const double deltaZ = cameraZ - headZ;
            const float distance = static_cast<float>(std::sqrt(
                deltaX * deltaX + deltaY * deltaY +
                deltaZ * deltaZ));
            if (std::isfinite(distance))
                g_external_camera_distance = distance;
        }
    }

    if (g_telemetry_driving.load() && !Gui::is_visible())
    {
        // ETS2/ATS use 1 for the interior camera and 2-9/0 for outside
        // cameras by default. Head telemetry freshness below remains the
        // fallback for controller users and custom bindings.
        if ((GetAsyncKeyState('1') & 1) != 0 ||
            (GetAsyncKeyState(VK_NUMPAD1) & 1) != 0)
        {
            g_camera_interior_hint = true;
        }
        for (int key = '2'; key <= '9'; ++key)
        {
            if ((GetAsyncKeyState(key) & 1) != 0)
                g_camera_interior_hint = false;
        }
        if ((GetAsyncKeyState('0') & 1) != 0)
            g_camera_interior_hint = false;
        for (int key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key)
        {
            if (key != VK_NUMPAD1 &&
                (GetAsyncKeyState(key) & 1) != 0)
                g_camera_interior_hint = false;
        }
    }

    // Wind audio needs only a tiny control update. The sound itself is
    // generated in the helper process, away from the render thread.
    const bool windDriving = g_telemetry_driving.load();
    const uint64_t windNow = GetTickCount64();
    const uint64_t windLastHead = g_last_head_update_tick.load();
    const bool windHeadFresh = windDriving && windLastHead != 0 &&
        windNow >= windLastHead && windNow - windLastHead <= 500;
    const bool windExternalCamera = windDriving &&
        (g_camera_bridge_connected.load()
            ? g_camera_type.load() != kSpfInteriorCamera
            : (!g_camera_interior_hint.load() || !windHeadFresh));
    wind_audio::update(
        windDriving,
        windDriving && !windExternalCamera);

    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        for (auto& screen : g_screens)
        {
            if (!screen.source && !screen.reverseSource &&
                !screen.reverseCameraEnabled)
                continue;

            if (screen.reverseSource)
                screen.reverseSource->SetPaused(
                    !(reverseActive || screen.reversePreview));

            if (screen.type == screen_type_t::GPS)
                has_gps = true;
            else if (screen.type == screen_type_t::DASHBOARD)
                has_dash = true;
            else if (screen.type == screen_type_t::CUSTOM)
                has_custom = true;

            if (screen.source && screen.source->SupportsSpatialAudio())
            {
                if (!spatialAudioFallback)
                    spatialAudioFallback = screen.source.get();
                if (!spatialAudioTarget && screen.adaptiveAudioEnabled)
                {
                    spatialAudioTarget = screen.source.get();
                    spatialAudioScreen = &screen;
                }
            }

            if (screen.source &&
                screen.source->SupportsVehiclePowerControl())
            {
                const bool powered =
                    !screen.followTruckEngine ||
                    !g_telemetry_driving.load() ||
                    g_engine_enabled.load();
                screen.source->SetVehiclePowered(powered);
            }
        }

        if (spatialAudioTarget && spatialAudioScreen)
        {
            const bool driving = g_telemetry_driving.load();
            const uint64_t now = GetTickCount64();
            const uint64_t lastHeadUpdate =
                g_last_head_update_tick.load();
            const bool headTelemetryFresh =
                driving && lastHeadUpdate != 0 &&
                now >= lastHeadUpdate &&
                now - lastHeadUpdate <= 500;
            const bool externalCamera =
                driving &&
                (g_camera_bridge_connected.load()
                    ? g_camera_type.load() != kSpfInteriorCamera
                    : (!g_camera_interior_hint.load() ||
                        !headTelemetryFresh));

            if (!driving)
            {
                const float menuVolume = (std::clamp)(
                    spatialAudioScreen->adaptiveAudioMenuVolume,
                    0.0f, 1.0f);
                spatialAudioTarget->SetSpatialAudio(
                    menuVolume, 0.0f, true, 20000.0f);
                g_adaptive_audio_distance_gain = menuVolume;
                g_adaptive_audio_lowpass_hz = 20000.0f;
            }
            else if (externalCamera)
            {
                float gain = (std::clamp)(
                    spatialAudioScreen->
                        adaptiveAudioExternalNearVolume,
                    0.0f, 1.0f);
                const float nearCutoff = (std::clamp)(
                    spatialAudioScreen->
                        adaptiveAudioExternalNearCutoff,
                    20.0f, 20000.0f);
                float lowpassHz =
                    spatialAudioScreen->
                        adaptiveAudioExternalLowPassEnabled
                    ? nearCutoff
                    : 20000.0f;
                const float farCutoff = (std::min)(
                    nearCutoff,
                    (std::clamp)(
                        spatialAudioScreen->
                            adaptiveAudioExternalMinimumCutoff,
                        20.0f, 8000.0f));

                const bool useExactDistance =
                    spatialAudioScreen->
                        adaptiveAudioExternalDistanceEnabled &&
                    g_camera_bridge_connected.load() &&
                    g_head_anchor_calibrated.load();
                if (useExactDistance)
                {
                    const float distance = (std::max)(
                        0.0f, g_external_camera_distance.load());
                    const float fullDistance = (std::clamp)(
                        spatialAudioScreen->
                            adaptiveAudioExternalFullVolumeDistance,
                        0.0f, 25.0f);
                    const float muteDistance = (std::max)(
                        fullDistance + 0.5f,
                        spatialAudioScreen->
                            adaptiveAudioExternalMuteDistance);
                    const float blend = (std::clamp)(
                        (distance - fullDistance) /
                            (muteDistance - fullDistance),
                        0.0f, 1.0f);
                    const float audible =
                        std::pow(1.0f - blend, 1.35f);
                    const float minimumGain = (std::clamp)(
                        spatialAudioScreen->
                            adaptiveAudioOutsideVolume,
                        0.0f, 1.0f);
                    const float nearGain = (std::clamp)(
                        spatialAudioScreen->
                            adaptiveAudioExternalNearVolume,
                        0.0f, 1.0f);
                    gain = minimumGain +
                        (nearGain - minimumGain) * audible;

                    if (spatialAudioScreen->
                        adaptiveAudioExternalLowPassEnabled)
                    {
                        const float logCutoff =
                            std::log(nearCutoff) +
                            blend *
                                (std::log(farCutoff) -
                                    std::log(nearCutoff));
                        lowpassHz = std::exp(logCutoff);
                    }
                    else
                    {
                        lowpassHz = 20000.0f;
                    }
                }

                spatialAudioTarget->SetSpatialAudio(
                    gain, 0.0f, true, lowpassHz);
                g_adaptive_audio_distance_gain = gain;
                g_adaptive_audio_lowpass_hz = lowpassHz;
            }
            else
            {
                float heading = g_head_heading.load();
                if (heading > 0.5f)
                    heading -= 1.0f;
                else if (heading < -0.5f)
                    heading += 1.0f;

                float relativeDegrees =
                    -spatialAudioScreen->adaptiveAudioSpeakerAzimuth -
                    heading * 360.0f;
                while (relativeDegrees > 180.0f)
                    relativeDegrees -= 360.0f;
                while (relativeDegrees < -180.0f)
                    relativeDegrees += 360.0f;

                const float relativeRadians =
                    relativeDegrees * kPi / 180.0f;
                const float strength = (std::clamp)(
                    spatialAudioScreen->adaptiveAudioStrength,
                    0.0f, 1.0f);
                const float pan = (std::clamp)(
                    -std::sin(relativeRadians) * strength,
                    -1.0f, 1.0f);
                const float frontAmount =
                    (std::cos(relativeRadians) + 1.0f) * 0.5f;
                const float facingAwayVolume = (std::clamp)(
                    spatialAudioScreen->adaptiveAudioFacingAwayVolume,
                    0.0f, 1.0f);
                const float directionalGain =
                    facingAwayVolume +
                    (1.0f - facingAwayVolume) * frontAmount;
                float gain = 1.0f -
                    strength * (1.0f - directionalGain);

                const float x = g_head_offset_x.load();
                const float y = g_head_offset_y.load();
                const float z = g_head_offset_z.load();
                const float distance =
                    std::sqrt(x * x + y * y + z * z);
                const float outsideDistance = (std::clamp)(
                    spatialAudioScreen->adaptiveAudioOutsideDistance,
                    0.25f, 5.0f);
                const float outsideBlend = (std::clamp)(
                    (distance - outsideDistance) / 0.35f,
                    0.0f, 1.0f);
                const float outsideVolume = (std::clamp)(
                    spatialAudioScreen->adaptiveAudioOutsideVolume,
                    0.0f, 1.0f);
                gain *= 1.0f -
                    outsideBlend * (1.0f - outsideVolume);

                spatialAudioTarget->SetSpatialAudio(
                    gain, pan, true, 20000.0f);
                g_adaptive_audio_distance_gain = gain;
                g_adaptive_audio_lowpass_hz = 20000.0f;
            }
        }
        else if (spatialAudioFallback)
        {
            spatialAudioFallback->SetSpatialAudio(
                1.0f, 0.0f, false, 20000.0f);
        }
    }

    if (has_gps != gps_patched)
    {
        static uint64_t patch_addr{};
        if (!patch_addr) {
            patch_addr = bmem::patternScan("49 8B 96 ?? ?? ?? ?? 48 85 D2 0F 84 ?? ?? ?? ?? 4D 8B 86") + 10;
        }

        DWORD oldProtect;
        VirtualProtect((void*)patch_addr, 2, PAGE_EXECUTE_READWRITE, &oldProtect);

        if (has_gps) {
            // JMP
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x90;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0xE9;
        }
        else {
            // JE
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x0F;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0x84;
        }

        VirtualProtect((void*)patch_addr, 2, oldProtect, &oldProtect);

        gps_patched = has_gps;
    }

    if (has_dash != dash_patched)
    {
        static uint64_t patch_addr{};
        if (!patch_addr) {
            patch_addr = bmem::patternScan("4C 8D 3D ?? ?? ?? ?? 48 85 D2 0F 84") + 10;
        }

        DWORD oldProtect;
        VirtualProtect((void*)patch_addr, 2, PAGE_EXECUTE_READWRITE, &oldProtect);

        if (has_dash) {
            // JMP
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x90;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0xE9;
        }
        else {
            // JE
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x0F;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0x84;
        }

        VirtualProtect((void*)patch_addr, 2, oldProtect, &oldProtect);

        dash_patched = has_dash;
    }

    if (has_custom != custom_patched)
    {
        static uint64_t patch_addr{};
        if (!patch_addr) {
            patch_addr = bmem::patternScan("0F 84 ?? ?? ?? ?? 45 84 E4 4D 0F 45 FD");
        }

        DWORD oldProtect;
        VirtualProtect((void*)patch_addr, 2, PAGE_EXECUTE_READWRITE, &oldProtect);

        if (has_custom) {
            // JMP
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x90;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0xE9;
        }
        else {
            // JE
            *(reinterpret_cast<uint8_t*>(patch_addr + 0)) = 0x0F;
            *(reinterpret_cast<uint8_t*>(patch_addr + 1)) = 0x84;
        }

        VirtualProtect((void*)patch_addr, 2, oldProtect, &oldProtect);

        custom_patched = has_custom;
    }
}

#pragma comment( linker, "/export:scs_telemetry_init=scs_telemetry_init" )
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    scs_logging::init(params, "PrismTextureStreamer v" + std::string(g_version));
    scs_log(0, "Starting PrismTextureStreamer | By: Baldy09");

    const scs_telemetry_init_params_v101_t* version_params = reinterpret_cast<const scs_telemetry_init_params_v101_t*>(params);
    version_params->register_for_event(SCS_TELEMETRY_EVENT_frame_start, telemetry_tick, nullptr);
    version_params->register_for_event(
        SCS_TELEMETRY_EVENT_paused,
        driving_state_changed,
        nullptr);
    version_params->register_for_event(
        SCS_TELEMETRY_EVENT_started,
        driving_state_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_head_offset,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_fplacement,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        head_offset_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_world_placement,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_dplacement,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        truck_world_placement_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_engine_enabled,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_bool,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        engine_enabled_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_s32,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        selected_gear_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_light_reverse,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_bool,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        reverse_light_changed,
        nullptr);
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_speed,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        truck_speed_changed,
        nullptr);

    if (MH_Initialize() != MH_OK) {
        scs_log(0, "Failed to initialize MinHook!");
        return SCS_RESULT_generic_error;
    }

    scs_log(0, "Starting Direct Input 8 hooks...");
    dinput8::init();

    scs_log(0, "Starting DX11 hooks...");
    if (!dx11::init()) {
        scs_log(
            2,
            "DX11 hook initialization failed. The plugin will "
            "not apply screen overrides, preventing a black GPS.");
        dinput8::shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        return SCS_RESULT_generic_error;
    }

    scs_log(0, "Starting Prism3D hooks...");
    prism::init();

    scs_log(0, "Starting Menu GUI...");
    Gui::init();

    settings::load();

    scs_log(0, "Plugin Started");
    return SCS_RESULT_ok;
}

#pragma comment( linker, "/export:scs_telemetry_shutdown=scs_telemetry_shutdown" )
SCSAPI_VOID scs_telemetry_shutdown()
{
    // Persist the last estimated window positions as ETS2/ATS also keeps the
    // physical window animation between sessions.
    settings::save();
    wind_audio::shutdown();
    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        for (auto& screen : g_screens)
        {
            if (screen.liveTexture) screen.liveTexture->Release();
            if (screen.uploadTexture)
                screen.uploadTexture->Release();
            if (screen.liveTextureRenderTarget)
                screen.liveTextureRenderTarget->Release();
            if (screen.immediateContext) screen.immediateContext->Release();
            if (screen.source && screen.source->SupportsSpatialAudio())
                screen.source->SetSpatialAudio(
                    1.0f, 0.0f, false, 20000.0f);
        }
        g_screens.clear(); // Stops the sources
    }

    sources::WgcDispatcher::Instance().Stop();
    camera_bridge::shutdown();

    dx11::shutdown();
    win32::shutdown();
    dinput8::shutdown();
    //prism::shutdown();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    scs_log(0, "Plugin Shutdown");
}
