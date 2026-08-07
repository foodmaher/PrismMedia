#define NOMINMAX
#include "native_media.h"

#include "../scs_logging.h"
#include "../thread_scheduling.h"

#include <Windows.h>
#include <audioclient.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
using namespace scs_logging;

namespace {
    std::wstring utf8_to_wide(const std::string& value)
    {
        if (value.empty())
            return {};
        const int length = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
            static_cast<int>(value.size()), nullptr, 0);
        if (length <= 0)
            return std::wstring(value.begin(), value.end());
        std::wstring result(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.c_str(),
            static_cast<int>(value.size()), result.data(), length);
        return result;
    }

    std::wstring media_source_url(const std::string& value)
    {
        std::wstring wide = utf8_to_wide(value);
        if (value.find("://") != std::string::npos)
            return wide;

        wchar_t fullPath[MAX_PATH]{};
        if (GetFullPathNameW(
            wide.c_str(), static_cast<DWORD>(_countof(fullPath)),
            fullPath, nullptr) == 0)
            return wide;

        wchar_t url[2048]{};
        DWORD urlLength = static_cast<DWORD>(_countof(url));
        if (SUCCEEDED(UrlCreateFromPathW(fullPath, url, &urlLength, 0)))
            return url;
        return wide;
    }

    class MediaEngineNotify final : public IMFMediaEngineNotify
    {
    public:
        explicit MediaEngineNotify(
            std::atomic<bool>& can_play,
            std::atomic<bool>& failed)
            : m_canPlay(can_play), m_failed(failed)
        {
        }

        STDMETHODIMP QueryInterface(REFIID iid, void** value) override
        {
            if (!value)
                return E_POINTER;
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFMediaEngineNotify))
            {
                *value = static_cast<IMFMediaEngineNotify*>(this);
                AddRef();
                return S_OK;
            }
            *value = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return ++m_refCount;
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG result = --m_refCount;
            if (result == 0)
                delete this;
            return result;
        }

        STDMETHODIMP EventNotify(
            DWORD event, DWORD_PTR parameter1, DWORD) override
        {
            if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE)
                SetEvent(reinterpret_cast<HANDLE>(parameter1));
            else if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY ||
                event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA)
                m_canPlay = true;
            else if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
                m_failed = true;
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_refCount{ 1 };
        std::atomic<bool>& m_canPlay;
        std::atomic<bool>& m_failed;
    };
}

namespace sources {
    class NativeMediaSource final : public IContentSource
    {
    private:
        struct ReadbackSlot
        {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11Query> completion;
            bool pending{};
        };

        std::string m_mediaUrl;
        std::atomic<uint8_t> m_framerate{ 30 };
        std::atomic<uint32_t> m_outputWidth{ 1280 };
        std::atomic<uint32_t> m_outputHeight{ 720 };
        std::atomic<uint32_t> m_width{};
        std::atomic<uint32_t> m_height{};
        std::atomic<bool> m_paused{};
        std::atomic<bool> m_playbackPaused{};
        std::atomic<bool> m_vehiclePowered{ true };
        std::atomic<bool> m_stopRequested{};
        std::atomic<bool> m_canPlay{};
        std::atomic<bool> m_failed{};
        std::atomic<bool> m_initialized{};
        std::atomic<bool> m_startFinished{};

        std::thread m_thread;
        std::mutex m_startMutex;
        std::condition_variable m_startCondition;

        std::mutex m_bufferMutex;
        std::vector<uint8_t> m_frameBuffer;
        uint64_t m_frameGeneration{};
        uint64_t m_lastCopiedGeneration{};
        std::atomic<bool> m_haveFrame{};

        std::mutex m_commandMutex;
        std::deque<media_command_t> m_commands;

        std::atomic<double> m_workerCpuMs{};
        std::atomic<double> m_readbackMs{};
        std::atomic<double> m_deliveredFps{};
        std::atomic<uint64_t> m_droppedFrames{};
        std::chrono::steady_clock::time_point m_lastDelivered{};

        void signal_initialized(bool success)
        {
            m_initialized = success;
            m_startFinished = true;
            {
                std::lock_guard<std::mutex> lock(m_startMutex);
            }
            m_startCondition.notify_all();
        }

