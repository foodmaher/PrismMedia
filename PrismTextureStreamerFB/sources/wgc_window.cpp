#define _CRT_SECURE_NO_WARNINGS // Thanks microsoft

#include "wgc_window.h"

#include "../scs_logging.h"
using namespace scs_logging;

#include "../dx11/dx11.h"

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
using namespace winrt::Windows;

#include "wgc_dispatcher.h"

#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <array>
#include <chrono>

#pragma comment(lib, "dxgi.lib")

struct FindWindowData {
    const char* exeName;
    const char* windowTitle;
    HWND result;
};
static HWND FindWindowByNameAndTitle(const char* exeName, const char* windowTitle)
{
    FindWindowData data{ exeName, windowTitle, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* data = reinterpret_cast<FindWindowData*>(lParam);

        if (!IsWindowVisible(hwnd))
            return TRUE;

        LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
        if (exStyle & WS_EX_TOOLWINDOW)
            return TRUE;

        bool titleMatch{};
        if (data->windowTitle) {
            LRESULT lengthResult = 0;
            if (!SendMessageTimeoutA(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, (PDWORD_PTR)&lengthResult)) {
                return TRUE; // didn't respond in time - skip it
            }
            int titleLen = static_cast<int>(lengthResult);

            if (titleLen != 0)
            {
                std::string windowTitle = std::string(titleLen + 1, '\0');
                GetWindowTextA(hwnd, windowTitle.data(), titleLen + 1);
                windowTitle.resize(titleLen);

                if (windowTitle == std::string_view(data->windowTitle))
                    titleMatch = true; // Title matches, but so must the exe name
            }
            else
                return TRUE; // Window title was given as a search filter, so if no title, skip
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return TRUE;

        char path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameA(hProc, 0, path, &size);
        CloseHandle(hProc);

        std::string_view applicationName(path);
        auto pos = applicationName.rfind('\\');
        applicationName = pos != std::string::npos ? applicationName.substr(pos + 1) : applicationName;

        bool exeMatch = applicationName == std::string_view(data->exeName);

        if (exeMatch && (!data->windowTitle || titleMatch)) {
            data->result = hwnd;
            return FALSE; // Exe matches, and there is either no title, or the title matched
        }

        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));

    return data.result;
}

static Graphics::Capture::GraphicsCaptureItem CreateCaptureItemForWindow(HWND hwnd)
{
    auto interopFactory = winrt::get_activation_factory<Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();

    Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interopFactory->CreateForWindow(
        hwnd,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        winrt::put_abi(item))
    );
    return item;
}

static Graphics::DirectX::Direct3D11::IDirect3DDevice CreateD3DDeviceForWgc(ID3D11Device* d3dDevice)
{
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

winrt::com_ptr<IDXGIAdapter> GetAdapterForWindow(HWND hwnd)
{
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())));

    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i, adapter = nullptr)
    {
        winrt::com_ptr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j, output = nullptr)
        {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor == monitor)
                return adapter.as<IDXGIAdapter>();
        }
    }

    return nullptr; // caller falls back to default adapter
}

winrt::com_ptr<ID3D11Device> CreateWgcCaptureDevice(HWND hwnd)
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;

    D3D_FEATURE_LEVEL featureLevel;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    auto adapter = GetAdapterForWindow(hwnd);
    D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    HRESULT hr = D3D11CreateDevice(
        adapter.get(),
        driverType,
        nullptr,
        flags,
        nullptr, 0,
        D3D11_SDK_VERSION,
        device.put(),
        &featureLevel,
        context.put());

    winrt::check_hresult(hr);
    return device;
}

namespace sources {
    class WgcWindowSource : public IContentSource
    {
    private:
        char* m_appname{};
        char* m_apptitle{};
        HWND m_hwnd{};
        std::atomic<uint32_t> m_width{};
        std::atomic<uint32_t> m_height{};
        std::atomic<uint8_t> m_framerate{ 30 };
        std::atomic<bool> m_paused{};
        std::atomic<uint32_t> m_outputWidth{ 1280 };
        std::atomic<uint32_t> m_outputHeight{ 720 };

        winrt::com_ptr < ID3D11Device> m_d3dDevice{};
        Graphics::DirectX::Direct3D11::IDirect3DDevice m_wgcDevice{ nullptr };

        Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
        Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
        Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;

        Graphics::SizeInt32 m_lastSize{};

        std::vector<uint8_t> m_frameBuffer;
        std::mutex m_bufferMutex;
        std::mutex m_frameMutex; // Stops the frame arrived running for things like destruction

