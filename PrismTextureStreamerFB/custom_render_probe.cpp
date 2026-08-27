#define NOMINMAX
#include "custom_render_probe.h"

#include "bmem.h"
#include "diagnostic_log.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr uint32_t kMaximumEvents = 192;
    constexpr uint32_t kMaximumUniqueSignatures = 48;
    constexpr uint64_t kProbeTimeoutMilliseconds = 1500;
    constexpr uint32_t kStackWords = 8;

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
        uint32_t threadId{};
        uint8_t branchTaken{};
    };

    struct unique_signature_t
    {
        probe_event_t event{};
        uint32_t hits{};
    };

    uint64_t g_branchAddress{};
    int32_t g_originalRelativeDisplacement{};
    uint64_t g_takenAddress{};
    uint64_t g_fallthroughAddress{};
    DWORD g_originalProtection{};
    PVOID g_exceptionHandler{};
    bool g_resolutionFailed{};
    bool g_patchEnabled{};
    std::atomic<bool> g_probeInstalled{};
    std::atomic<bool> g_captureCompleted{};
    uint64_t g_probeStartedTick{};
    std::atomic<bool> g_capturePending{};
    std::atomic<bool> g_captureReserved{};
    std::atomic<bool> g_trapArmed{};
    std::atomic<bool> g_trapCompleted{};
    std::atomic<uint32_t> g_activeHandlers{};
    std::atomic<uint32_t> g_eventCount{};
    std::atomic<uintptr_t> g_selectedTexture{};
    std::array<probe_event_t, kMaximumEvents> g_events{};
    char g_selectedDisplayId[96]{};
    char g_selectedOriginalTexture[320]{};

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

    LONG CALLBACK exception_handler(PEXCEPTION_POINTERS exception)
    {
        if (!exception || !exception->ExceptionRecord ||
            !exception->ContextRecord ||
            exception->ExceptionRecord->ExceptionCode !=
                EXCEPTION_BREAKPOINT ||
            reinterpret_cast<uint64_t>(
                exception->ExceptionRecord->ExceptionAddress) !=
                g_branchAddress)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CONTEXT* context = exception->ContextRecord;
        g_activeHandlers.fetch_add(1, std::memory_order_acq_rel);
        const bool recordEvent = g_trapArmed.load(
            std::memory_order_acquire);
        const uint32_t index = recordEvent
            ? g_eventCount.fetch_add(1, std::memory_order_relaxed)
            : kMaximumEvents;
        if (recordEvent && index < kMaximumEvents)
        {
            probe_event_t& event = g_events[index];
            event.registers[reg_rax] = context->Rax;
            event.registers[reg_rbx] = context->Rbx;
            event.registers[reg_rcx] = context->Rcx;
            event.registers[reg_rdx] = context->Rdx;
            event.registers[reg_rsi] = context->Rsi;
            event.registers[reg_rdi] = context->Rdi;
            event.registers[reg_rbp] = context->Rbp;
            event.registers[reg_rsp] = context->Rsp;
            event.registers[reg_r8] = context->R8;
            event.registers[reg_r9] = context->R9;
            event.registers[reg_r10] = context->R10;
            event.registers[reg_r11] = context->R11;
            event.registers[reg_r12] = context->R12;
            event.registers[reg_r13] = context->R13;
            event.registers[reg_r14] = context->R14;
            event.registers[reg_r15] = context->R15;
            event.tick = GetTickCount64();
            event.threadId = GetCurrentThreadId();
            event.branchTaken = (context->EFlags & 0x40U) != 0 ? 1 : 0;
            copy_stack_words(context->Rsp, event.stack);
        }

        const bool taken = (context->EFlags & 0x40U) != 0;
        context->Rip = taken ? g_takenAddress : g_fallthroughAddress;

        if (recordEvent && index + 1 >= kMaximumEvents)
        {
            InterlockedExchange8(
                reinterpret_cast<volatile char*>(g_branchAddress),
                static_cast<char>(0x0F));
            FlushInstructionCache(
                GetCurrentProcess(),
                reinterpret_cast<void*>(g_branchAddress),
                1);
            g_trapCompleted.store(true, std::memory_order_release);
        }
        g_activeHandlers.fetch_sub(1, std::memory_order_acq_rel);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    bool start_probe()
    {
        if (g_probeInstalled || g_captureCompleted || !resolve_branch())
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

        if (!VirtualProtect(
                bytes, 6, PAGE_EXECUTE_READWRITE,
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
            VirtualProtect(bytes, 6, g_originalProtection, &ignored);
            diagnostic_log::writef(
                "error",
                "Could not install per-instance exception handler "
                "(Win32 error %lu).",
                GetLastError());
            return false;
        }

        for (auto& event : g_events)
            event = {};
        g_eventCount = 0;
        g_trapCompleted = false;
        g_probeStartedTick = GetTickCount64();
        g_probeInstalled = true;
        g_trapArmed.store(true, std::memory_order_release);
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(g_branchAddress),
            static_cast<char>(0xCC));
        FlushInstructionCache(GetCurrentProcess(), bytes, 1);

        diagnostic_log::writef(
            "probe",
            "Per-instance custom render capture started for display '%s' "
            "(%s), selectedTexture=0x%llX. The original game branch is "
            "being emulated for at most %u hits/%llums.",
            g_selectedDisplayId,
            g_selectedOriginalTexture,
            static_cast<unsigned long long>(g_selectedTexture.load()),
            kMaximumEvents,
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
            for (uint32_t index = 0; index < 32; ++index)
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

    void log_probe_results()
    {
        const uint32_t captured = (std::min)(
            g_eventCount.load(), kMaximumEvents);
        std::array<unique_signature_t, kMaximumUniqueSignatures> unique{};
        uint32_t uniqueCount{};
        uint32_t takenCount{};
        for (uint32_t index = 0; index < captured; ++index)
        {
            const probe_event_t& event = g_events[index];
            if (event.branchTaken)
                ++takenCount;
            uint32_t signature{};
            for (; signature < uniqueCount; ++signature)
            {
                if (same_signature(event, unique[signature].event))
                    break;
            }
            if (signature < uniqueCount)
            {
                ++unique[signature].hits;
            }
            else if (uniqueCount < kMaximumUniqueSignatures)
            {
                unique[uniqueCount].event = event;
                unique[uniqueCount].hits = 1;
                ++uniqueCount;
            }
        }

        const uintptr_t selectedTexture = g_selectedTexture.load();
        diagnostic_log::writef(
            "probe",
            "Per-instance capture completed: display='%s' events=%u "
            "JE-taken=%u fallthrough=%u unique=%u selectedTexture=0x%llX.",
            g_selectedDisplayId, captured, takenCount,
            captured - takenCount, uniqueCount,
            static_cast<unsigned long long>(selectedTexture));

        for (uint32_t index = 0; index < uniqueCount; ++index)
        {
            const probe_event_t& event = unique[index].event;
            int32_t memberOffset = -1;
            const char* memberRegister = "none";
            static constexpr register_index_t candidates[] = {
                reg_rbx, reg_rcx, reg_rdx, reg_rsi, reg_rdi,
                reg_r8, reg_r9, reg_r12, reg_r13, reg_r14, reg_r15
            };
            static constexpr const char* candidateNames[] = {
                "RBX", "RCX", "RDX", "RSI", "RDI",
                "R8", "R9", "R12", "R13", "R14", "R15"
            };
            for (uint32_t candidate = 0;
                candidate < sizeof(candidates) / sizeof(candidates[0]);
                ++candidate)
            {
                memberOffset = find_texture_member(
                    event.registers[candidates[candidate]], selectedTexture);
                if (memberOffset >= 0)
                {
                    memberRegister = candidateNames[candidate];
                    break;
                }
            }

            diagnostic_log::writef(
                "probe",
                "signature[%u] hits=%u thread=%lu decision=%s "
                "RCX=%llX RDX=%llX R8=%llX R9=%llX RBX=%llX RSI=%llX "
                "RDI=%llX R12=%llX R13=%llX R14=%llX R15=%llX "
                "directTexture=%s textureMember=%s offset=%d.",
                index, unique[index].hits, event.threadId,
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
                memberOffset >= 0 ? memberRegister : "none",
                memberOffset);
        }
    }

    void finish_probe(bool customDisplayActive)
    {
        if (!g_probeInstalled)
            return;

        auto* bytes = reinterpret_cast<uint8_t*>(g_branchAddress);
        InterlockedExchange8(
            reinterpret_cast<volatile char*>(g_branchAddress),
            static_cast<char>(0x0F));
        bytes[1] = 0x84;
        FlushInstructionCache(GetCurrentProcess(), bytes, 2);
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
        VirtualProtect(bytes, 6, g_originalProtection, &ignored);
        g_probeInstalled = false;
        g_captureCompleted = true;
        g_trapCompleted = false;
        g_patchEnabled = false;
        log_probe_results();

        // Preserve the user's working custom display after diagnostics. This
        // fallback is intentionally restored until the captured pointers can
        // be used to implement the final selective detour.
        write_branch(customDisplayActive);
    }
}

namespace custom_render_probe
{
    void request_capture(
        const char* displayId,
        const char* originalTexture,
        ID3D11Texture2D* liveTexture)
    {
        if (g_captureCompleted || g_probeInstalled)
            return;

        bool expected = false;
        if (!g_captureReserved.compare_exchange_strong(expected, true))
            return;

        strncpy_s(
            g_selectedDisplayId,
            displayId && displayId[0] ? displayId : "custom",
            _TRUNCATE);
        strncpy_s(
            g_selectedOriginalTexture,
            originalTexture && originalTexture[0]
                ? originalTexture : "unknown",
            _TRUNCATE);
        g_selectedTexture.store(
            reinterpret_cast<uintptr_t>(liveTexture),
            std::memory_order_release);
        g_capturePending.store(true, std::memory_order_release);
        diagnostic_log::writef(
            "probe",
            "Queued one per-instance render capture after exact custom route "
            "match: display='%s' texture='%s' liveTexture=0x%llX.",
            g_selectedDisplayId,
            g_selectedOriginalTexture,
            static_cast<unsigned long long>(g_selectedTexture.load()));
    }

    void update(bool customDisplayActive)
    {
        if (g_probeInstalled)
        {
            const uint64_t now = GetTickCount64();
            if (g_trapCompleted.load(std::memory_order_acquire) ||
                now < g_probeStartedTick ||
                now - g_probeStartedTick >= kProbeTimeoutMilliseconds)
            {
                finish_probe(customDisplayActive);
            }
            return;
        }

        if (customDisplayActive && !g_captureCompleted &&
            g_capturePending.exchange(false))
        {
            if (!start_probe())
            {
                g_captureCompleted = true;
                write_branch(customDisplayActive);
            }
            return;
        }

        write_branch(customDisplayActive);
    }

    void shutdown()
    {
        if (g_probeInstalled)
            finish_probe(false);
        else
            write_branch(false);
        g_capturePending = false;
        g_captureReserved = false;
    }
}
