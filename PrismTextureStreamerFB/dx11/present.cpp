#include "dx11.h"
#include "internal_render_probe.h"
#include "../thread_scheduling.h"
#include "../win32/win32.h"
#include <MinHook/MinHook.h>
#include "../scs_logging.h"
using namespace scs_logging;

#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_dx11.h>
#include <ImGui/imgui_impl_win32.h>

#include <vector>
#include <functional>

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

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
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


        if (device) device->Release();
        if (context) context->Release();
        if (mainRenderTargetView) mainRenderTargetView->Release();
    }

    void on_frame(std::function<void()> callback)
    {
        pending_callbacks.push_back(callback);
    }
}