        std::atomic<bool> m_haveFrame{};
        std::atomic<bool> m_stopping{};
        uint64_t m_frameGeneration{};
        uint64_t m_lastCopiedGeneration{};
        std::chrono::steady_clock::time_point m_lastCapture{};
        std::chrono::steady_clock::time_point m_lastDelivered{};
        std::atomic<double> m_workerCpuMs{};
        std::atomic<double> m_readbackMs{};
        std::atomic<double> m_deliveredFps{};
        std::atomic<uint64_t> m_droppedFrames{};

        struct StagingSlot
        {
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::com_ptr<ID3D11Query> completion;
            bool pending{};
        };
        std::array<StagingSlot, 3> m_stagingSlots;
        UINT m_stagingWidth{};
        UINT m_stagingHeight{};
        size_t m_nextStagingSlot{};
        std::vector<uint32_t> m_readbackScaleX;
        uint32_t m_readbackSourceWidth{};
        uint32_t m_readbackOutputWidth{};

        void RecreateStagingResources(const D3D11_TEXTURE2D_DESC& textureDesc)
        {
            for (auto& slot : m_stagingSlots)
            {
                slot.texture = nullptr;
                slot.completion = nullptr;
                slot.pending = false;
                winrt::check_hresult(m_d3dDevice->CreateTexture2D(
                    &textureDesc, nullptr, slot.texture.put()));

                D3D11_QUERY_DESC queryDesc{};
                queryDesc.Query = D3D11_QUERY_EVENT;
                winrt::check_hresult(m_d3dDevice->CreateQuery(
                    &queryDesc, slot.completion.put()));
            }
            m_stagingWidth = textureDesc.Width;
            m_stagingHeight = textureDesc.Height;
            m_nextStagingSlot = 0;
        }

