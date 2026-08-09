#define NOMINMAX
#include "traffic_audio.h"

#include "diagnostic_log.h"
#include "telemetry_state.h"
#include "thread_scheduling.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
    constexpr ULONG_PTR kPrismCopyDataId = 0x50524953;
    constexpr char kTrafficWindowTitle[] = "Prism Traffic Radio";
    constexpr float kPi = 3.14159265358979323846f;

    struct desired_state_t
    {
        bool enabled{};
        bool active{};
        std::string url;
        int32_t emitterId{ -1 };
        uint32_t randomSeed{};
        float gain{};
        float pan{};
        float cutoffHz{ 20000.0f };
    };

    std::mutex g_workerMutex;
    std::condition_variable g_workerCondition;
    std::thread g_worker;
    bool g_workerStarted{};
    bool g_stopRequested{};
    uint64_t g_generation{};
    desired_state_t g_desired{};

    std::mutex g_statusMutex;
    traffic_audio::status_t g_status{};
    int32_t g_selectedEmitter{ -1 };

    uint32_t mix_id(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352dU;
        value ^= value >> 15;
        value *= 0x846ca68bU;
        value ^= value >> 16;
        return value;
    }

    std::string module_directory()
    {
        static int moduleAnchor{};
        HMODULE module{};
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&moduleAnchor), &module);

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

    std::string media_client_executable()
    {
        const std::string root = module_directory();
        const std::string organized =
            root + "PrismTextureStreamerFB\\PrismMediaClient.exe";
        if (GetFileAttributesA(organized.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
            return organized;
        return root + "PrismMediaClient.exe";
    }

    std::string path_directory(const std::string& path)
    {
        const auto separator = path.find_last_of("\\/");
        return separator == std::string::npos
            ? module_directory()
            : path.substr(0, separator + 1);
    }

    HWND find_helper()
    {
        return FindWindowA(nullptr, kTrafficWindowTitle);
    }

    bool send_payload(
        HWND window,
        const std::string& payload,
        DWORD timeoutMs = 100)
    {
        if (!window)
            return false;
        COPYDATASTRUCT data{};
        data.dwData = kPrismCopyDataId;
        data.cbData = static_cast<DWORD>(payload.size() + 1);
        data.lpData = const_cast<char*>(payload.c_str());
        DWORD_PTR ignored{};
        return SendMessageTimeoutA(
            window, WM_COPYDATA, 0,
            reinterpret_cast<LPARAM>(&data),
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            timeoutMs, &ignored) != 0;
    }

    HWND launch_helper()
    {
        if (HWND existing = find_helper())
            return existing;

        const std::string executable = media_client_executable();
        if (GetFileAttributesA(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            diagnostic_log::writef(
                "traffic-audio", "Media helper is missing: %s",
                executable.c_str());
            return nullptr;
        }

        std::string commandLine =
            "\"" + executable +
            "\" --silent --parent-pid " +
            std::to_string(GetCurrentProcessId()) +
            " --window-title \"" + kTrafficWindowTitle +
            "\" --profile-suffix TrafficRadio";
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
            diagnostic_log::writef(
                "traffic-audio",
                "Traffic radio helper launch failed (Win32 error %lu).",
                GetLastError());
            return nullptr;
        }

        SetPriorityClass(process.hProcess, BELOW_NORMAL_PRIORITY_CLASS);
        thread_scheduling::apply_thread_preference(process.hThread, 2);
        HWND window{};
        for (int attempt = 0; attempt < 160; ++attempt)
        {
            {
                std::lock_guard<std::mutex> lock(g_workerMutex);
                if (g_stopRequested)
                    break;
            }
            if ((attempt % 40) == 0)
                thread_scheduling::apply_thread_preference(
                    process.hThread, 2);
            window = find_helper();
            if (window)
                break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50));
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (window)
        {
            diagnostic_log::write(
                "traffic-audio",
                "Dedicated traffic radio helper is ready.");
        }
        else
        {
            diagnostic_log::write(
                "traffic-audio",
                "Timed out waiting for the traffic radio helper.");
        }
        return window;
    }

    bool desired_equal(
        const desired_state_t& first,
        const desired_state_t& second)
    {
        return first.enabled == second.enabled &&
            first.active == second.active &&
            first.url == second.url &&
            first.emitterId == second.emitterId &&
            std::fabs(first.gain - second.gain) < 0.008f &&
            std::fabs(first.pan - second.pan) < 0.015f &&
            std::fabs(first.cutoffHz - second.cutoffHz) < 30.0f;
    }

    void worker_main()
    {
        HWND window{};
        std::string loadedUrl;
        int32_t loadedEmitter = -1;
        uint64_t observedGeneration{};

        for (;;)
        {
            desired_state_t desired;
            {
                std::unique_lock<std::mutex> lock(g_workerMutex);
                g_workerCondition.wait_for(
                    lock, std::chrono::milliseconds(250),
                    [&]()
                    {
                        return g_stopRequested ||
                            observedGeneration != g_generation;
                    });
                if (g_stopRequested)
                    break;
                desired = g_desired;
                observedGeneration = g_generation;
            }

            if (window && !IsWindow(window))
            {
                window = nullptr;
                loadedUrl.clear();
                loadedEmitter = -1;
            }

            if (!desired.enabled)
            {
                if (window)
                    send_payload(window, "shutdown", 100);
                window = nullptr;
                loadedUrl.clear();
                loadedEmitter = -1;
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status.helperReady = false;
                g_status.active = false;
                continue;
            }

            if (desired.active && !window)
                window = launch_helper();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_status.helperReady = window != nullptr;
                g_status.active = desired.active && window != nullptr;
            }
            if (!window)
                continue;

            if (!desired.active)
            {
                send_payload(
                    window,
                    "spatial|1|0.0000|0.0000|20000.0", 50);
                continue;
            }

            if (loadedUrl != desired.url ||
                loadedEmitter != desired.emitterId)
            {
                if (!send_payload(window, "load|" + desired.url))
                    continue;
                send_payload(
                    window,
                    "randomize|" +
                    std::to_string(desired.randomSeed));
                loadedUrl = desired.url;
                loadedEmitter = desired.emitterId;
                diagnostic_log::writef(
                    "traffic-audio",
                    "Traffic radio moved to AI id=%d (seed=%u).",
                    desired.emitterId, desired.randomSeed);
            }

            char payload[128]{};
            std::snprintf(
                payload, sizeof(payload),
                "spatial|1|%.4f|%.4f|%.1f",
                desired.gain, desired.pan, desired.cutoffHz);
            send_payload(window, payload, 50);
        }

        if (!window)
            window = find_helper();
        if (window)
            send_payload(window, "shutdown", 100);
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.helperReady = false;
        g_status.active = false;
    }

    void ensure_worker()
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        if (g_workerStarted)
            return;
        g_stopRequested = false;
        g_workerStarted = true;
        g_worker = std::thread(worker_main);
    }
}

