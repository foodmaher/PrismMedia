#define NOMINMAX
#include "media_client.h"

#include "wgc_window.h"
#include "../scs_logging.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

using namespace scs_logging;

namespace {
    constexpr ULONG_PTR kPrismCopyDataId = 0x50524953;

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

    bool send_payload(const std::string& payload)
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
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored) != 0;
    }

    bool launch_media_client()
    {
        if (find_media_client())
            return true;

        const std::string executable =
            module_directory() + sources::kMediaClientExecutable;
        if (GetFileAttributesA(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            scs_log(2, "[MediaClient] Missing helper: %s", executable.c_str());
            return false;
        }

        std::string commandLine = "\"" + executable + "\"";
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessA(
            executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, module_directory().c_str(),
            &startup, &process))
        {
            scs_log(2, "[MediaClient] Launch failed, err=%lu", GetLastError());
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        for (int attempt = 0; attempt < 160; ++attempt)
        {
            if (find_media_client())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        scs_log(2, "[MediaClient] Timed out waiting for helper window");
        return false;
    }

    class MediaClientSource final : public IContentSource
    {
    public:
        explicit MediaClientSource(std::unique_ptr<IContentSource> capture)
            : m_capture(std::move(capture))
        {
        }

        uint32_t GetWidth() const override { return m_capture->GetWidth(); }
        uint32_t GetHeight() const override { return m_capture->GetHeight(); }
        void SetFramerate(uint8_t framerate) override
        {
            m_capture->SetFramerate(framerate);
        }
        void SetPaused(bool paused) override { m_capture->SetPaused(paused); }
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
            return m_capture->CopyLatestFrame(destination, width, height);
        }

        bool SupportsMediaControls() const override { return true; }
        bool LoadMedia(const std::string& url) override
        {
            return send_payload("load|" + url);
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
            return name && send_payload(name);
        }
        source_performance_stats_t GetPerformanceStats() const override
        {
            auto result = m_capture->GetPerformanceStats();
            result.hardwareDecoded = true;
            return result;
        }
        std::string GetStatusText() const override
        {
            return "Integrated Media Client running";
        }

    private:
        std::unique_ptr<IContentSource> m_capture;
    };
}

namespace sources {
    bool IsMediaClientInstalled()
    {
        return GetFileAttributesA(
            (module_directory() + kMediaClientExecutable).c_str()) !=
            INVALID_FILE_ATTRIBUTES;
    }

    std::unique_ptr<IContentSource> CreateMediaClientSource(
        const std::string& media_url,
        uint8_t framerate,
        uint32_t output_width,
        uint32_t output_height)
    {
        if (!launch_media_client())
            return nullptr;

        if (!media_url.empty())
        {
            // The helper can receive commands as soon as its Win32 window is
            // visible; it queues the URL until WebView2 has initialized.
            send_payload("load|" + media_url);
        }
        send_payload(
            "resize|" + std::to_string(output_width) + "x" +
            std::to_string(output_height));

        auto capture = CreateWgcWindowSource(
            kMediaClientExecutable, kMediaClientWindowTitle,
            framerate, output_width, output_height);
        if (!capture)
            return nullptr;
        return std::make_unique<MediaClientSource>(std::move(capture));
    }
}