        void process_commands(IMFMediaEngine* engine)
        {
            std::deque<media_command_t> commands;
            {
                std::lock_guard<std::mutex> lock(m_commandMutex);
                commands.swap(m_commands);
            }

            for (const auto command : commands)
            {
                switch (command)
                {
                case media_command_t::PLAY_PAUSE:
                    m_playbackPaused = !m_playbackPaused.load();
                    break;
                case media_command_t::NEXT:
                    engine->SetCurrentTime((std::min)(
                        engine->GetDuration(), engine->GetCurrentTime() + 30.0));
                    break;
                case media_command_t::PREVIOUS:
                    engine->SetCurrentTime((std::max)(0.0, engine->GetCurrentTime() - 30.0));
                    break;
                case media_command_t::MUTE:
                    engine->SetMuted(!engine->GetMuted());
                    break;
                case media_command_t::VOLUME_UP:
                    engine->SetVolume((std::min)(1.0, engine->GetVolume() + 0.05));
                    break;
                case media_command_t::VOLUME_DOWN:
                    engine->SetVolume((std::max)(0.0, engine->GetVolume() - 0.05));
                    break;
                }
            }

            const bool shouldPause =
                m_paused.load() || m_playbackPaused.load() ||
                !m_vehiclePowered.load();
            if (shouldPause && !engine->IsPaused())
                engine->Pause();
            else if (!shouldPause && m_canPlay.load() && engine->IsPaused())
                engine->Play();
        }