namespace traffic_audio
{
    void update(const config_t& input)
    {
        config_t config = input;
        config.vehicleDensity = (std::clamp)(
            config.vehicleDensity, 0.0f, 1.0f);
        config.maximumVolume = (std::clamp)(
            config.maximumVolume, 0.0f, 1.0f);
        config.fullVolumeDistance = (std::clamp)(
            config.fullVolumeDistance, 0.0f, 25.0f);
        config.muteDistance = (std::max)(
            config.fullVolumeDistance + 0.5f,
            (std::clamp)(config.muteDistance, 1.0f, 100.0f));
        config.nearCutoffHz = (std::clamp)(
            config.nearCutoffHz, 20.0f, 20000.0f);
        config.farCutoffHz = (std::clamp)(
            config.farCutoffHz, 20.0f, config.nearCutoffHz);

        const uint64_t now = GetTickCount64();
        const ai_traffic_snapshot_t traffic = ai_traffic_snapshot();
        const bool trafficFresh =
            traffic.available && traffic.updatedTick != 0 &&
            now >= traffic.updatedTick &&
            now - traffic.updatedTick <= 1000;
        const bool canRun =
            config.enabled && !config.playlistUrl.empty() &&
            g_telemetry_driving.load() && trafficFresh;

        const double truckX = g_truck_world_x.load();
        const double truckY = g_truck_world_y.load();
        const double truckZ = g_truck_world_z.load();
        const float truckYaw = g_truck_heading.load() * 2.0f * kPi;
        const float cosine = std::cos(truckYaw);
        const float sine = std::sin(truckYaw);

        float localX = g_head_offset_x.load();
        float localY = g_head_offset_y.load();
        float localZ = g_head_offset_z.load();
        if (g_head_anchor_calibrated.load() &&
            g_head_anchor_uses_truck_local.load())
        {
            localX = g_head_anchor_local_x.load();
            localY = g_head_anchor_local_y.load();
            localZ = g_head_anchor_local_z.load();
        }
        double listenerX =
            truckX + localX * cosine + localZ * sine;
        double listenerY = truckY + localY;
        double listenerZ =
            truckZ - localX * sine + localZ * cosine;

        // SPF camera floats and SCS double placements normally share the
        // same origin. Reject a clearly different floating origin and retain
        // the truck-relative listener above instead of producing bad ranges.
        if (g_camera_bridge_connected.load())
        {
            const double cameraX = g_camera_world_x.load();
            const double cameraY = g_camera_world_y.load();
            const double cameraZ = g_camera_world_z.load();
            const double originDx = cameraX - truckX;
            const double originDy = cameraY - truckY;
            const double originDz = cameraZ - truckZ;
            if (originDx * originDx + originDy * originDy +
                    originDz * originDz < 1000000.0)
            {
                listenerX = cameraX;
                listenerY = cameraY;
                listenerZ = cameraZ;
            }
        }

        int32_t nearestId = -1;
        float nearestDistance = config.muteDistance + 1.0f;
        uint32_t eligibleCount{};
        float selectedDx{};
        float selectedDz{};
        float retainedDistance = config.muteDistance + 1.0f;
        float retainedDx{};
        float retainedDz{};
        bool retained{};
        if (canRun)
        {
            const uint32_t densityThreshold = static_cast<uint32_t>(
                config.vehicleDensity * 4294967295.0);
            for (uint32_t index = 0; index < traffic.count; ++index)
            {
                const auto& vehicle = traffic.vehicles[index];
                if (config.vehicleDensity <= 0.0f ||
                    mix_id(static_cast<uint32_t>(vehicle.id)) >
                    densityThreshold)
                    continue;
                ++eligibleCount;

                const double dx = vehicle.x - listenerX;
                const double dy = vehicle.y - listenerY;
                const double dz = vehicle.z - listenerZ;
                const float distance = static_cast<float>(std::sqrt(
                    dx * dx + dy * dy + dz * dz));
                if (!std::isfinite(distance))
                    continue;
                if (vehicle.id == g_selectedEmitter &&
                    distance <= config.muteDistance)
                {
                    retained = true;
                    retainedDistance = distance;
                    retainedDx = static_cast<float>(dx);
                    retainedDz = static_cast<float>(dz);
                }
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearestId = vehicle.id;
                    selectedDx = static_cast<float>(dx);
                    selectedDz = static_cast<float>(dz);
                }
            }
        }

