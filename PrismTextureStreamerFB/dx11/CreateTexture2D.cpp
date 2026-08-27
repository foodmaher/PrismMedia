#include "dx11.h"
#include <d3d11.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#include <MinHook/MinHook.h>

#include "../scs_logging.h"
using namespace scs_logging;

#include "../diagnostic_log.h"
#include "../custom_render_probe.h"
#include "../engine_standby.h"
#include "../screens.h"
#include "../telemetry_state.h"


typedef HRESULT(__stdcall* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t CreateTexture2D_Original = nullptr;
typedef HRESULT(__stdcall* CreateShaderResourceView_t)(
    ID3D11Device*, ID3D11Resource*,
    const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
typedef void(__stdcall* PSSetShaderResources_t)(
    ID3D11DeviceContext*, UINT, UINT,
    ID3D11ShaderResourceView* const*);
typedef void(__stdcall* DrawIndexed_t)(
    ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(__stdcall* Draw_t)(
    ID3D11DeviceContext*, UINT, UINT);
typedef void(__stdcall* DrawIndexedInstanced_t)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(__stdcall* DrawInstanced_t)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(__stdcall* DrawAuto_t)(ID3D11DeviceContext*);
typedef void(__stdcall* DrawIndexedInstancedIndirect_t)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void(__stdcall* DrawInstancedIndirect_t)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);

static CreateShaderResourceView_t CreateShaderResourceView_Original{};
static PSSetShaderResources_t PSSetShaderResources_Original{};
static DrawIndexed_t DrawIndexed_Original{};
static Draw_t Draw_Original{};
static DrawIndexedInstanced_t DrawIndexedInstanced_Original{};
static DrawInstanced_t DrawInstanced_Original{};
static DrawAuto_t DrawAuto_Original{};
static DrawIndexedInstancedIndirect_t DrawIndexedInstancedIndirect_Original{};
static DrawInstancedIndirect_t DrawInstancedIndirect_Original{};
static void* g_createShaderResourceViewAddress{};
static void* g_psSetShaderResourcesAddress{};
static void* g_drawIndexedAddress{};
static void* g_drawAddress{};
static void* g_drawIndexedInstancedAddress{};
static void* g_drawInstancedAddress{};
static void* g_drawAutoAddress{};
static void* g_drawIndexedInstancedIndirectAddress{};
static void* g_drawInstancedIndirectAddress{};
static bool g_customProbeHooksCreated{};
static bool g_customProbeHooksEnabled{};

namespace
{
    const char* screen_type_name(screen_type_t type)
    {
        switch (type)
        {
        case screen_type_t::GPS: return "GPS";
        case screen_type_t::DASHBOARD: return "dashboard";
        case screen_type_t::CUSTOM: return "custom";
        }
        return "unknown";
    }

    bool inspect_magenta_frame(
        const std::vector<uint8_t>& frame,
        uint32_t width,
        uint32_t height,
        uint32_t& magentaSamples,
        uint32_t& totalSamples)
    {
        magentaSamples = 0;
        totalSamples = 0;
        if (width == 0 || height == 0 ||
            frame.size() < static_cast<size_t>(width) * height * 4)
            return false;

        constexpr uint32_t columns = 5;
        constexpr uint32_t rows = 4;
        for (uint32_t row = 0; row < rows; ++row)
        {
            const uint32_t y = (row * 2 + 1) * height / (rows * 2);
            for (uint32_t column = 0; column < columns; ++column)
            {
                const uint32_t x =
                    (column * 2 + 1) * width / (columns * 2);
                const size_t offset =
                    (static_cast<size_t>(y) * width + x) * 4;
                const uint8_t blue = frame[offset + 0];
                const uint8_t green = frame[offset + 1];
                const uint8_t red = frame[offset + 2];
                ++totalSamples;
                if (red >= 175 && blue >= 150 && green <= 115 &&
                    red >= green + 55 && blue >= green + 40)
                    ++magentaSamples;
            }
        }
        return totalSamples >= 12 &&
            magentaSamples * 4 >= totalSamples * 3;
    }

    bool inspect_black_frame(
        const std::vector<uint8_t>& frame,
        uint32_t width,
        uint32_t height,
        uint32_t& blackSamples,
        uint32_t& totalSamples)
    {
        blackSamples = 0;
        totalSamples = 0;
        if (width == 0 || height == 0 ||
            frame.size() < static_cast<size_t>(width) * height * 4)
            return false;

        // A denser grid avoids classifying a dark Spotify/YouTube theme as a
        // failed capture merely because the old 5x4 sample missed its text and
        // artwork. A compositor-black frame is uniformly near zero.
        constexpr uint32_t columns = 9;
        constexpr uint32_t rows = 6;
        for (uint32_t row = 0; row < rows; ++row)
        {
            const uint32_t y = (row * 2 + 1) * height / (rows * 2);
            for (uint32_t column = 0; column < columns; ++column)
            {
                const uint32_t x =
                    (column * 2 + 1) * width / (columns * 2);
                const size_t offset =
                    (static_cast<size_t>(y) * width + x) * 4;
                const uint8_t blue = frame[offset + 0];
                const uint8_t green = frame[offset + 1];
                const uint8_t red = frame[offset + 2];
                ++totalSamples;
                if (red <= 8 && green <= 8 && blue <= 8)
                    ++blackSamples;
            }
        }
        return totalSamples >= 40 &&
            blackSamples * 100 >= totalSamples * 96;
    }

    void set_stale_state(screen_t& screen, bool stale, uint64_t now)
    {
        if (screen.sourceFrameStale == stale)
            return;
        screen.sourceFrameStale = stale;
        if (stale)
        {
            diagnostic_log::writef(
                "render",
                "%s source stopped delivering new frames for over 2 seconds.",
                screen_type_name(screen.type));
        }
        else
        {
            diagnostic_log::writef(
                "render", "%s source frame delivery recovered.",
                screen_type_name(screen.type));
        }
        screen.lastIssueDiagnosticTick = now;
    }

    void inspect_source_frame(
        screen_t& screen,
        const std::vector<uint8_t>& frame,
        uint32_t width,
        uint32_t height,
        uint64_t now)
    {
        screen.lastSourceFrameTick = now;
        set_stale_state(screen, false, now);
        if (screen.lastFrameInspectionTick != 0 &&
            now - screen.lastFrameInspectionTick < 1000)
            return;
        screen.lastFrameInspectionTick = now;

        uint32_t magentaSamples{};
        uint32_t totalSamples{};
        const bool suspicious = inspect_magenta_frame(
            frame, width, height, magentaSamples, totalSamples);
        screen.magentaSampleCount = magentaSamples;
        screen.diagnosticSampleCount = totalSamples;
        if (screen.suspiciousMagentaFrame != suspicious)
        {
            screen.suspiciousMagentaFrame = suspicious;
            if (suspicious)
            {
                diagnostic_log::writef(
                    "render",
                    "%s source is predominantly magenta/pink (%u/%u "
                    "samples, %ux%u). This usually indicates a WebView "
                    "protected-video "
                    "or compositor capture failure; check PrismMediaClient.log "
                    "navigation and WebView process events.",
                    screen_type_name(screen.type), magentaSamples,
                    totalSamples, width, height);
            }
            else
            {
                diagnostic_log::writef(
                    "render", "%s magenta/pink source condition cleared.",
                    screen_type_name(screen.type));
            }
            screen.lastIssueDiagnosticTick = now;
        }

        uint32_t blackSamples{};
        uint32_t blackTotal{};
        const bool currentlyBlack = inspect_black_frame(
            frame, width, height, blackSamples, blackTotal);
        screen.blackSampleCount = blackSamples;
        if (currentlyBlack)
            ++screen.consecutiveBlackFrameInspections;
        else
            screen.consecutiveBlackFrameInspections = 0;

        // Ignore startup black while WebView navigates. Three one-second
        // inspections after five seconds distinguish a persistent capture
        // failure from a normal video fade or page transition.
        const bool persistentBlack = currentlyBlack &&
            screen.consecutiveBlackFrameInspections >= 3 &&
            screen.sourceCreatedTick != 0 && now > screen.sourceCreatedTick &&
            now - screen.sourceCreatedTick >= 5000;
        if (screen.suspiciousBlackFrame != persistentBlack)
        {
            screen.suspiciousBlackFrame = persistentBlack;
            if (persistentBlack)
            {
                diagnostic_log::writef(
                    "error",
                    "%s source capture is persistently near-black (%u/%u "
                    "samples, %ux%u) although frames are arriving. The "
                    "WebView/capture path is black; repairing TOBJ/DDS assets "
                    "will not fix this condition.",
                    screen_type_name(screen.type), blackSamples, blackTotal,
                    width, height);
            }
            else
            {
                diagnostic_log::writef(
                    "render", "%s near-black capture condition cleared.",
                    screen_type_name(screen.type));
            }
            screen.lastIssueDiagnosticTick = now;
        }
    }
}

HRESULT __stdcall HookedCreateShaderResourceView(
    ID3D11Device* device,
    ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* description,
    ID3D11ShaderResourceView** view)
{
    const HRESULT result = CreateShaderResourceView_Original(
        device, resource, description, view);
    if (SUCCEEDED(result) && view && *view)
        custom_render_probe::notify_shader_resource_view(resource, *view);
    return result;
}

void __stdcall HookedPSSetShaderResources(
    ID3D11DeviceContext* context,
    UINT startSlot,
    UINT viewCount,
    ID3D11ShaderResourceView* const* views)
{
    PSSetShaderResources_Original(
        context, startSlot, viewCount, views);
    custom_render_probe::notify_pixel_shader_resources(
        startSlot, viewCount, views);
}

void __stdcall HookedDrawIndexed(
    ID3D11DeviceContext* context,
    UINT indexCount,
    UINT startIndexLocation,
    INT baseVertexLocation)
{
    custom_render_probe::notify_draw("DrawIndexed");
    DrawIndexed_Original(
        context, indexCount, startIndexLocation, baseVertexLocation);
}

void __stdcall HookedDraw(
    ID3D11DeviceContext* context,
    UINT vertexCount,
    UINT startVertexLocation)
{
    custom_render_probe::notify_draw("Draw");
    Draw_Original(context, vertexCount, startVertexLocation);
}

void __stdcall HookedDrawIndexedInstanced(
    ID3D11DeviceContext* context,
    UINT indexCountPerInstance,
    UINT instanceCount,
    UINT startIndexLocation,
    INT baseVertexLocation,
    UINT startInstanceLocation)
{
    custom_render_probe::notify_draw("DrawIndexedInstanced");
    DrawIndexedInstanced_Original(
        context, indexCountPerInstance, instanceCount,
        startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void __stdcall HookedDrawInstanced(
    ID3D11DeviceContext* context,
    UINT vertexCountPerInstance,
    UINT instanceCount,
    UINT startVertexLocation,
    UINT startInstanceLocation)
{
    custom_render_probe::notify_draw("DrawInstanced");
    DrawInstanced_Original(
        context, vertexCountPerInstance, instanceCount,
        startVertexLocation, startInstanceLocation);
}

void __stdcall HookedDrawAuto(ID3D11DeviceContext* context)
{
    custom_render_probe::notify_draw("DrawAuto");
    DrawAuto_Original(context);
}

void __stdcall HookedDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* context,
    ID3D11Buffer* arguments,
    UINT alignedByteOffset)
{
    custom_render_probe::notify_draw("DrawIndexedIndirect");
    DrawIndexedInstancedIndirect_Original(
        context, arguments, alignedByteOffset);
}

void __stdcall HookedDrawInstancedIndirect(
    ID3D11DeviceContext* context,
    ID3D11Buffer* arguments,
    UINT alignedByteOffset)
{
    custom_render_probe::notify_draw("DrawInstancedIndirect");
    DrawInstancedIndirect_Original(context, arguments, alignedByteOffset);
}

HRESULT HookedCreateTexture2D(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    if (!g_screen_source_creation_in_progress.load()) {
        std::lock_guard<std::mutex> lock(g_screens_mutex);

        for (screen_t& screen : g_screens)
        {
	            if (!screen.enabled || !screen.source)
	                continue;

            if (!pDesc) continue;
            if (pDesc->Width != screen.override_texture_size_w) continue;
            if (pDesc->Height != screen.override_texture_size_h) continue;
            if (pDesc->Format != DXGI_FORMAT_BC3_UNORM) continue;
            if (pDesc->Usage != D3D11_USAGE_DEFAULT) continue;
            if (pDesc->BindFlags != D3D11_BIND_SHADER_RESOURCE) continue;
            if (pInitialData) continue;
            if (pDesc->MipLevels != 1) continue; // Dynamic textures require exactly 1 mip

            const uint64_t now = GetTickCount64();
            if (!screen.textureRouteArmed)
            {
                if (screen.lastTextureRouteSkipLogTick == 0 ||
                    now - screen.lastTextureRouteSkipLogTick >= 5000)
                {
                    diagnostic_log::writef(
                        "route",
                        "Ignored unarmed %ux%u GPU texture candidate for "
                        "display %s; no exact %s redirect is pending.",
                        pDesc->Width, pDesc->Height,
                        screen.mediaClientId.c_str(),
                        screen.original_texture.c_str());
                    screen.lastTextureRouteSkipLogTick = now;
                }
                continue;
            }
            if (now < screen.textureRouteArmedTick ||
                now - screen.textureRouteArmedTick >
                    kTextureRouteArmTimeoutMilliseconds)
            {
                diagnostic_log::writef(
                    "route",
                    "Rejected expired GPU candidate for route #%llu, "
                    "display %s (%ux%u after %llums).",
                    static_cast<unsigned long long>(
                        screen.textureRouteSequence),
                    screen.mediaClientId.c_str(),
                    pDesc->Width, pDesc->Height,
                    static_cast<unsigned long long>(
                        now >= screen.textureRouteArmedTick
                            ? now - screen.textureRouteArmedTick : 0));
                screen.textureRouteArmed = false;
                continue;
            }


            D3D11_TEXTURE2D_DESC modifiedDesc = *pDesc;
            modifiedDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            // Preserve the stable v2.7 resource contract. Making this
            // game-owned replacement render-target capable caused
            // DXGI_ERROR_DEVICE_REMOVED during swap-chain resize on systems
            // with other DXGI hooks.
            modifiedDesc.Usage = D3D11_USAGE_DYNAMIC;
            modifiedDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            modifiedDesc.MiscFlags = 0;
            modifiedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            modifiedDesc.Width = screen.targetLiveTextureWidth;
            modifiedDesc.Height = screen.targetLiveTextureHeight;

            HRESULT hr = CreateTexture2D_Original(pDevice, &modifiedDesc, pInitialData, ppTexture2D);
            const uint64_t matchedRoute = screen.textureRouteSequence;
            screen.textureRouteArmed = false;
            if (SUCCEEDED(hr) && ppTexture2D && *ppTexture2D)
            {
                if (screen.liveTexture) screen.liveTexture->Release();
                if (screen.uploadTexture) screen.uploadTexture->Release();
                if (screen.liveTextureRenderTarget)
                    screen.liveTextureRenderTarget->Release();
                if (screen.immediateContext) screen.immediateContext->Release();

                screen.liveTextureWidth = modifiedDesc.Width;
                screen.liveTextureHeight = modifiedDesc.Height;
                screen.hasUploadedFrame = false;
                screen.lastTextureMatchTick = GetTickCount64();
                screen.textureRouteMatchedSequence = matchedRoute;

                screen.liveTexture = *ppTexture2D;
                screen.liveTexture->AddRef(); // own a ref independent of the games
                screen.uploadTexture = nullptr;
                screen.liveTextureRenderTarget = nullptr;
                pDevice->GetImmediateContext(&screen.immediateContext);

                scs_log(
                    0,
                    "[dx11::create_texture_2d] matched texture '%s' "
                    "(safe dynamic upload)",
                    screen.original_texture.c_str());
                diagnostic_log::writef(
                    "route",
                    "Consumed exact route #%llu for %s display %s (%s); "
                    "created a %ux%u dynamic BGRA upload target.",
                    static_cast<unsigned long long>(matchedRoute),
                    screen_type_name(screen.type),
                    screen.mediaClientId.c_str(),
                    screen.original_texture.c_str(),
                    modifiedDesc.Width, modifiedDesc.Height);
                if (screen.type == screen_type_t::CUSTOM)
                {
                    custom_render_probe::request_capture(
                        screen.mediaClientId.c_str(),
                        screen.original_texture.c_str(),
                        screen.liveTexture);
                }
            }
            else {
                scs_log(2, "[dx11::create_texture_2d] rewrite of %s FAILED, hr=0x%08X", screen.original_texture.c_str(), hr);
                diagnostic_log::writef(
                    "error", "Route #%llu texture rewrite failed for %s "
                    "(HRESULT 0x%08X); route was consumed safely.",
                    static_cast<unsigned long long>(matchedRoute),
                    screen.original_texture.c_str(),
                    static_cast<unsigned>(hr));
            }
            return hr;
        }
    }

    return CreateTexture2D_Original(pDevice, pDesc, pInitialData, ppTexture2D);
}


void new_frame()
{
    std::lock_guard<std::mutex> lock(g_screens_mutex);
	    for (auto& screen : g_screens)
	    {
	        const auto workStarted = std::chrono::steady_clock::now();
	        const bool showEngineStandby =
	            screen.followTruckEngine &&
	            g_telemetry_driving.load() &&
	            !g_engine_enabled.load();
        if (!screen.liveTexture || !screen.immediateContext)
            continue;

        truck_identity_snapshot_t truckIdentity{};
        if (showEngineStandby)
        {
            truckIdentity = truck_identity_snapshot();
            const bool logoNeedsRebuild =
                screen.engineStandbyScratch.empty() ||
                screen.engineStandbyScratchWidth != screen.liveTextureWidth ||
                screen.engineStandbyScratchHeight != screen.liveTextureHeight ||
                screen.engineStandbyIdentityRevision != truckIdentity.revision;
            if (logoNeedsRebuild)
            {
                engine_standby::render_truck_logo(
                    screen.engineStandbyScratch,
                    screen.liveTextureWidth,
                    screen.liveTextureHeight,
                    truckIdentity.brand,
                    truckIdentity.name);
                screen.engineStandbyScratchWidth = screen.liveTextureWidth;
                screen.engineStandbyScratchHeight = screen.liveTextureHeight;
                screen.engineStandbyIdentityRevision = truckIdentity.revision;
                screen.hasUploadedFrame = false;
                diagnostic_log::writef(
                    "render",
                    "%s engine-off logo prepared for '%s' '%s'.",
                    screen_type_name(screen.type),
                    truckIdentity.brand.empty()
                        ? "Truck" : truckIdentity.brand.c_str(),
                    truckIdentity.name.empty()
                        ? "" : truckIdentity.name.c_str());
            }
            if (screen.engineStandbyWasDisplayed &&
                screen.hasUploadedFrame)
                continue;
        }
        else if (screen.engineStandbyWasDisplayed)
        {
            // Restore the cached media immediately. A newer live source frame
            // replaces it normally as capture resumes.
            screen.engineStandbyWasDisplayed = false;
            screen.hasUploadedFrame = false;
        }

		        IContentSource* activeSource = showEngineStandby
		            ? nullptr : screen.source.get();

		        if (!showEngineStandby && screen.paused &&
		                screen.hasUploadedFrame)
		            continue;

        const std::vector<uint8_t>* activeFrame =
            &screen.frameScratch;
        uint32_t srcWidth = screen.frameScratchWidth;
        uint32_t srcHeight = screen.frameScratchHeight;
        if (showEngineStandby)
        {
            activeFrame = &screen.engineStandbyScratch;
            srcWidth = screen.engineStandbyScratchWidth;
            srcHeight = screen.engineStandbyScratchHeight;
        }
        else
        {
            if (!activeSource)
                continue;
            const bool hasNewFrame =
                activeSource->CopyLatestFrame(
                    screen.frameScratch, srcWidth, srcHeight);
            if (hasNewFrame)
            {
                screen.frameScratchWidth = srcWidth;
                screen.frameScratchHeight = srcHeight;
                inspect_source_frame(
                    screen, screen.frameScratch,
                    srcWidth, srcHeight, GetTickCount64());
            }
            else
            {
                const uint64_t now = GetTickCount64();
                const bool expectsFrames = !screen.paused &&
                    (!screen.followTruckEngine || g_engine_enabled.load()) &&
                    g_telemetry_driving.load();
                const uint64_t reference = screen.lastSourceFrameTick != 0
                    ? screen.lastSourceFrameTick
                    : screen.sourceCreatedTick;
                set_stale_state(
                    screen,
                    expectsFrames && reference != 0 && now > reference &&
                        now - reference >= 2000,
                    now);
                if (screen.hasUploadedFrame || screen.frameScratch.empty())
                    continue;
            }
        }

        const UINT dstWidth = screen.liveTextureWidth;
        const UINT dstHeight = screen.liveTextureHeight;
        if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0)
        {
            const uint64_t now = GetTickCount64();
            if (screen.lastIssueDiagnosticTick == 0 ||
                now - screen.lastIssueDiagnosticTick >= 5000)
            {
                diagnostic_log::writef(
                    "error",
                    "%s upload skipped because dimensions were invalid "
                    "(source %ux%u, target %ux%u).",
                    screen_type_name(screen.type), srcWidth, srcHeight,
                    dstWidth, dstHeight);
                screen.lastIssueDiagnosticTick = now;
            }
            continue;
        }
        if (activeFrame->size() <
            static_cast<size_t>(srcWidth) * srcHeight * 4)
        {
            const uint64_t now = GetTickCount64();
            if (screen.lastIssueDiagnosticTick == 0 ||
                now - screen.lastIssueDiagnosticTick >= 5000)
            {
                diagnostic_log::writef(
                    "error",
                    "%s source buffer is too small (%llu bytes for %ux%u).",
                    screen_type_name(screen.type),
                    static_cast<unsigned long long>(activeFrame->size()),
                    srcWidth, srcHeight);
                screen.lastIssueDiagnosticTick = now;
            }
            continue;
        }

        UINT srcX{};
        UINT srcY{};
        UINT srcSpanWidth = srcWidth;
        UINT srcSpanHeight = srcHeight;
        UINT dstX{};
        UINT dstY{};
        UINT renderWidth = dstWidth;
        UINT renderHeight = dstHeight;

        const uint64_t sourceAspectProduct = static_cast<uint64_t>(srcWidth) * dstHeight;
        const uint64_t destinationAspectProduct = static_cast<uint64_t>(srcHeight) * dstWidth;

        if (screen.scaleMode == scale_mode_t::FIT)
        {
            if (sourceAspectProduct > destinationAspectProduct)
            {
                renderHeight = (std::max)(1U, static_cast<UINT>(
                    static_cast<uint64_t>(dstWidth) * srcHeight / srcWidth));
                dstY = (dstHeight - renderHeight) / 2;
            }
            else
            {
                renderWidth = (std::max)(1U, static_cast<UINT>(
                    static_cast<uint64_t>(dstHeight) * srcWidth / srcHeight));
                dstX = (dstWidth - renderWidth) / 2;
            }
        }
        else if (screen.scaleMode == scale_mode_t::CROP)
        {
            if (sourceAspectProduct > destinationAspectProduct)
            {
                srcSpanWidth = (std::max)(1U, static_cast<UINT>(
                    static_cast<uint64_t>(srcHeight) * dstWidth / dstHeight));
                srcX = (srcWidth - srcSpanWidth) / 2;
            }
            else
            {
                srcSpanHeight = (std::max)(1U, static_cast<UINT>(
                    static_cast<uint64_t>(srcWidth) * dstHeight / dstWidth));
                srcY = (srcHeight - srcSpanHeight) / 2;
            }
        }

        if (screen.scaleXSourceOffset != srcX ||
            screen.scaleXSourceSpan != srcSpanWidth ||
            screen.scaleXDestinationWidth != renderWidth)
        {
            screen.scaleX.resize(renderWidth);
            for (UINT x = 0; x < renderWidth; ++x)
                screen.scaleX[x] = srcX + static_cast<UINT>(
                    static_cast<uint64_t>(x) * srcSpanWidth / renderWidth);
            screen.scaleXSourceOffset = srcX;
            screen.scaleXSourceSpan = srcSpanWidth;
            screen.scaleXDestinationWidth = renderWidth;
        }

        // The integrated media helper applies brightness in WebView2's GPU
        // compositor. Other sources retain the compatible CPU fallback.
        const bool sourceHandlesBrightness =
            !showEngineStandby &&
            activeSource &&
            activeSource->SupportsSourceBrightness();
        const float brightness = showEngineStandby
            ? (std::clamp)(
                screen.effectiveBrightness * screen.engineOffBrightness,
                0.05f, 2.0f)
            : sourceHandlesBrightness
                ? 1.0f
                : (std::clamp)(screen.effectiveBrightness, 0.05f, 2.0f);
        const bool adjustBrightness =
            std::fabs(brightness - 1.0f) > 0.001f;
        if (adjustBrightness &&
            std::fabs(screen.brightnessLutScale - brightness) > 0.0001f)
        {
            for (size_t value = 0;
                value < screen.brightnessLut.size(); ++value)
            {
                screen.brightnessLut[value] =
                    static_cast<uint8_t>((std::min)(
                        255L,
                        std::lround(
                            static_cast<float>(value) * brightness)));
            }
            screen.brightnessLutScale = brightness;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult = screen.immediateContext->Map(
            screen.liveTexture,
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped);
        if (FAILED(mapResult))
        {
            ++screen.consecutiveMapFailures;
            screen.lastMapResult = mapResult;
            const uint64_t now = GetTickCount64();
            if (screen.lastIssueDiagnosticTick == 0 ||
                now - screen.lastIssueDiagnosticTick >= 5000)
            {
                diagnostic_log::writef(
                    "error",
                    "%s dynamic texture Map failed (HRESULT 0x%08X, "
                    "consecutive=%u).",
                    screen_type_name(screen.type),
                    static_cast<unsigned>(mapResult),
                    screen.consecutiveMapFailures);
                screen.lastIssueDiagnosticTick = now;
            }
            continue;
        }
        if (screen.consecutiveMapFailures != 0)
        {
            diagnostic_log::writef(
                "render", "%s texture mapping recovered after %u failures.",
                screen_type_name(screen.type),
                screen.consecutiveMapFailures);
            screen.consecutiveMapFailures = 0;
            screen.lastMapResult = 0;
        }

        const uint8_t* src = activeFrame->data();
        uint8_t* dstBase = static_cast<uint8_t*>(mapped.pData);

        if (screen.scaleMode == scale_mode_t::FIT &&
            (dstX != 0 || dstY != 0 || renderWidth != dstWidth || renderHeight != dstHeight))
        {
            for (UINT y = 0; y < dstHeight; ++y)
                memset(dstBase + static_cast<size_t>(y) * mapped.RowPitch, 0,
                    static_cast<size_t>(dstWidth) * 4);
        }

        for (UINT y = 0; y < renderHeight; ++y)
        {
            const UINT sampledSourceY = srcY + static_cast<UINT>(
                static_cast<uint64_t>(y) * srcSpanHeight / renderHeight);
            const UINT logicalDestinationY = dstY + y;
            const UINT destinationRow =
                screen.flipVertical
                ? (dstHeight - 1 - logicalDestinationY)
                : logicalDestinationY;
            const uint8_t* srcRow =
                src + static_cast<size_t>(sampledSourceY) * srcWidth * 4;
            uint8_t* dstRowPtr =
                dstBase + static_cast<size_t>(destinationRow) * mapped.RowPitch +
                static_cast<size_t>(dstX) * 4;

            const bool directHorizontalCopy =
                srcX == 0 && srcSpanWidth == renderWidth;
            if (directHorizontalCopy && !adjustBrightness) {
                memcpy(dstRowPtr, srcRow, static_cast<size_t>(renderWidth) * 4);
                continue;
            }

            const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(srcRow);
            uint32_t* dstPixels = reinterpret_cast<uint32_t*>(dstRowPtr);
            for (UINT x = 0; x < renderWidth; ++x) {
                UINT sourcePixelX =
                    directHorizontalCopy ? x : screen.scaleX[x];
                const uint32_t pixel = srcPixels[sourcePixelX];
                if (!adjustBrightness)
                {
                    dstPixels[x] = pixel;
                    continue;
                }

                dstPixels[x] =
                    (pixel & 0xFF000000u) |
                    static_cast<uint32_t>(
                        screen.brightnessLut[pixel & 0xFFu]) |
                    (static_cast<uint32_t>(
                        screen.brightnessLut[(pixel >> 8) & 0xFFu]) << 8) |
                    (static_cast<uint32_t>(
                        screen.brightnessLut[(pixel >> 16) & 0xFFu]) << 16);
            }
        }

        // Some truck GPS materials sample slightly beyond the visible UV
        // rectangle. With clamp sampling, a bright video edge can then tint
        // the surrounding bezel. A tiny opaque-black guard makes all
        // out-of-range samples black without adding a visible thick frame.
        const UINT edgeGuard = (std::min)(
            static_cast<UINT>(screen.edgeBleedGuard),
            (std::min)(dstWidth / 2, dstHeight / 2));
        if (edgeGuard > 0)
        {
            constexpr uint32_t opaqueBlack = 0xFF000000u;
            for (UINT y = 0; y < dstHeight; ++y)
            {
                auto* row = reinterpret_cast<uint32_t*>(
                    dstBase + static_cast<size_t>(y) * mapped.RowPitch);
                if (y < edgeGuard || y >= dstHeight - edgeGuard)
                {
                    std::fill_n(row, dstWidth, opaqueBlack);
                }
                else
                {
                    std::fill_n(row, edgeGuard, opaqueBlack);
                    std::fill_n(
                        row + dstWidth - edgeGuard,
                        edgeGuard,
                        opaqueBlack);
                }
            }
        }

        screen.immediateContext->Unmap(
            screen.liveTexture, 0);
        screen.hasUploadedFrame = true;
        screen.engineStandbyWasDisplayed = showEngineStandby;
        ++screen.uploadedFrames;

        const double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - workStarted).count();
        screen.uploadCpuMs = screen.uploadCpuMs == 0.0
            ? elapsedMs
            : (screen.uploadCpuMs * 0.90 + elapsedMs * 0.10);

        const uint64_t nowTick = GetTickCount64();
        if (screen.lastUploadTick != 0 && nowTick > screen.lastUploadTick)
        {
            const double instantaneousFps =
                1000.0 / static_cast<double>(nowTick - screen.lastUploadTick);
            screen.deliveredFps = screen.deliveredFps == 0.0
                ? instantaneousFps
                : (screen.deliveredFps * 0.90 + instantaneousFps * 0.10);
        }
        screen.lastUploadTick = nowTick;

	        const auto sourceStats = activeSource
                ? activeSource->GetPerformanceStats()
                : source_performance_stats_t{};
	        screen.totalPluginCpuMs =
	            screen.uploadCpuMs + sourceStats.workerCpuMs;

            if (screen.lastRenderDiagnosticTick == 0 ||
                nowTick - screen.lastRenderDiagnosticTick >= 10000)
            {
                diagnostic_log::writef(
                    "render",
                    "%s summary: source=%ux%u target=%ux%u upload=%.3fms "
                    "worker=%.3fms readback=%.3fms delivered=%.1ffps "
                    "uploaded=%llu dropped=%llu stale=%d pink=%d black=%d.",
                    screen_type_name(screen.type), srcWidth, srcHeight,
                    dstWidth, dstHeight, screen.uploadCpuMs,
                    sourceStats.workerCpuMs, sourceStats.readbackMs,
                    sourceStats.deliveredFps > 0.0
                        ? sourceStats.deliveredFps : screen.deliveredFps,
                    static_cast<unsigned long long>(screen.uploadedFrames),
                    static_cast<unsigned long long>(
                        sourceStats.droppedFrames),
                    screen.sourceFrameStale ? 1 : 0,
                    screen.suspiciousMagentaFrame ? 1 : 0,
                    screen.suspiciousBlackFrame ? 1 : 0);
                screen.lastRenderDiagnosticTick = nowTick;
            }
	    }
}


namespace dx11::create_texture_2d {
	bool init()
	{
        dx11::present::on_frame(new_frame);

        ID3D11Device* pDummyDevice = nullptr;
        ID3D11DeviceContext* pDummyContext = nullptr;

        if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &pDummyDevice, nullptr, &pDummyContext)))
        {
            scs_log(0, "[dx11::create_texture_2d] D3D11CreateDevice failed");
            return false;
        }

        void** deviceVtbl = *reinterpret_cast<void***>(pDummyDevice);
        void* createTexture2DAddr = deviceVtbl[5];

        MH_STATUS hookResult = MH_CreateHook(
            createTexture2DAddr,
            reinterpret_cast<LPVOID>(&HookedCreateTexture2D),
            reinterpret_cast<LPVOID*>(
                &CreateTexture2D_Original));
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::create_texture_2d] Hook creation "
                "failed: %d",
                static_cast<int>(hookResult));
            pDummyContext->Release();
            pDummyDevice->Release();
            return false;
        }
        hookResult = MH_EnableHook(createTexture2DAddr);
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::create_texture_2d] Hook enable "
                "failed: %d",
                static_cast<int>(hookResult));
            MH_RemoveHook(createTexture2DAddr);
            pDummyContext->Release();
            pDummyDevice->Release();
            return false;
        }

        void** contextVtbl = *reinterpret_cast<void***>(pDummyContext);
        g_createShaderResourceViewAddress = deviceVtbl[7];
        g_psSetShaderResourcesAddress = contextVtbl[8];
        g_drawIndexedAddress = contextVtbl[12];
        g_drawAddress = contextVtbl[13];
        g_drawIndexedInstancedAddress = contextVtbl[20];
        g_drawInstancedAddress = contextVtbl[21];
        g_drawAutoAddress = contextVtbl[38];
        g_drawIndexedInstancedIndirectAddress = contextVtbl[39];
        g_drawInstancedIndirectAddress = contextVtbl[40];

        const MH_STATUS createSrvResult = MH_CreateHook(
            g_createShaderResourceViewAddress,
            reinterpret_cast<LPVOID>(&HookedCreateShaderResourceView),
            reinterpret_cast<LPVOID*>(
                &CreateShaderResourceView_Original));
        const MH_STATUS psSetResult = MH_CreateHook(
            g_psSetShaderResourcesAddress,
            reinterpret_cast<LPVOID>(&HookedPSSetShaderResources),
            reinterpret_cast<LPVOID*>(
                &PSSetShaderResources_Original));
        const MH_STATUS drawIndexedResult = MH_CreateHook(
            g_drawIndexedAddress,
            reinterpret_cast<LPVOID>(&HookedDrawIndexed),
            reinterpret_cast<LPVOID*>(
                &DrawIndexed_Original));
        const MH_STATUS drawResult = MH_CreateHook(
            g_drawAddress,
            reinterpret_cast<LPVOID>(&HookedDraw),
            reinterpret_cast<LPVOID*>(
                &Draw_Original));
        const MH_STATUS drawIndexedInstancedResult = MH_CreateHook(
            g_drawIndexedInstancedAddress,
            reinterpret_cast<LPVOID>(&HookedDrawIndexedInstanced),
            reinterpret_cast<LPVOID*>(
                &DrawIndexedInstanced_Original));
        const MH_STATUS drawInstancedResult = MH_CreateHook(
            g_drawInstancedAddress,
            reinterpret_cast<LPVOID>(&HookedDrawInstanced),
            reinterpret_cast<LPVOID*>(
                &DrawInstanced_Original));
        const MH_STATUS drawAutoResult = MH_CreateHook(
            g_drawAutoAddress,
            reinterpret_cast<LPVOID>(&HookedDrawAuto),
            reinterpret_cast<LPVOID*>(&DrawAuto_Original));
        const MH_STATUS drawIndexedIndirectResult = MH_CreateHook(
            g_drawIndexedInstancedIndirectAddress,
            reinterpret_cast<LPVOID>(
                &HookedDrawIndexedInstancedIndirect),
            reinterpret_cast<LPVOID*>(
                &DrawIndexedInstancedIndirect_Original));
        const MH_STATUS drawIndirectResult = MH_CreateHook(
            g_drawInstancedIndirectAddress,
            reinterpret_cast<LPVOID>(&HookedDrawInstancedIndirect),
            reinterpret_cast<LPVOID*>(
                &DrawInstancedIndirect_Original));
        g_customProbeHooksCreated =
            createSrvResult == MH_OK && psSetResult == MH_OK &&
            drawIndexedResult == MH_OK && drawResult == MH_OK &&
            drawIndexedInstancedResult == MH_OK &&
            drawInstancedResult == MH_OK && drawAutoResult == MH_OK &&
            drawIndexedIndirectResult == MH_OK &&
            drawIndirectResult == MH_OK;
        if (!g_customProbeHooksCreated)
        {
            diagnostic_log::writef(
                "error",
                "Could not prepare all temporary targeted-test hooks "
                "(CreateSRV=%d PSSetSRV=%d DrawIndexed=%d Draw=%d "
                "DrawIndexedInstanced=%d DrawInstanced=%d DrawAuto=%d "
                "DrawIndexedIndirect=%d DrawIndirect=%d).",
                static_cast<int>(createSrvResult),
                static_cast<int>(psSetResult),
                static_cast<int>(drawIndexedResult),
                static_cast<int>(drawResult),
                static_cast<int>(drawIndexedInstancedResult),
                static_cast<int>(drawInstancedResult),
                static_cast<int>(drawAutoResult),
                static_cast<int>(drawIndexedIndirectResult),
                static_cast<int>(drawIndirectResult));
        }

        pDummyContext->Release();
        pDummyDevice->Release();

        return true;
	}

    bool set_custom_probe_hooks_enabled(bool enabled)
    {
        if (!g_customProbeHooksCreated)
            return !enabled;
        if (g_customProbeHooksEnabled == enabled)
            return true;

        void* const addresses[] = {
            g_createShaderResourceViewAddress,
            g_psSetShaderResourcesAddress,
            g_drawIndexedAddress,
            g_drawAddress,
            g_drawIndexedInstancedAddress,
            g_drawInstancedAddress,
            g_drawAutoAddress,
            g_drawIndexedInstancedIndirectAddress,
            g_drawInstancedIndirectAddress
        };
        if (enabled)
        {
            for (void* address : addresses)
            {
                const MH_STATUS result = MH_EnableHook(address);
                if (result != MH_OK && result != MH_ERROR_ENABLED)
                {
                    for (void* rollback : addresses)
                        MH_DisableHook(rollback);
                    diagnostic_log::writef(
                        "error",
                        "Could not enable the temporary targeted-test "
                        "hooks (MinHook status %d).",
                        static_cast<int>(result));
                    g_customProbeHooksEnabled = false;
                    return false;
                }
            }
            g_customProbeHooksEnabled = true;
            diagnostic_log::write(
                "probe",
                "Temporary CreateSRV/PSSetSRV/Draw diagnostic hooks "
                "enabled for this one capture.");
            return true;
        }

        for (void* address : addresses)
            MH_DisableHook(address);
        g_customProbeHooksEnabled = false;
        diagnostic_log::write(
            "probe",
            "Temporary high-frequency Direct3D diagnostic hooks disabled.");
        return true;
    }
}