        bool create_readback_resources(
            ID3D11Device* device,
            uint32_t width,
            uint32_t height,
            ComPtr<ID3D11Texture2D>& renderTarget,
            std::array<ReadbackSlot, 3>& slots)
        {
            D3D11_TEXTURE2D_DESC renderDesc{};
            renderDesc.Width = width;
            renderDesc.Height = height;
            renderDesc.MipLevels = 1;
            renderDesc.ArraySize = 1;
            renderDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            renderDesc.SampleDesc.Count = 1;
            renderDesc.Usage = D3D11_USAGE_DEFAULT;
            renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&renderDesc, nullptr, renderTarget.ReleaseAndGetAddressOf())))
                return false;

            D3D11_TEXTURE2D_DESC stagingDesc = renderDesc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& slot : slots)
            {
                if (FAILED(device->CreateTexture2D(
                    &stagingDesc, nullptr, slot.texture.ReleaseAndGetAddressOf())))
                    return false;

                D3D11_QUERY_DESC queryDesc{};
                queryDesc.Query = D3D11_QUERY_EVENT;
                if (FAILED(device->CreateQuery(
                    &queryDesc, slot.completion.ReleaseAndGetAddressOf())))
                    return false;
                slot.pending = false;
            }
            return true;
        }

        bool try_readback(
            ID3D11DeviceContext* context,
            std::array<ReadbackSlot, 3>& slots,
            uint32_t width,
            uint32_t height)
        {
            const auto readStarted = std::chrono::steady_clock::now();
            for (auto& slot : slots)
            {
                if (!slot.pending)
                    continue;

                BOOL complete{};
                if (context->GetData(
                    slot.completion.Get(), &complete, sizeof(complete),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !complete)
                    continue;

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (context->Map(
                    slot.texture.Get(), 0, D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
                    continue;

                {
                    std::lock_guard<std::mutex> lock(m_bufferMutex);
                    const size_t rowBytes = static_cast<size_t>(width) * 4;
                    m_frameBuffer.resize(rowBytes * height);
                    for (uint32_t y = 0; y < height; ++y)
                    {
                        memcpy(
                            m_frameBuffer.data() + static_cast<size_t>(y) * rowBytes,
                            static_cast<const uint8_t*>(mapped.pData) +
                                static_cast<size_t>(y) * mapped.RowPitch,
                            rowBytes);
                    }
                    m_width = width;
                    m_height = height;
                    ++m_frameGeneration;
                    m_haveFrame = true;
                }

                context->Unmap(slot.texture.Get(), 0);
                slot.pending = false;

                const auto deliveredAt = std::chrono::steady_clock::now();
                const double readbackMs = std::chrono::duration<double, std::milli>(
                    deliveredAt - readStarted).count();
                const double oldReadback = m_readbackMs.load();
                m_readbackMs = oldReadback == 0.0
                    ? readbackMs : oldReadback * 0.90 + readbackMs * 0.10;

                if (m_lastDelivered.time_since_epoch().count() != 0)
                {
                    const double interval = std::chrono::duration<double>(
                        deliveredAt - m_lastDelivered).count();
                    if (interval > 0.0)
                    {
                        const double fps = 1.0 / interval;
                        const double oldFps = m_deliveredFps.load();
                        m_deliveredFps = oldFps == 0.0
                            ? fps : oldFps * 0.90 + fps * 0.10;
                    }
                }
                m_lastDelivered = deliveredAt;
                return true;
            }
            return false;
        }

        void worker()
        {
            thread_scheduling::refresh_current_thread_preference();
            const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const bool uninitializeCom = SUCCEEDED(comResult);
            if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
            {
                signal_initialized(false);
                return;
            }

            if (FAILED(MFStartup(MF_VERSION)))
            {
                signal_initialized(false);
                if (uninitializeCom) CoUninitialize();
                return;
            }

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            D3D_FEATURE_LEVEL featureLevel{};
            const HRESULT deviceResult = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION,
                device.GetAddressOf(), &featureLevel, context.GetAddressOf());
            if (FAILED(deviceResult))
            {
                scs_log(2, "[NativeMedia] D3D11 device creation failed: 0x%08X", deviceResult);
                signal_initialized(false);
                MFShutdown();
                if (uninitializeCom) CoUninitialize();
                return;
            }
            ComPtr<ID3D10Multithread> multithread;
            if (SUCCEEDED(device.As(&multithread)))
                multithread->SetMultithreadProtected(TRUE);

            UINT managerToken{};
            ComPtr<IMFDXGIDeviceManager> deviceManager;
            ComPtr<IMFAttributes> attributes;
            ComPtr<IMFMediaEngineClassFactory> factory;
            ComPtr<IMFMediaEngine> engine;
            ComPtr<IMFMediaEngineNotify> notify;

            HRESULT result = MFCreateDXGIDeviceManager(
                &managerToken, deviceManager.GetAddressOf());
            if (SUCCEEDED(result))
                result = deviceManager->ResetDevice(device.Get(), managerToken);
            if (SUCCEEDED(result))
                result = MFCreateAttributes(attributes.GetAddressOf(), 5);
            if (SUCCEEDED(result))
            {
                notify.Attach(new MediaEngineNotify(m_canPlay, m_failed));
                result = attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
            }
            if (SUCCEEDED(result))
                result = attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, deviceManager.Get());
            if (SUCCEEDED(result))
                result = attributes->SetUINT32(
                    MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                    static_cast<UINT32>(DXGI_FORMAT_B8G8R8A8_UNORM));
            if (SUCCEEDED(result))
                result = attributes->SetUINT32(
                    MF_MEDIA_ENGINE_AUDIO_CATEGORY,
                    static_cast<UINT32>(AudioCategory_Media));
            if (SUCCEEDED(result))
            {
                result = CoCreateInstance(
                    CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(factory.GetAddressOf()));
            }
            if (SUCCEEDED(result))
                result = factory->CreateInstance(0, attributes.Get(), engine.GetAddressOf());
            if (SUCCEEDED(result))
            {
                // Do not depend on a Media Foundation implementation's prior
                // or implicit audio state. A newly-created native source should
                // always begin audible; the in-game Mute/Volume controls can
                // then change it intentionally.
                engine->SetMuted(FALSE);
                engine->SetVolume(1.0);
            }
            if (FAILED(result))
            {
                scs_log(2, "[NativeMedia] Media Engine creation failed: 0x%08X", result);
                signal_initialized(false);
                MFShutdown();
                if (uninitializeCom) CoUninitialize();
                return;
            }

            const std::wstring wideUrl = media_source_url(m_mediaUrl);
            BSTR source = SysAllocStringLen(
                wideUrl.data(), static_cast<UINT>(wideUrl.size()));
            result = source ? engine->SetSource(source) : E_OUTOFMEMORY;
            if (source) SysFreeString(source);
            if (FAILED(result))
            {
                scs_log(2, "[NativeMedia] SetSource failed: 0x%08X", result);
                signal_initialized(false);
                MFShutdown();
                if (uninitializeCom) CoUninitialize();
                return;
            }
            engine->SetAutoPlay(TRUE);
            engine->Load();
            signal_initialized(true);

            ComPtr<ID3D11Texture2D> renderTarget;
            std::array<ReadbackSlot, 3> readbackSlots;
            uint32_t textureWidth{};
            uint32_t textureHeight{};
            size_t nextSlot{};
            auto lastFrame = std::chrono::steady_clock::now();

            while (!m_stopRequested.load())
            {
                thread_scheduling::refresh_current_thread_preference();
                const auto workStarted = std::chrono::steady_clock::now();
                process_commands(engine.Get());

                if (m_failed.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                const auto fps = (std::max)(static_cast<uint8_t>(1), m_framerate.load());
                const auto interval = std::chrono::microseconds(1000000 / fps);
                if (workStarted - lastFrame < interval)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                lastFrame = workStarted;

                const uint32_t requestedWidth = (std::max)(1U, m_outputWidth.load());
                const uint32_t requestedHeight = (std::max)(1U, m_outputHeight.load());
                if (!renderTarget ||
                    textureWidth != requestedWidth || textureHeight != requestedHeight)
                {
                    renderTarget.Reset();
                    for (auto& slot : readbackSlots)
                    {
                        slot.texture.Reset();
                        slot.completion.Reset();
                        slot.pending = false;
                    }
                    if (!create_readback_resources(
                        device.Get(), requestedWidth, requestedHeight,
                        renderTarget, readbackSlots))
                    {
                        m_failed = true;
                        continue;
                    }
                    textureWidth = requestedWidth;
                    textureHeight = requestedHeight;
                    nextSlot = 0;
                }

                try_readback(
                    context.Get(), readbackSlots, textureWidth, textureHeight);

                LONGLONG presentationTime{};
                if (!m_paused.load() && !m_playbackPaused.load() &&
                    m_vehiclePowered.load() &&
                    engine->OnVideoStreamTick(&presentationTime) == S_OK)
                {
                    MFVideoNormalizedRect sourceRect{ 0.0f, 0.0f, 1.0f, 1.0f };
                    RECT destination{
                        0, 0,
                        static_cast<LONG>(textureWidth),
                        static_cast<LONG>(textureHeight)
                    };
                    MFARGB background{ 0, 0, 0, 255 };
                    if (SUCCEEDED(engine->TransferVideoFrame(
                        renderTarget.Get(), &sourceRect, &destination, &background)))
                    {
                        bool queued = false;
                        for (size_t attempt = 0; attempt < readbackSlots.size(); ++attempt)
                        {
                            const size_t index =
                                (nextSlot + attempt) % readbackSlots.size();
                            auto& slot = readbackSlots[index];
                            if (slot.pending)
                                continue;
                            context->CopyResource(slot.texture.Get(), renderTarget.Get());
                            context->End(slot.completion.Get());
                            slot.pending = true;
                            nextSlot = (index + 1) % readbackSlots.size();
                            queued = true;
                            break;
                        }
                        if (!queued)
                            ++m_droppedFrames;
                    }
                }

                const double workerMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - workStarted).count();
                const double oldWorker = m_workerCpuMs.load();
                m_workerCpuMs = oldWorker == 0.0
                    ? workerMs : oldWorker * 0.90 + workerMs * 0.10;
            }

            engine->Pause();
            engine->Shutdown();
            engine.Reset();
            factory.Reset();
            attributes.Reset();
            deviceManager.Reset();
            notify.Reset();
            MFShutdown();
            if (uninitializeCom)
                CoUninitialize();
        }

    public:
        NativeMediaSource(
            std::string mediaUrl,
            uint8_t framerate,
            uint32_t outputWidth,
            uint32_t outputHeight)
            : m_mediaUrl(std::move(mediaUrl))
        {
            SetFramerate(framerate);
            SetOutputSize(outputWidth, outputHeight);
        }

        ~NativeMediaSource() override
        {
            m_stopRequested = true;
            if (m_thread.joinable())
                m_thread.join();
        }

        bool Start()
        {
            if (m_mediaUrl.empty())
                return false;
            m_thread = std::thread(&NativeMediaSource::worker, this);

            std::unique_lock<std::mutex> lock(m_startMutex);
            m_startCondition.wait_for(
                lock, std::chrono::seconds(5),
                [this]() { return m_startFinished.load(); });
            return m_initialized.load();
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

        bool CopyLatestFrame(
            std::vector<uint8_t>& destination,
            uint32_t& width,
            uint32_t& height) override
        {
            if (!m_haveFrame.load())
                return false;
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (m_lastCopiedGeneration == m_frameGeneration)
                return false;
            destination.swap(m_frameBuffer);
            width = m_width.load();
            height = m_height.load();
            m_lastCopiedGeneration = m_frameGeneration;
            return true;
        }

        bool SupportsMediaControls() const override { return true; }
        bool SupportsVehiclePowerControl() const override { return true; }
        void SetVehiclePowered(bool powered) override
        {
            m_vehiclePowered = powered;
        }
        bool SendMediaCommand(media_command_t command) override
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            m_commands.push_back(command);
            return true;
        }

        source_performance_stats_t GetPerformanceStats() const override
        {
            source_performance_stats_t result;
            result.workerCpuMs = m_workerCpuMs.load();
            result.readbackMs = m_readbackMs.load();
            result.deliveredFps = m_deliveredFps.load();
            result.droppedFrames = m_droppedFrames.load();
            result.hardwareDecoded = true;
            result.directMedia = true;
            return result;
        }
        std::string GetStatusText() const override
        {
            if (m_failed.load())
                return "Media error: unsupported URL/codec or network failure";
            if (!m_canPlay.load())
                return "Loading native media...";
            return m_playbackPaused.load() || m_paused.load() ||
                !m_vehiclePowered.load()
                ? "Native media paused" : "Native media playing";
        }
    };

    std::unique_ptr<IContentSource> CreateNativeMediaSource(
        const std::string& media_url,
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height)
    {
        auto source = std::make_unique<NativeMediaSource>(
            media_url, framerate, output_width, output_height);
        if (!source->Start())
            return nullptr;
        return source;
    }
}
