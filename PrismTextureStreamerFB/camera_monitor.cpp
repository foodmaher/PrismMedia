#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "camera_monitor.h"
#include "scs_logging.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

using namespace scs_logging;

namespace
{
    HANDLE g_mapping{};
    prism_camera_monitor::SharedState* g_shared{};
    LONG g_consumedRequestSequence{};
    LONG g_consumedPhaseRequestSequence{};
    std::atomic<uint64_t> g_runId{};
    std::atomic<uint64_t> g_frameSequence{};
    uint64_t g_lastHeartbeatTick{};

    template <size_t Size>
    void copy_text(char (&destination)[Size], const char* source)
    {
        destination[0] = '\0';
        if (!source)
            return;
        strncpy_s(destination, Size, source, _TRUNCATE);
    }

    void begin_write()
    {
        InterlockedIncrement(&g_shared->sequence);
        MemoryBarrier();
    }

    void end_write()
    {
        MemoryBarrier();
        InterlockedIncrement(&g_shared->sequence);
    }

    std::wstring module_directory()
    {
        HMODULE module{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&camera_monitor::initialize),
                &module))
        {
            return {};
        }

        wchar_t path[MAX_PATH]{};
        const DWORD capacity = static_cast<DWORD>(_countof(path));
        const DWORD length = GetModuleFileNameW(
            module, path, capacity);
        if (length == 0 || length >= capacity)
            return {};
        std::wstring result(path, length);
        const size_t separator = result.find_last_of(L"\\/");
        return separator == std::wstring::npos
            ? std::wstring{}
            : result.substr(0, separator);
    }

    bool file_exists(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
}

