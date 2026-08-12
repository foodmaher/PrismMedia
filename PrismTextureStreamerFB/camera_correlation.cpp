#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "camera_correlation.h"
#include "camera_monitor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint32_t kFirstPhase = 1;
    constexpr uint32_t kLastPhase = 7;
    constexpr size_t kPhaseCount = 7;
    constexpr size_t kBlockBytes = 0x600;
    constexpr size_t kMaximumBlocks = 128;
    constexpr size_t kPointerScanBytes = 0x100;
    constexpr uint64_t kDispatchSampleIntervalMilliseconds = 1;
    constexpr uint64_t kBlockSampleIntervalMilliseconds = 50;
    constexpr uint64_t kRecentBlockMilliseconds = 1500;

    struct tracked_block_t
    {
        uintptr_t address{};
        prism_camera_monitor::CorrelationSource source{
            prism_camera_monitor::CorrelationSource::Unknown};
        uint32_t parentOffset{UINT32_MAX};
        std::array<uint8_t, kBlockBytes> latest{};
        std::array<std::array<uint8_t, kBlockBytes>, kPhaseCount> phase{};
        uint32_t phaseMask{};
        uint32_t sampleCount{};
        uint64_t lastSeenTick{};
        uint64_t lastSampleTick{};
    };

    std::atomic<bool> g_active{};
    std::atomic<bool> g_complete{};
    std::atomic<uint32_t> g_currentPhase{kFirstPhase};
    std::atomic<uint32_t> g_completedPhaseMask{};
    std::atomic<uint64_t> g_samples{};
    std::atomic<uint64_t> g_lastSampleTick{};
    std::atomic<uint64_t> g_lastPublishTick{};
    std::atomic<uint64_t> g_observedJobs{};
    std::atomic<uint64_t> g_rejectedSlot7Jobs{};
    std::atomic<bool> g_finished{};
    std::mutex g_mutex;
    std::vector<tracked_block_t> g_blocks;
    std::array<prism_camera_monitor::CorrelationCandidate,
        prism_camera_monitor::kMaximumCorrelationCandidates> g_candidates{};
    uint32_t g_candidateCount{};

    uint32_t phase_bit(uint32_t phase)
    {
        return phase >= kFirstPhase && phase <= kLastPhase
            ? 1U << (phase - 1)
            : 0;
    }

    uint32_t bit_count(uint32_t value)
    {
        uint32_t count{};
        while (value)
        {
            count += value & 1U;
            value >>= 1;
        }
        return count;
    }

    const char* phase_instruction(uint32_t phase)
    {
        switch (phase)
        {
        case 1:
            return "Cabin view: centre the camera, hide enlarged mirrors, "
                "keep the game moving, then click Capture and continue.";
        case 2:
            return "Stay in the cabin and rotate the player camera far LEFT. "
                "Hold it still, then click Capture and continue.";
        case 3:
            return "Rotate the cabin camera far RIGHT. Hold it still, then "
                "click Capture and continue.";
        case 4:
            return "Switch to an EXTERIOR player camera. Keep the view still, "
                "then click Capture and continue.";
        case 5:
            return "Return to the cabin and display the enlarged LEFT mirror. "
                "Then click Capture and continue.";
        case 6:
            return "Display the enlarged RIGHT mirror instead. Then click "
                "Capture and continue.";
        case 7:
            return "Return to the centred cabin view with enlarged mirrors "
                "hidden. Click Capture and analyse.";
        default:
            return "Correlation capture is complete.";
        }
    }

    const char* source_name(
        prism_camera_monitor::CorrelationSource source)
    {
        switch (source)
        {
        case prism_camera_monitor::CorrelationSource::CameraInput:
            return "camera";
        case prism_camera_monitor::CorrelationSource::CameraPointer:
            return "camera-ptr";
        case prism_camera_monitor::CorrelationSource::RenderRequest:
            return "request";
        case prism_camera_monitor::CorrelationSource::RequestPointer:
            return "request-ptr";
        case prism_camera_monitor::CorrelationSource::RenderCommand:
            return "command";
        case prism_camera_monitor::CorrelationSource::CommandPointer:
            return "command-ptr";
        default:
            return "unknown";
        }
    }

    bool readable_region(
        const void* address, size_t size, bool requireWritable)
    {
        if (!address || size == 0)
            return false;
        MEMORY_BASIC_INFORMATION information{};
        if (VirtualQuery(address, &information, sizeof(information)) == 0 ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const DWORD protection = information.Protect & 0xFFU;
        const bool readable =
            protection == PAGE_READONLY ||
            protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        const bool writable =
            protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE ||
            protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (requireWritable && !writable))
            return false;
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        const uintptr_t regionStart =
            reinterpret_cast<uintptr_t>(information.BaseAddress);
        const uintptr_t regionEnd = regionStart + information.RegionSize;
        return start >= regionStart && start <= regionEnd &&
            size <= regionEnd - start;
    }

    bool safe_copy(const void* source, void* destination, size_t size)
    {
        if (!readable_region(source, size, false))
            return false;
        __try
        {
            std::memcpy(destination, source, size);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            std::memset(destination, 0, size);
            return false;
        }
    }

    bool plausible_pointer(uintptr_t value)
    {
        return value >= 0x10000ULL &&
            value < 0x0000800000000000ULL &&
            (value & (sizeof(void*) - 1)) == 0;
    }

    tracked_block_t* find_or_add_block(
        uintptr_t address,
        prism_camera_monitor::CorrelationSource source,
        uint32_t parentOffset)
    {
        for (auto& block : g_blocks)
        {
            if (block.address == address && block.source == source &&
                block.parentOffset == parentOffset)
                return &block;
        }
        if (g_blocks.size() >= kMaximumBlocks)
            return nullptr;
        g_blocks.emplace_back();
        auto& block = g_blocks.back();
        block.address = address;
        block.source = source;
        block.parentOffset = parentOffset;
        return &block;
    }

    tracked_block_t* sample_block(
        void* address,
        prism_camera_monitor::CorrelationSource source,
        uint64_t now,
        bool requireWritable,
        uint32_t parentOffset = UINT32_MAX)
    {
        if (!address || !readable_region(address, kBlockBytes, requireWritable))
            return nullptr;
        auto* block = find_or_add_block(
            reinterpret_cast<uintptr_t>(address), source, parentOffset);
        if (!block)
            return nullptr;
        if (block->lastSampleTick != 0 &&
            now >= block->lastSampleTick &&
            now - block->lastSampleTick < kBlockSampleIntervalMilliseconds)
        {
            block->lastSeenTick = now;
            return block;
        }
        if (!safe_copy(
                address, block->latest.data(), block->latest.size()))
        {
            return nullptr;
        }
        block->lastSeenTick = now;
        block->lastSampleTick = now;
        ++block->sampleCount;
        g_samples.fetch_add(1, std::memory_order_relaxed);
        return block;
    }

    void sample_pointer_targets(
        const tracked_block_t* parent,
        prism_camera_monitor::CorrelationSource pointerSource,
        uint64_t now)
    {
        if (!parent)
            return;
        uint32_t accepted{};
        for (size_t offset = 0;
            offset + sizeof(uintptr_t) <= kPointerScanBytes && accepted < 6;
            offset += sizeof(uintptr_t))
        {
            uintptr_t value{};
            std::memcpy(
                &value, parent->latest.data() + offset, sizeof(value));
            if (!plausible_pointer(value))
                continue;
            if (sample_block(
                    reinterpret_cast<void*>(value),
                    pointerSource, now, true,
                    static_cast<uint32_t>(offset)))
            {
                ++accepted;
            }
        }
    }

    bool valid_float(float value)
    {
        return std::isfinite(value) && std::fabs(value) <= 1000000.0f;
    }

    float float4_difference(const float* left, const float* right)
    {
        float difference{};
        for (uint32_t index = 0; index < 4; ++index)
            difference = (std::max)(
                difference, std::fabs(left[index] - right[index]));
        return difference;
    }

    bool looks_like_quaternion(const float* values)
    {
        float norm{};
        for (uint32_t index = 0; index < 4; ++index)
        {
            if (!valid_float(values[index]) ||
                std::fabs(values[index]) > 1.2f)
            {
                return false;
            }
            norm += values[index] * values[index];
        }
        return norm >= 0.75f && norm <= 1.25f;
    }

    bool looks_like_matrix(const uint8_t* bytes, size_t offset)
    {
        if (offset + sizeof(float) * 16 > kBlockBytes)
            return false;
        std::array<float, 16> matrix{};
        std::memcpy(
            matrix.data(), bytes + offset, sizeof(float) * matrix.size());
        const auto* values = matrix.data();
        uint32_t meaningful{};
        for (uint32_t index = 0; index < 16; ++index)
        {
            if (!valid_float(values[index]) ||
                std::fabs(values[index]) > 10000.0f)
            {
                return false;
            }
            if (std::fabs(values[index]) > 0.0001f)
                ++meaningful;
        }
        if (meaningful < 6)
            return false;
        for (uint32_t row = 0; row < 3; ++row)
        {
            float norm{};
            for (uint32_t column = 0; column < 3; ++column)
            {
                const float value = values[row * 4 + column];
                norm += value * value;
            }
            if (norm < 0.20f || norm > 4.0f)
                return false;
        }
        return true;
    }

    uint32_t first_phase_index(uint32_t mask)
    {
        for (uint32_t index = 0; index < kPhaseCount; ++index)
            if ((mask & (1U << index)) != 0)
                return index;
        return 0;
    }

    void analyse_locked()
    {
        std::vector<prism_camera_monitor::CorrelationCandidate> ranked;
        for (const auto& block : g_blocks)
        {
            if (bit_count(block.phaseMask) < 2)
                continue;
            const uint32_t referenceIndex =
                first_phase_index(block.phaseMask);
            for (size_t offset = 0;
                offset + sizeof(float) * 4 <= kBlockBytes;
                offset += sizeof(float))
            {
                std::array<std::array<float, 4>, kPhaseCount> values{};
                bool valid = true;
                bool meaningful{};
                uint32_t changedMask{};
                uint32_t quaternionPhases{};
                for (uint32_t phase = 0; phase < kPhaseCount; ++phase)
                {
                    if ((block.phaseMask & (1U << phase)) == 0)
                        continue;
                    std::memcpy(
                        values[phase].data(),
                        block.phase[phase].data() + offset,
                        sizeof(float) * 4);
                    for (float value : values[phase])
                    {
                        if (!valid_float(value))
                            valid = false;
                        if (std::fabs(value) > 0.00001f)
                            meaningful = true;
                    }
                    if (looks_like_quaternion(values[phase].data()))
                        ++quaternionPhases;
                    if (float4_difference(
                            values[referenceIndex].data(),
                            values[phase].data()) > 0.0005f)
                    {
                        changedMask |= 1U << phase;
                    }
                }
                if (!valid || !meaningful || changedMask == 0)
                    continue;

                const uint32_t capturedCount = bit_count(block.phaseMask);
                const uint32_t changedCount = bit_count(changedMask);
                float score = static_cast<float>(
                    capturedCount * 2 + changedCount * 3);
                const bool quaternion =
                    quaternionPhases == capturedCount;
                if (quaternion)
                    score += 12.0f;
                const bool matrix = (offset % 16) == 0 &&
                    looks_like_matrix(
                        block.phase[referenceIndex].data(), offset);
                if (matrix)
                    score += 10.0f;
                if ((block.phaseMask & 0x07U) == 0x07U &&
                    float4_difference(
                        values[1].data(), values[2].data()) > 0.001f)
                {
                    score += 6.0f;
                }
                if ((block.phaseMask & 0x41U) == 0x41U &&
                    float4_difference(
                        values[0].data(), values[6].data()) < 0.01f)
                {
                    score += 8.0f;
                }

                prism_camera_monitor::CorrelationCandidate candidate{};
                candidate.address = block.address;
                candidate.offset = static_cast<uint32_t>(offset);
                candidate.sourceKind =
                    static_cast<uint32_t>(block.source) |
                    ((block.parentOffset == UINT32_MAX
                        ? 0xFFFFFFU : block.parentOffset) << 8);
                candidate.score = score;
                candidate.changedPhaseMask = changedMask;
                candidate.capturedPhaseMask = block.phaseMask;
                candidate.sampleCount = block.sampleCount;
                uint32_t latestIndex = referenceIndex;
                for (uint32_t phase = 0; phase < kPhaseCount; ++phase)
                    if ((block.phaseMask & (1U << phase)) != 0)
                        latestIndex = phase;
                std::memcpy(
                    candidate.values, values[latestIndex].data(),
                    sizeof(candidate.values));
                std::snprintf(
                    candidate.label, sizeof(candidate.label), "%s %s",
                    source_name(block.source),
                    quaternion ? "quat" : matrix ? "matrix" : "float4");
                ranked.push_back(candidate);
            }
        }

        std::sort(
            ranked.begin(), ranked.end(),
            [](const auto& left, const auto& right)
            {
                if (left.score != right.score)
                    return left.score > right.score;
                if (left.address != right.address)
                    return left.address < right.address;
                if (left.offset != right.offset)
                    return left.offset < right.offset;
                return left.sourceKind < right.sourceKind;
            });

        g_candidates.fill(
            prism_camera_monitor::CorrelationCandidate{});
        g_candidateCount = 0;
        for (const auto& candidate : ranked)
        {
            bool overlaps{};
            for (uint32_t index = 0; index < g_candidateCount; ++index)
            {
                const auto& selected = g_candidates[index];
                const uint32_t distance = selected.offset > candidate.offset
                    ? selected.offset - candidate.offset
                    : candidate.offset - selected.offset;
                if (selected.address == candidate.address && distance < 16)
                {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps)
                continue;
            g_candidates[g_candidateCount++] = candidate;
            if (g_candidateCount >= g_candidates.size())
                break;
        }
    }

    void publish_guidance(const char* detail)
    {
        const uint32_t phase = g_currentPhase.load(
            std::memory_order_relaxed);
        camera_monitor::publish_correlation(
            prism_camera_monitor::Stage::GuidedCorrelation,
            prism_camera_monitor::kPluginConnected |
                prism_camera_monitor::kSlot7Disabled |
                prism_camera_monitor::kDiagnosticRunning |
                prism_camera_monitor::kCorrelationActive,
            static_cast<prism_camera_monitor::CorrelationPhase>(phase),
            g_completedPhaseMask.load(std::memory_order_relaxed),
            g_samples.load(std::memory_order_relaxed),
            "Guided camera-memory correlation",
            detail,
            phase_instruction(phase),
            nullptr, 0,
            g_observedJobs.load(std::memory_order_relaxed),
            g_rejectedSlot7Jobs.load(std::memory_order_relaxed));
    }

    void publish_results_locked()
    {
        char detail[512]{};
        std::snprintf(
            detail, sizeof(detail),
            "Correlation complete: %u ranked candidate blocks from %zu "
            "tracked readable objects and %llu bounded samples. These are "
            "read-only leads; no game or camera memory was modified.",
            g_candidateCount, g_blocks.size(),
            static_cast<unsigned long long>(
                g_samples.load(std::memory_order_relaxed)));
        const bool found = g_candidateCount != 0;
        camera_monitor::publish_correlation(
            found
                ? prism_camera_monitor::Stage::CorrelationReady
                : prism_camera_monitor::Stage::Blocked,
            prism_camera_monitor::kPluginConnected |
                prism_camera_monitor::kSlot7Disabled |
                (found ? prism_camera_monitor::kCandidatesAvailable : 0U),
            prism_camera_monitor::CorrelationPhase::AnalysisComplete,
            g_completedPhaseMask.load(std::memory_order_relaxed),
            g_samples.load(std::memory_order_relaxed),
            found ? "Camera-memory candidates ranked"
                : "No correlated camera-memory candidates",
            detail,
            found
                ? "Review the ranked table and save the report. The next "
                    "build can validate the highest-scoring offsets."
                : "No candidate changed consistently across the guided phases. "
                    "A deeper constructor/call-site trace is required.",
            g_candidates.data(), g_candidateCount,
            g_observedJobs.load(std::memory_order_relaxed),
            g_rejectedSlot7Jobs.load(std::memory_order_relaxed));
    }
}