        bool TryReadCompletedFrame(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& desc)
        {
            const auto readbackStarted = std::chrono::steady_clock::now();
            for (auto& slot : m_stagingSlots)
            {
                if (!slot.pending)
                    continue;

                BOOL complete{};
                const HRESULT queryResult = ctx->GetData(
                    slot.completion.get(), &complete, sizeof(complete),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (queryResult != S_OK || !complete)
                    continue;

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (ctx->Map(slot.texture.get(), 0, D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
                    continue;

                uint32_t outputWidth = desc.Width;
                uint32_t outputHeight = desc.Height;
                const uint32_t maxOutputWidth = (std::max)(1U, m_outputWidth.load());
                const uint32_t maxOutputHeight = (std::max)(1U, m_outputHeight.load());
                if (outputWidth > maxOutputWidth || outputHeight > maxOutputHeight)
                {
                    if (static_cast<uint64_t>(desc.Width) * maxOutputHeight >
                        static_cast<uint64_t>(desc.Height) * maxOutputWidth)
                    {
                        outputWidth = maxOutputWidth;
                        outputHeight = (std::max)(1U, static_cast<uint32_t>(
                            static_cast<uint64_t>(desc.Height) * outputWidth / desc.Width));
                    }
                    else
                    {
                        outputHeight = maxOutputHeight;
                        outputWidth = (std::max)(1U, static_cast<uint32_t>(
                            static_cast<uint64_t>(desc.Width) * outputHeight / desc.Height));
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    m_frameBuffer.resize(
                        static_cast<size_t>(outputWidth) * outputHeight * 4);
                    if (outputWidth == desc.Width && outputHeight == desc.Height)
                    {
                        const size_t rowBytes = static_cast<size_t>(outputWidth) * 4;
                        for (uint32_t y = 0; y < outputHeight; ++y)
                        {
                            const uint8_t* sourceRow =
                                static_cast<uint8_t*>(mapped.pData) +
                                static_cast<size_t>(y) * mapped.RowPitch;
                            memcpy(m_frameBuffer.data() + static_cast<size_t>(y) * rowBytes,
                                sourceRow, rowBytes);
                        }
                    }
                    else
                    {
                        if (m_readbackSourceWidth != desc.Width ||
                            m_readbackOutputWidth != outputWidth)
                        {
                            m_readbackScaleX.resize(outputWidth);
                            for (uint32_t x = 0; x < outputWidth; ++x)
                            {
                                m_readbackScaleX[x] = static_cast<uint32_t>(
                                    static_cast<uint64_t>(x) * desc.Width / outputWidth);
                            }
                            m_readbackSourceWidth = desc.Width;
                            m_readbackOutputWidth = outputWidth;
                        }

                        auto* destinationPixels =
                            reinterpret_cast<uint32_t*>(m_frameBuffer.data());
                        for (uint32_t y = 0; y < outputHeight; ++y)
                        {
                            const uint32_t sourceY = static_cast<uint32_t>(
                                static_cast<uint64_t>(y) * desc.Height / outputHeight);
                            const uint8_t* srcRow =
                                static_cast<uint8_t*>(mapped.pData) +
                                static_cast<size_t>(sourceY) * mapped.RowPitch;
                            const auto* sourcePixels =
                                reinterpret_cast<const uint32_t*>(srcRow);

                            for (uint32_t x = 0; x < outputWidth; ++x)
                            {
                                destinationPixels[static_cast<size_t>(y) * outputWidth + x] =
                                    sourcePixels[m_readbackScaleX[x]];
                            }
                        }
                    }
                    m_width = outputWidth;
                    m_height = outputHeight;
                    ++m_frameGeneration;
                    m_haveFrame = true;
                }

                ctx->Unmap(slot.texture.get(), 0);
                slot.pending = false;

                const auto deliveredAt = std::chrono::steady_clock::now();
                const double readbackMs = std::chrono::duration<double, std::milli>(
                    deliveredAt - readbackStarted).count();
                const double oldReadback = m_readbackMs.load();
                m_readbackMs = oldReadback == 0.0
                    ? readbackMs : oldReadback * 0.90 + readbackMs * 0.10;
                if (m_lastDelivered.time_since_epoch().count() != 0)
                {
                    const double interval = std::chrono::duration<double>(
                        deliveredAt - m_lastDelivered).count();
                    if (interval > 0.0)
                    {
                        const double instantaneousFps = 1.0 / interval;
                        const double oldFps = m_deliveredFps.load();
                        m_deliveredFps = oldFps == 0.0
                            ? instantaneousFps : oldFps * 0.90 + instantaneousFps * 0.10;
                    }
                }
                m_lastDelivered = deliveredAt;
                return true;
            }
            return false;
        }


        void OnFrameArrived(Graphics::Capture::Direct3D11CaptureFramePool const& sender, Foundation::IInspectable const&)
        {
            const auto workerStarted = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lockFrame(m_frameMutex);
            if (m_stopping.load())
                return;

            try {
                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                if (m_paused.load() && m_haveFrame.load()) return;

                auto contentSize = frame.ContentSize();
                if (contentSize.Width <= 0 || contentSize.Height <= 0)
                    return;

                if (contentSize.Width != m_lastSize.Width || contentSize.Height != m_lastSize.Height)
                {
                    m_lastSize = contentSize;
                    m_framePool.Recreate(m_wgcDevice, Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, contentSize);
                    return; // next frame arrives at the correct size
                }

                const auto now = std::chrono::steady_clock::now();
                const auto fps = (std::max)(static_cast<uint8_t>(1), m_framerate.load());
                const auto interval = std::chrono::microseconds(1000000 / fps);
                if (m_lastCapture.time_since_epoch().count() != 0 && now - m_lastCapture < interval)
                    return;
                m_lastCapture = now;

                auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> gpuTexture;
                access->GetInterface(IID_PPV_ARGS(gpuTexture.put()));

                D3D11_TEXTURE2D_DESC desc;
                gpuTexture->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING;
                desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.MiscFlags = 0;

                winrt::com_ptr<ID3D11DeviceContext> ctx;
                m_d3dDevice->GetImmediateContext(ctx.put());

                if (!m_stagingSlots[0].texture ||
                    m_stagingWidth != desc.Width || m_stagingHeight != desc.Height)
                {
                    RecreateStagingResources(desc);
                }

                // Read a completed older copy without making the CPU wait for
                // the GPU, then queue the newest capture into a free slot.
                TryReadCompletedFrame(ctx.get(), desc);

                bool queued = false;
                for (size_t attempt = 0; attempt < m_stagingSlots.size(); ++attempt)
                {
                    const size_t index = (m_nextStagingSlot + attempt) % m_stagingSlots.size();
                    auto& slot = m_stagingSlots[index];
                    if (slot.pending)
                        continue;

                    ctx->CopyResource(slot.texture.get(), gpuTexture.get());
                    ctx->End(slot.completion.get());
                    slot.pending = true;
                    m_nextStagingSlot = (index + 1) % m_stagingSlots.size();
                    queued = true;
                    break;
                }
                if (!queued)
                    ++m_droppedFrames;

                const double workerMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - workerStarted).count();
                const double oldWorker = m_workerCpuMs.load();
                m_workerCpuMs = oldWorker == 0.0
                    ? workerMs : oldWorker * 0.90 + workerMs * 0.10;
            }
            catch (const winrt::hresult_error& e) {
                scs_log(2, "[WgcWindowSource] OnFrameArrived failed: 0x%08X", e.code().value);
            }
        }

    public:
        explicit WgcWindowSource(
            const char* application_name,
            const char* application_title,
            uint8_t framerate,
            uint32_t output_width,
            uint32_t output_height)
        {
            m_framerate = (std::max)(static_cast<uint8_t>(1), framerate);
            SetOutputSize(output_width, output_height);
            // Create our own ownership
            m_appname = new char[strlen(application_name) + 1] {};
            strcpy(m_appname, application_name);

            // title is optional
            if (application_title) {
                m_apptitle = new char[strlen(application_title) + 1] {};
                strcpy(m_apptitle, application_title);
            }
        }
        ~WgcWindowSource() override
        {
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_stopping = true;
            }

            // Uses post result so it blocks, we dont want to deconstruct before this is done
            WgcDispatcher::Instance().PostResult([this]() {
                m_frameArrivedRevoker.revoke();
                if (m_session) m_session.Close();
                if (m_framePool) m_framePool.Close();
            });


            scs_log(0, "[WgcWindowSource] Source for %s (%s) has stopped", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");
            if (m_appname) delete[] m_appname;
            if (m_apptitle) delete[] m_apptitle;
        }

        bool Start()
        {
            try {
                m_hwnd = FindWindowByNameAndTitle(m_appname, m_apptitle);
                if (!m_hwnd) { scs_log(2, "[WgcWindowSource] Application %s (%s) not found at source startup", m_appname, m_apptitle ? m_apptitle : "NO_TITLE"); return false; }

                m_d3dDevice = CreateWgcCaptureDevice(m_hwnd);

                m_wgcDevice = CreateD3DDeviceForWgc(m_d3dDevice.get());
                m_item = CreateCaptureItemForWindow(m_hwnd);

                m_lastSize = m_item.Size();
                if (m_lastSize.Width == 0 || m_lastSize.Height == 0) {
                    scs_log(2, "[WgcWindowSource] target window has zero size, deferring start");
                    return false;
                }

                m_framePool = Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    m_wgcDevice,
                    Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    m_lastSize
                );

                m_session = m_framePool.CreateCaptureSession(m_item);
                m_frameArrivedRevoker = m_framePool.FrameArrived(winrt::auto_revoke, { this, &WgcWindowSource::OnFrameArrived });

                m_session.StartCapture();
                //m_session.IsBorderRequired(false); // Disable the windows orange border from capturing

                scs_log(0, "[WgcWindowSource] Source for %s (%s) has started", m_appname, m_apptitle ? m_apptitle : "NO_TITLE");

                return true;
            }
            catch (const winrt::hresult_error& e)
            {
                scs_log(2, "[WgcWindowSource] Start failed: 0x%08X %ls", e.code().value, e.message().c_str());
                return false;
            }
        }

        uint32_t GetWidth() const override { return m_width.load(); }
        uint32_t GetHeight() const override { return m_height.load(); }
        void SetFramerate(uint8_t framerate) override
        {
            m_framerate = (std::max)(static_cast<uint8_t>(1), framerate);
        }
        void SetPaused(bool paused) override { m_paused = paused; }
        void SetOutputSize(uint32_t width, uint32_t height) override
        {
            m_outputWidth = (std::max)(1U, width);
            m_outputHeight = (std::max)(1U, height);
        }

        bool CopyLatestFrame(std::vector<uint8_t>& dst, uint32_t& width, uint32_t& height) override
        {
            if (!m_haveFrame.load()) return false;

            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (m_lastCopiedGeneration == m_frameGeneration)
                return false;
            dst.swap(m_frameBuffer);
            width = m_width.load();
            height = m_height.load();
            m_lastCopiedGeneration = m_frameGeneration;
            return true;
        }

        source_performance_stats_t GetPerformanceStats() const override
        {
            source_performance_stats_t result;
            result.workerCpuMs = m_workerCpuMs.load();
            result.readbackMs = m_readbackMs.load();
            result.deliveredFps = m_deliveredFps.load();
            result.droppedFrames = m_droppedFrames.load();
            return result;
        }
    };


    std::unique_ptr<IContentSource> CreateWgcWindowSource(
        const char* application_name,
        const char* window_title,
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height)
    {
        std::string appname(application_name);
        std::string apptitle = window_title ? window_title : std::string();

        return WgcDispatcher::Instance().PostResult(
            [appname, apptitle, framerate, output_width, output_height]() -> std::unique_ptr<IContentSource> {
            auto src = std::make_unique<WgcWindowSource>(
                appname.c_str(),
                apptitle.empty() ? nullptr : apptitle.c_str(),
                framerate,
                output_width,
                output_height);
            if (!src->Start()) return nullptr;
            return src;
        });
    }
}
