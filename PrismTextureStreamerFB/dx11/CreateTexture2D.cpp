#include "dx11.h"
#include <d3d11.h>
#include <algorithm>
#include <chrono>
#include <cmath>

#include <MinHook/MinHook.h>

#include "../scs_logging.h"
using namespace scs_logging;

#include "../diagnostic_log.h"
#include "../screens.h"
#include "../telemetry_state.h"
#include "../sources/reverse_camera.h"
#include "internal_render_probe.h"


typedef HRESULT(__stdcall* CreateTexture2D_t)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static CreateTexture2D_t CreateTexture2D_Original = nullptr;

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
        if (screen.suspiciousMagentaFrame == suspicious)
            return;

        screen.suspiciousMagentaFrame = suspicious;
        if (suspicious)
        {
            diagnostic_log::writef(
                "render",
                "%s source is predominantly magenta/pink (%u/%u samples, "
                "%ux%u). This usually indicates a WebView protected-video "
                "or compositor capture failure; switch Spotify to Embed and "
                "check PrismMediaClient.log navigation/process events.",
                screen_type_name(screen.type), magentaSamples, totalSamples,
                width, height);
        }
        else
        {
            diagnostic_log::writef(
                "render", "%s magenta/pink source condition cleared.",
                screen_type_name(screen.type));
        }
        screen.lastIssueDiagnosticTick = now;
    }
}

HRESULT HookedCreateTexture2D(ID3D11Device* pDevice, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D)
{
    if (!g_screen_source_creation_in_progress.load()) {
        std::lock_guard<std::mutex> lock(g_screens_mutex);

        for (screen_t& screen : g_screens)
        {
	            if (!screen.source && !screen.reverseSource &&
	                !screen.reverseCameraEnabled)
	                continue;

            if (!pDesc) continue;
            if (pDesc->Width != screen.override_texture_size_w) continue;
            if (pDesc->Height != screen.override_texture_size_h) continue;
            if (pDesc->Format != DXGI_FORMAT_BC3_UNORM) continue;
            if (pDesc->Usage != D3D11_USAGE_DEFAULT) continue;
            if (pDesc->BindFlags != D3D11_BIND_SHADER_RESOURCE) continue;
            if (pInitialData) continue;
            if (pDesc->MipLevels != 1) continue; // Dynamic textures require exactly 1 mip


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
                    "render",
                    "Matched %s texture %s and created a %ux%u dynamic "
                    "BGRA upload target.",
                    screen_type_name(screen.type),
                    screen.original_texture.c_str(),
                    modifiedDesc.Width, modifiedDesc.Height);
            }
            else {
                scs_log(2, "[dx11::create_texture_2d] rewrite of %s FAILED, hr=0x%08X", screen.original_texture.c_str(), hr);
                diagnostic_log::writef(
                    "error", "Texture rewrite failed for %s (HRESULT 0x%08X).",
                    screen.original_texture.c_str(),
                    static_cast<unsigned>(hr));
            }
            return hr;
        }
    }

    return CreateTexture2D_Original(
        pDevice, pDesc, pInitialData, ppTexture2D);
}