        // Keep the current station until another eligible car is at least
        // four metres closer. This prevents rapid source reloads in traffic.
        if (retained &&
            (nearestId < 0 || retainedDistance <= nearestDistance + 4.0f))
        {
            nearestId = g_selectedEmitter;
            nearestDistance = retainedDistance;
            selectedDx = retainedDx;
            selectedDz = retainedDz;
        }
        g_selectedEmitter = nearestId;

        desired_state_t desired{};
        desired.enabled = config.enabled;
        desired.url = config.playlistUrl;
        desired.emitterId = nearestId;
        desired.randomSeed = mix_id(static_cast<uint32_t>(nearestId));
        desired.active = canRun && nearestId >= 0 &&
            nearestDistance <= config.muteDistance;
        if (desired.active)
        {
            const float blend = (std::clamp)(
                (nearestDistance - config.fullVolumeDistance) /
                    (config.muteDistance - config.fullVolumeDistance),
                0.0f, 1.0f);
            desired.gain = config.maximumVolume *
                std::pow(1.0f - blend, 1.35f);
            desired.cutoffHz = std::exp(
                std::log(config.nearCutoffHz) + blend *
                    (std::log(config.farCutoffHz) -
                        std::log(config.nearCutoffHz)));

            const float right =
                selectedDx * cosine - selectedDz * sine;
            const float forward =
                selectedDx * sine + selectedDz * cosine;
            const float targetAngle = std::atan2(right, forward);
            const float headAngle =
                g_head_heading.load() * 2.0f * kPi;
            desired.pan = (std::clamp)(
                std::sin(targetAngle - headAngle), -1.0f, 1.0f);
        }

        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.bridgeAvailable = trafficFresh;
            g_status.observedVehicles = traffic.count;
            g_status.eligibleVehicles = eligibleCount;
            g_status.emitterId = desired.active ? nearestId : -1;
            g_status.distance = desired.active ? nearestDistance : 0.0f;
            g_status.gain = desired.gain;
            g_status.pan = desired.pan;
            g_status.cutoffHz = desired.cutoffHz;
            if (!desired.active)
                g_status.active = false;
        }

        if (!desired.enabled)
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            if (!g_workerStarted)
                return;
        }
        ensure_worker();
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            if (desired_equal(g_desired, desired))
                return;
            g_desired = std::move(desired);
            ++g_generation;
        }
        g_workerCondition.notify_one();
    }

    status_t status()
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        return g_status;
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            if (!g_workerStarted)
                return;
            g_stopRequested = true;
            ++g_generation;
        }
        g_workerCondition.notify_one();
        if (g_worker.joinable())
            g_worker.join();
        std::lock_guard<std::mutex> lock(g_workerMutex);
        g_workerStarted = false;
        g_stopRequested = false;
        g_desired = {};
    }
}
