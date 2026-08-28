#define NOMINMAX
#include "custom_render_probe.h"

#include "bmem.h"
#include "diagnostic_log.h"
#include "dx11/CreateTexture2D.h"

#include <Windows.h>
#include <d3d11.h>
#include <MinHook/MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr uint32_t kMaximumEvents = 2048;
    constexpr uint32_t kMaximumUniqueSignatures = 48;
    // Mod-heavy truck reloads can take more than 20 seconds before the exact
    // custom TOBJ is recreated. The breakpoint is still bounded by the event
    // limit and restores the original JE itself after the final event.
    constexpr uint64_t kProbeTimeoutMilliseconds = 60000;
    constexpr uint32_t kStackWords = 8;
    constexpr uint32_t kObjectScanWords = 64;
    constexpr uint32_t kBranchBytesBefore = 64;
    // Include the full loop body and its native advance/backedge so a failed
    // runtime verification can be diagnosed from this same test.
    constexpr uint32_t kBranchBytesAfter = 160;
    constexpr uint32_t kBranchWindowBytes =
        kBranchBytesBefore + kBranchBytesAfter;
    constexpr uint32_t kMaximumCorrelationSamples = 12;
    constexpr uint32_t kTargetCorrelationSamples = 6;
    constexpr uint32_t kMaximumTrackedSrvs = 8;
    constexpr uint32_t kStackFrames = 12;
    constexpr uint32_t kBranchLookbackEvents = 32;
    constexpr uint64_t kPostMatchTimeoutMilliseconds = 10000;
    constexpr uint64_t kPendingDrawMaximumMicroseconds = 10000;
    // The instruction immediately after the loop setup is `mov r9,rsi`.
    // Probing it exposes the actual list slot passed to the game call instead
    // of the stale R9 value that exists at the earlier empty-list JE.
    constexpr uint32_t kLoopInstructionOffset = 23;
    constexpr uint32_t kLoopInstructionBytes = 3;
    constexpr uint32_t kMaximumLoopEvents = 256;
    constexpr uint32_t kLoopSlotWords = 4;
    constexpr uint32_t kLoopObjectWords = 32;
    constexpr uint32_t kLoopComparisonWords = 16;
    // A standard COM Release hook is correlated with the already-stable list
    // breakpoint. No private game cleanup function is detoured.
    constexpr uint32_t kMaximumReleaseSamples = 16;
    constexpr uint64_t kDefaultReleaseScopeWindowMicroseconds = 250000;
    constexpr uint64_t kMinimumReleaseScopeWindowMicroseconds = 1000;
    constexpr uint64_t kMaximumReleaseScopeWindowMicroseconds = 5000000;

    enum register_index_t : uint32_t
    {
        reg_rax,
        reg_rbx,
        reg_rcx,
        reg_rdx,
        reg_rsi,
        reg_rdi,
        reg_rbp,
        reg_rsp,
        reg_r8,
        reg_r9,
        reg_r10,
        reg_r11,
        reg_r12,
        reg_r13,
        reg_r14,
        reg_r15,
        register_count
    };

    struct probe_event_t
    {
        uint64_t registers[register_count]{};
        uint64_t stack[kStackWords]{};
        uint64_t tick{};
        uint64_t qpc{};
        uint32_t threadId{};
        uint8_t branchTaken{};
        uint8_t emulatedTaken{};
    };

    struct unique_signature_t
    {
        probe_event_t event{};
        uint32_t hits{};
        uint32_t beforeTextureHits{};
        uint32_t afterTextureHits{};
    };

    struct texture_path_t
    {
        const char* registerName = "none";
        uint64_t object{};
        int32_t directOffset = -1;
        int32_t childSlotOffset = -1;
        int32_t childTextureOffset = -1;
    };

    struct correlation_sample_t
    {
        probe_event_t branch{};
        uint64_t bindQpc{};
        uint64_t drawQpc{};
        uint64_t r9Hash{};
        uint64_t rsiHash{};
        uint64_t r14Hash{};
        uint64_t bindStack[kStackFrames]{};
        uint64_t drawStack[kStackFrames]{};
        uint32_t branchIndex{ UINT32_MAX };
        uint32_t threadId{};
        uint32_t startSlot{};
        uint32_t matchedSlot{};
        uint16_t bindStackCount{};
        uint16_t drawStackCount{};
        uint8_t matchedByResourceInspection{};
        char drawKind[24]{};
    };

    enum class loop_candidate_t : uint8_t
    {
        none,
        slot,
        entry,
        slot_next,
        entry_word
    };

    struct loop_event_t
    {
        probe_event_t event{};
        uint64_t slotWords[kLoopSlotWords]{};
        uint64_t objectWords[kLoopObjectWords]{};
        uint64_t matchObjectWords[kLoopComparisonWords]{};
        uint64_t drawObjectWords[kLoopComparisonWords]{};
        uint64_t objectBase{};
        uint64_t captureSlotHash{};
        uint64_t captureObjectHash{};
        uint64_t matchSlotHash{};
        uint64_t matchObjectHash{};
        uint64_t drawSlotHash{};
        uint64_t drawObjectHash{};
        uint32_t branchIndex{ UINT32_MAX };
        uint32_t listOrdinal{ UINT32_MAX };
        int32_t directOffset{ -1 };
        int32_t childSlotOffset{ -1 };
        int32_t childTextureOffset{ -1 };
        uint16_t candidateIndex{};
        loop_candidate_t candidate{ loop_candidate_t::none };
        int32_t oldPathFirstOffset{ -1 };
        int32_t oldPathSecondOffset{ -1 };
        int32_t oldPathThirdOffset{ -1 };
        uint8_t oldPathDepth{};
        uint8_t oldPathTargetKind{};
        uint8_t selectivelySkipped{};
    };

    struct release_sample_t
    {
        uint64_t releaseThis{};
        uint64_t qpc{};
        uint64_t loopQpc{};
        uint64_t stack[kStackFrames]{};
        uint32_t loopIndex{ UINT32_MAX };
        uint32_t candidateLoopIndex{ UINT32_MAX };
        uint32_t threadId{};
        int64_t loopDeltaMicroseconds{ -1 };
        uint16_t stackCount{};
        uint8_t scopeValidated{};
    };

    using release_t = ULONG(__stdcall*)(IUnknown*);

    uint64_t g_branchAddress{};
    int32_t g_originalRelativeDisplacement{};
    uint64_t g_takenAddress{};
    uint64_t g_fallthroughAddress{};
    uint64_t g_loopProbeAddress{};
    uint64_t g_loopAdvanceAddress{};
    void* g_releaseTarget{};
    release_t g_releaseOriginal{};
    DWORD g_originalProtection{};
    PVOID g_exceptionHandler{};
    bool g_resolutionFailed{};
    bool g_patchEnabled{};
    std::atomic<bool> g_probeInstalled{};
    std::atomic<bool> g_captureCompleted{};
    std::atomic<bool> g_captureDataReady{};
    std::atomic<bool> g_textureReady{};
    std::atomic<bool> g_resultsLogged{};
    uint64_t g_probeStartedTick{};
    std::atomic<bool> g_captureReserved{};
    std::atomic<bool> g_trapArmed{};
    std::atomic<bool> g_trapCompleted{};
    std::atomic<uint32_t> g_activeHandlers{};
    std::atomic<uint32_t> g_eventCount{};
    std::atomic<uint32_t> g_completedEventCount{};
    std::atomic<uint32_t> g_latestBranchIndex{ UINT32_MAX };
    std::atomic<uint32_t> g_loopEventCount{};
    std::atomic<uint32_t> g_completedLoopEventCount{};
    std::atomic<uintptr_t> g_selectedTexture{};
    std::atomic<uintptr_t> g_previousTexture{};
    std::atomic<uintptr_t> g_previousTextureIdentity{};
    std::atomic<uintptr_t> g_selectedTextureIdentity{};
    std::atomic<uint64_t> g_textureMatchedTick{};
    std::atomic<uint64_t> g_textureMatchedQpc{};
    std::atomic<bool> g_dxCorrelationActive{};
    std::atomic<bool> g_dxCorrelationComplete{};
    std::atomic<uint32_t> g_dxCallbacks{};
    std::atomic<uint32_t> g_selectedSrvCount{};
    std::array<std::atomic<uintptr_t>, kMaximumTrackedSrvs> g_selectedSrvs{};
    std::atomic<uint32_t> g_resourceInspections{};
    std::atomic<uint32_t> g_correlationSampleCount{};
    std::atomic<uint32_t> g_drawCorrelationCount{};
    std::atomic<uint32_t> g_oldTexturePathCount{};
    std::atomic<uint32_t> g_selectiveSkipCount{};
    std::atomic<bool> g_releaseHookActive{};
    std::atomic<uint32_t> g_releaseCallbacks{};
    std::atomic<uint32_t> g_releaseSampleCount{};
    std::atomic<uint32_t> g_exactOldReleaseCount{};
    std::atomic<uint32_t> g_unscopedOldReleaseCount{};
    std::atomic<uint32_t> g_validatedOldReleaseCount{};
    std::atomic<uint64_t> g_releaseScopeWindowMicroseconds{
        kDefaultReleaseScopeWindowMicroseconds };
    std::atomic<custom_render_probe::fallback_mode_t> g_fallbackMode{
        custom_render_probe::fallback_mode_t::automatic };
    std::atomic<bool> g_loopMatchSnapshotTaken{};
    std::atomic<bool> g_loopDrawSnapshotTaken{};
    std::array<correlation_sample_t, kMaximumCorrelationSamples>
        g_correlationSamples{};
    std::array<std::atomic<bool>, kMaximumCorrelationSamples>
        g_correlationSampleReady{};
    std::array<std::atomic<bool>, kMaximumCorrelationSamples>
        g_drawSampleReady{};
    std::array<probe_event_t, kMaximumEvents> g_events{};
    std::array<loop_event_t, kMaximumLoopEvents> g_loopEvents{};
    std::array<std::atomic<bool>, kMaximumLoopEvents> g_loopEventReady{};
    std::array<release_sample_t, kMaximumReleaseSamples> g_releaseSamples{};
    std::array<std::atomic<bool>, kMaximumReleaseSamples>
        g_releaseSampleReady{};
    char g_selectedDisplayId[96]{};
    char g_selectedOriginalTexture[320]{};

    thread_local uint32_t t_pendingCorrelationSample = UINT32_MAX;
    thread_local uint32_t t_activeLoopIndex = UINT32_MAX;
    thread_local uint64_t t_activeLoopQpc{};

    int64_t qpc_delta_microseconds(uint64_t later, uint64_t earlier);

    uint64_t performance_counter()
    {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<uint64_t>(counter.QuadPart);
    }

    void log_branch_code_window()
    {
        const uint64_t moduleStart = bmem::moduleBase;
        const uint64_t moduleEnd = moduleStart + bmem::moduleSize;
        if (moduleStart == 0 || moduleEnd <= moduleStart)
            return;

        const uint64_t requestedStart =
            g_branchAddress > kBranchBytesBefore
                ? g_branchAddress - kBranchBytesBefore
                : moduleStart;
        const uint64_t start = (std::max)(requestedStart, moduleStart);
        const uint64_t end = (std::min)(
            g_branchAddress + kBranchBytesAfter, moduleEnd);

        char bytesText[kBranchWindowBytes * 3 + 1]{};
        size_t cursor{};
        const uint32_t count = static_cast<uint32_t>(end - start);
        bool readable = true;
        __try
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(start);
            for (uint32_t index = 0; index < count; ++index)
            {
                const int written = std::snprintf(
                    bytesText + cursor,
                    sizeof(bytesText) - cursor,
                    "%02X%s",
                    static_cast<unsigned>(bytes[index]),
                    index + 1 == count ? "" : " ");
                if (written <= 0)
                {
                    readable = false;
                    break;
                }
                cursor += static_cast<size_t>(written);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            readable = false;
        }

        // One queue entry prevents the critical instruction immediately
        // before the JE from being lost when several diagnostic lines arrive
        // in the same millisecond.
        diagnostic_log::writef_important(
            "probe",
            "branch-code start=0x%llX branchOffset=%llu bytes=%s",
            static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(g_branchAddress - start),
            readable ? bytesText : "<unreadable>");
    }

    bool resolve_branch()
    {
        if (g_branchAddress != 0)
            return true;
        if (g_resolutionFailed)
            return false;

        g_branchAddress = bmem::patternScan(
            "0F 84 ?? ?? ?? ?? 45 84 E4 4D 0F 45 FD");
        if (g_branchAddress == 0)
        {
            g_branchAddress = bmem::patternScan(
                "90 E9 ?? ?? ?? ?? 45 84 E4 4D 0F 45 FD");
        }
        if (g_branchAddress == 0)
        {
            diagnostic_log::write(
                "error",
                "Could not resolve the custom-display render branch; "
                "the per-instance diagnostic probe and compatibility "
                "fallback are unavailable.");
            g_resolutionFailed = true;
            return false;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(g_branchAddress);
        std::memcpy(
            &g_originalRelativeDisplacement,
            bytes + 2,
            sizeof(g_originalRelativeDisplacement));
        g_fallthroughAddress = g_branchAddress + 6;
        g_takenAddress = g_fallthroughAddress +
            static_cast<int64_t>(g_originalRelativeDisplacement);
        diagnostic_log::writef(
            "probe",
            "Resolved custom render branch at 0x%llX (taken=0x%llX, "
            "fallthrough=0x%llX).",
            static_cast<unsigned long long>(g_branchAddress),
            static_cast<unsigned long long>(g_takenAddress),
            static_cast<unsigned long long>(g_fallthroughAddress));
        log_branch_code_window();
        return true;
    }

    bool resolve_loop_probe()
    {
        if (g_loopProbeAddress != 0)
            return true;
        if (!resolve_branch())
            return false;

        const uint64_t candidate =
            g_branchAddress + kLoopInstructionOffset;
        const auto* bytes = reinterpret_cast<const uint8_t*>(candidate);
        bool expected = false;
        __try
        {
            expected = bytes[0] == 0x4C && bytes[1] == 0x8B &&
                bytes[2] == 0xCE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            expected = false;
        }
        if (!expected)
        {
            diagnostic_log::writef_important(
                "error",
                "Could not arm the list-entry probe: expected mov r9,rsi "
                "at branch+%u (0x%llX). The compatibility fallback was "
                "left available.",
                kLoopInstructionOffset,
                static_cast<unsigned long long>(candidate));
            return false;
        }
        g_loopProbeAddress = candidate;
        g_loopAdvanceAddress = 0;
        __try
        {
            const uint64_t searchStart =
                candidate + kLoopInstructionBytes;
            const uint64_t searchEnd = g_takenAddress;
            for (uint64_t address = searchStart;
                address + 9 <= searchEnd; ++address)
            {
                const auto* instruction =
                    reinterpret_cast<const uint8_t*>(address);
                const bool advancesRsiByEight =
                    (instruction[0] == 0x48 &&
                     instruction[1] == 0x83 &&
                     instruction[2] == 0xC6 &&
                     instruction[3] == 0x08) ||
                    (instruction[0] == 0x48 &&
                     instruction[1] == 0x8D &&
                     instruction[2] == 0x76 &&
                     instruction[3] == 0x08);
                if (!advancesRsiByEight)
                    continue;

                const uint64_t compareEnd = (std::min)(
                    address + 20, searchEnd);
                for (uint64_t compare = address + 4;
                    compare + 5 <= compareEnd; ++compare)
                {
                    const auto* compareBytes =
                        reinterpret_cast<const uint8_t*>(compare);
                    if (compareBytes[0] != 0x49 ||
                        compareBytes[1] != 0x3B ||
                        compareBytes[2] != 0xF6)
                    {
                        continue;
                    }

                    uint64_t jumpTarget{};
                    if (compareBytes[3] == 0x75)
                    {
                        jumpTarget = compare + 5 +
                            static_cast<int8_t>(compareBytes[4]);
                    }
                    else if (compare + 9 <= searchEnd &&
                        compareBytes[3] == 0x0F &&
                        compareBytes[4] == 0x85)
                    {
                        int32_t displacement{};
                        std::memcpy(
                            &displacement, compareBytes + 5,
                            sizeof(displacement));
                        jumpTarget = compare + 9 +
                            static_cast<int64_t>(displacement);
                    }
                    if (jumpTarget >= g_fallthroughAddress &&
                        jumpTarget <= g_loopProbeAddress)
                    {
                        g_loopAdvanceAddress = address;
                        break;
                    }
                }
                if (g_loopAdvanceAddress != 0)
                    break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_loopAdvanceAddress = 0;
        }
        diagnostic_log::writef_important(
            "probe",
            "Resolved list-entry probe at 0x%llX (branch+%u, "
            "instruction=mov r9,rsi, nativeAdvance=0x%llX).",
            static_cast<unsigned long long>(g_loopProbeAddress),
            kLoopInstructionOffset,
            static_cast<unsigned long long>(g_loopAdvanceAddress));
        if (g_loopAdvanceAddress == 0)
        {
            diagnostic_log::write_important(
                "probe",
                "Native RSI loop-advance sequence was not verified; this "
                "run will capture old-texture paths but will not skip any "
                "entry. The compatibility fallback remains available.");
        }
        return true;
    }

    ULONG __stdcall hooked_release(IUnknown* object)
    {
        if (!g_releaseHookActive.load(std::memory_order_acquire))
            return g_releaseOriginal(object);

        g_releaseCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const uintptr_t pointer = reinterpret_cast<uintptr_t>(object);
        const bool exactOld = pointer != 0 &&
            (pointer == g_previousTexture.load(std::memory_order_acquire) ||
             pointer == g_previousTextureIdentity.load(
                 std::memory_order_acquire));
        if (exactOld)
        {
            g_exactOldReleaseCount.fetch_add(1, std::memory_order_acq_rel);
            const uint32_t sampleIndex = g_releaseSampleCount.fetch_add(
                1, std::memory_order_acq_rel);
            if (sampleIndex < kMaximumReleaseSamples)
            {
                release_sample_t& sample = g_releaseSamples[sampleIndex];
                sample = {};
                sample.releaseThis = pointer;
                sample.qpc = performance_counter();
                sample.loopQpc = t_activeLoopQpc;
                sample.candidateLoopIndex = t_activeLoopIndex;
                sample.threadId = GetCurrentThreadId();
                sample.loopDeltaMicroseconds =
                    sample.candidateLoopIndex != UINT32_MAX &&
                    sample.loopQpc != 0
                        ? qpc_delta_microseconds(
                            sample.qpc, sample.loopQpc)
                        : -1;
                const uint64_t scopeWindow =
                    g_releaseScopeWindowMicroseconds.load(
                        std::memory_order_acquire);
                const bool scopeValid =
                    sample.candidateLoopIndex != UINT32_MAX &&
                    sample.loopDeltaMicroseconds >= 0 &&
                    static_cast<uint64_t>(sample.loopDeltaMicroseconds) <=
                        scopeWindow;
                sample.loopIndex = scopeValid
                    ? sample.candidateLoopIndex : UINT32_MAX;
                sample.scopeValidated = scopeValid ? 1 : 0;
                if (scopeValid)
                {
                    g_validatedOldReleaseCount.fetch_add(
                        1, std::memory_order_acq_rel);
                }
                sample.stackCount = static_cast<uint16_t>(
                    CaptureStackBackTrace(
                        1, kStackFrames,
                        reinterpret_cast<void**>(sample.stack), nullptr));
                if (sample.loopIndex == UINT32_MAX)
                {
                    g_unscopedOldReleaseCount.fetch_add(
                        1, std::memory_order_acq_rel);
                }
                g_releaseSampleReady[sampleIndex].store(
                    true, std::memory_order_release);
            }
        }
        const ULONG result = g_releaseOriginal(object);
        g_releaseCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    void remove_release_hook()
    {
        if (!g_releaseOriginal)
        {
            g_releaseHookActive.store(false, std::memory_order_release);
            return;
        }

        g_releaseHookActive.store(false, std::memory_order_release);
        const uint64_t waitStarted = GetTickCount64();
        while (g_releaseCallbacks.load(std::memory_order_acquire) != 0 &&
            GetTickCount64() - waitStarted < 100)
        {
            Sleep(0);
        }
        MH_DisableHook(g_releaseTarget);
        MH_RemoveHook(g_releaseTarget);
        g_releaseOriginal = nullptr;
        g_releaseTarget = nullptr;
        diagnostic_log::write(
            "probe",
            "Temporary exact COM Release correlation hook disabled.");
    }

    bool install_release_hook(ID3D11Texture2D* currentLiveTexture)
    {
        if (!currentLiveTexture)
            return false;

        __try
        {
            void** vtable = *reinterpret_cast<void***>(currentLiveTexture);
            g_releaseTarget = vtable ? vtable[2] : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_releaseTarget = nullptr;
        }
        if (!g_releaseTarget)
            return false;

        g_releaseSampleCount = 0;
        g_exactOldReleaseCount = 0;
        g_unscopedOldReleaseCount = 0;
        g_validatedOldReleaseCount = 0;
        g_releaseCallbacks = 0;
        for (auto& sample : g_releaseSamples)
            sample = {};
        for (auto& ready : g_releaseSampleReady)
            ready = false;

        const MH_STATUS createResult = MH_CreateHook(
            g_releaseTarget,
            reinterpret_cast<void*>(&hooked_release),
            reinterpret_cast<void**>(&g_releaseOriginal));
        if (createResult != MH_OK)
        {
            diagnostic_log::writef_important(
                "error",
                "Could not create the exact COM Release hook (status=%d); "
                "the stable branch/list/DX probes remain available.",
                static_cast<int>(createResult));
            g_releaseTarget = nullptr;
            return false;
        }

        const MH_STATUS enableResult = MH_EnableHook(g_releaseTarget);
        if (enableResult != MH_OK && enableResult != MH_ERROR_ENABLED)
        {
            MH_RemoveHook(g_releaseTarget);
            g_releaseOriginal = nullptr;
            g_releaseTarget = nullptr;
            diagnostic_log::writef_important(
                "error",
                "Could not enable the exact COM Release hook (status=%d).",
                static_cast<int>(enableResult));
            return false;
        }

        g_releaseHookActive.store(true, std::memory_order_release);
        diagnostic_log::writef_important(
            "probe",
            "Safe one-cycle Release correlation enabled: Release=0x%llX. "
            "Private game cleanup functions are not detoured.",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(g_releaseTarget)));
        return true;
    }

    bool write_branch(bool forceFallthrough)
    {
        if (!forceFallthrough && g_branchAddress == 0 &&
            !g_patchEnabled && !g_probeInstalled)
        {
            return true;
        }
        if (!resolve_branch())
            return false;
        if (g_probeInstalled)
            return false;

        auto* bytes = reinterpret_cast<uint8_t*>(g_branchAddress);
        const bool original = bytes[0] == 0x0F && bytes[1] == 0x84;
        const bool forced = bytes[0] == 0x90 && bytes[1] == 0xE9;
        if (!original && !forced)
        {
            diagnostic_log::writef(
                "error",
                "Custom render branch has unexpected bytes %02X %02X; "
                "it was left untouched.",
                static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(bytes[1]));
            return false;
        }
        if (forced == forceFallthrough)
        {
            g_patchEnabled = forceFallthrough;
            return true;
        }

        DWORD oldProtection{};
        if (!VirtualProtect(
                bytes, 2, PAGE_EXECUTE_READWRITE, &oldProtection))
        {
            diagnostic_log::writef(
                "error",
                "Could not change custom render branch protection "
                "(Win32 error %lu).",
                GetLastError());
            return false;
        }
        bytes[0] = forceFallthrough ? 0x90 : 0x0F;
        bytes[1] = forceFallthrough ? 0xE9 : 0x84;
        FlushInstructionCache(GetCurrentProcess(), bytes, 2);
        DWORD ignored{};
        VirtualProtect(bytes, 2, oldProtection, &ignored);
        g_patchEnabled = forceFallthrough;
        diagnostic_log::writef(
            "route",
            "Custom-display render compatibility branch %s.",
            forceFallthrough
                ? "enabled after the diagnostic window"
                : "restored to the game's original condition");
        return true;
    }

    void copy_stack_words(
        uint64_t stackPointer,
        uint64_t (&destination)[kStackWords])
    {
        __try
        {
            const auto* source = reinterpret_cast<const uint64_t*>(
                stackPointer);
            for (uint32_t index = 0; index < kStackWords; ++index)
                destination[index] = source[index];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            for (auto& word : destination)
                word = 0;
        }
    }

    bool readable_pointer(uint64_t value);
    uint64_t memory_fingerprint_words(uint64_t address, uint32_t wordCount);
    uint64_t memory_fingerprint(uint64_t address);

    bool readable_graph_pointer(uint64_t value)
    {
        value &= ~uint64_t{ 7 };
        if (value < 0x10000ULL)
            return false;
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(value),
                &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }
        const DWORD access = region.Protect & 0xFFU;
        return access == PAGE_READONLY || access == PAGE_READWRITE ||
            access == PAGE_WRITECOPY;
    }

    bool read_graph_word(
        uint64_t address,
        uint32_t wordIndex,
        uint64_t& value)
    {
        address &= ~uint64_t{ 7 };
        bool read = false;
        __try
        {
            value = reinterpret_cast<const uint64_t*>(address)[wordIndex];
            read = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            value = 0;
        }
        return read;
    }

    bool find_previous_texture_path(
        loop_event_t& loop,
        uintptr_t previousTexture)
    {
        if (previousTexture == 0 || loop.objectBase == 0)
            return false;
        if (loop.objectBase == previousTexture)
        {
            loop.oldPathDepth = 1;
            return true;
        }

        // Search only the current entry object, never the surrounding list.
        // An exact pointer match is required before any entry is skipped.
        constexpr uint32_t rootWords = kLoopObjectWords;
        constexpr uint32_t childWords = 32;
        constexpr uint32_t grandchildWords = 16;
        for (uint32_t first = 0; first < rootWords; ++first)
        {
            const uint64_t firstValue = loop.objectWords[first];
            if (firstValue == previousTexture)
            {
                loop.oldPathDepth = 2;
                loop.oldPathFirstOffset = static_cast<int32_t>(
                    first * sizeof(uint64_t));
                return true;
            }

            const uint64_t child = firstValue & ~uint64_t{ 7 };
            if (!readable_graph_pointer(child) ||
                child == loop.objectBase)
            {
                continue;
            }
            for (uint32_t second = 0; second < childWords; ++second)
            {
                uint64_t secondValue{};
                if (!read_graph_word(child, second, secondValue))
                    break;
                if (secondValue == previousTexture)
                {
                    loop.oldPathDepth = 3;
                    loop.oldPathFirstOffset = static_cast<int32_t>(
                        first * sizeof(uint64_t));
                    loop.oldPathSecondOffset = static_cast<int32_t>(
                        second * sizeof(uint64_t));
                    return true;
                }

                const uint64_t grandchild =
                    secondValue & ~uint64_t{ 7 };
                if (!readable_graph_pointer(grandchild) ||
                    grandchild == child ||
                    grandchild == loop.objectBase)
                {
                    continue;
                }
                for (uint32_t third = 0;
                    third < grandchildWords; ++third)
                {
                    uint64_t thirdValue{};
                    if (!read_graph_word(
                            grandchild, third, thirdValue))
                    {
                        break;
                    }
                    if (thirdValue == previousTexture)
                    {
                        loop.oldPathDepth = 4;
                        loop.oldPathFirstOffset = static_cast<int32_t>(
                            first * sizeof(uint64_t));
                        loop.oldPathSecondOffset = static_cast<int32_t>(
                            second * sizeof(uint64_t));
                        loop.oldPathThirdOffset = static_cast<int32_t>(
                            third * sizeof(uint64_t));
                        return true;
                    }
                }
            }
        }
        return false;
    }

    void copy_registers(const CONTEXT& context, probe_event_t& event)
    {
        event.registers[reg_rax] = context.Rax;
        event.registers[reg_rbx] = context.Rbx;
        event.registers[reg_rcx] = context.Rcx;
        event.registers[reg_rdx] = context.Rdx;
        event.registers[reg_rsi] = context.Rsi;
        event.registers[reg_rdi] = context.Rdi;
        event.registers[reg_rbp] = context.Rbp;
        event.registers[reg_rsp] = context.Rsp;
        event.registers[reg_r8] = context.R8;
        event.registers[reg_r9] = context.R9;
        event.registers[reg_r10] = context.R10;
        event.registers[reg_r11] = context.R11;
        event.registers[reg_r12] = context.R12;
        event.registers[reg_r13] = context.R13;
        event.registers[reg_r14] = context.R14;
        event.registers[reg_r15] = context.R15;
        event.tick = GetTickCount64();
        event.qpc = performance_counter();
        event.threadId = GetCurrentThreadId();
        copy_stack_words(context.Rsp, event.stack);
    }

    uint64_t snapshot_hash(const uint64_t* words, uint32_t count)
    {
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t index = 0; index < count; ++index)
        {
            uint64_t value = words[index];
            for (uint32_t byte = 0; byte < sizeof(value); ++byte)
            {
                hash ^= static_cast<uint8_t>(value & 0xFFU);
                hash *= 1099511628211ULL;
                value >>= 8;
            }
        }
        return hash;
    }

    void capture_loop_event(CONTEXT& context, uint32_t index)
    {
        // Emulate the overwritten `mov r9,rsi` before capturing registers.
        context.R9 = context.Rsi;
        if (index >= kMaximumLoopEvents)
            return;

        loop_event_t& loop = g_loopEvents[index];
        copy_registers(context, loop.event);

        uint32_t branchIndex = g_latestBranchIndex.load(
            std::memory_order_acquire);
        const uint32_t completedBranches = (std::min)(
            g_completedEventCount.load(std::memory_order_acquire),
            kMaximumEvents);
        if (branchIndex >= completedBranches ||
            g_events[branchIndex].threadId != loop.event.threadId)
        {
            branchIndex = UINT32_MAX;
            for (uint32_t cursor = completedBranches;
                cursor > 0; --cursor)
            {
                const uint32_t candidate = cursor - 1;
                if (g_events[candidate].threadId == loop.event.threadId &&
                    g_events[candidate].qpc <= loop.event.qpc)
                {
                    branchIndex = candidate;
                    break;
                }
            }
        }
        if (branchIndex < completedBranches)
        {
            loop.branchIndex = branchIndex;
            const uint64_t listStart =
                g_events[branchIndex].registers[reg_rsi];
            if (context.Rsi >= listStart &&
                ((context.Rsi - listStart) % sizeof(uint64_t)) == 0)
            {
                loop.listOrdinal = static_cast<uint32_t>(
                    (context.Rsi - listStart) / sizeof(uint64_t));
            }
        }

        __try
        {
            const auto* slots = reinterpret_cast<const uint64_t*>(context.Rsi);
            const uint64_t remainingBytes = context.R14 > context.Rsi
                ? context.R14 - context.Rsi : sizeof(uint64_t);
            const uint32_t availableWords = static_cast<uint32_t>((std::min)(
                remainingBytes / sizeof(uint64_t),
                static_cast<uint64_t>(kLoopSlotWords)));
            for (uint32_t word = 0; word < availableWords; ++word)
                loop.slotWords[word] = slots[word];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        loop.objectBase = loop.slotWords[0] & ~uint64_t{ 7 };
        if (readable_pointer(loop.objectBase))
        {
            __try
            {
                const auto* object = reinterpret_cast<const uint64_t*>(
                    loop.objectBase);
                for (uint32_t word = 0; word < kLoopObjectWords; ++word)
                    loop.objectWords[word] = object[word];
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        loop.captureSlotHash = snapshot_hash(
            loop.slotWords, kLoopSlotWords);
        loop.captureObjectHash = snapshot_hash(
            loop.objectWords, kLoopComparisonWords);
        const uintptr_t previousTexture = g_previousTexture.load(
            std::memory_order_acquire);
        const uintptr_t previousIdentity = g_previousTextureIdentity.load(
            std::memory_order_acquire);
        bool oldPathFound = find_previous_texture_path(
            loop, previousTexture);
        if (oldPathFound)
            loop.oldPathTargetKind = 1;
        else if (previousIdentity != 0 &&
            previousIdentity != previousTexture)
        {
            oldPathFound = find_previous_texture_path(
                loop, previousIdentity);
            if (oldPathFound)
                loop.oldPathTargetKind = 2;
        }
        if (oldPathFound)
        {
            g_oldTexturePathCount.fetch_add(
                1, std::memory_order_acq_rel);
            if (g_loopAdvanceAddress != 0)
            {
                loop.selectivelySkipped = 1;
                g_selectiveSkipCount.fetch_add(
                    1, std::memory_order_acq_rel);
            }
        }
        g_loopEventReady[index].store(true, std::memory_order_release);
        g_completedLoopEventCount.store(index + 1, std::memory_order_release);
    }

    LONG CALLBACK exception_handler(PEXCEPTION_POINTERS exception)
    {
        if (!exception || !exception->ExceptionRecord ||
            !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode !=
                EXCEPTION_BREAKPOINT ||
            (reinterpret_cast<uint64_t>(
                exception->ExceptionRecord->ExceptionAddress) !=
                g_branchAddress &&
             reinterpret_cast<uint64_t>(
                exception->ExceptionRecord->ExceptionAddress) !=
                g_loopProbeAddress))
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CONTEXT* context = exception->ContextRecord;
        g_activeHandlers.fetch_add(1, std::memory_order_acq_rel);
        const uint64_t exceptionAddress = reinterpret_cast<uint64_t>(
            exception->ExceptionRecord->ExceptionAddress);
        if (exceptionAddress == g_loopProbeAddress)
        {
            const bool recordEvent = g_trapArmed.load(
                std::memory_order_acquire);
            const uint32_t index = recordEvent
                ? g_loopEventCount.fetch_add(1, std::memory_order_relaxed)
                : kMaximumLoopEvents;
            capture_loop_event(*context, index);
            t_activeLoopIndex = index < kMaximumLoopEvents
                ? index : UINT32_MAX;
            t_activeLoopQpc = index < kMaximumLoopEvents
                ? g_loopEvents[index].event.qpc : 0;
            const bool skipEntry = index < kMaximumLoopEvents &&
                g_loopEvents[index].selectivelySkipped != 0;
            if (skipEntry)
            {
                // Rejoin the game's verified native add-RSI/compare/backedge
                // sequence. This preserves its exact loop-exit register state
                // while bypassing only the matched entry's cleanup body.
                context->Rip = g_loopAdvanceAddress;
            }
            else
            {
                context->Rip =
                    g_loopProbeAddress + kLoopInstructionBytes;
            }
            g_activeHandlers.fetch_sub(1, std::memory_order_acq_rel);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        const bool recordEvent = g_trapArmed.load(
            std::memory_order_acquire);
        t_activeLoopIndex = UINT32_MAX;
        t_activeLoopQpc = 0;
        const uint32_t index = recordEvent
            ? g_eventCount.fetch_add(1, std::memory_order_relaxed)
            : kMaximumEvents;
        const bool originalTaken = (context->EFlags & 0x40U) != 0;
        // Once the exact replacement texture exists, temporarily emulate the
        // working compatibility jump. That makes the selected texture reach
        // the Direct3D bind/draw hooks while the INT3 remains in place, which
        // is what lets one test connect the global branch to one real display.
        const bool emulatedTaken =
            g_textureReady.load(std::memory_order_acquire)
                ? true : originalTaken;
        if (recordEvent && index < kMaximumEvents)
        {
            probe_event_t& event = g_events[index];
            copy_registers(*context, event);
            event.branchTaken = originalTaken ? 1 : 0;
            event.emulatedTaken = emulatedTaken ? 1 : 0;
            g_completedEventCount.store(
                index + 1, std::memory_order_release);
            g_latestBranchIndex.store(index, std::memory_order_release);
        }

        context->Rip = emulatedTaken
            ? g_takenAddress : g_fallthroughAddress;

        g_activeHandlers.fetch_sub(1, std::memory_order_acq_rel);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool start_probe()
    {
        if (g_probeInstalled || g_captureCompleted ||
            !resolve_branch() || !resolve_loop_probe())
            return false;

        // The probe emulates the original JE itself. Restore the branch first
        // so unrelated cabin screens keep the game's native decision during
        // the diagnostic window.
        if (!write_branch(false))
            return false;

        auto* bytes = reinterpret_cast<uint8_t*>(g_branchAddress);
        if (bytes[0] != 0x0F || bytes[1] != 0x84)
        {
            diagnostic_log::writef(
                "error",
                "Per-instance probe expected JE bytes but found %02X %02X; "
                "capture was cancelled safely.",
                static_cast<unsigned>(bytes[0]),
                static_cast<unsigned>(bytes[1]));
            return false;
        }

        constexpr SIZE_T protectedBytes =
            kLoopInstructionOffset + kLoopInstructionBytes;
        if (!VirtualProtect(
                bytes, protectedBytes, PAGE_EXECUTE_READWRITE,
                &g_originalProtection))
        {
            diagnostic_log::writef(
                "error",
                "Could not arm per-instance render probe (Win32 error %lu).",
                GetLastError());
            return false;
        }

        g_exceptionHandler = AddVectoredExceptionHandler(
            1, exception_handler);
        if (!g_exceptionHandler)
        {
            DWORD ignored{};
            VirtualProtect(
                bytes, protectedBytes, g_originalProtection, &ignored);
            diagnostic_log::writef(
                "error",
                "Could not install per-instance exception handler "
                "(Win32 error %lu).",
                GetLastError());
            return false;
        }

        for (auto& event : g_events)
            event = {};
        for (auto& event : g_loopEvents)
            event = {};
        for (auto& ready : g_loopEventReady)
            ready = false;
        g_eventCount = 0;
        g_completedEventCount = 0;
        g_latestBranchIndex = UINT32_MAX;
        g_loopEventCount = 0;
        g_completedLoopEventCount = 0;
        g_oldTexturePathCount = 0;
        g_selectiveSkipCount = 0;
        g_trapCompleted = false;
        g_probeStartedTick = GetTickCount64();
        g_probeInstalled = true;
        g_trapArmed.store(true, std::memory_order_release);
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(g_branchAddress),
            static_cast<char>(0xCC));
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(g_loopProbeAddress),
            static_cast<char>(0xCC));
        FlushInstructionCache(
            GetCurrentProcess(), bytes, protectedBytes);

        diagnostic_log::writef(
            "probe",
            "Per-instance custom render capture started for display '%s' "
            "(%s), previousTexture=0x%llX selectedTexture=0x%llX. "
            "The original game branch and "
            "post-R9 list loop are being emulated for at most %u/%u "
            "hits and %llums.",
            g_selectedDisplayId,
            g_selectedOriginalTexture,
            static_cast<unsigned long long>(g_previousTexture.load()),
            static_cast<unsigned long long>(g_selectedTexture.load()),
            kMaximumEvents,
            kMaximumLoopEvents,
            static_cast<unsigned long long>(kProbeTimeoutMilliseconds));
        return true;
    }

    bool same_signature(
        const probe_event_t& left,
        const probe_event_t& right)
    {
        if (left.branchTaken != right.branchTaken ||
            left.threadId != right.threadId)
            return false;
        constexpr register_index_t compared[] = {
            reg_rbx, reg_rcx, reg_rdx, reg_rsi, reg_rdi,
            reg_r8, reg_r9, reg_r12, reg_r13, reg_r14, reg_r15
        };
        for (const register_index_t index : compared)
        {
            if (left.registers[index] != right.registers[index])
                return false;
        }
        return true;
    }

    const char* direct_texture_match(
        const probe_event_t& event,
        uintptr_t texture)
    {
        static constexpr const char* names[register_count] = {
            "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
            "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"
        };
        for (uint32_t index = 0; index < register_count; ++index)
        {
            if (event.registers[index] == texture)
                return names[index];
        }
        if ((event.registers[reg_r9] & ~uint64_t{ 7 }) == texture)
            return "R9(tag-cleared)";
        for (uint32_t index = 0; index < kStackWords; ++index)
        {
            if (event.stack[index] == texture)
                return "STACK";
        }
        return "none";
    }

    bool readable_pointer(uint64_t value)
    {
        if (value < 0x10000ULL)
            return false;
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(value),
                &region, sizeof(region)) != sizeof(region))
            return false;
        if (region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD access = region.Protect & 0xFFU;
        return access == PAGE_READONLY || access == PAGE_READWRITE ||
            access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
            access == PAGE_EXECUTE_READWRITE ||
            access == PAGE_EXECUTE_WRITECOPY;
    }

    int32_t find_texture_member(uint64_t object, uintptr_t texture)
    {
        if (!readable_pointer(object))
            return -1;
        __try
        {
            const auto* words = reinterpret_cast<const uintptr_t*>(object);
            for (uint32_t index = 0; index < kObjectScanWords; ++index)
            {
                if (words[index] == texture)
                    return static_cast<int32_t>(index * sizeof(uintptr_t));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        return -1;
    }

    bool find_texture_child_member(
        uint64_t object,
        uintptr_t texture,
        int32_t& childSlotOffset,
        int32_t& childTextureOffset)
    {
        if (!readable_pointer(object))
            return false;

        __try
        {
            const auto* words = reinterpret_cast<const uintptr_t*>(object);
            for (uint32_t index = 0; index < kObjectScanWords; ++index)
            {
                // Prism3D commonly stores tag bits in the low end of an
                // otherwise aligned object pointer. Clearing only three bits
                // preserves the allocation while removing the observed R9 +2
                // tag from this ETS2 build.
                const uint64_t child =
                    static_cast<uint64_t>(words[index]) & ~uint64_t{ 7 };
                const int32_t offset =
                    find_texture_member(child, texture);
                if (offset >= 0)
                {
                    childSlotOffset = static_cast<int32_t>(
                        index * sizeof(uintptr_t));
                    childTextureOffset = offset;
                    return true;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return false;
    }

    bool set_loop_texture_path(
        loop_event_t& loop,
        loop_candidate_t candidate,
        uint16_t candidateIndex,
        uint64_t address,
        uintptr_t texture)
    {
        address &= ~uint64_t{ 7 };
        if (address == texture)
        {
            loop.candidate = candidate;
            loop.candidateIndex = candidateIndex;
            loop.directOffset = 0;
            return true;
        }

        const int32_t directOffset =
            find_texture_member(address, texture);
        if (directOffset >= 0)
        {
            loop.candidate = candidate;
            loop.candidateIndex = candidateIndex;
            loop.directOffset = directOffset;
            return true;
        }

        int32_t childSlotOffset = -1;
        int32_t childTextureOffset = -1;
        if (find_texture_child_member(
                address, texture,
                childSlotOffset, childTextureOffset))
        {
            loop.candidate = candidate;
            loop.candidateIndex = candidateIndex;
            loop.childSlotOffset = childSlotOffset;
            loop.childTextureOffset = childTextureOffset;
            return true;
        }
        return false;
    }

    void find_loop_texture_path(loop_event_t& loop, uintptr_t texture)
    {
        if (texture == 0 || loop.candidate != loop_candidate_t::none)
            return;

        if (set_loop_texture_path(
                loop, loop_candidate_t::slot, 0,
                loop.event.registers[reg_rsi], texture) ||
            set_loop_texture_path(
                loop, loop_candidate_t::entry, 0,
                loop.objectBase, texture) ||
            set_loop_texture_path(
                loop, loop_candidate_t::slot_next, 1,
                loop.slotWords[1], texture))
        {
            return;
        }

        // Treat the first 16 captured entry words as independent child roots.
        // This covers [RSI], [[RSI]], [[RSI]+8], and the most likely adjacent
        // fields without committing to one guessed Prism3D object layout.
        for (uint16_t word = 0; word < kLoopComparisonWords; ++word)
        {
            if (set_loop_texture_path(
                    loop, loop_candidate_t::entry_word, word,
                    loop.objectWords[word], texture))
            {
                return;
            }
        }
    }

    void snapshot_loop_state(bool drawPhase, uintptr_t texture)
    {
        std::atomic<bool>& taken = drawPhase
            ? g_loopDrawSnapshotTaken : g_loopMatchSnapshotTaken;
        bool expected = false;
        if (!taken.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
        {
            return;
        }

        const uint32_t count = (std::min)(
            g_completedLoopEventCount.load(std::memory_order_acquire),
            kMaximumLoopEvents);
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!g_loopEventReady[index].load(std::memory_order_acquire))
                continue;
            loop_event_t& loop = g_loopEvents[index];
            const uint64_t slotHash = memory_fingerprint_words(
                loop.event.registers[reg_rsi], kLoopSlotWords);
            const uint64_t objectHash = memory_fingerprint(loop.objectBase);
            uint64_t* liveWords = drawPhase
                ? loop.drawObjectWords : loop.matchObjectWords;
            if (readable_pointer(loop.objectBase))
            {
                __try
                {
                    const auto* object = reinterpret_cast<const uint64_t*>(
                        loop.objectBase);
                    for (uint32_t word = 0;
                        word < kLoopComparisonWords; ++word)
                    {
                        liveWords[word] = object[word];
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                }
            }
            if (drawPhase)
            {
                loop.drawSlotHash = slotHash;
                loop.drawObjectHash = objectHash;
            }
            else
            {
                loop.matchSlotHash = slotHash;
                loop.matchObjectHash = objectHash;
                find_loop_texture_path(loop, texture);
            }
        }
    }

    const char* loop_candidate_name(loop_candidate_t candidate)
    {
        switch (candidate)
        {
        case loop_candidate_t::slot: return "RSI-slot";
        case loop_candidate_t::entry: return "[RSI]-entry";
        case loop_candidate_t::slot_next: return "[RSI+8]";
        case loop_candidate_t::entry_word: return "entry-word";
        default: return "none";
        }
    }

    uint32_t loop_difference_mask(
        const uint64_t* captured,
        const uint64_t* compared)
    {
        uint32_t mask{};
        for (uint32_t word = 0; word < kLoopComparisonWords; ++word)
        {
            if (captured[word] != compared[word])
                mask |= 1U << word;
        }
        return mask;
    }

    int32_t exact_texture_word(
        const uint64_t* words,
        uintptr_t texture)
    {
        for (uint32_t word = 0; word < kLoopComparisonWords; ++word)
        {
            if (words[word] == texture)
                return static_cast<int32_t>(word);
        }
        return -1;
    }

    texture_path_t find_texture_path(
        const probe_event_t& event,
        uintptr_t texture)
    {
        static constexpr register_index_t candidates[] = {
            reg_r9, reg_rsi, reg_r14, reg_rbx, reg_rcx, reg_rdx,
            reg_rdi, reg_r8, reg_r12, reg_r13, reg_r15
        };
        static constexpr const char* candidateNames[] = {
            "R9(tag-cleared)", "RSI", "R14", "RBX", "RCX", "RDX",
            "RDI", "R8", "R12", "R13", "R15"
        };

        texture_path_t result{};
        for (uint32_t candidate = 0;
            candidate < sizeof(candidates) / sizeof(candidates[0]);
            ++candidate)
        {
            uint64_t object = event.registers[candidates[candidate]];
            if (candidates[candidate] == reg_r9)
                object &= ~uint64_t{ 7 };

            const int32_t directOffset =
                find_texture_member(object, texture);
            if (directOffset >= 0)
            {
                result.registerName = candidateNames[candidate];
                result.object = object;
                result.directOffset = directOffset;
                return result;
            }

            int32_t childSlotOffset = -1;
            int32_t childTextureOffset = -1;
            if (find_texture_child_member(
                    object, texture,
                    childSlotOffset, childTextureOffset))
            {
                result.registerName = candidateNames[candidate];
                result.object = object;
                result.childSlotOffset = childSlotOffset;
                result.childTextureOffset = childTextureOffset;
                return result;
            }
        }
        return result;
    }

    bool matches_selected_resource(IUnknown* resource)
    {
        if (!resource)
            return false;
        const uintptr_t selectedTexture = g_selectedTexture.load(
            std::memory_order_acquire);
        if (reinterpret_cast<uintptr_t>(resource) == selectedTexture)
            return true;

        const uintptr_t selectedIdentity = g_selectedTextureIdentity.load(
            std::memory_order_acquire);
        if (selectedIdentity == 0)
            return false;

        IUnknown* identity{};
        const HRESULT result = resource->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&identity));
        if (FAILED(result) || !identity)
            return false;
        const bool match = reinterpret_cast<uintptr_t>(identity) ==
            selectedIdentity;
        identity->Release();
        return match;
    }

    bool track_selected_srv(ID3D11ShaderResourceView* view)
    {
        if (!view)
            return false;
        const uintptr_t value = reinterpret_cast<uintptr_t>(view);
        const uint32_t existingCount = (std::min)(
            g_selectedSrvCount.load(std::memory_order_acquire),
            kMaximumTrackedSrvs);
        for (uint32_t index = 0; index < existingCount; ++index)
        {
            if (g_selectedSrvs[index].load(
                    std::memory_order_acquire) == value)
                return true;
        }

        uint32_t slot = g_selectedSrvCount.fetch_add(
            1, std::memory_order_acq_rel);
        if (slot >= kMaximumTrackedSrvs)
        {
            g_selectedSrvCount.store(
                kMaximumTrackedSrvs, std::memory_order_release);
            return false;
        }
        view->AddRef();
        g_selectedSrvs[slot].store(value, std::memory_order_release);
        return true;
    }

    bool is_tracked_srv(ID3D11ShaderResourceView* view)
    {
        if (!view)
            return false;
        const uintptr_t value = reinterpret_cast<uintptr_t>(view);
        const uint32_t count = (std::min)(
            g_selectedSrvCount.load(std::memory_order_acquire),
            kMaximumTrackedSrvs);
        for (uint32_t index = 0; index < count; ++index)
        {
            if (g_selectedSrvs[index].load(
                    std::memory_order_acquire) == value)
                return true;
        }
        return false;
    }

    uint64_t memory_fingerprint_words(uint64_t address, uint32_t wordCount)
    {
        address &= ~uint64_t{ 7 };
        if (!readable_pointer(address) || wordCount == 0)
            return 0;
        const uint32_t bytesToHash = wordCount * sizeof(uint64_t);
        uint64_t hash = 1469598103934665603ULL;
        __try
        {
            const auto* bytes = reinterpret_cast<const uint8_t*>(address);
            for (uint32_t index = 0; index < bytesToHash; ++index)
            {
                hash ^= bytes[index];
                hash *= 1099511628211ULL;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
        return hash;
    }

    uint64_t memory_fingerprint(uint64_t address)
    {
        return memory_fingerprint_words(address, kLoopComparisonWords);
    }

    bool nearest_branch_event(
        uint32_t threadId,
        uint64_t bindQpc,
        probe_event_t& event,
        uint32_t& eventIndex)
    {
        const uint32_t completed = (std::min)(
            g_completedEventCount.load(std::memory_order_acquire),
            kMaximumEvents);
        const uint32_t first = completed > kBranchLookbackEvents
            ? completed - kBranchLookbackEvents : 0;
        for (uint32_t cursor = completed; cursor > first; --cursor)
        {
            const uint32_t index = cursor - 1;
            const probe_event_t& candidate = g_events[index];
            if (candidate.threadId == threadId &&
                candidate.qpc != 0 && candidate.qpc <= bindQpc)
            {
                event = candidate;
                eventIndex = index;
                return true;
            }
        }
        return false;
    }

    int64_t qpc_delta_microseconds(uint64_t later, uint64_t earlier)
    {
        if (later == 0 || earlier == 0 || later < earlier)
            return -1;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart <= 0)
            return -1;
        return static_cast<int64_t>(
            ((later - earlier) * 1000000ULL) /
            static_cast<uint64_t>(frequency.QuadPart));
    }

    void append_stack_text(
        char* destination,
        size_t capacity,
        const uint64_t* frames,
        uint32_t count)
    {
        size_t cursor{};
        const uint64_t moduleStart = bmem::moduleBase;
        const uint64_t moduleEnd = moduleStart + bmem::moduleSize;
        for (uint32_t index = 0; index < count && cursor < capacity; ++index)
        {
            const uint64_t address = frames[index];
            const bool inGame = address >= moduleStart && address < moduleEnd;
            const int written = std::snprintf(
                destination + cursor,
                capacity - cursor,
                "%s%s%llX",
                index == 0 ? "" : ",",
                inGame ? "game+0x" : "0x",
                static_cast<unsigned long long>(
                    inGame ? address - moduleStart : address));
            if (written <= 0)
                break;
            cursor += static_cast<size_t>(written);
        }
    }

    void log_dx_correlations()
    {
        const uint32_t samples = (std::min)(
            g_correlationSampleCount.load(std::memory_order_acquire),
            kMaximumCorrelationSamples);
        diagnostic_log::writef_important(
            "probe",
            "DX correlation summary: trackedSRVs=%u bindSamples=%u "
            "drawSamples=%u resourceInspections=%u target=%u.",
            (std::min)(g_selectedSrvCount.load(), kMaximumTrackedSrvs),
            samples,
            g_drawCorrelationCount.load(),
            g_resourceInspections.load(),
            kTargetCorrelationSamples);

        for (uint32_t index = 0; index < samples; ++index)
        {
            if (!g_correlationSampleReady[index].load(
                    std::memory_order_acquire))
                continue;
            const correlation_sample_t& sample =
                g_correlationSamples[index];
            const bool hasBranch = sample.branchIndex != UINT32_MAX;
            diagnostic_log::writef_important(
                "probe",
                "DX sample[%u] method=%s slot=%u thread=%lu branch=%s "
                "branchIndex=%u bindDelta=%lldus original=%s emulated=%s "
                "R9=%llX R9base=%llX RSI=%llX R14=%llX "
                "hashes=%llX/%llX/%llX draw=%s drawDelta=%lldus.",
                index,
                sample.matchedByResourceInspection
                    ? "bound-resource" : "tracked-SRV",
                sample.matchedSlot,
                sample.threadId,
                hasBranch ? "matched" : "none",
                sample.branchIndex,
                hasBranch
                    ? static_cast<long long>(qpc_delta_microseconds(
                        sample.bindQpc, sample.branch.qpc)) : -1LL,
                hasBranch && sample.branch.branchTaken
                    ? "JE-taken" : "fallthrough",
                hasBranch && sample.branch.emulatedTaken
                    ? "JE-taken" : "fallthrough",
                static_cast<unsigned long long>(
                    hasBranch ? sample.branch.registers[reg_r9] : 0),
                static_cast<unsigned long long>(
                    hasBranch
                        ? sample.branch.registers[reg_r9] & ~uint64_t{ 7 }
                        : 0),
                static_cast<unsigned long long>(
                    hasBranch ? sample.branch.registers[reg_rsi] : 0),
                static_cast<unsigned long long>(
                    hasBranch ? sample.branch.registers[reg_r14] : 0),
                static_cast<unsigned long long>(sample.r9Hash),
                static_cast<unsigned long long>(sample.rsiHash),
                static_cast<unsigned long long>(sample.r14Hash),
                sample.drawKind[0] ? sample.drawKind : "none",
                sample.drawQpc != 0
                    ? static_cast<long long>(qpc_delta_microseconds(
                        sample.drawQpc, sample.bindQpc)) : -1LL);

            char bindStackText[768]{};
            append_stack_text(
                bindStackText, sizeof(bindStackText),
                sample.bindStack, sample.bindStackCount);
            diagnostic_log::writef_important(
                "probe", "DX sample[%u] bind-stack=%s",
                index, bindStackText[0] ? bindStackText : "none");

            if (sample.drawStackCount != 0)
            {
                char drawStackText[768]{};
                append_stack_text(
                    drawStackText, sizeof(drawStackText),
                    sample.drawStack, sample.drawStackCount);
                diagnostic_log::writef_important(
                    "probe", "DX sample[%u] draw-stack=%s",
                    index, drawStackText[0] ? drawStackText : "none");
            }
        }
    }

    void log_release_correlations()
    {
        const uint32_t captured = (std::min)(
            g_releaseSampleCount.load(std::memory_order_acquire),
            kMaximumReleaseSamples);
        uint32_t scoped{};
        for (uint32_t index = 0; index < captured; ++index)
        {
            if (g_releaseSampleReady[index].load(
                    std::memory_order_acquire) &&
                g_releaseSamples[index].loopIndex != UINT32_MAX)
            {
                ++scoped;
            }
        }

        diagnostic_log::writef_important(
            "probe",
            "Safe Release correlation summary: captured=%u "
            "exactOldRelease=%u scopedToListEntry=%u unscoped=%u "
            "scopeWindow=%lluus oldTexture=0x%llX oldIdentity=0x%llX.",
            captured,
            g_exactOldReleaseCount.load(std::memory_order_acquire),
            scoped,
            g_unscopedOldReleaseCount.load(std::memory_order_acquire),
            static_cast<unsigned long long>(
                g_releaseScopeWindowMicroseconds.load()),
            static_cast<unsigned long long>(g_previousTexture.load()),
            static_cast<unsigned long long>(
                g_previousTextureIdentity.load()));

        const uint32_t completedLoops = (std::min)(
            g_completedLoopEventCount.load(std::memory_order_acquire),
            kMaximumLoopEvents);
        for (uint32_t index = 0; index < captured; ++index)
        {
            if (!g_releaseSampleReady[index].load(
                    std::memory_order_acquire))
            {
                continue;
            }
            const release_sample_t& sample = g_releaseSamples[index];
            uint32_t ordinal = UINT32_MAX;
            uint64_t slot{};
            uint64_t object{};
            if (sample.loopIndex < completedLoops &&
                g_loopEventReady[sample.loopIndex].load(
                    std::memory_order_acquire))
            {
                const loop_event_t& loop = g_loopEvents[sample.loopIndex];
                ordinal = loop.listOrdinal;
                slot = loop.event.registers[reg_rsi];
                object = loop.objectBase;
            }
            diagnostic_log::writef_important(
                "probe",
                "release[%u] this=%llX thread=%lu candidateLoop=%u "
                "validatedLoop=%u scopeValid=%u loopDelta=%lldus "
                "ordinal=%u slot=%llX object=%llX.",
                index,
                static_cast<unsigned long long>(sample.releaseThis),
                sample.threadId, sample.candidateLoopIndex,
                sample.loopIndex, sample.scopeValidated,
                static_cast<long long>(sample.loopDeltaMicroseconds), ordinal,
                static_cast<unsigned long long>(slot),
                static_cast<unsigned long long>(object));

            char stackText[768]{};
            append_stack_text(
                stackText, sizeof(stackText),
                sample.stack, sample.stackCount);
            diagnostic_log::writef_important(
                "probe", "release[%u] stack=%s",
                index, stackText[0] ? stackText : "none");
        }
    }

    void log_loop_results()
    {
        constexpr uint32_t maximumLoggedEntries = 64;
        const uint32_t captured = (std::min)(
            g_completedLoopEventCount.load(std::memory_order_acquire),
            kMaximumLoopEvents);
        uint32_t textureMatches{};
        uint32_t objectChangesAtMatch{};
        uint32_t objectChangesAtDraw{};
        for (uint32_t index = 0; index < captured; ++index)
        {
            if (!g_loopEventReady[index].load(std::memory_order_acquire))
                continue;
            const loop_event_t& loop = g_loopEvents[index];
            if (loop.candidate != loop_candidate_t::none)
                ++textureMatches;
            if (loop.matchObjectHash != 0 &&
                loop.matchObjectHash != loop.captureObjectHash)
            {
                ++objectChangesAtMatch;
            }
            if (loop.drawObjectHash != 0 &&
                loop.drawObjectHash != loop.captureObjectHash)
            {
                ++objectChangesAtDraw;
            }
        }

        diagnostic_log::writef_important(
            "probe",
            "List-entry correlation summary: captured=%u logged=%u "
            "texturePaths=%u objectChangedAtMatch=%u "
            "objectChangedAtDraw=%u oldTexture=0x%llX "
            "oldIdentity=0x%llX oldPaths=%u "
            "selectiveSkips=%u matchSnapshot=%d drawSnapshot=%d.",
            captured, (std::min)(captured, maximumLoggedEntries),
            textureMatches, objectChangesAtMatch, objectChangesAtDraw,
            static_cast<unsigned long long>(g_previousTexture.load()),
            static_cast<unsigned long long>(
                g_previousTextureIdentity.load()),
            g_oldTexturePathCount.load(),
            g_selectiveSkipCount.load(),
            g_loopMatchSnapshotTaken.load() ? 1 : 0,
            g_loopDrawSnapshotTaken.load() ? 1 : 0);

        const uint32_t logged = (std::min)(captured, maximumLoggedEntries);
        const uintptr_t selectedTexture = g_selectedTexture.load(
            std::memory_order_acquire);
        for (uint32_t index = 0; index < logged; ++index)
        {
            if (!g_loopEventReady[index].load(std::memory_order_acquire))
                continue;
            const loop_event_t& loop = g_loopEvents[index];
            diagnostic_log::writef_important(
                "probe",
                "list-entry[%u] branch=%u ordinal=%u thread=%lu "
                "RSI=%llX R9=%llX RAX=%llX R14=%llX "
                "slots=%llX/%llX/%llX/%llX object=%llX.",
                index, loop.branchIndex, loop.listOrdinal,
                loop.event.threadId,
                static_cast<unsigned long long>(
                    loop.event.registers[reg_rsi]),
                static_cast<unsigned long long>(
                    loop.event.registers[reg_r9]),
                static_cast<unsigned long long>(
                    loop.event.registers[reg_rax]),
                static_cast<unsigned long long>(
                    loop.event.registers[reg_r14]),
                static_cast<unsigned long long>(loop.slotWords[0]),
                static_cast<unsigned long long>(loop.slotWords[1]),
                static_cast<unsigned long long>(loop.slotWords[2]),
                static_cast<unsigned long long>(loop.slotWords[3]),
                static_cast<unsigned long long>(loop.objectBase));
            diagnostic_log::writef_important(
                "probe",
                "list-entry[%u] words=%llX/%llX/%llX/%llX/"
                "%llX/%llX/%llX/%llX hashes(slot)=%llX/%llX/%llX "
                "hashes(object)=%llX/%llX/%llX path=%s[%u] "
                "direct=%d childSlot=%d childTexture=%d "
                "oldPathDepth=%u oldTarget=%u oldOffsets=%d/%d/%d "
                "skipped=%u.",
                index,
                static_cast<unsigned long long>(loop.objectWords[0]),
                static_cast<unsigned long long>(loop.objectWords[1]),
                static_cast<unsigned long long>(loop.objectWords[2]),
                static_cast<unsigned long long>(loop.objectWords[3]),
                static_cast<unsigned long long>(loop.objectWords[4]),
                static_cast<unsigned long long>(loop.objectWords[5]),
                static_cast<unsigned long long>(loop.objectWords[6]),
                static_cast<unsigned long long>(loop.objectWords[7]),
                static_cast<unsigned long long>(loop.captureSlotHash),
                static_cast<unsigned long long>(loop.matchSlotHash),
                static_cast<unsigned long long>(loop.drawSlotHash),
                static_cast<unsigned long long>(loop.captureObjectHash),
                static_cast<unsigned long long>(loop.matchObjectHash),
                static_cast<unsigned long long>(loop.drawObjectHash),
                loop_candidate_name(loop.candidate), loop.candidateIndex,
                loop.directOffset, loop.childSlotOffset,
                loop.childTextureOffset, loop.oldPathDepth,
                loop.oldPathTargetKind,
                loop.oldPathFirstOffset, loop.oldPathSecondOffset,
                loop.oldPathThirdOffset, loop.selectivelySkipped);
            diagnostic_log::writef_important(
                "probe",
                "list-entry[%u] live(match)=%llX/%llX/%llX/%llX/"
                "%llX/%llX/%llX/%llX live(draw)=%llX/%llX/%llX/%llX/"
                "%llX/%llX/%llX/%llX diffMasks=%04X/%04X "
                "exactWords=%d/%d.",
                index,
                static_cast<unsigned long long>(loop.matchObjectWords[0]),
                static_cast<unsigned long long>(loop.matchObjectWords[1]),
                static_cast<unsigned long long>(loop.matchObjectWords[2]),
                static_cast<unsigned long long>(loop.matchObjectWords[3]),
                static_cast<unsigned long long>(loop.matchObjectWords[4]),
                static_cast<unsigned long long>(loop.matchObjectWords[5]),
                static_cast<unsigned long long>(loop.matchObjectWords[6]),
                static_cast<unsigned long long>(loop.matchObjectWords[7]),
                static_cast<unsigned long long>(loop.drawObjectWords[0]),
                static_cast<unsigned long long>(loop.drawObjectWords[1]),
                static_cast<unsigned long long>(loop.drawObjectWords[2]),
                static_cast<unsigned long long>(loop.drawObjectWords[3]),
                static_cast<unsigned long long>(loop.drawObjectWords[4]),
                static_cast<unsigned long long>(loop.drawObjectWords[5]),
                static_cast<unsigned long long>(loop.drawObjectWords[6]),
                static_cast<unsigned long long>(loop.drawObjectWords[7]),
                loop_difference_mask(
                    loop.objectWords, loop.matchObjectWords),
                loop_difference_mask(
                    loop.objectWords, loop.drawObjectWords),
                exact_texture_word(
                    loop.matchObjectWords, selectedTexture),
                exact_texture_word(
                    loop.drawObjectWords, selectedTexture));
        }

        if (captured > logged)
        {
            diagnostic_log::writef_important(
                "probe",
                "List-entry log bounded: %u additional captures omitted; "
                "the summary still includes all captures.",
                captured - logged);
        }
    }

    void log_probe_results()
    {
        const uint32_t captured = (std::min)(
            g_eventCount.load(), kMaximumEvents);
        std::array<unique_signature_t, kMaximumUniqueSignatures> unique{};
        uint32_t uniqueCount{};
        uint32_t takenCount{};
        uint32_t emulatedTakenCount{};
        uint32_t beforeTextureCount{};
        uint32_t afterTextureCount{};
        const uint64_t textureMatchedTick = g_textureMatchedTick.load();
        for (uint32_t index = 0; index < captured; ++index)
        {
            const probe_event_t& event = g_events[index];
            const bool afterTexture = textureMatchedTick != 0 &&
                event.tick >= textureMatchedTick;
            if (event.branchTaken)
                ++takenCount;
            if (event.emulatedTaken)
                ++emulatedTakenCount;
            if (afterTexture)
                ++afterTextureCount;
            else
                ++beforeTextureCount;
            uint32_t signature{};
            for (; signature < uniqueCount; ++signature)
            {
                if (same_signature(event, unique[signature].event))
                    break;
            }
            if (signature < uniqueCount)
            {
                ++unique[signature].hits;
                if (afterTexture)
                    ++unique[signature].afterTextureHits;
                else
                    ++unique[signature].beforeTextureHits;
            }
            else if (uniqueCount < kMaximumUniqueSignatures)
            {
                unique[uniqueCount].event = event;
                unique[uniqueCount].hits = 1;
                unique[uniqueCount].afterTextureHits = afterTexture ? 1 : 0;
                unique[uniqueCount].beforeTextureHits = afterTexture ? 0 : 1;
                ++uniqueCount;
            }
        }

        const uintptr_t selectedTexture = g_selectedTexture.load();
        diagnostic_log::writef_important(
            "probe",
            "Per-instance capture completed: display='%s' events=%u "
            "listEntries=%u JE-taken=%u fallthrough=%u unique=%u "
            "beforeTexture=%u "
            "afterTexture=%u emulatedTaken=%u selectedTexture=0x%llX "
            "previousTexture=0x%llX oldPaths=%u selectiveSkips=%u "
            "matchTick=%llu.",
            g_selectedDisplayId, captured,
            (std::min)(g_loopEventCount.load(), kMaximumLoopEvents),
            takenCount,
            captured - takenCount, uniqueCount,
            beforeTextureCount, afterTextureCount,
            emulatedTakenCount,
            static_cast<unsigned long long>(selectedTexture),
            static_cast<unsigned long long>(g_previousTexture.load()),
            g_oldTexturePathCount.load(),
            g_selectiveSkipCount.load(),
            static_cast<unsigned long long>(textureMatchedTick));

        log_dx_correlations();
        log_release_correlations();
        log_loop_results();

        for (uint32_t index = 0; index < uniqueCount; ++index)
        {
            const probe_event_t& event = unique[index].event;
            const texture_path_t texturePath =
                find_texture_path(event, selectedTexture);
            const int64_t matchDelta = textureMatchedTick == 0
                ? 0
                : static_cast<int64_t>(event.tick) -
                    static_cast<int64_t>(textureMatchedTick);

            diagnostic_log::writef(
                "probe",
                "signature[%u] hits=%u before=%u after=%u firstDelta=%lldms "
                "thread=%lu decision=%s "
                "RCX=%llX RDX=%llX R8=%llX R9=%llX RBX=%llX RSI=%llX "
                "RDI=%llX R12=%llX R13=%llX R14=%llX R15=%llX "
                "directTexture=%s R9base=%llX texturePath=%s object=%llX "
                "directOffset=%d childSlot=%d childOffset=%d.",
                index, unique[index].hits,
                unique[index].beforeTextureHits,
                unique[index].afterTextureHits,
                static_cast<long long>(matchDelta),
                event.threadId,
                event.branchTaken ? "JE-taken" : "fallthrough",
                static_cast<unsigned long long>(
                    event.registers[reg_rcx]),
                static_cast<unsigned long long>(
                    event.registers[reg_rdx]),
                static_cast<unsigned long long>(
                    event.registers[reg_r8]),
                static_cast<unsigned long long>(
                    event.registers[reg_r9]),
                static_cast<unsigned long long>(
                    event.registers[reg_rbx]),
                static_cast<unsigned long long>(
                    event.registers[reg_rsi]),
                static_cast<unsigned long long>(
                    event.registers[reg_rdi]),
                static_cast<unsigned long long>(
                    event.registers[reg_r12]),
                static_cast<unsigned long long>(
                    event.registers[reg_r13]),
                static_cast<unsigned long long>(
                    event.registers[reg_r14]),
                static_cast<unsigned long long>(
                    event.registers[reg_r15]),
                direct_texture_match(event, selectedTexture),
                static_cast<unsigned long long>(
                    event.registers[reg_r9] & ~uint64_t{ 7 }),
                texturePath.registerName,
                static_cast<unsigned long long>(texturePath.object),
                texturePath.directOffset,
                texturePath.childSlotOffset,
                texturePath.childTextureOffset);
        }
    }

    void release_dx_correlation_resources()
    {
        for (auto& value : g_selectedSrvs)
        {
            const uintptr_t pointer = value.exchange(
                0, std::memory_order_acq_rel);
            if (pointer != 0)
            {
                reinterpret_cast<ID3D11ShaderResourceView*>(pointer)->Release();
            }
        }
        const uintptr_t identity = g_selectedTextureIdentity.exchange(
            0, std::memory_order_acq_rel);
        if (identity != 0)
            reinterpret_cast<IUnknown*>(identity)->Release();
    }

    void reset_dx_correlation()
    {
        release_dx_correlation_resources();
        g_dxCorrelationActive = false;
        g_dxCorrelationComplete = false;
        g_dxCallbacks = 0;
        g_selectedSrvCount = 0;
        g_resourceInspections = 0;
        g_correlationSampleCount = 0;
        g_drawCorrelationCount = 0;
        g_loopMatchSnapshotTaken = false;
        g_loopDrawSnapshotTaken = false;
        for (auto& sample : g_correlationSamples)
            sample = {};
        for (auto& ready : g_correlationSampleReady)
            ready = false;
        for (auto& ready : g_drawSampleReady)
            ready = false;
        t_pendingCorrelationSample = UINT32_MAX;
    }

    void finalize_results()
    {
        if (!g_captureDataReady.load(std::memory_order_acquire) ||
            !g_textureReady.load(std::memory_order_acquire))
        {
            return;
        }

        bool expected = false;
        if (!g_resultsLogged.compare_exchange_strong(expected, true))
            return;

        log_probe_results();
        g_captureCompleted.store(true, std::memory_order_release);
        g_captureReserved.store(false, std::memory_order_release);
    }

    void finish_probe(bool customDisplayActive)
    {
        if (!g_probeInstalled)
            return;

        g_dxCorrelationActive.store(false, std::memory_order_release);
        dx11::create_texture_2d::set_custom_probe_hooks_enabled(false);
        remove_release_hook();
        const uint64_t callbackWaitStarted = GetTickCount64();
        while (g_dxCallbacks.load(std::memory_order_acquire) != 0 &&
            GetTickCount64() - callbackWaitStarted < 100)
        {
            Sleep(0);
        }

        auto* bytes = reinterpret_cast<uint8_t*>(g_branchAddress);
        auto* loopBytes = reinterpret_cast<uint8_t*>(g_loopProbeAddress);
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(loopBytes),
            static_cast<char>(0x4C));
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(g_branchAddress),
            static_cast<char>(0x0F));
        bytes[1] = 0x84;
        constexpr SIZE_T protectedBytes =
            kLoopInstructionOffset + kLoopInstructionBytes;
        FlushInstructionCache(
            GetCurrentProcess(), bytes, protectedBytes);
        const uint64_t waitStarted = GetTickCount64();
        while (g_activeHandlers.load(std::memory_order_acquire) != 0 &&
            GetTickCount64() - waitStarted < 100)
        {
            Sleep(0);
        }
        g_trapArmed.store(false, std::memory_order_release);
        if (g_exceptionHandler)
        {
            RemoveVectoredExceptionHandler(g_exceptionHandler);
            g_exceptionHandler = nullptr;
        }
        DWORD ignored{};
        VirtualProtect(
            bytes, protectedBytes, g_originalProtection, &ignored);
        g_probeInstalled = false;
        g_captureDataReady.store(true, std::memory_order_release);
        g_trapCompleted = false;
        g_patchEnabled = false;

        diagnostic_log::writef(
            "probe",
            "Early branch/list capture phase completed: display='%s' "
            "branchEvents=%u listEntries=%u oldPaths=%u selectiveSkips=%u "
            "textureReady=%d dxCorrelated=%d. %s",
            g_selectedDisplayId,
            (std::min)(g_eventCount.load(), kMaximumEvents),
            (std::min)(g_loopEventCount.load(), kMaximumLoopEvents),
            g_oldTexturePathCount.load(),
            g_selectiveSkipCount.load(),
            g_textureReady.load() ? 1 : 0,
            g_dxCorrelationComplete.load() ? 1 : 0,
            g_textureReady.load()
                ? "Correlating against the exact matched texture."
                : "Waiting for the exact custom texture match before "
                  "correlation.");

        // Preserve the user's working custom display after diagnostics. This
        // fallback is intentionally restored until the captured pointers can
        // be used to implement the final selective detour.
        write_branch(customDisplayActive || g_captureReserved.load());
        finalize_results();
        release_dx_correlation_resources();
    }
}

namespace custom_render_probe
{
    bool prepare_capture(
        const char* displayId,
        const char* originalTexture,
        ID3D11Texture2D* currentLiveTexture)
    {
        if (g_captureCompleted.load(std::memory_order_acquire) ||
            g_probeInstalled.load(std::memory_order_acquire) ||
            g_captureDataReady.load(std::memory_order_acquire) ||
            !currentLiveTexture)
        {
            return false;
        }

        bool expected = false;
        if (!g_captureReserved.compare_exchange_strong(expected, true))
            return false;

        strncpy_s(
            g_selectedDisplayId,
            displayId && displayId[0] ? displayId : "custom",
            _TRUNCATE);
        strncpy_s(
            g_selectedOriginalTexture,
            originalTexture && originalTexture[0]
                ? originalTexture : "unknown",
            _TRUNCATE);
        g_selectedTexture.store(0, std::memory_order_release);
        g_previousTexture.store(
            reinterpret_cast<uintptr_t>(currentLiveTexture),
            std::memory_order_release);
        g_previousTextureIdentity.store(0, std::memory_order_release);
        IUnknown* previousIdentity{};
        if (SUCCEEDED(currentLiveTexture->QueryInterface(
                __uuidof(IUnknown),
                reinterpret_cast<void**>(&previousIdentity))) &&
            previousIdentity)
        {
            g_previousTextureIdentity.store(
                reinterpret_cast<uintptr_t>(previousIdentity),
                std::memory_order_release);
            // Preserve only the numeric canonical identity. The screen still
            // owns the live texture; retaining an extra COM reference would
            // mask the cleanup behavior this test must observe.
            previousIdentity->Release();
        }
        g_textureMatchedTick.store(0, std::memory_order_release);
        g_textureMatchedQpc.store(0, std::memory_order_release);
        g_textureReady.store(false, std::memory_order_release);
        g_captureDataReady.store(false, std::memory_order_release);
        reset_dx_correlation();

        const bool releaseHookReady = install_release_hook(currentLiveTexture);
        if (!releaseHookReady)
        {
            diagnostic_log::write(
                "error",
                "The safe test could not enable exact COM Release "
                "correlation; it will continue with the established "
                "bounded list and Direct3D draw coverage.");
        }

        diagnostic_log::writef(
            "probe",
            "Preparing early per-instance render capture before truck "
            "reload: display='%s' texture='%s' oldLiveTexture=0x%llX "
            "oldIdentity=0x%llX. "
            "Only an entry with an exact bounded path to this old texture "
            "may be skipped.",
            g_selectedDisplayId,
            g_selectedOriginalTexture,
            static_cast<unsigned long long>(g_previousTexture.load()),
            static_cast<unsigned long long>(
                g_previousTextureIdentity.load()));

        const bool dxHooksReady =
            dx11::create_texture_2d::set_custom_probe_hooks_enabled(true);
        if (!dxHooksReady)
        {
            diagnostic_log::write(
                "error",
                "The targeted test could not enable all temporary "
                "Direct3D correlation hooks; the branch capture will "
                "continue with reduced coverage.");
        }

        if (start_probe())
            return true;

        dx11::create_texture_2d::set_custom_probe_hooks_enabled(false);
        remove_release_hook();
        g_captureReserved.store(false, std::memory_order_release);
        g_captureCompleted.store(true, std::memory_order_release);
        return false;
    }

    void request_capture(
        const char* displayId,
        const char* originalTexture,
        ID3D11Texture2D* liveTexture)
    {
        if (!g_captureReserved.load(std::memory_order_acquire) ||
            g_captureCompleted.load(std::memory_order_acquire) ||
            !liveTexture)
            return;

        const char* candidateDisplay =
            displayId && displayId[0] ? displayId : "custom";
        const char* candidateTexture =
            originalTexture && originalTexture[0]
                ? originalTexture : "unknown";
        if (std::strcmp(candidateDisplay, g_selectedDisplayId) != 0 ||
            std::strcmp(candidateTexture, g_selectedOriginalTexture) != 0)
        {
            return;
        }

        g_selectedTexture.store(
            reinterpret_cast<uintptr_t>(liveTexture),
            std::memory_order_release);
        g_textureMatchedTick.store(
            GetTickCount64(), std::memory_order_release);
        g_textureMatchedQpc.store(
            performance_counter(), std::memory_order_release);

        IUnknown* identity{};
        if (SUCCEEDED(liveTexture->QueryInterface(
                __uuidof(IUnknown),
                reinterpret_cast<void**>(&identity))) && identity)
        {
            const uintptr_t oldIdentity = g_selectedTextureIdentity.exchange(
                reinterpret_cast<uintptr_t>(identity),
                std::memory_order_acq_rel);
            if (oldIdentity != 0)
                reinterpret_cast<IUnknown*>(oldIdentity)->Release();
        }
        g_textureReady.store(true, std::memory_order_release);
        g_dxCorrelationActive.store(true, std::memory_order_release);
        snapshot_loop_state(false, g_selectedTexture.load(
            std::memory_order_acquire));
        diagnostic_log::writef(
            "probe",
            "Exact custom route matched for the prepared early capture: "
            "display='%s' texture='%s' liveTexture=0x%llX captureReady=%d. "
            "Wide DX correlation is now active.",
            g_selectedDisplayId,
            g_selectedOriginalTexture,
            static_cast<unsigned long long>(g_selectedTexture.load()),
            g_captureDataReady.load() ? 1 : 0);
        finalize_results();
    }

    void notify_shader_resource_view(
        ID3D11Resource* resource,
        ID3D11ShaderResourceView* view)
    {
        if (!g_dxCorrelationActive.load(std::memory_order_acquire) ||
            !resource || !view)
            return;

        g_dxCallbacks.fetch_add(1, std::memory_order_acq_rel);
        if (g_dxCorrelationActive.load(std::memory_order_acquire) &&
            matches_selected_resource(resource))
        {
            track_selected_srv(view);
        }
        g_dxCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void notify_pixel_shader_resources(
        unsigned int startSlot,
        unsigned int viewCount,
        ID3D11ShaderResourceView* const* views)
    {
        if (!g_dxCorrelationActive.load(std::memory_order_acquire) ||
            !views || viewCount == 0)
            return;

        g_dxCallbacks.fetch_add(1, std::memory_order_acq_rel);
        if (!g_dxCorrelationActive.load(std::memory_order_acquire))
        {
            g_dxCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        uint32_t matchedView = UINT32_MAX;
        bool resourceInspectionMatch = false;
        for (uint32_t index = 0; index < viewCount; ++index)
        {
            if (is_tracked_srv(views[index]))
            {
                matchedView = index;
                break;
            }
        }

        // Independent fallback: if CreateShaderResourceView was missed or
        // returned a different COM interface pointer, ask each bound SRV for
        // its underlying resource and compare canonical IUnknown identity.
        if (matchedView == UINT32_MAX)
        {
            for (uint32_t index = 0; index < viewCount; ++index)
            {
                if (!views[index])
                    continue;
                ID3D11Resource* resource{};
                views[index]->GetResource(&resource);
                g_resourceInspections.fetch_add(1,
                    std::memory_order_relaxed);
                const bool match = matches_selected_resource(resource);
                if (resource)
                    resource->Release();
                if (match)
                {
                    track_selected_srv(views[index]);
                    matchedView = index;
                    resourceInspectionMatch = true;
                    break;
                }
            }
        }

        if (matchedView != UINT32_MAX)
        {
            snapshot_loop_state(true, g_selectedTexture.load(
                std::memory_order_acquire));
            const uint32_t sampleIndex = g_correlationSampleCount.fetch_add(
                1, std::memory_order_acq_rel);
            if (sampleIndex < kMaximumCorrelationSamples)
            {
                correlation_sample_t& sample =
                    g_correlationSamples[sampleIndex];
                sample.bindQpc = performance_counter();
                sample.threadId = GetCurrentThreadId();
                sample.startSlot = startSlot;
                sample.matchedSlot = startSlot + matchedView;
                sample.matchedByResourceInspection =
                    resourceInspectionMatch ? 1 : 0;
                nearest_branch_event(
                    sample.threadId, sample.bindQpc,
                    sample.branch, sample.branchIndex);
                if (sample.branchIndex != UINT32_MAX)
                {
                    sample.r9Hash = memory_fingerprint(
                        sample.branch.registers[reg_r9]);
                    sample.rsiHash = memory_fingerprint(
                        sample.branch.registers[reg_rsi]);
                    sample.r14Hash = memory_fingerprint(
                        sample.branch.registers[reg_r14]);
                }
                sample.bindStackCount = static_cast<uint16_t>(
                    CaptureStackBackTrace(
                        2, kStackFrames,
                        reinterpret_cast<void**>(sample.bindStack),
                        nullptr));
                g_correlationSampleReady[sampleIndex].store(
                    true, std::memory_order_release);
                t_pendingCorrelationSample = sampleIndex;
            }
            else
            {
                g_correlationSampleCount.store(
                    kMaximumCorrelationSamples,
                    std::memory_order_release);
            }
        }

        g_dxCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void notify_draw(const char* drawKind)
    {
        if (!g_dxCorrelationActive.load(std::memory_order_acquire) ||
            t_pendingCorrelationSample >= kMaximumCorrelationSamples)
            return;

        g_dxCallbacks.fetch_add(1, std::memory_order_acq_rel);
        const uint32_t sampleIndex = t_pendingCorrelationSample;
        t_pendingCorrelationSample = UINT32_MAX;
        if (!g_dxCorrelationActive.load(std::memory_order_acquire) ||
            !g_correlationSampleReady[sampleIndex].load(
                std::memory_order_acquire))
        {
            g_dxCallbacks.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        correlation_sample_t& sample = g_correlationSamples[sampleIndex];
        const uint64_t nowQpc = performance_counter();
        const int64_t delay = qpc_delta_microseconds(
            nowQpc, sample.bindQpc);
        if (delay >= 0 &&
            static_cast<uint64_t>(delay) <=
                kPendingDrawMaximumMicroseconds)
        {
            sample.drawQpc = nowQpc;
            strncpy_s(
                sample.drawKind,
                drawKind && drawKind[0] ? drawKind : "draw",
                _TRUNCATE);
            sample.drawStackCount = static_cast<uint16_t>(
                CaptureStackBackTrace(
                    2, kStackFrames,
                    reinterpret_cast<void**>(sample.drawStack),
                    nullptr));
            g_drawSampleReady[sampleIndex].store(
                true, std::memory_order_release);
            const uint32_t completed = g_drawCorrelationCount.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            if (completed >= kTargetCorrelationSamples)
            {
                g_dxCorrelationComplete.store(
                    true, std::memory_order_release);
                g_trapCompleted.store(true, std::memory_order_release);
            }
        }
        g_dxCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    void update(bool customDisplayActive)
    {
        const fallback_mode_t mode = g_fallbackMode.load(
            std::memory_order_acquire);
        const bool fallbackActive = mode == fallback_mode_t::forced_on ||
            (mode == fallback_mode_t::automatic && customDisplayActive);
        if (g_probeInstalled)
        {
            const uint64_t now = GetTickCount64();
            const uint64_t matched = g_textureMatchedTick.load(
                std::memory_order_acquire);
            if (g_trapCompleted.load(std::memory_order_acquire) ||
                now < g_probeStartedTick ||
                now - g_probeStartedTick >= kProbeTimeoutMilliseconds ||
                (matched != 0 && now >= matched &&
                    now - matched >= kPostMatchTimeoutMilliseconds))
            {
                finish_probe(fallbackActive);
            }
            return;
        }

        write_branch(
            fallbackActive ||
            (g_captureReserved.load(std::memory_order_acquire) &&
                g_captureDataReady.load(std::memory_order_acquire)));
    }

    status_t status()
    {
        status_t result{};
        result.active = g_probeInstalled.load(std::memory_order_acquire);
        result.waitingForTexture =
            g_captureReserved.load(std::memory_order_acquire) &&
            g_captureDataReady.load(std::memory_order_acquire) &&
            !g_textureReady.load(std::memory_order_acquire);
        result.completed = g_captureCompleted.load(std::memory_order_acquire);
        result.textureReady = g_textureReady.load(std::memory_order_acquire);
        result.branchEvents = (std::min)(
            g_eventCount.load(std::memory_order_acquire), kMaximumEvents);
        result.listEntries = (std::min)(
            g_loopEventCount.load(std::memory_order_acquire),
            kMaximumLoopEvents);
        result.exactOldReleases = g_exactOldReleaseCount.load(
            std::memory_order_acquire);
        result.validatedReleases = g_validatedOldReleaseCount.load(
            std::memory_order_acquire);
        result.drawSamples = g_drawCorrelationCount.load(
            std::memory_order_acquire);
        result.releaseScopeWindowMicroseconds =
            g_releaseScopeWindowMicroseconds.load(std::memory_order_acquire);
        result.fallbackMode = g_fallbackMode.load(std::memory_order_acquire);
        return result;
    }

    bool reset_session()
    {
        if (g_probeInstalled.load(std::memory_order_acquire) ||
            (g_captureReserved.load(std::memory_order_acquire) &&
                !g_captureCompleted.load(std::memory_order_acquire)))
            return false;

        g_dxCorrelationActive = false;
        dx11::create_texture_2d::set_custom_probe_hooks_enabled(false);
        remove_release_hook();
        reset_dx_correlation();
        g_captureReserved = false;
        g_captureCompleted = false;
        g_captureDataReady = false;
        g_textureReady = false;
        g_resultsLogged = false;
        g_trapCompleted = false;
        g_selectedTexture = 0;
        g_previousTexture = 0;
        g_previousTextureIdentity = 0;
        g_textureMatchedTick = 0;
        g_textureMatchedQpc = 0;
        g_eventCount = 0;
        g_completedEventCount = 0;
        g_loopEventCount = 0;
        g_completedLoopEventCount = 0;
        g_releaseSampleCount = 0;
        g_exactOldReleaseCount = 0;
        g_unscopedOldReleaseCount = 0;
        g_validatedOldReleaseCount = 0;
        g_selectedDisplayId[0] = '\0';
        g_selectedOriginalTexture[0] = '\0';
        t_activeLoopIndex = UINT32_MAX;
        t_activeLoopQpc = 0;
        diagnostic_log::write(
            "console", "Diagnostic session reset for another safe run.");
        return true;
    }

    bool abort_capture(bool customDisplayActive)
    {
        const bool wasActive =
            g_probeInstalled.load(std::memory_order_acquire) ||
            g_captureReserved.load(std::memory_order_acquire);
        if (g_probeInstalled.load(std::memory_order_acquire))
            finish_probe(customDisplayActive);
        g_dxCorrelationActive = false;
        dx11::create_texture_2d::set_custom_probe_hooks_enabled(false);
        remove_release_hook();
        g_captureReserved = false;
        g_captureCompleted = true;
        release_dx_correlation_resources();
        if (wasActive)
            diagnostic_log::write(
                "console", "Diagnostic capture aborted safely.");
        return wasActive;
    }

    bool set_release_scope_window_microseconds(uint64_t value)
    {
        if (value < kMinimumReleaseScopeWindowMicroseconds ||
            value > kMaximumReleaseScopeWindowMicroseconds ||
            g_probeInstalled.load(std::memory_order_acquire))
        {
            return false;
        }
        g_releaseScopeWindowMicroseconds.store(
            value, std::memory_order_release);
        diagnostic_log::writef(
            "console", "Release scope window set to %lluus.",
            static_cast<unsigned long long>(value));
        return true;
    }

    void set_fallback_mode(fallback_mode_t mode)
    {
        g_fallbackMode.store(mode, std::memory_order_release);
    }

    fallback_mode_t fallback_mode()
    {
        return g_fallbackMode.load(std::memory_order_acquire);
    }

    void shutdown()
    {
        if (g_probeInstalled)
            finish_probe(false);
        else
        {
            g_dxCorrelationActive = false;
            dx11::create_texture_2d::set_custom_probe_hooks_enabled(false);
            remove_release_hook();
            write_branch(false);
        }
        if (g_captureDataReady.load() && !g_textureReady.load() &&
            !g_resultsLogged.load())
        {
            diagnostic_log::writef(
                "probe",
                "Diagnostic session ended before the prepared display's "
                "exact texture matched; retained branch events=%u.",
                (std::min)(g_eventCount.load(), kMaximumEvents));
        }
        g_captureReserved = false;
        release_dx_correlation_resources();
    }
}