namespace camera_monitor
{
    bool initialize()
    {
        if (g_shared)
            return true;

        g_mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(prism_camera_monitor::SharedState)),
            prism_camera_monitor::kMappingName);
        if (!g_mapping)
            return false;

        const bool newlyCreated = GetLastError() != ERROR_ALREADY_EXISTS;
        g_shared = static_cast<prism_camera_monitor::SharedState*>(
            MapViewOfFile(
                g_mapping,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                sizeof(prism_camera_monitor::SharedState)));
        if (!g_shared)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
            return false;
        }

        const LONG pendingRequest = g_shared->requestSequence;
        if (newlyCreated ||
            g_shared->magic != prism_camera_monitor::kMagic ||
            g_shared->version != prism_camera_monitor::kVersion)
        {
            std::memset(g_shared, 0, sizeof(*g_shared));
            g_shared->requestSequence = pendingRequest;
        }
        g_consumedRequestSequence = g_shared->requestSequence;
        g_consumedPhaseRequestSequence =
            g_shared->phaseRequestSequence;
        g_shared->magic = prism_camera_monitor::kMagic;
        g_shared->version = prism_camera_monitor::kVersion;
        publish(
            prism_camera_monitor::Stage::PluginReady,
            prism_camera_monitor::kPluginConnected |
                prism_camera_monitor::kSlot7Disabled,
            "Plugin ready",
            "Independent Camera Lab is connected. Slot 7 is disabled; "
            "no mirror or park frame can enter this channel.");
        return true;
    }

    void shutdown()
    {
        if (g_shared)
        {
            publish(
                prism_camera_monitor::Stage::Offline,
                prism_camera_monitor::kSlot7Disabled,
                "Plugin offline",
                "The game plugin shut down.");
            UnmapViewOfFile(g_shared);
            g_shared = nullptr;
        }
        if (g_mapping)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
    }

    void launch_viewer()
    {
        const std::wstring directory = module_directory();
        if (directory.empty())
            return;

        const std::wstring packaged = directory +
            L"\\PrismTextureStreamerFB\\PrismCameraMonitor.exe";
        const std::wstring adjacent = directory +
            L"\\PrismCameraMonitor.exe";
        const std::wstring executable = file_exists(packaged)
            ? packaged
            : adjacent;
        if (!file_exists(executable))
        {
            scs_log(2,
                "[Camera Lab] PrismCameraMonitor.exe was not found beside "
                "the plugin package.");
            return;
        }

        std::wstring command = L"\"" + executable + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(
                executable.c_str(),
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                nullptr,
                directory.c_str(),
                &startup,
                &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        else
        {
            scs_log(2,
                "[Camera Lab] Failed to launch PrismCameraMonitor.exe "
                "(Win32 error %lu).",
                GetLastError());
        }
    }

    void heartbeat()
    {
        if (!g_shared && !initialize())
            return;
        const uint64_t now = GetTickCount64();
        if (g_lastHeartbeatTick != 0 && now >= g_lastHeartbeatTick &&
            now - g_lastHeartbeatTick < 250)
            return;
        g_lastHeartbeatTick = now;
        begin_write();
        g_shared->updatedTick = now;
        end_write();
    }

    bool consume_run_request()
    {
        if (!g_shared && !initialize())
            return false;
        const LONG requested = g_shared->requestSequence;
        if (requested == g_consumedRequestSequence)
            return false;
        g_consumedRequestSequence = requested;
        return true;
    }

    bool consume_phase_request(uint32_t& phase)
    {
        phase = 0;
        if (!g_shared && !initialize())
            return false;
        const LONG requested = g_shared->phaseRequestSequence;
        if (requested == g_consumedPhaseRequestSequence)
            return false;
        g_consumedPhaseRequestSequence = requested;
        phase = g_shared->requestedPhase;
        return true;
    }

    void begin_run(const char* detail)
    {
        if (!g_shared && !initialize())
            return;
        g_runId.fetch_add(1, std::memory_order_relaxed);
        g_frameSequence.store(0, std::memory_order_relaxed);
        begin_write();
        g_shared->frameSequence = 0;
        g_shared->width = 0;
        g_shared->height = 0;
        g_shared->stride = 0;
        g_shared->pixelBytes = 0;
        g_shared->correlationSamples = 0;
        g_shared->currentPhase = static_cast<uint32_t>(
            prism_camera_monitor::CorrelationPhase::Idle);
        g_shared->requestedPhase = 0;
        g_shared->completedPhaseMask = 0;
        g_shared->candidateCount = 0;
        std::memset(
            g_shared->instructionText, 0,
            sizeof(g_shared->instructionText));
        std::memset(
            g_shared->candidates, 0,
            sizeof(g_shared->candidates));
        end_write();
        publish(
            prism_camera_monitor::Stage::DiagnosticStarted,
            prism_camera_monitor::kPluginConnected |
                prism_camera_monitor::kSlot7Disabled |
                prism_camera_monitor::kDiagnosticRunning,
            "Diagnostic started",
            detail);
    }

    void publish(
        prism_camera_monitor::Stage stage,
        uint32_t flags,
        const char* stageText,
        const char* detailText,
        int32_t errorCode,
        uint64_t observedRenderJobs,
        uint64_t rejectedSlot7Jobs,
        uint64_t taggedTargets,
        uint64_t readbackFrames)
    {
        if (!g_shared)
            return;
        begin_write();
        g_shared->magic = prism_camera_monitor::kMagic;
        g_shared->version = prism_camera_monitor::kVersion;
        g_shared->stage = static_cast<uint32_t>(stage);
        g_shared->flags = flags;
        g_shared->errorCode = errorCode;
        g_shared->updatedTick = GetTickCount64();
        g_shared->runId = g_runId.load(std::memory_order_relaxed);
        g_shared->observedRenderJobs = observedRenderJobs;
        g_shared->rejectedSlot7Jobs = rejectedSlot7Jobs;
        g_shared->taggedTargets = taggedTargets;
        g_shared->readbackFrames = readbackFrames;
        g_shared->correlationSamples = 0;
        g_shared->currentPhase = static_cast<uint32_t>(
            prism_camera_monitor::CorrelationPhase::Idle);
        g_shared->completedPhaseMask = 0;
        g_shared->candidateCount = 0;
        copy_text(g_shared->stageText, stageText);
        copy_text(g_shared->detailText, detailText);
        std::memset(
            g_shared->instructionText, 0,
            sizeof(g_shared->instructionText));
        std::memset(
            g_shared->candidates, 0,
            sizeof(g_shared->candidates));
        end_write();
    }

    void publish_frame(
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        const char* detailText)
    {
        if (!g_shared || !bgraPixels || width == 0 || height == 0 ||
            width > prism_camera_monitor::kMaximumWidth ||
            height > prism_camera_monitor::kMaximumHeight ||
            stride < width * 4)
            return;

        const uint64_t byteCount64 =
            static_cast<uint64_t>(width) * height * 4;
        if (byteCount64 > prism_camera_monitor::kMaximumPixelBytes)
            return;

        begin_write();
        for (uint32_t row = 0; row < height; ++row)
        {
            std::memcpy(
                g_shared->pixels + static_cast<size_t>(row) * width * 4,
                bgraPixels + static_cast<size_t>(row) * stride,
                static_cast<size_t>(width) * 4);
        }
        g_shared->stage = static_cast<uint32_t>(
            prism_camera_monitor::Stage::FrameReady);
        g_shared->flags =
            prism_camera_monitor::kPluginConnected |
            prism_camera_monitor::kSlot7Disabled |
            prism_camera_monitor::kCameraStateVerified |
            prism_camera_monitor::kOwnedTargetVerified |
            prism_camera_monitor::kFrameAvailable;
        g_shared->updatedTick = GetTickCount64();
        g_shared->runId = g_runId.load(std::memory_order_relaxed);
        g_shared->frameSequence =
            g_frameSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        g_shared->width = width;
        g_shared->height = height;
        g_shared->stride = width * 4;
        g_shared->pixelBytes = static_cast<uint32_t>(byteCount64);
        ++g_shared->readbackFrames;
        copy_text(g_shared->stageText, "Verified custom-camera frame");
        copy_text(g_shared->detailText, detailText);
        end_write();
    }

    void publish_correlation(
        prism_camera_monitor::Stage stage,
        uint32_t flags,
        prism_camera_monitor::CorrelationPhase phase,
        uint32_t completedPhaseMask,
        uint64_t correlationSamples,
        const char* stageText,
        const char* detailText,
        const char* instructionText,
        const prism_camera_monitor::CorrelationCandidate* candidates,
        uint32_t candidateCount,
        uint64_t observedRenderJobs,
        uint64_t rejectedSlot7Jobs)
    {
        if (!g_shared)
            return;
        candidateCount = (std::min)(
            candidateCount,
            prism_camera_monitor::kMaximumCorrelationCandidates);
        begin_write();
        g_shared->stage = static_cast<uint32_t>(stage);
        g_shared->flags = flags;
        g_shared->errorCode = 0;
        g_shared->updatedTick = GetTickCount64();
        g_shared->runId = g_runId.load(std::memory_order_relaxed);
        g_shared->observedRenderJobs = observedRenderJobs;
        g_shared->rejectedSlot7Jobs = rejectedSlot7Jobs;
        g_shared->taggedTargets = 0;
        g_shared->readbackFrames = 0;
        g_shared->correlationSamples = correlationSamples;
        g_shared->currentPhase = static_cast<uint32_t>(phase);
        g_shared->completedPhaseMask = completedPhaseMask;
        g_shared->candidateCount = candidateCount;
        copy_text(g_shared->stageText, stageText);
        copy_text(g_shared->detailText, detailText);
        copy_text(g_shared->instructionText, instructionText);
        std::memset(
            g_shared->candidates, 0,
            sizeof(g_shared->candidates));
        if (candidates && candidateCount != 0)
        {
            std::memcpy(
                g_shared->candidates,
                candidates,
                static_cast<size_t>(candidateCount) *
                    sizeof(prism_camera_monitor::CorrelationCandidate));
        }
        end_write();
    }
}
