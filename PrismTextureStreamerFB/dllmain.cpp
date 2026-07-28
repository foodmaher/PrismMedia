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

    g_head_offset_x = value->value_fplacement.position.x;
    g_head_offset_y = value->value_fplacement.position.y;
    g_head_offset_z = value->value_fplacement.position.z;
    g_head_heading = value->value_fplacement.orientation.heading;
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
        }

        if (spatialAudioTarget && spatialAudioScreen)
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

            constexpr float kPi = 3.14159265358979323846f;
            const float relativeRadians = relativeDegrees * kPi / 180.0f;
            const float strength = (std::clamp)(
                spatialAudioScreen->adaptiveAudioStrength, 0.0f, 1.0f);
            const float pan = (std::clamp)(
                -std::sin(relativeRadians) * strength, -1.0f, 1.0f);
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
            const float distance = std::sqrt(x * x + y * y + z * z);
            const float outsideDistance = (std::clamp)(
                spatialAudioScreen->adaptiveAudioOutsideDistance,
                0.25f, 5.0f);
            const float outsideBlend = (std::clamp)(
                (distance - outsideDistance) / 0.35f, 0.0f, 1.0f);
            const float outsideVolume = (std::clamp)(
                spatialAudioScreen->adaptiveAudioOutsideVolume,
                0.0f, 1.0f);
            gain *= 1.0f -
                outsideBlend * (1.0f - outsideVolume);

            spatialAudioTarget->SetSpatialAudio(gain, pan, true);
        }
        else if (spatialAudioFallback)
        {
            spatialAudioFallback->SetSpatialAudio(1.0f, 0.0f, false);
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
    version_params->register_for_channel(
        SCS_TELEMETRY_TRUCK_CHANNEL_head_offset,
        SCS_U32_NIL,
        SCS_VALUE_TYPE_fplacement,
        SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
        head_offset_changed,
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

    if (MH_Initialize() != MH_OK) {
        scs_log(0, "Failed to initialize MinHook!");
        return SCS_RESULT_generic_error;
    }

    scs_log(0, "Starting Direct Input 8 hooks...");
    dinput8::init();

    scs_log(0, "Starting DX11 hooks...");
    dx11::init();

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
    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        for (auto& screen : g_screens)
        {
            if (screen.liveTexture) screen.liveTexture->Release();
            if (screen.immediateContext) screen.immediateContext->Release();
            if (screen.source && screen.source->SupportsSpatialAudio())
                screen.source->SetSpatialAudio(1.0f, 0.0f, false);
        }
        g_screens.clear(); // Stops the sources
    }

    sources::WgcDispatcher::Instance().Stop();

    dx11::shutdown();
    win32::shutdown();
    dinput8::shutdown();
    //prism::shutdown();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    scs_log(0, "Plugin Shutdown");
}
