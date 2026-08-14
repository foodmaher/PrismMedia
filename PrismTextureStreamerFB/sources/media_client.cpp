#define NOMINMAX
#include "media_client.h"

#include "wgc_window.h"
#include "../diagnostic_log.h"
#include "../scs_logging.h"
#include "../thread_scheduling.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

using namespace scs_logging;

namespace {
    constexpr ULONG_PTR kPrismCopyDataId = 0x50524953;
    constexpr UINT kPrismEnvironmentMessage = WM_APP + 0x351;

    std::string module_directory()
    {
        static int moduleAnchor{};
        HMODULE module{};
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&moduleAnchor),
            &module);

        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        std::string result(path);
        const auto separator = result.find_last_of("\\/");
        if (separator != std::string::npos)
            result.resize(separator + 1);
        else
            result.clear();
        return result;
    }

    HWND find_media_client()
    {
        return FindWindowA(nullptr, sources::kMediaClientWindowTitle);
    }

    std::string media_client_executable()
    {
        const std::string root = module_directory();
        const std::string organized =
            root + sources::kMediaClientFolder +
            sources::kMediaClientExecutable;
        if (GetFileAttributesA(organized.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
            return organized;

        // Keep older flat installations working during the transition to the
        // organized runtime folder.
        return root + sources::kMediaClientExecutable;
    }

    std::string path_directory(const std::string& path)
    {
        const auto separator = path.find_last_of("\\/");
        return separator == std::string::npos
            ? module_directory()
            : path.substr(0, separator + 1);
    }

    bool send_payload(const std::string& payload, DWORD timeout_ms = 1000)
    {
        const HWND window = find_media_client();
        if (!window)
            return false;

        COPYDATASTRUCT data{};
        data.dwData = kPrismCopyDataId;
        data.cbData = static_cast<DWORD>(payload.size() + 1);
        data.lpData = const_cast<char*>(payload.c_str());
        DWORD_PTR ignored{};
        return SendMessageTimeoutA(
            window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout_ms, &ignored) != 0;
    }

    bool launch_media_client()
    {
        if (find_media_client())
        {
            diagnostic_log::write(
                "media", "Reusing the existing PrismMediaClient helper.");
            const bool parentSent = send_payload(
                "parent|" + std::to_string(GetCurrentProcessId()));
            send_payload("initialize", 100);
            return parentSent;
        }

        const std::string executable = media_client_executable();
        if (GetFileAttributesA(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            diagnostic_log::writef(
                "error", "PrismMediaClient is missing: %s",
                executable.c_str());
            scs_log(2, "[MediaClient] Missing helper: %s", executable.c_str());
            return false;
        }

        std::string commandLine =
            "\"" + executable + "\" --silent --parent-pid " +
            std::to_string(GetCurrentProcessId());
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessA(
            executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
            nullptr, path_directory(executable).c_str(),
            &startup, &process))
        {
            const DWORD error = GetLastError();
            diagnostic_log::writef(
                "error", "PrismMediaClient launch failed (Win32 error %lu).",
                error);
            scs_log(2, "[MediaClient] Launch failed, err=%lu", error);
            return false;
        }
		// PrismMediaClient and the WebView2 processes it creates are separate
		// from ETS2/ATS. A below-normal class lets the game win CPU contention;
		// the ideal-processor hint is soft and never restricts game affinity.
		SetPriorityClass(process.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
		thread_scheduling::apply_thread_preference(process.hThread, 2);
        diagnostic_log::writef(
            "media",
            "PrismMediaClient launched (pid=%lu, priority=BelowNormal).",
            process.dwProcessId);

        for (int attempt = 0; attempt < 160; ++attempt)
        {
            // Keep the primary-thread handle only during startup so a CPU
            // preference learned after the first Presents can still be
            // applied without retaining a process handle for the session.
            if ((attempt % 40) == 0)
                thread_scheduling::apply_thread_preference(
                    process.hThread, 2);
            if (find_media_client())
            {
                send_payload(
                    "parent|" +
                    std::to_string(GetCurrentProcessId()));
                diagnostic_log::write(
                    "media", "PrismMediaClient window is ready.");
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        scs_log(2, "[MediaClient] Timed out waiting for helper window");
        diagnostic_log::write(
            "error", "Timed out waiting for PrismMediaClient window.");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }

    class MediaClientSource final : public IContentSource
    {
    public:
        explicit MediaClientSource(
            std::unique_ptr<IContentSource> capture,
            bool fullSpotifyWeb)
            : m_capture(std::move(capture)),
              m_fullSpotifyWeb(fullSpotifyWeb)
        {
        }
        ~MediaClientSource() override
        {
            if (m_spatialEnabled)
                send_payload(
                    "spatial|0|1.0000|0.0000|20000.0", 30);
            if (!m_vehiclePowered.load())
                send_payload("vehiclepower|1", 30);
        }

        uint32_t GetWidth() const override { return m_capture->GetWidth(); }
        uint32_t GetHeight() const override { return m_capture->GetHeight(); }
        void SetFramerate(uint8_t framerate) override
        {
            m_capture->SetFramerate(framerate);
        }
        void SetPaused(bool paused) override
        {
            m_userCapturePaused = paused;
            m_capture->SetPaused(
                m_userCapturePaused.load() ||
                !m_vehiclePowered.load());
        }
        void SetOutputSize(uint32_t width, uint32_t height) override
        {
            m_capture->SetOutputSize(width, height);
            send_payload(
                "resize|" + std::to_string(width) + "x" +
                std::to_string(height));
        }
        bool CopyLatestFrame(
            std::vector<uint8_t>& destination,
            uint32_t& width,
            uint32_t& height) override
        {
            refresh_helper_cpu_hints();
            const bool copied =
                m_capture->CopyLatestFrame(destination, width, height);
            if (!m_brightnessRefreshPending.load())
                return copied;

            // WM_COPYDATA schedules the WebView update on its UI thread. Keep
            // capture awake briefly and discard any older queued frame so a
            // paused GPS receives the newly filtered image.
            if (GetTickCount64() <
                m_brightnessRefreshRequestedTick.load() + 300)
                return false;

            if (copied)
            {
                m_brightnessRefreshPending = false;
                m_capture->SetPaused(
                    m_userCapturePaused.load() ||
                    !m_vehiclePowered.load());
            }
            return copied;
        }

        bool SupportsMediaControls() const override { return true; }
        bool SupportsVehiclePowerControl() const override { return true; }
        void SetVehiclePowered(bool powered) override
        {
            if (powered == m_vehiclePowered.load())
                return;

            if (send_payload(
                powered ? "vehiclepower|1" : "vehiclepower|0", 30))
            {
                m_vehiclePowered = powered;
                m_capture->SetPaused(
                    m_userCapturePaused.load() ||
                    !m_vehiclePowered.load());
            }
        }
        bool SupportsSpatialAudio() const override { return true; }
        bool LoadMedia(const std::string& url) override
        {
            const bool sent = send_payload(
                (m_fullSpotifyWeb ? "loadspotifyweb|" : "load|") + url);
            diagnostic_log::writef(
                "media", "%s media load request: %s",
                m_fullSpotifyWeb ? "Spotify Web" : "YouTube/direct",
                sent ? "delivered" : "failed");
            return sent;
        }
        bool ShowInteractivePlayer(bool show) override
        {
            return send_payload(show ? "spotifylogin" : "spotifyhide");
        }
        bool ClearBrowserSession() override
        {
            return send_payload("clearspotify");
        }
        bool SendMediaCommand(media_command_t command) override
        {
            const char* name = nullptr;
            switch (command)
            {
            case media_command_t::PLAY_PAUSE: name = "playpause"; break;
            case media_command_t::NEXT: name = "next"; break;
            case media_command_t::PREVIOUS: name = "previous"; break;
            case media_command_t::MUTE: name = "mute"; break;
            case media_command_t::VOLUME_UP: name = "volumeup"; break;
            case media_command_t::VOLUME_DOWN: name = "volumedown"; break;
            }
            const bool sent = name && send_payload(name);
            if (name)
            {
                diagnostic_log::writef(
                    "media", "Command %s: %s", name,
                    sent ? "delivered" : "failed");
            }
            return sent;
        }
        void SetSpatialAudio(
            float gain,
            float pan,
            bool enabled,
            float lowpassHz) override
        {
            gain = (std::clamp)(gain, 0.0f, 1.0f);
            pan = (std::clamp)(pan, -1.0f, 1.0f);
            lowpassHz = (std::clamp)(lowpassHz, 20.0f, 20000.0f);

            const uint64_t now = GetTickCount64();
            const bool stateChanged = enabled != m_spatialEnabled;
            const bool valueChanged =
                std::fabs(gain - m_lastSpatialGain) >= 0.01f ||
                std::fabs(pan - m_lastSpatialPan) >= 0.01f ||
                std::fabs(lowpassHz - m_lastLowpassHz) >= 35.0f;
            if (!stateChanged && !valueChanged)
                return;
            if (!stateChanged && now - m_lastSpatialSendTick < 45)
                return;

            char payload[128]{};
            std::snprintf(
                payload, sizeof(payload),
                "spatial|%d|%.4f|%.4f|%.1f",
                enabled ? 1 : 0, gain, pan, lowpassHz);
            if (send_payload(payload, 30))
            {
                m_spatialEnabled = enabled;
                m_lastSpatialGain = gain;
                m_lastSpatialPan = pan;
                m_lastLowpassHz = lowpassHz;
                m_lastSpatialSendTick = now;
            }
        }
        bool SupportsSourceBrightness() const override { return true; }
        void SetSourceBrightness(float brightness) override
        {
            brightness = (std::clamp)(brightness, 0.05f, 2.0f);
            if (std::fabs(brightness - m_lastBrightness) < 0.001f)
                return;

            char payload[48]{};
            std::snprintf(
                payload, sizeof(payload), "brightness|%.4f", brightness);
            if (send_payload(payload, 30))
            {
                m_lastBrightness = brightness;
                if (m_userCapturePaused.load() ||
                    !m_vehiclePowered.load())
                {
                    m_brightnessRefreshRequestedTick = GetTickCount64();
                    m_brightnessRefreshPending = true;
                    m_capture->SetPaused(false);
                }
            }
        }
        source_performance_stats_t GetPerformanceStats() const override
        {
            auto result = m_capture->GetPerformanceStats();
            result.hardwareDecoded = true;
            return result;
        }
        std::string GetStatusText() const override
        {
            return m_fullSpotifyWeb
                ? "Integrated Media Client: Full Spotify Web Player"
                : "Integrated Media Client running";
        }

    private:
        void refresh_helper_cpu_hints()
        {
            const uint64_t now = GetTickCount64();
            if (m_lastCpuHintCheckTick != 0 &&
                now - m_lastCpuHintCheckTick < 2000)
                return;
            m_lastCpuHintCheckTick = now;

            const DWORD cpu0 = thread_scheduling::preferred_processor(0);
            const DWORD cpu1 = thread_scheduling::preferred_processor(1);
            const DWORD cpu2 = thread_scheduling::preferred_processor(2);
            if (cpu0 == thread_scheduling::kUnassignedProcessor)
                return;
            if (cpu0 == m_lastCpuHint0 && cpu1 == m_lastCpuHint1 &&
                cpu2 == m_lastCpuHint2)
                return;

            const std::string payload =
                "cpuhints|" + std::to_string(cpu0) + "," +
                std::to_string(
                    cpu1 == thread_scheduling::kUnassignedProcessor
                        ? cpu0 : cpu1) + "," +
                std::to_string(
                    cpu2 == thread_scheduling::kUnassignedProcessor
                        ? cpu0 : cpu2);
            if (send_payload(payload, 30))
            {
                m_lastCpuHint0 = cpu0;
                m_lastCpuHint1 = cpu1;
                m_lastCpuHint2 = cpu2;
                diagnostic_log::writef(
                    "scheduler", "Sent helper soft CPU hints: LP %lu, %lu, %lu.",
                    static_cast<unsigned long>(cpu0),
                    static_cast<unsigned long>(
                        cpu1 == thread_scheduling::kUnassignedProcessor
                            ? cpu0 : cpu1),
                    static_cast<unsigned long>(
                        cpu2 == thread_scheduling::kUnassignedProcessor
                            ? cpu0 : cpu2));
            }
        }

        std::unique_ptr<IContentSource> m_capture;
        std::atomic<bool> m_userCapturePaused{};
        std::atomic<bool> m_vehiclePowered{ true };
        bool m_spatialEnabled{};
        float m_lastSpatialGain{ 1.0f };
        float m_lastSpatialPan{};
        float m_lastLowpassHz{ 20000.0f };
        uint64_t m_lastSpatialSendTick{};
        float m_lastBrightness{ -1.0f };
        std::atomic<bool> m_brightnessRefreshPending{};
        std::atomic<uint64_t> m_brightnessRefreshRequestedTick{};
        bool m_fullSpotifyWeb{};
        uint64_t m_lastCpuHintCheckTick{};
        DWORD m_lastCpuHint0{ thread_scheduling::kUnassignedProcessor };
        DWORD m_lastCpuHint1{ thread_scheduling::kUnassignedProcessor };
        DWORD m_lastCpuHint2{ thread_scheduling::kUnassignedProcessor };
    };
}

namespace sources {
    bool IsMediaClientInstalled()
    {
        return GetFileAttributesA(media_client_executable().c_str()) !=
            INVALID_FILE_ATTRIBUTES;
    }

    bool SetMediaClientDucking(float gain)
    {
        const HWND window = find_media_client();
        if (!window)
            return false;

        gain = (std::clamp)(gain, 0.0f, 1.0f);
        const WPARAM scaledGain = static_cast<WPARAM>(
            std::lround(gain * 10000.0f));
        return PostMessageA(
            window, kPrismEnvironmentMessage, scaledGain, 0) != FALSE;
    }

    void ShutdownMediaClient()
    {
        const HWND window = find_media_client();
        if (!window)
            return;
        // Close the owned helper before DXGI and telemetry start tearing down.
        if (!send_payload("shutdown", 30))
            PostMessageA(window, WM_CLOSE, 0, 0);
    }

    std::unique_ptr<IContentSource> CreateMediaClientSource(
        const std::string& media_url,
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height,
        bool full_spotify_web)
    {
        if (!launch_media_client())
            return nullptr;

        if (!media_url.empty())
        {
            // The helper can receive commands as soon as its Win32 window is
            // visible; it queues the URL until WebView2 has initialized.
            send_payload(
                (full_spotify_web ? "loadspotifyweb|" : "load|") +
                media_url);
        }
        send_payload(
            "resize|" + std::to_string(output_width) + "x" +
            std::to_string(output_height));

        auto capture = CreateWgcWindowSource(
            kMediaClientExecutable, kMediaClientWindowTitle,
            framerate, output_width, output_height);
        if (!capture)
            return nullptr;
        return std::make_unique<MediaClientSource>(
            std::move(capture), full_spotify_web);
    }
}
