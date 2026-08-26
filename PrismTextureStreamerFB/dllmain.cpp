#include <Windows.h>
#include <scs_sdk/scssdk_telemetry.h>
#include <scs_sdk/common/scssdk_telemetry_common_configs.h>
#include <scs_sdk/common/scssdk_telemetry_truck_common_channels.h>
#include "scs_logging.h"

#include "version.h"

#include "sources/wgc_dispatcher.h"
#include "sources/media_client.h"
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
#include "diagnostic_log.h"
#include "environment_audio.h"
#include "thread_scheduling.h"
#include "update_checker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
using namespace scs_logging;
#pragma comment(lib, "minhook.x64.lib")

namespace
{
    uint64_t g_customRenderPatchAddress{};
    bool g_customRenderPatchEnabled{};
    bool g_customRenderPatchResolutionFailed{};

    bool set_custom_render_patch(bool enabled)
    {
        if (!enabled && !g_customRenderPatchEnabled &&
            g_customRenderPatchAddress == 0)
            return true;
        if (enabled == g_customRenderPatchEnabled &&
            g_customRenderPatchAddress != 0)
            return true;
        if (g_customRenderPatchResolutionFailed)
            return false;

        if (g_customRenderPatchAddress == 0)
        {
            g_customRenderPatchAddress = bmem::patternScan(
                "0F 84 ?? ?? ?? ?? 45 84 E4 4D 0F 45 FD");
            if (g_customRenderPatchAddress == 0)
            {
                // A previous plugin build may have left the branch enabled
                // during an in-process DLL reload. Resolve that form too so
                // shutdown can always restore the game's original bytes.
                g_customRenderPatchAddress = bmem::patternScan(
                    "90 E9 ?? ?? ?? ?? 45 84 E4 4D 0F 45 FD");
            }
            if (g_customRenderPatchAddress == 0)
            {
                if (!g_customRenderPatchResolutionFailed)
                {
                    diagnostic_log::write(
                        "error",
                        "Could not resolve the custom-display render branch; "
                        "custom textures can upload but will not be presented.");
                    g_customRenderPatchResolutionFailed = true;
                }
                return false;
            }
        }

        auto* bytes = reinterpret_cast<uint8_t*>(
            g_customRenderPatchAddress);
        const bool currentlyEnabled =
            bytes[0] == 0x90 && bytes[1] == 0xE9;
        const bool currentlyDisabled =
            bytes[0] == 0x0F && bytes[1] == 0x84;
        if (!currentlyEnabled && !currentlyDisabled)
        {
            diagnostic_log::writef(
                "error",
                "Custom-display render branch has unexpected bytes %02X %02X; "
                "the plugin left it untouched.",
                static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(bytes[1]));
            return false;
        }
        if (currentlyEnabled == enabled)
        {
            g_customRenderPatchEnabled = enabled;
            return true;
        }

        DWORD oldProtect{};
        if (!VirtualProtect(
                bytes, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            diagnostic_log::writef(
                "error",
                "Could not change custom-display render protection "
                "(Win32 error %lu).",
                GetLastError());
            return false;
        }

        bytes[0] = enabled ? 0x90 : 0x0F;
        bytes[1] = enabled ? 0xE9 : 0x84;
        FlushInstructionCache(GetCurrentProcess(), bytes, 2);
        DWORD ignoredProtect{};
        VirtualProtect(bytes, 2, oldProtect, &ignoredProtect);
        g_customRenderPatchEnabled = enabled;
        diagnostic_log::writef(
            "route",
            "Custom-display render compatibility branch %s %s.",
            enabled ? "enabled" : "restored",
            enabled
                ? "after an exact custom GPU route was matched"
                : "because no matched custom display remains");
        return true;
    }

    float calculate_effective_brightness(const screen_t& screen)
    {
        float effective = (std::clamp)(
            screen.brightness, 0.10f, 2.0f);
        if (screen.autoBrightnessEnabled &&
            g_game_lighting_valid.load())
        {
            const float luminance = g_game_lighting_luminance.load();
            float scene = (std::clamp)(
                (luminance - 0.06f) / 0.54f, 0.0f, 1.0f);
            scene = scene * scene * (3.0f - 2.0f * scene);
            const float multiplier =
                screen.autoBrightnessDarkMultiplier +
                (screen.autoBrightnessBrightMultiplier -
                    screen.autoBrightnessDarkMultiplier) * scene;
            effective *= multiplier;
        }
        return (std::clamp)(effective, 0.05f, 2.0f);
    }

}

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
    const bool previous = g_telemetry_driving.exchange(driving);
    if (previous != driving)
    {
        diagnostic_log::writef(
            "telemetry", "Driving state changed: %s",
            driving ? "started" : "paused/menu");
    }
    if (driving)
    {
        // The normal camera after loading a truck is the interior camera.
        // A fresh head sample or a camera key will refine this immediately.
        g_camera_interior_hint = true;
        g_last_head_update_tick = 0;
    }
	else
	{
		// Frame telemetry can stop immediately after the paused event.
		environment_audio::reset();
	}
}