namespace camera_correlation
{
    void begin()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_blocks.clear();
        g_blocks.reserve(kMaximumBlocks);
        g_candidates.fill(
            prism_camera_monitor::CorrelationCandidate{});
        g_candidateCount = 0;
        g_samples.store(0, std::memory_order_relaxed);
        g_lastSampleTick.store(0, std::memory_order_relaxed);
        g_lastPublishTick.store(0, std::memory_order_relaxed);
        g_observedJobs.store(0, std::memory_order_relaxed);
        g_rejectedSlot7Jobs.store(0, std::memory_order_relaxed);
        g_finished.store(false, std::memory_order_relaxed);
        g_completedPhaseMask.store(0, std::memory_order_relaxed);
        g_currentPhase.store(kFirstPhase, std::memory_order_relaxed);
        g_complete.store(false, std::memory_order_release);
        g_active.store(true, std::memory_order_release);
        publish_guidance(
            "Seven read-only snapshots will be correlated. Complete each "
            "instruction in the game, hold the view still, then capture it.");
    }

    void stop()
    {
        g_active.store(false, std::memory_order_release);
    }

    void observe(
        void* cameraInput,
        void* renderRequest,
        void* renderCommand)
    {
        if (!g_active.load(std::memory_order_acquire))
            return;
        const uint64_t now = GetTickCount64();
        uint64_t previous = g_lastSampleTick.load(std::memory_order_relaxed);
        if (previous != 0 && now >= previous &&
            now - previous < kDispatchSampleIntervalMilliseconds)
        {
            return;
        }
        if (!g_lastSampleTick.compare_exchange_strong(
                previous, now, std::memory_order_acq_rel))
        {
            return;
        }

        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        auto* camera = sample_block(
            cameraInput,
            prism_camera_monitor::CorrelationSource::CameraInput,
            now, true);
        auto* request = sample_block(
            renderRequest,
            prism_camera_monitor::CorrelationSource::RenderRequest,
            now, true);
        auto* command = sample_block(
            renderCommand,
            prism_camera_monitor::CorrelationSource::RenderCommand,
            now, true);
        sample_pointer_targets(
            camera,
            prism_camera_monitor::CorrelationSource::CameraPointer,
            now);
        sample_pointer_targets(
            request,
            prism_camera_monitor::CorrelationSource::RequestPointer,
            now);
        sample_pointer_targets(
            command,
            prism_camera_monitor::CorrelationSource::CommandPointer,
            now);
    }

    bool capture_phase(uint32_t requestedPhase)
    {
        if (!g_active.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint32_t current = g_currentPhase.load(
            std::memory_order_relaxed);
        if (requestedPhase != current ||
            current < kFirstPhase || current > kLastPhase)
        {
            publish_guidance(
                "The phase request was stale. Follow the current instruction "
                "and press Capture and continue again.");
            return false;
        }

        const uint64_t now = GetTickCount64();
        uint32_t capturedBlocks{};
        for (auto& block : g_blocks)
        {
            if (block.lastSeenTick == 0 || now < block.lastSeenTick ||
                now - block.lastSeenTick > kRecentBlockMilliseconds)
            {
                continue;
            }
            block.phase[current - 1] = block.latest;
            block.phaseMask |= phase_bit(current);
            ++capturedBlocks;
        }
        if (capturedBlocks == 0)
        {
            publish_guidance(
                "No recent readable camera objects were sampled. Resume the "
                "game for a moment, keep the requested view visible, and retry.");
            return false;
        }

        g_completedPhaseMask.fetch_or(
            phase_bit(current), std::memory_order_relaxed);
        if (current < kLastPhase)
        {
            g_currentPhase.store(current + 1, std::memory_order_relaxed);
            char detail[256]{};
            std::snprintf(
                detail, sizeof(detail),
                "Phase %u captured from %u recent readable objects. Prepare "
                "the next view; slot 7 remains rejected.",
                current, capturedBlocks);
            publish_guidance(detail);
            return false;
        }

        analyse_locked();
        g_currentPhase.store(
            static_cast<uint32_t>(
                prism_camera_monitor::CorrelationPhase::AnalysisComplete),
            std::memory_order_relaxed);
        g_complete.store(true, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
        g_finished.store(true, std::memory_order_release);
        publish_results_locked();
        return true;
    }

    void tick(uint64_t observedJobs, uint64_t rejectedSlot7Jobs)
    {
        g_observedJobs.store(observedJobs, std::memory_order_relaxed);
        g_rejectedSlot7Jobs.store(
            rejectedSlot7Jobs, std::memory_order_relaxed);
        if (!g_active.load(std::memory_order_acquire))
            return;
        const uint64_t now = GetTickCount64();
        uint64_t previous = g_lastPublishTick.load(
            std::memory_order_relaxed);
        if (previous != 0 && now >= previous && now - previous < 250)
            return;
        if (!g_lastPublishTick.compare_exchange_strong(
                previous, now, std::memory_order_acq_rel))
        {
            return;
        }
        publish_guidance(
            "Sampling readable camera, request, command, and one-pointer-depth "
            "objects. Hold the requested view still before capturing.");
    }

    void finish(bool timedOut)
    {
        if (g_finished.exchange(true, std::memory_order_acq_rel))
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_active.store(false, std::memory_order_release);
        if (g_complete.load(std::memory_order_acquire))
        {
            publish_results_locked();
            return;
        }
        char detail[512]{};
        std::snprintf(
            detail, sizeof(detail),
            "%s after phases mask 0x%02X with %llu bounded samples. "
            "Restart the guided diagnosis and complete all seven captures.",
            timedOut ? "Guided correlation timed out" :
                "Guided correlation stopped",
            g_completedPhaseMask.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(
                g_samples.load(std::memory_order_relaxed)));
        camera_monitor::publish_correlation(
            prism_camera_monitor::Stage::Blocked,
            prism_camera_monitor::kPluginConnected |
                prism_camera_monitor::kSlot7Disabled,
            static_cast<prism_camera_monitor::CorrelationPhase>(
                g_currentPhase.load(std::memory_order_relaxed)),
            g_completedPhaseMask.load(std::memory_order_relaxed),
            g_samples.load(std::memory_order_relaxed),
            "Guided correlation incomplete",
            detail,
            phase_instruction(
                g_currentPhase.load(std::memory_order_relaxed)),
            nullptr, 0,
            g_observedJobs.load(std::memory_order_relaxed),
            g_rejectedSlot7Jobs.load(std::memory_order_relaxed));
    }

    bool active()
    {
        return g_active.load(std::memory_order_acquire);
    }

    bool complete()
    {
        return g_complete.load(std::memory_order_acquire);
    }

    uint32_t candidate_count()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_candidateCount;
    }
}
