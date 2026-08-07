#include "dx11.h"
#include "internal_render_probe.h"
#include "../telemetry_state.h"
#include "../thread_scheduling.h"
#include "../win32/win32.h"
#include <MinHook/MinHook.h>
#include "../scs_logging.h"
using namespace scs_logging;

#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_dx11.h>
#include <ImGui/imgui_impl_win32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

static std::vector<std::function<void()>> frame_callbacks{};
static std::vector<std::function<void()>> pending_callbacks{};

static ID3D11Device* device{};
static ID3D11DeviceContext* context{};
static ID3D11RenderTargetView* mainRenderTargetView{};

static void* present_function_address{};
static void* resize_buffers_function_address{};

namespace
{
    constexpr wchar_t kProbeWindowClass[] =
        L"PrismTextureStreamerDX11Probe";

    constexpr UINT kLightingGridSize = 4;
    constexpr uint64_t kLightingSampleIntervalMs = 250;

    struct lighting_sample_slot_t
    {
        ID3D11Texture2D* staging{};
        ID3D11Query* completion{};
        bool pending{};
    };

    std::array<lighting_sample_slot_t, 2> lightingSlots{};
    UINT lightingBackbufferWidth{};
    UINT lightingBackbufferHeight{};
    DXGI_FORMAT lightingBackbufferFormat{ DXGI_FORMAT_UNKNOWN };
    size_t nextLightingSlot{};
    uint64_t lastLightingSampleTick{};

    void reset_lighting_sampler()
    {
        for (auto& slot : lightingSlots)
        {
            if (slot.completion)
                slot.completion->Release();
            if (slot.staging)
                slot.staging->Release();
            slot = {};
        }
        lightingBackbufferWidth = 0;
        lightingBackbufferHeight = 0;
        lightingBackbufferFormat = DXGI_FORMAT_UNKNOWN;
        nextLightingSlot = 0;
        lastLightingSampleTick = 0;
        g_game_lighting_valid = false;
    }