SCSAPI_VOID truck_configuration_changed(
    const scs_event_t,
    const void* const eventInfo,
    const scs_context_t)
{
    const auto* configuration =
        static_cast<const scs_telemetry_configuration_t*>(eventInfo);
    if (!configuration || !configuration->id ||
        std::strcmp(configuration->id, SCS_TELEMETRY_CONFIG_truck) != 0)
        return;

    std::string brand;
    std::string name;
    for (const scs_named_value_t* attribute = configuration->attributes;
        attribute && attribute->name; ++attribute)
    {
        if (attribute->value.type != SCS_VALUE_TYPE_string ||
            !attribute->value.value_string.value)
            continue;
        if (std::strcmp(
            attribute->name,
            SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand) == 0)
        {
            brand = attribute->value.value_string.value;
        }
        else if (std::strcmp(
            attribute->name,
            SCS_TELEMETRY_CONFIG_ATTRIBUTE_name) == 0)
        {
            name = attribute->value.value_string.value;
        }
    }

    set_truck_identity(brand, name);
    diagnostic_log::writef(
        "telemetry", "Truck identity changed: brand='%s', name='%s'.",
        brand.empty() ? "Truck" : brand.c_str(),
        name.empty() ? "unknown" : name.c_str());
}

SCSAPI_VOID engine_enabled_changed(
    const scs_string_t,
    const scs_u32_t,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_bool)
        return;

    const bool enabled = value->value_bool.value != 0;
    if (g_engine_enabled.load(std::memory_order_relaxed) == enabled)
        return;
    g_engine_enabled.store(enabled, std::memory_order_relaxed);
    diagnostic_log::writef(
        "telemetry", "Truck engine changed: %s",
        enabled ? "running" : "stopped");
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

SCSAPI_VOID wheel_on_ground_changed(
    const scs_string_t,
    const scs_u32_t index,
    const scs_value_t* const value,
    const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_bool ||
        index >= kTrackedTruckWheelCount)
        return;

    g_environment_wheel_on_ground[index] =
        value->value_bool.value != 0;
    g_environment_wheel_sample_seen[index] = true;
}