void new_frame()
{
    bool internalParkActivationRequested{};
    bool internalParkRenderRequested{};
    uint32_t internalParkTargetFramerate = 1;
    uint32_t internalParkTargetVariant = 1;
    std::lock_guard<std::mutex> lock(g_screens_mutex);
	    for (auto& screen : g_screens)
	    {
	        const auto workStarted = std::chrono::steady_clock::now();
	        const bool reverseRequested =
	            screen.reverseCameraEnabled &&
	            (g_reverse_active.load() || screen.reversePreview);
            const bool internalParkMethod =
                screen.reverseCameraMethod ==
                    reverse_camera_method_t::INTERNAL_PARK_PROBE;
            internalParkActivationRequested |=
                screen.reverseCameraEnabled &&
                internalParkMethod;
            internalParkRenderRequested |=
                reverseRequested && internalParkMethod;
            if (screen.reverseCameraEnabled && internalParkMethod)
            {
                internalParkTargetFramerate = (std::max)(
                    internalParkTargetFramerate,
                    static_cast<uint32_t>(
                        screen.reverseFramerate));
                internalParkTargetVariant =
                    screen.reverseInternalTargetVariant;
            }
            const bool windowReverseRequested =
                reverseRequested && !internalParkMethod;

	        const uint64_t reverseNowTick = GetTickCount64();
	        if (windowReverseRequested && !screen.reverseSource &&
	            (screen.reverseLastStartAttemptTick == 0 ||
	                reverseNowTick - screen.reverseLastStartAttemptTick >= 2000))
	        {
	            g_screen_source_creation_in_progress = true;
	            screen.reverseLastStartAttemptTick = reverseNowTick;
	            screen.reverseSource = sources::CreateReverseCameraSource(
	                screen.reverseFramerate,
	                screen.reverseCaptureWidth,
	                screen.reverseCaptureHeight,
	                screen.reverseCropLeft,
	                screen.reverseCropTop,
	                screen.reverseCropWidth,
	                screen.reverseCropHeight);
	            g_screen_source_creation_in_progress = false;
	        }
	        else if (!reverseRequested &&
	            screen.reverseZeroForwardImpact &&
	            screen.reverseSource)
	        {
	            g_screen_source_creation_in_progress = true;
	            screen.reverseSource.reset();
	            screen.reverseLastStartAttemptTick = 0;
	            g_screen_source_creation_in_progress = false;
	        }

	        const bool reverseActive =
	            windowReverseRequested && screen.reverseSource;

        if (!screen.liveTexture || !screen.immediateContext)
            continue;

        bool usingInternalParkFrame{};
        if (reverseRequested && internalParkMethod)
        {
            uint32_t parkWidth =
                screen.internalParkScratchWidth;
            uint32_t parkHeight =
                screen.internalParkScratchHeight;
            const bool hasNewParkFrame =
                dx11::internal_render_probe::copy_park_frame(
                    screen.internalParkScratch,
                    parkWidth,
                    parkHeight,
                    screen.internalParkFrameSequence);
            if (hasNewParkFrame)
            {
                screen.internalParkScratchWidth = parkWidth;
                screen.internalParkScratchHeight = parkHeight;
                screen.internalParkWasDisplayed = true;
                screen.internalParkLastBlitTick =
                    GetTickCount64();
                usingInternalParkFrame = true;
            }
            else if (screen.internalParkWasDisplayed)
            {
                // The staged readback is deliberately non-blocking. Keep the
                // last uploaded park frame until a newer one is ready instead
                // of re-uploading or waiting on the GPU.
                continue;
            }
            // Until the first staged frame arrives, retain normal media.
        }
        else
        {
            screen.internalParkLastBlitTick = 0;
            if (screen.internalParkWasDisplayed)
            {
                // Force one upload of the last normal media frame, even when
                // that media source is paused, so the rear view never remains
                // stuck on the GPS after leaving reverse.
                screen.internalParkWasDisplayed = false;
                screen.hasUploadedFrame = false;
            }
        }

	        IContentSource* activeSource = reverseActive
	            ? screen.reverseSource.get()
	            : screen.source.get();

	        if (screen.paused && !reverseRequested &&
                screen.hasUploadedFrame)
	            continue;

        const std::vector<uint8_t>* activeFrame =
            &screen.frameScratch;
        uint32_t srcWidth = usingInternalParkFrame
            ? screen.internalParkScratchWidth
            : screen.frameScratchWidth;
        uint32_t srcHeight = usingInternalParkFrame
            ? screen.internalParkScratchHeight
            : screen.frameScratchHeight;
        if (usingInternalParkFrame)
        {
            activeFrame = &screen.internalParkScratch;
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
            !usingInternalParkFrame &&
            !reverseActive &&
            activeSource &&
            activeSource->SupportsSourceBrightness();
        const float brightness = sourceHandlesBrightness
            ? 1.0f
            : (std::clamp)(screen.brightness, 0.10f, 2.0f);
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
            const UINT destinationRow = screen.flipVertical
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
                const uint32_t pixel = srcPixels[
                    directHorizontalCopy ? x : screen.scaleX[x]];
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
                    "uploaded=%llu dropped=%llu stale=%d pink=%d.",
                    screen_type_name(screen.type), srcWidth, srcHeight,
                    dstWidth, dstHeight, screen.uploadCpuMs,
                    sourceStats.workerCpuMs, sourceStats.readbackMs,
                    sourceStats.deliveredFps > 0.0
                        ? sourceStats.deliveredFps : screen.deliveredFps,
                    static_cast<unsigned long long>(screen.uploadedFrames),
                    static_cast<unsigned long long>(
                        sourceStats.droppedFrames),
                    screen.sourceFrameStale ? 1 : 0,
                    screen.suspiciousMagentaFrame ? 1 : 0);
                screen.lastRenderDiagnosticTick = nowTick;
            }
	    }

    dx11::internal_render_probe::set_park_activation_requested(
        internalParkActivationRequested);
    dx11::internal_render_probe::set_park_render_requested(
        internalParkRenderRequested);
    dx11::internal_render_probe::set_park_target_framerate(
        internalParkTargetFramerate);
    dx11::internal_render_probe::set_park_target_variant(
        internalParkTargetVariant);
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
            &HookedCreateTexture2D,
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

        pDummyContext->Release();
        pDummyDevice->Release();

        return true;
	}
}