    bool supported_lighting_format(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    bool ensure_lighting_sampler(const D3D11_TEXTURE2D_DESC& backbuffer)
    {
        if (!device || backbuffer.Width < kLightingGridSize ||
            backbuffer.Height < kLightingGridSize ||
            backbuffer.SampleDesc.Count != 1 ||
            !supported_lighting_format(backbuffer.Format))
            return false;

        if (lightingSlots[0].staging &&
            lightingBackbufferWidth == backbuffer.Width &&
            lightingBackbufferHeight == backbuffer.Height &&
            lightingBackbufferFormat == backbuffer.Format)
            return true;

        reset_lighting_sampler();

        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = kLightingGridSize;
        staging.Height = kLightingGridSize;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = backbuffer.Format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        for (auto& slot : lightingSlots)
        {
            if (FAILED(device->CreateTexture2D(
                &staging, nullptr, &slot.staging)) ||
                FAILED(device->CreateQuery(
                    &query, &slot.completion)))
            {
                reset_lighting_sampler();
                return false;
            }
        }

        lightingBackbufferWidth = backbuffer.Width;
        lightingBackbufferHeight = backbuffer.Height;
        lightingBackbufferFormat = backbuffer.Format;
        return true;
    }

    float pixel_luminance(const uint8_t* pixel, DXGI_FORMAT format)
    {
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const float red = pixel[bgra ? 2 : 0] / 255.0f;
        const float green = pixel[1] / 255.0f;
        const float blue = pixel[bgra ? 0 : 2] / 255.0f;
        return red * 0.2126f + green * 0.7152f + blue * 0.0722f;
    }

    void consume_lighting_samples()
    {
        if (!context)
            return;

        for (auto& slot : lightingSlots)
        {
            if (!slot.pending || !slot.completion || !slot.staging)
                continue;
            if (context->GetData(
                slot.completion, nullptr, 0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapResult = context->Map(
                slot.staging, 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (mapResult == DXGI_ERROR_WAS_STILL_DRAWING)
                continue;

            slot.pending = false;
            if (FAILED(mapResult))
                continue;

            std::array<float, kLightingGridSize * kLightingGridSize>
                luminance{};
            size_t sample = 0;
            for (UINT y = 0; y < kLightingGridSize; ++y)
            {
                const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch;
                for (UINT x = 0; x < kLightingGridSize; ++x)
                {
                    luminance[sample++] = pixel_luminance(
                        row + static_cast<size_t>(x) * 4,
                        lightingBackbufferFormat);
                }
            }
            context->Unmap(slot.staging, 0);

            std::sort(luminance.begin(), luminance.end());
            // The upper quartile sees the world through a dark cab without a
            // single headlight or UI pixel dominating the adjustment.
            const float measured = luminance[11];
            const float previous = g_game_lighting_luminance.load();
            const float smoothed = g_game_lighting_valid.load()
                ? previous * 0.72f + measured * 0.28f
                : measured;
            g_game_lighting_luminance = smoothed;
            g_game_lighting_valid = true;
        }
    }

    void sample_game_lighting(IDXGISwapChain* swapChain)
    {
        if (!g_auto_brightness_requested.load() || !context)
            return;

        consume_lighting_samples();

        const uint64_t now = GetTickCount64();
        if (lastLightingSampleTick != 0 &&
            now >= lastLightingSampleTick &&
            now - lastLightingSampleTick < kLightingSampleIntervalMs)
            return;

        size_t targetIndex = lightingSlots.size();
        for (size_t offset = 0; offset < lightingSlots.size(); ++offset)
        {
            const size_t candidateIndex =
                (nextLightingSlot + offset) % lightingSlots.size();
            auto& candidate = lightingSlots[candidateIndex];
            if (!candidate.pending)
            {
                targetIndex = candidateIndex;
                break;
            }
        }
        if (targetIndex == lightingSlots.size())
            return;

        ID3D11Texture2D* backbuffer{};
        if (FAILED(swapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backbuffer))))
            return;

        D3D11_TEXTURE2D_DESC description{};
        backbuffer->GetDesc(&description);
        if (!ensure_lighting_sampler(description))
        {
            backbuffer->Release();
            return;
        }

        // The slot array has stable indices even when a resize recreates its
        // resources inside ensure_lighting_sampler().
        auto& target = lightingSlots[targetIndex];
        nextLightingSlot = (targetIndex + 1) % lightingSlots.size();
        for (UINT y = 0; y < kLightingGridSize; ++y)
        {
            for (UINT x = 0; x < kLightingGridSize; ++x)
            {
                const UINT sourceX = (x + 1) * description.Width /
                    (kLightingGridSize + 1);
                const UINT sourceY = (y + 1) * description.Height /
                    (kLightingGridSize + 1);
                const D3D11_BOX box{
                    sourceX, sourceY, 0,
                    sourceX + 1, sourceY + 1, 1
                };
                context->CopySubresourceRegion(
                    target.staging, 0, x, y, 0,
                    backbuffer, 0, &box);
            }
        }
        context->End(target.completion);
        target.pending = true;
        lastLightingSampleTick = now;
        backbuffer->Release();
    }

    HWND create_probe_window(HINSTANCE instance)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_OWNDC;
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kProbeWindowClass;

        if (!RegisterClassExW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            scs_log(
                2,
                "[dx11::present] RegisterClassExW failed: %lu",
                GetLastError());
            return nullptr;
        }

        HWND window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kProbeWindowClass,
            L"Prism DX11 probe",
            WS_POPUP,
            0,
            0,
            8,
            8,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (!window)
        {
            scs_log(
                2,
                "[dx11::present] CreateWindowExW failed: %lu",
                GetLastError());
        }
        return window;
    }

    void destroy_probe_window(HWND window, HINSTANCE instance)
    {
        if (window)
            DestroyWindow(window);
        UnregisterClassW(kProbeWindowClass, instance);
    }
}

typedef HRESULT(__stdcall* present_t)(IDXGISwapChain*, UINT, UINT);
static present_t original_present{};
HRESULT hooked_present(IDXGISwapChain* SwapChain, UINT SyncInterval, UINT Flags)
{
    thread_scheduling::observe_render_thread();
    static bool init{};
    if (!init) {
        DXGI_SWAP_CHAIN_DESC desc;
        HRESULT result = SwapChain->GetDesc(&desc);
        if (!SUCCEEDED(result)) {
            scs_log(2, "Failed to get description.");
            return original_present(
                SwapChain, SyncInterval, Flags);
        }

        result = SwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
        if (!SUCCEEDED(result)) {
            scs_log(2, "Failed to get device.");
            return original_present(
                SwapChain, SyncInterval, Flags);
        }

        device->GetImmediateContext(&context);


        // Create render target view
        ID3D11Texture2D* backBuffer{};
        result = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBuffer);
        if (!SUCCEEDED(result)) {
            scs_log(2, "Failed to create back buffer.");
            device->Release();
            device = nullptr;
            context->Release();
            context = nullptr;
            return original_present(
                SwapChain, SyncInterval, Flags);
        }

        result = device->CreateRenderTargetView(backBuffer, NULL, &mainRenderTargetView);
        if (!SUCCEEDED(result)) {
            scs_log(2, "Failed to create render target view.");
            backBuffer->Release();
            device->Release();
            device = nullptr;
            context->Release();
            context = nullptr;
            return original_present(
                SwapChain, SyncInterval, Flags);
        }

        backBuffer->Release();

        win32::init(desc.OutputWindow);

        ImGuiContext* ctx = ImGui::CreateContext();
        ImGui::SetCurrentContext(ctx);

        ImGui_ImplWin32_Init(desc.OutputWindow);
        ImGui_ImplDX11_Init(device, context);

        ImGuiIO& io = ImGui::GetIO();
        io.FontDefault = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Arial.ttf", 15.0f);
        io.Fonts->Build();

        init = true;
    }

