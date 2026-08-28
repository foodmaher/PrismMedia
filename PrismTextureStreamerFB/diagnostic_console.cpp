#define NOMINMAX
#include "diagnostic_console.h"

#include "custom_render_probe.h"
#include "diagnostic_log.h"
#include "prism/execute_command.h"
#include "prism/prism.h"
#include "screens.h"
#include "version.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    constexpr size_t kMaximumCommandBytes = 2048;
    constexpr DWORD kPipePollMilliseconds = 20;
    constexpr auto kCommandTimeout = std::chrono::seconds(10);

    std::atomic<bool> g_running{};
    std::thread g_worker;
    std::wstring g_pipeName;
    std::mutex g_requestMutex;
    std::condition_variable g_requestCompleted;
    bool g_requestPending{};
    bool g_responseReady{};
    std::string g_command;
    std::string g_response;

    std::string trim(std::string value)
    {
        while (!value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.pop_back();
        }
        size_t first{};
        while (first < value.size() &&
            std::isspace(static_cast<unsigned char>(value[first])))
        {
            ++first;
        }
        return value.substr(first);
    }

    std::string lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    }

    const char* fallback_name(custom_render_probe::fallback_mode_t mode)
    {
        switch (mode)
        {
        case custom_render_probe::fallback_mode_t::forced_on:
            return "on";
        case custom_render_probe::fallback_mode_t::forced_off:
            return "off";
        default:
            return "auto";
        }
    }

    std::string status_response()
    {
        const custom_render_probe::status_t state =
            custom_render_probe::status();
        char text[768]{};
        std::snprintf(
            text, sizeof(text),
            "OK version=%s state=%s texture=%s branchEvents=%u "
            "listEntries=%u exactReleases=%u validatedReleases=%u "
            "drawSamples=%u releaseWindowUs=%llu fallback=%s",
            g_version,
            state.active ? "active" :
                (state.waitingForTexture ? "waiting-texture" :
                    (state.completed ? "completed" : "ready")),
            state.textureReady ? "matched" : "pending",
            state.branchEvents, state.listEntries,
            state.exactOldReleases, state.validatedReleases,
            state.drawSamples,
            static_cast<unsigned long long>(
                state.releaseScopeWindowMicroseconds),
            fallback_name(state.fallbackMode));
        return text;
    }

    std::string list_displays()
    {
        std::ostringstream output;
        output << "OK displays";
        size_t customOrdinal{};
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        for (const screen_t& screen : g_screens)
        {
            if (screen.type != screen_type_t::CUSTOM)
                continue;
            ++customOrdinal;
            output << "\n" << customOrdinal
                << " id=" << screen.mediaClientId
                << " enabled=" << (screen.enabled ? 1 : 0)
                << " live=" << (screen.liveTexture ? 1 : 0)
                << " texture=" << screen.original_texture;
        }
        if (customOrdinal == 0)
            output << "\nnone";
        return output.str();
    }

    void log_display_snapshots()
    {
        std::lock_guard<std::mutex> lock(g_screens_mutex);
        size_t ordinal{};
        for (const screen_t& screen : g_screens)
        {
            if (screen.type != screen_type_t::CUSTOM)
                continue;
            ++ordinal;
            diagnostic_log::writef(
                "console",
                "Display snapshot: ordinal=%llu id='%s' enabled=%d "
                "texture='%s' live=%p route=%llu matched=%llu.",
                static_cast<unsigned long long>(ordinal),
                screen.mediaClientId.c_str(), screen.enabled ? 1 : 0,
                screen.original_texture.c_str(),
                static_cast<void*>(screen.liveTexture),
                static_cast<unsigned long long>(
                    screen.textureRouteSequence),
                static_cast<unsigned long long>(
                    screen.textureRouteMatchedSequence));
        }
    }

    bool effective_fallback(bool customDisplayActive)
    {
        const auto mode = custom_render_probe::fallback_mode();
        return mode == custom_render_probe::fallback_mode_t::forced_on ||
            (mode == custom_render_probe::fallback_mode_t::automatic &&
                customDisplayActive);
    }

    std::string run_capture(
        const std::string& requestedDisplay,
        bool driving,
        bool customDisplayActive)
    {
        if (!driving)
            return "ERROR Enter the truck before starting a diagnostic run.";

        const auto state = custom_render_probe::status();
        if (state.active)
            return "ERROR A diagnostic run is already active.";
        if (state.waitingForTexture)
            return "ERROR The previous run is still waiting for its exact "
                "texture. Use status, or abort before starting another run.";
        if (!custom_render_probe::reset_session())
            return "ERROR The previous diagnostic cannot be reset yet.";

        std::string displayId;
        std::string originalTexture;
        ID3D11Texture2D* liveTexture{};
        {
            std::lock_guard<std::mutex> lock(g_screens_mutex);
            for (const screen_t& screen : g_screens)
            {
                if (screen.type != screen_type_t::CUSTOM ||
                    !screen.enabled || !screen.liveTexture ||
                    screen.original_texture.empty())
                {
                    continue;
                }
                if (!requestedDisplay.empty() &&
                    requestedDisplay != "auto" &&
                    requestedDisplay != screen.mediaClientId)
                {
                    continue;
                }
                displayId = screen.mediaClientId;
                originalTexture = screen.original_texture;
                liveTexture = screen.liveTexture;
                liveTexture->AddRef();
                break;
            }
        }
        if (!liveTexture)
            return "ERROR No matching enabled custom display has a live texture.";

        const bool prepared = custom_render_probe::prepare_capture(
            displayId.c_str(), originalTexture.c_str(), liveTexture);
        liveTexture->Release();
        if (!prepared)
            return "ERROR The safe diagnostic probe could not be armed.";

        prism::string command("game");
        if (!prism::execute_command::call(&command, -1))
        {
            custom_render_probe::abort_capture(
                effective_fallback(customDisplayActive));
            return "ERROR The game rejected the texture reload command; "
                "the probe was removed safely.";
        }

        diagnostic_log::writef(
            "console",
            "Runtime console started diagnostic for display='%s' "
            "texture='%s'.",
            displayId.c_str(), originalTexture.c_str());
        return "OK Diagnostic armed and texture reload accepted for " +
            displayId + ". Use status to monitor it.";
    }

    std::string execute_command(
        const std::string& rawCommand,
        bool driving,
        bool customDisplayActive)
    {
        std::istringstream input(trim(rawCommand));
        std::string verb;
        input >> verb;
        verb = lowercase(verb);

        if (verb.empty() || verb == "help")
        {
            return
                "OK commands:\n"
                "  status\n"
                "  displays\n"
                "  run [auto|display-id]\n"
                "  abort\n"
                "  reset\n"
                "  set release_window_us <1000..5000000>\n"
                "  fallback <auto|on|off>\n"
                "  snapshot\n"
                "  ping\n"
                "Only built-in bounded diagnostics are accepted.";
        }
        if (verb == "ping")
            return std::string("OK PrismMedia ") + g_version;
        if (verb == "status")
            return status_response();
        if (verb == "displays" || verb == "list")
            return list_displays();
        if (verb == "snapshot")
        {
            log_display_snapshots();
            return "OK Display routing snapshot written to PrismMedia.log.";
        }
        if (verb == "run")
        {
            std::string display;
            input >> display;
            return run_capture(
                display.empty() ? "auto" : display,
                driving, customDisplayActive);
        }
        if (verb == "abort")
        {
            return custom_render_probe::abort_capture(
                effective_fallback(customDisplayActive))
                ? "OK Diagnostic capture aborted and temporary hooks removed."
                : "OK No diagnostic capture was active.";
        }
        if (verb == "reset")
        {
            return custom_render_probe::reset_session()
                ? "OK Diagnostic session reset; another run is allowed."
                : "ERROR Abort or wait for the active run before resetting.";
        }
        if (verb == "fallback")
        {
            std::string value;
            input >> value;
            value = lowercase(value);
            if (value == "auto")
            {
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::automatic);
            }
            else if (value == "on")
            {
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_on);
            }
            else if (value == "off")
            {
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_off);
            }
            else
            {
                return "ERROR Use fallback auto, on, or off.";
            }
            custom_render_probe::update(customDisplayActive);
            diagnostic_log::writef(
                "console", "Fallback mode changed to %s.", value.c_str());
            return "OK Fallback mode=" + value;
        }
        if (verb == "set")
        {
            std::string setting;
            uint64_t value{};
            input >> setting >> value;
            setting = lowercase(setting);
            if (setting != "release_window_us")
                return "ERROR Unknown setting. Use release_window_us.";
            if (!custom_render_probe::set_release_scope_window_microseconds(
                    value))
            {
                return "ERROR Window must be 1000..5000000 microseconds "
                    "and cannot change during an active run.";
            }
            return "OK release_window_us=" + std::to_string(value);
        }
        return "ERROR Unknown command. Enter help.";
    }

    void worker_loop()
    {
        while (g_running.load(std::memory_order_acquire))
        {
            const HANDLE pipe = CreateNamedPipeW(
                g_pipeName.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT |
                    PIPE_REJECT_REMOTE_CLIENTS,
                1, 8192, 8192, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE)
            {
                diagnostic_log::writef(
                    "error", "Diagnostic console pipe creation failed (%lu).",
                    GetLastError());
                break;
            }

            bool connected{};
            while (g_running.load(std::memory_order_acquire) && !connected)
            {
                connected = ConnectNamedPipe(pipe, nullptr) != FALSE;
                if (!connected)
                {
                    const DWORD error = GetLastError();
                    connected = error == ERROR_PIPE_CONNECTED;
                    if (!connected && error != ERROR_PIPE_LISTENING)
                        break;
                }
                if (!connected)
                    Sleep(kPipePollMilliseconds);
            }

            std::string command;
            while (g_running.load(std::memory_order_acquire) && connected)
            {
                char buffer[kMaximumCommandBytes]{};
                DWORD read{};
                if (ReadFile(
                        pipe, buffer,
                        static_cast<DWORD>(sizeof(buffer) - 1),
                        &read, nullptr))
                {
                    command.assign(buffer, read);
                    break;
                }
                const DWORD error = GetLastError();
                if (error == ERROR_NO_DATA)
                {
                    Sleep(kPipePollMilliseconds);
                    continue;
                }
                connected = false;
            }

            if (connected && !trim(command).empty())
            {
                const std::string readOnlyVerb = lowercase(trim(command));
                if (readOnlyVerb == "status" || readOnlyVerb == "ping" ||
                    readOnlyVerb == "help")
                {
                    const std::string response = execute_command(
                        readOnlyVerb, false, false);
                    DWORD written{};
                    WriteFile(
                        pipe, response.data(),
                        static_cast<DWORD>(response.size()),
                        &written, nullptr);
                    FlushFileBuffers(pipe);
                    DisconnectNamedPipe(pipe);
                    CloseHandle(pipe);
                    continue;
                }

                std::unique_lock<std::mutex> lock(g_requestMutex);
                g_command = trim(command);
                g_response.clear();
                g_responseReady = false;
                g_requestPending = true;
                const bool completed = g_requestCompleted.wait_for(
                    lock, kCommandTimeout, [] {
                        return g_responseReady || !g_running.load();
                    });
                const std::string response = completed && g_responseReady
                    ? g_response
                    : "ERROR Plugin command timed out; check status before "
                      "retrying because the game thread may have resumed.";
                lock.unlock();

                DWORD written{};
                WriteFile(
                    pipe, response.data(),
                    static_cast<DWORD>(response.size()),
                    &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }
}

namespace diagnostic_console
{
    void start()
    {
        bool expected = false;
        if (!g_running.compare_exchange_strong(expected, true))
            return;
        g_pipeName = L"\\\\.\\pipe\\PrismMediaDiagnostic-" +
            std::to_wstring(GetCurrentProcessId());
        g_worker = std::thread(worker_loop);
        diagnostic_log::writef(
            "console",
            "Restricted runtime diagnostic console ready (pid=%lu).",
            GetCurrentProcessId());
    }

    void update(bool driving, bool customDisplayActive)
    {
        std::string command;
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            if (!g_requestPending)
                return;
            command = g_command;
            g_requestPending = false;
        }

        const std::string response = execute_command(
            command, driving, customDisplayActive);
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            g_response = response;
            g_responseReady = true;
        }
        g_requestCompleted.notify_one();
    }

    void stop()
    {
        if (!g_running.exchange(false))
        {
            if (g_worker.joinable())
                g_worker.join();
            return;
        }
        g_requestCompleted.notify_all();
        if (g_worker.joinable())
            g_worker.join();
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_requestPending = false;
        g_responseReady = false;
    }
}