SCSAPI_VOID telemetry_tick(const scs_event_t event, const void* const event_info, scs_context_t context)
{
    static bool gps_patched{};
    static bool dash_patched{};

    bool has_gps{};
    bool has_dash{};
    bool has_custom{};

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

    // Estimate the game's environment level from official live telemetry.
    // The caller-side camera preparation is gated too, so the whole feature
    // performs meaningful work at no more than 20 Hz.
    const uint64_t environmentNow = GetTickCount64();
    static uint64_t lastEnvironmentEvaluation{};
    static bool environmentExternalCamera{};
    if (lastEnvironmentEvaluation == 0 ||
        environmentNow < lastEnvironmentEvaluation ||
        environmentNow - lastEnvironmentEvaluation >= 50)
    {
        lastEnvironmentEvaluation = environmentNow;
        const bool environmentDriving = g_telemetry_driving.load();
        const uint64_t environmentLastHead =
            g_last_head_update_tick.load();
        const bool environmentHeadFresh = environmentDriving &&
            environmentLastHead != 0 &&
            environmentNow >= environmentLastHead &&
            environmentNow - environmentLastHead <= 500;
        environmentExternalCamera = environmentDriving &&
            (g_camera_bridge_connected.load()
                ? g_camera_type.load() != kSpfInteriorCamera
                : (!g_camera_interior_hint.load() ||
                    !environmentHeadFresh));
        environment_audio::update(
            environmentDriving,
            environmentDriving && !environmentExternalCamera);
    }

    // A compact, rate-limited status line captures enough context for issue
    // reports without doing file I/O or verbose logging on the game thread.
    static uint64_t lastDiagnosticTick{};
    if (lastDiagnosticTick == 0 || environmentNow < lastDiagnosticTick ||
        environmentNow - lastDiagnosticTick >= 10000)
    {
        lastDiagnosticTick = environmentNow;
        const DWORD processor0 = thread_scheduling::preferred_processor(0);
        const DWORD processor1 = thread_scheduling::preferred_processor(1);
        const DWORD processor2 = thread_scheduling::preferred_processor(2);
        diagnostic_log::writef(
            "runtime",
            "driving=%d engine=%d camera=%s speed=%.1fkm/h "
            "environment=%.3f media_gain=%.3f spatial_gain=%.3f "
            "lighting=%.3f lighting_valid=%d "
            "cutoff=%.0fHz estimator=%.1fus "
            "background_lp=%ld,%ld,%ld",
            g_telemetry_driving.load() ? 1 : 0,
            g_engine_enabled.load() ? 1 : 0,
            environmentExternalCamera ? "exterior" : "interior",
            std::fabs(g_truck_speed_mps.load()) * 3.6f,
            g_environment_intensity.load(),
            g_environment_media_gain.load(),
            g_adaptive_audio_distance_gain.load(),
            g_game_lighting_luminance.load(),
            g_game_lighting_valid.load() ? 1 : 0,
            g_adaptive_audio_lowpass_hz.load(),
            g_environment_update_cpu_us.load(),
            processor0 == thread_scheduling::kUnassignedProcessor
                ? -1L : static_cast<long>(processor0),
            processor1 == thread_scheduling::kUnassignedProcessor
                ? -1L : static_cast<long>(processor1),
            processor2 == thread_scheduling::kUnassignedProcessor
                ? -1L : static_cast<long>(processor2));
    }

    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        bool autoBrightnessRequested = false;
        uint32_t autoBrightnessSampleHz = 1;
        for (auto& screen : g_screens)
        {
            if (!screen.source)
                continue;

            if (screen.type == screen_type_t::GPS)
                has_gps = true;
            else if (screen.type == screen_type_t::DASHBOARD)
                has_dash = true;
            else if (screen.type == screen_type_t::CUSTOM &&
                screen.liveTexture)
                has_custom = true;

            if (screen.source &&
                screen.source->SupportsVehiclePowerControl())
            {
                const bool powered =
                    !screen.followTruckEngine ||
                    !g_telemetry_driving.load() ||
                    g_engine_enabled.load();
                screen.source->SetVehiclePowered(powered);
            }

            const bool screenPowered = !screen.followTruckEngine ||
                !g_telemetry_driving.load() || g_engine_enabled.load();
            autoBrightnessRequested = autoBrightnessRequested ||
                (screen.autoBrightnessEnabled && screenPowered);
            if (screen.autoBrightnessEnabled && screenPowered &&
                screen.type == screen_type_t::GPS)
            {
                autoBrightnessSampleHz = (std::max)(
                    autoBrightnessSampleHz,
                    (std::max)(1U,
                        static_cast<uint32_t>(screen.framerate) / 2U));
            }
            const float targetBrightness =
                calculate_effective_brightness(screen);
            float nextBrightness = targetBrightness;
            if (screen.autoBrightnessEnabled &&
                g_game_lighting_valid.load())
            {
                const uint64_t last = screen.brightnessLastAdjustmentTick;
                const float elapsed = last == 0 || environmentNow < last
                    ? 1.0f / 60.0f
                    : (std::clamp)(
                        (environmentNow - last) / 1000.0f,
                        0.001f, 0.10f);
                // Phone-like adaptation: brighten in roughly 2.5 seconds and
                // dim more gently in roughly 4 seconds. This also removes
                // visible flicker from small backbuffer-luminance changes.
                const float timeConstant = targetBrightness >
                    screen.effectiveBrightness ? 0.85f : 1.35f;
                const float blend =
                    1.0f - std::exp(-elapsed / timeConstant);
                nextBrightness = screen.effectiveBrightness +
                    (targetBrightness - screen.effectiveBrightness) * blend;
                if (std::fabs(
                    targetBrightness - screen.effectiveBrightness) < 0.004f)
                    nextBrightness = targetBrightness;
            }
            screen.brightnessLastAdjustmentTick = environmentNow;
            if (std::fabs(
                nextBrightness - screen.effectiveBrightness) >= 0.0005f)
            {
                screen.effectiveBrightness = nextBrightness;
                if (screen.source &&
                    screen.source->SupportsSourceBrightness())
                {
                    screen.source->SetSourceBrightness(
                        screen.effectiveBrightness);
                }
                // CPU-brightness sources can reuse their cached frame. The
                // engine-off standby logo applies its own saved multiplier in
                // the texture upload path.
                screen.hasUploadedFrame = false;
            }
        }
        g_auto_brightness_requested = autoBrightnessRequested;
        g_auto_brightness_sample_hz = autoBrightnessSampleHz;

        const bool driving = g_telemetry_driving.load();
        const uint64_t spatialNow = GetTickCount64();
        const uint64_t lastHeadUpdate = g_last_head_update_tick.load();
        const bool headTelemetryFresh = driving && lastHeadUpdate != 0 &&
            spatialNow >= lastHeadUpdate &&
            spatialNow - lastHeadUpdate <= 500;
        const bool externalCamera = driving &&
            (g_camera_bridge_connected.load()
                ? g_camera_type.load() != kSpfInteriorCamera
                : (!g_camera_interior_hint.load() || !headTelemetryFresh));
        bool metricsPublished{};

        for (auto& screen : g_screens)
        {
            if (!screen.source || !screen.source->SupportsSpatialAudio())
                continue;
            if (!screen.adaptiveAudioEnabled)
            {
                screen.source->SetSpatialAudio(
                    1.0f, 0.0f, false, 20000.0f);
                continue;
            }

            float gain = 1.0f;
            float pan{};
            float lowpassHz = 20000.0f;
            if (!driving)
            {
                gain = (std::clamp)(
                    screen.adaptiveAudioMenuVolume, 0.0f, 1.0f);
            }
            else if (externalCamera)
            {
                gain = (std::clamp)(
                    screen.adaptiveAudioExternalNearVolume, 0.0f, 1.0f);
                const float nearCutoff = (std::clamp)(
                    screen.adaptiveAudioExternalNearCutoff,
                    20.0f, 20000.0f);
                lowpassHz = screen.adaptiveAudioExternalLowPassEnabled
                    ? nearCutoff : 20000.0f;
                const float farCutoff = (std::min)(nearCutoff,
                    (std::clamp)(
                        screen.adaptiveAudioExternalMinimumCutoff,
                        20.0f, 8000.0f));

                const bool useExactDistance =
                    screen.adaptiveAudioExternalDistanceEnabled &&
                    g_camera_bridge_connected.load() &&
                    g_head_anchor_calibrated.load();
                if (useExactDistance)
                {
                    const float distance = (std::max)(
                        0.0f, g_external_camera_distance.load());
                    const float fullDistance = (std::clamp)(
                        screen.adaptiveAudioExternalFullVolumeDistance,
                        0.0f, 25.0f);
                    const float muteDistance = (std::max)(
                        fullDistance + 0.5f,
                        screen.adaptiveAudioExternalMuteDistance);
                    const float blend = (std::clamp)(
                        (distance - fullDistance) /
                            (muteDistance - fullDistance),
                        0.0f, 1.0f);
                    const float audible =
                        std::pow(1.0f - blend, 1.35f);
                    const float minimumGain = (std::clamp)(
                        screen.adaptiveAudioOutsideVolume, 0.0f, 1.0f);
                    const float nearGain = (std::clamp)(
                        screen.adaptiveAudioExternalNearVolume,
                        0.0f, 1.0f);
                    gain = minimumGain +
                        (nearGain - minimumGain) * audible;
                    if (screen.adaptiveAudioExternalLowPassEnabled)
                    {
                        const float logCutoff = std::log(nearCutoff) +
                            blend * (std::log(farCutoff) -
                                std::log(nearCutoff));
                        lowpassHz = std::exp(logCutoff);
                    }
                }
            }
            else
            {
                float heading = g_head_heading.load();
                if (heading > 0.5f) heading -= 1.0f;
                else if (heading < -0.5f) heading += 1.0f;
                float relativeDegrees =
                    -screen.adaptiveAudioSpeakerAzimuth - heading * 360.0f;
                while (relativeDegrees > 180.0f) relativeDegrees -= 360.0f;
                while (relativeDegrees < -180.0f) relativeDegrees += 360.0f;
                const float relativeRadians =
                    relativeDegrees * kPi / 180.0f;
                const float strength = (std::clamp)(
                    screen.adaptiveAudioStrength, 0.0f, 1.0f);
                pan = (std::clamp)(
                    -std::sin(relativeRadians) * strength, -1.0f, 1.0f);
                const float frontAmount =
                    (std::cos(relativeRadians) + 1.0f) * 0.5f;
                const float facingAwayVolume = (std::clamp)(
                    screen.adaptiveAudioFacingAwayVolume, 0.0f, 1.0f);
                const float directionalGain = facingAwayVolume +
                    (1.0f - facingAwayVolume) * frontAmount;
                gain = (1.0f - strength * (1.0f - directionalGain)) *
                    (std::clamp)(
                        screen.adaptiveAudioInteriorVolume, 0.0f, 1.0f);
                const float x = g_head_offset_x.load();
                const float y = g_head_offset_y.load();
                const float z = g_head_offset_z.load();
                const float distance = std::sqrt(x * x + y * y + z * z);
                const float outsideDistance = (std::clamp)(
                    screen.adaptiveAudioOutsideDistance, 0.25f, 5.0f);
                const float outsideBlend = (std::clamp)(
                    (distance - outsideDistance) / 0.35f, 0.0f, 1.0f);
                const float outsideVolume = (std::clamp)(
                    screen.adaptiveAudioOutsideVolume, 0.0f, 1.0f);
                gain *= 1.0f - outsideBlend * (1.0f - outsideVolume);
            }

            screen.source->SetSpatialAudio(
                gain, pan, true, lowpassHz);
            if (!metricsPublished || screen.hotkeyTarget)
            {
                g_adaptive_audio_distance_gain = gain;
                g_adaptive_audio_lowpass_hz = lowpassHz;
                metricsPublished = true;
            }
        }
        if (!metricsPublished)
        {
            g_adaptive_audio_distance_gain = 1.0f;
            g_adaptive_audio_lowpass_hz = 20000.0f;
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

    set_custom_render_patch(has_custom);
}

#pragma comment( linker, "/export:scs_telemetry_init=scs_telemetry_init" )
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    scs_logging::init(params, "PrismMedia v" + std::string(g_version));
    diagnostic_log::start();
    diagnostic_log::writef(
        "session", "PrismMedia %s initializing (pid=%lu)",
        g_version, GetCurrentProcessId());
    scs_log(0, "Starting PrismMedia | By: Baldy09");

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
    version_params->register_for_event(
        SCS_TELEMETRY_EVENT_configuration,
        truck_configuration_changed,
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
        SCS_TELEMETRY_TRUCK_CHANNEL_speed,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_float,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        truck_speed_changed,
        nullptr);
    for (scs_u32_t wheel = 0;
        wheel < static_cast<scs_u32_t>(kTrackedTruckWheelCount); ++wheel)
    {
        version_params->register_for_channel(
            SCS_TELEMETRY_TRUCK_CHANNEL_wheel_on_ground,
            wheel,
            SCS_VALUE_TYPE_bool,
            SCS_TELEMETRY_CHANNEL_FLAG_none,
            wheel_on_ground_changed,
            nullptr);
    }

    if (MH_Initialize() != MH_OK) {
        diagnostic_log::write("error", "MinHook initialization failed.");
        diagnostic_log::stop();
        scs_log(0, "Failed to initialize MinHook!");
        return SCS_RESULT_generic_error;
    }

    scs_log(0, "Starting Direct Input 8 hooks...");
    dinput8::init();

    scs_log(0, "Starting DX11 hooks...");
    if (!dx11::init()) {
        diagnostic_log::write(
            "error", "DX11 hook initialization failed; refusing unsafe screen override.");
        scs_log(
            2,
            "DX11 hook initialization failed. The plugin will "
            "not apply screen overrides, preventing a black GPS.");
        dinput8::shutdown();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        diagnostic_log::stop();
        return SCS_RESULT_generic_error;
    }

    scs_log(0, "Starting Prism3D hooks...");
    prism::init();

    scs_log(0, "Starting Menu GUI...");
    Gui::init();

    settings::load();
    update_checker::start();

    diagnostic_log::write("session", "Plugin initialization completed.");
    scs_log(0, "Plugin Started");
    return SCS_RESULT_ok;
}

#pragma comment( linker, "/export:scs_telemetry_shutdown=scs_telemetry_shutdown" )
SCSAPI_VOID scs_telemetry_shutdown()
{
    diagnostic_log::write("session", "Plugin shutdown started.");
    set_custom_render_patch(false);
    sources::ShutdownMediaClient();
    update_checker::shutdown();
    settings::save();
    environment_audio::reset();
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
    diagnostic_log::write("session", "Plugin shutdown completed.");
    diagnostic_log::stop();
}