    dx11::internal_render_probe::on_present_frame(context);
    // Capture the game's lighting before the plugin UI is rendered. The
    // asynchronous query is consumed on a later frame and never blocks here.
    sample_game_lighting(SwapChain);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Refresh from the actual Win32 client-space pointer after the backend
    // update and before ImGui consumes input for this frame.
    win32::sync_menu_mouse_position();
    ImGui::NewFrame();

    if (!pending_callbacks.empty()) {
        for (auto& cb : pending_callbacks)
            frame_callbacks.push_back(std::move(cb));
        pending_callbacks.clear();
    }

    for (auto& cb : frame_callbacks)
        cb();

    ImGui::EndFrame();
    ImGui::Render();

    context->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return original_present(SwapChain, SyncInterval, Flags);
}


typedef HRESULT(__stdcall* resize_buffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static resize_buffers_t original_resize_buffers{};
HRESULT hooked_resize_buffers(IDXGISwapChain* SwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (!device || !context) // We are not ready to handle this ourself
        return original_resize_buffers(SwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);


    context->OMSetRenderTargets(0, nullptr, nullptr);
    reset_lighting_sampler();
    if (mainRenderTargetView) {
        mainRenderTargetView->Release();
        mainRenderTargetView = nullptr;
    }

    HRESULT result = original_resize_buffers(SwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    if (!SUCCEEDED(result)) {
        scs_log(2, "Original \"resize buffers\" function failed.");
        return result;
    }

    ID3D11Texture2D* backBuffer{};
    result = SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBuffer);
    if (!SUCCEEDED(result)) {
        scs_log(2, "Failed to create back buffer.");
        return result;
    }

    result = device->CreateRenderTargetView(backBuffer, NULL, &mainRenderTargetView);
    if (!SUCCEEDED(result)) {
        scs_log(2, "Failed to create render target view.");
        backBuffer->Release();
        return result;
    }

    backBuffer->Release();

    return result;
}



namespace dx11::present {
	bool init()
	{
        // Create fakes to grab real vtable
        IDXGISwapChain* swapChain{};
        ID3D11Device* device{};
        ID3D11DeviceContext* context{};
        const HINSTANCE instance =
            GetModuleHandleW(nullptr);
        const HWND probeWindow =
            create_probe_window(instance);
        if (!probeWindow)
            return false;

        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferCount = 1;
        desc.BufferDesc.Width = 8;
        desc.BufferDesc.Height = 8;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferDesc.RefreshRate.Numerator = 60;
        desc.BufferDesc.RefreshRate.Denominator = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.OutputWindow = probeWindow;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.Windowed = TRUE;

        D3D_FEATURE_LEVEL level;
        const HRESULT createResult =
            D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &desc,
            &swapChain,
            &device,
            &level,
            &context
        );
        if (FAILED(createResult)) {
            scs_log(
                2,
                "[dx11::present] Private probe swap chain "
                "creation failed: 0x%08X",
                static_cast<unsigned int>(createResult));
            destroy_probe_window(probeWindow, instance);
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(swapChain);
        present_function_address = vtable[8];
        MH_STATUS hookResult = MH_CreateHook(
            present_function_address,
            &hooked_present,
            reinterpret_cast<void**>(&original_present)
        );
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::present] Present hook creation failed: %d",
                static_cast<int>(hookResult));
            context->Release();
            device->Release();
            swapChain->Release();
            destroy_probe_window(probeWindow, instance);
            return false;
        }
        hookResult = MH_EnableHook(present_function_address);
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::present] Present hook enable failed: %d",
                static_cast<int>(hookResult));
            MH_RemoveHook(present_function_address);
            context->Release();
            device->Release();
            swapChain->Release();
            destroy_probe_window(probeWindow, instance);
            return false;
        }

        resize_buffers_function_address = vtable[13];
        hookResult = MH_CreateHook(
            resize_buffers_function_address,
            &hooked_resize_buffers,
            reinterpret_cast<void**>(&original_resize_buffers)
        );
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::present] ResizeBuffers hook creation "
                "failed: %d",
                static_cast<int>(hookResult));
            MH_DisableHook(present_function_address);
            MH_RemoveHook(present_function_address);
            context->Release();
            device->Release();
            swapChain->Release();
            destroy_probe_window(probeWindow, instance);
            return false;
        }
        hookResult = MH_EnableHook(
            resize_buffers_function_address);
        if (hookResult != MH_OK)
        {
            scs_log(
                2,
                "[dx11::present] ResizeBuffers hook enable "
                "failed: %d",
                static_cast<int>(hookResult));
            MH_RemoveHook(resize_buffers_function_address);
            MH_DisableHook(present_function_address);
            MH_RemoveHook(present_function_address);
            context->Release();
            device->Release();
            swapChain->Release();
            destroy_probe_window(probeWindow, instance);
            return false;
        }


        // Have vtable, just get rid of fakes
        context->Release();
        device->Release();
        swapChain->Release();
        destroy_probe_window(probeWindow, instance);

        scs_log(
            0,
            "[dx11::present] Standalone DX11 hooks installed "
            "using private probe window.");
        return true;
	}

    void shutdown()
    {
        MH_DisableHook(present_function_address);
        MH_RemoveHook(present_function_address);

        MH_DisableHook(resize_buffers_function_address);
        MH_RemoveHook(resize_buffers_function_address);

        reset_lighting_sampler();
        if (device) device->Release();
        if (context) context->Release();
        if (mainRenderTargetView) mainRenderTargetView->Release();
    }

    void on_frame(std::function<void()> callback)
    {
        pending_callbacks.push_back(callback);
    }
}
