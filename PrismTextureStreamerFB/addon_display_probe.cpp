#define NOMINMAX
#include "addon_display_probe.h"

#include "diagnostic_log.h"

#include <Windows.h>
#include <MinHook/MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <sstream>
#include <string>

namespace
{
    // RVAs reported for the exact executable inspected by Craft Anything.
    // Both must match the running module before the hook is installed.
    constexpr uintptr_t kAddonCallRva = 0x543845;
    constexpr uintptr_t kExpectedTargetRva = 0x542FC0;
    constexpr uintptr_t kDescriptorIndexOffset = 0x160;
    constexpr uint32_t kMaximumSamples = 512;
    constexpr uint64_t kDefaultDurationSeconds = 60;
    constexpr uint64_t kMinimumDurationSeconds = 10;
    constexpr uint64_t kMaximumDurationSeconds = 300;

    using addon_create_t = uintptr_t(__fastcall*)(
        uintptr_t, uintptr_t, uintptr_t, uintptr_t,
        uintptr_t, uintptr_t, uintptr_t, uintptr_t);

    struct sample_t
    {
        uint64_t tick{};
        uint32_t threadId{};
        uintptr_t rcx{};
        uintptr_t addonObject{};       // RDX
        uintptr_t descriptor{};        // R8
        uintptr_t windowNamePointer{}; // R9
        uintptr_t stackArguments[4]{}; // possible arguments 5..8
        uint32_t descriptorIndex32{};
        uint64_t descriptorIndex64{};
        uintptr_t outputHandle{};      // RAX
        char windowName[96]{};
    };

    std::atomic<bool> g_active{};
    std::atomic<bool> g_hookInstalled{};
    std::atomic<bool> g_resolutionAttempted{};
    std::atomic<uint32_t> g_sampleCount{};
    std::atomic<uint32_t> g_completedSamples{};
    std::atomic<uint32_t> g_droppedSamples{};
    std::atomic<uint32_t> g_inFlightCallbacks{};
    std::atomic<uint64_t> g_deadline{};
    std::array<sample_t, kMaximumSamples> g_samples{};
    std::array<std::atomic<bool>, kMaximumSamples> g_sampleReady{};
    addon_create_t g_original{};
    void* g_target{};
    uintptr_t g_moduleBase{};
    uintptr_t g_moduleSize{};
    uintptr_t g_callAddress{};
    uintptr_t g_expectedReturn{};
    uintptr_t g_targetAddress{};
    char g_validationDetail[256]{ "not validated" };
    char g_callBytes[64]{ "unavailable" };

    std::string trim(std::string value)
    {
        while (!value.empty() &&
            std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        size_t first{};
        while (first < value.size() &&
            std::isspace(static_cast<unsigned char>(value[first])))
            ++first;
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

    bool readable_range(uintptr_t address, size_t bytes)
    {
        if (address < 0x10000 || bytes == 0 ||
            address > UINTPTR_MAX - bytes)
            return false;
        uintptr_t cursor = address;
        const uintptr_t end = address + bytes;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (VirtualQuery(reinterpret_cast<const void*>(cursor),
                    &region, sizeof(region)) != sizeof(region) ||
                region.State != MEM_COMMIT ||
                (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return false;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(
                region.BaseAddress) + region.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = (std::min)(end, regionEnd);
        }
        return true;
    }

    bool executable_address(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(address),
                &region, sizeof(region)) != sizeof(region) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD access = region.Protect & 0xFFU;
        return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
            access == PAGE_EXECUTE_READWRITE ||
            access == PAGE_EXECUTE_WRITECOPY;
    }

    void copy_ascii(uintptr_t source, char* destination, size_t capacity)
    {
        if (!destination || capacity == 0)
            return;
        destination[0] = '\0';
        if (!readable_range(source, 1))
            return;
        __try
        {
            const char* text = reinterpret_cast<const char*>(source);
            size_t index{};
            for (; index + 1 < capacity; ++index)
            {
                if (!readable_range(source + index, 1))
                    break;
                const unsigned char value =
                    static_cast<unsigned char>(text[index]);
                if (value == 0 || value < 0x20 || value > 0x7E)
                    break;
                destination[index] = static_cast<char>(value);
            }
            destination[index] = '\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            destination[0] = '\0';
        }
    }

    void read_descriptor_index(
        uintptr_t descriptor, uint32_t& index32, uint64_t& index64)
    {
        index32 = 0;
        index64 = 0;
        const uintptr_t field = descriptor + kDescriptorIndexOffset;
        if (field < descriptor || !readable_range(field, sizeof(uint64_t)))
            return;
        __try
        {
            std::memcpy(&index64, reinterpret_cast<const void*>(field),
                sizeof(index64));
            index32 = static_cast<uint32_t>(index64);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            index32 = 0;
            index64 = 0;
        }
    }

    uintptr_t __fastcall hooked_addon_create(
        uintptr_t rcx,
        uintptr_t rdx,
        uintptr_t r8,
        uintptr_t r9,
        uintptr_t argument5,
        uintptr_t argument6,
        uintptr_t argument7,
        uintptr_t argument8)
    {
        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool selectedCall = g_active.load(std::memory_order_acquire) &&
            returnAddress == g_expectedReturn;
        uint32_t reserved = UINT32_MAX;
        sample_t pending{};
        if (selectedCall)
        {
            g_inFlightCallbacks.fetch_add(1, std::memory_order_acq_rel);
            reserved = g_sampleCount.fetch_add(1, std::memory_order_acq_rel);
            if (reserved < kMaximumSamples)
            {
                pending.tick = GetTickCount64();
                pending.threadId = GetCurrentThreadId();
                pending.rcx = rcx;
                pending.addonObject = rdx;
                pending.descriptor = r8;
                pending.windowNamePointer = r9;
                pending.stackArguments[0] = argument5;
                pending.stackArguments[1] = argument6;
                pending.stackArguments[2] = argument7;
                pending.stackArguments[3] = argument8;
                read_descriptor_index(
                    r8, pending.descriptorIndex32,
                    pending.descriptorIndex64);
                copy_ascii(r9, pending.windowName,
                    sizeof(pending.windowName));
            }
            else
            {
                g_droppedSamples.fetch_add(1, std::memory_order_acq_rel);
            }
        }

        // The original function always runs exactly once.
        const uintptr_t result = g_original(
            rcx, rdx, r8, r9,
            argument5, argument6, argument7, argument8);

        if (selectedCall)
        {
            if (reserved < kMaximumSamples)
            {
                pending.outputHandle = result;
                g_samples[reserved] = pending;
                g_sampleReady[reserved].store(true,
                    std::memory_order_release);
                g_completedSamples.fetch_add(1,
                    std::memory_order_acq_rel);
            }
            g_inFlightCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        }
        return result;
    }

    bool resolve_and_install()
    {
        if (g_hookInstalled.load(std::memory_order_acquire))
            return true;
        if (g_resolutionAttempted.exchange(true, std::memory_order_acq_rel))
            return false;

        const HMODULE module = GetModuleHandleW(nullptr);
        g_moduleBase = reinterpret_cast<uintptr_t>(module);
        if (!module || !readable_range(g_moduleBase, sizeof(IMAGE_DOS_HEADER)))
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "main module is unavailable");
            return false;
        }

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(
            g_moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            !readable_range(g_moduleBase + dos->e_lfanew,
                sizeof(IMAGE_NT_HEADERS64)))
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "invalid PE headers");
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            g_moduleBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "invalid NT signature");
            return false;
        }
        g_moduleSize = nt->OptionalHeader.SizeOfImage;
        g_callAddress = g_moduleBase + kAddonCallRva;
        g_expectedReturn = g_callAddress + 5;
        if (kAddonCallRva + 16 >= g_moduleSize ||
            !readable_range(g_callAddress, 16))
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "RVA 0x%llX is outside the live executable",
                static_cast<unsigned long long>(kAddonCallRva));
            return false;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(g_callAddress);
        size_t cursor{};
        for (size_t index = 0;
            index < 12 && cursor < sizeof(g_callBytes); ++index)
        {
            const int written = std::snprintf(
                g_callBytes + cursor, sizeof(g_callBytes) - cursor,
                "%02X%s", static_cast<unsigned>(bytes[index]),
                index == 11 ? "" : " ");
            if (written <= 0)
                break;
            cursor += static_cast<size_t>(written);
        }
        if (bytes[0] != 0xE8)
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "RVA 0x%llX opcode is %02X, expected E8; no hook installed",
                static_cast<unsigned long long>(kAddonCallRva),
                static_cast<unsigned>(bytes[0]));
            return false;
        }

        int32_t displacement{};
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        g_targetAddress = g_expectedReturn +
            static_cast<int64_t>(displacement);
        const uintptr_t expectedTarget =
            g_moduleBase + kExpectedTargetRva;
        if (g_targetAddress != expectedTarget ||
            g_targetAddress < g_moduleBase ||
            g_targetAddress >= g_moduleBase + g_moduleSize ||
            !executable_address(g_targetAddress))
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "call target RVA is 0x%llX, expected 0x%llX; no hook installed",
                static_cast<unsigned long long>(
                    g_targetAddress >= g_moduleBase
                        ? g_targetAddress - g_moduleBase : 0),
                static_cast<unsigned long long>(kExpectedTargetRva));
            return false;
        }

        g_target = reinterpret_cast<void*>(g_targetAddress);
        const MH_STATUS createResult = MH_CreateHook(
            g_target,
            reinterpret_cast<void*>(&hooked_addon_create),
            reinterpret_cast<void**>(&g_original));
        if (createResult != MH_OK)
        {
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "MinHook create failed (%d)",
                static_cast<int>(createResult));
            g_target = nullptr;
            g_original = nullptr;
            return false;
        }
        const MH_STATUS enableResult = MH_EnableHook(g_target);
        if (enableResult != MH_OK && enableResult != MH_ERROR_ENABLED)
        {
            MH_RemoveHook(g_target);
            std::snprintf(g_validationDetail, sizeof(g_validationDetail),
                "MinHook enable failed (%d)",
                static_cast<int>(enableResult));
            g_target = nullptr;
            g_original = nullptr;
            return false;
        }

        std::snprintf(g_validationDetail, sizeof(g_validationDetail),
            "validated call RVA=0x%llX target RVA=0x%llX",
            static_cast<unsigned long long>(kAddonCallRva),
            static_cast<unsigned long long>(kExpectedTargetRva));
        g_hookInstalled.store(true, std::memory_order_release);
        diagnostic_log::writef_important(
            "addon",
            "Addon display capture hook installed after exact validation: "
            "callRva=0x%llX targetRva=0x%llX bytes=%s.",
            static_cast<unsigned long long>(kAddonCallRva),
            static_cast<unsigned long long>(kExpectedTargetRva),
            g_callBytes);
        return true;
    }

    void clear_samples()
    {
        g_sampleCount.store(0, std::memory_order_release);
        g_completedSamples.store(0, std::memory_order_release);
        g_droppedSamples.store(0, std::memory_order_release);
        for (auto& ready : g_sampleReady)
            ready.store(false, std::memory_order_release);
        for (auto& sample : g_samples)
            sample = {};
    }

    std::string status_text()
    {
        std::ostringstream output;
        output << "OK active=" << (g_active.load() ? 1 : 0)
            << " installed=" << (g_hookInstalled.load() ? 1 : 0)
            << " samples=" << g_completedSamples.load()
            << " dropped=" << g_droppedSamples.load()
            << " callRva=0x" << std::hex << kAddonCallRva
            << " targetRva=0x" << kExpectedTargetRva << std::dec
            << " detail=" << g_validationDetail
            << " bytes=" << g_callBytes;
        return output.str();
    }

    std::string sample_text(uint32_t index)
    {
        if (index >= kMaximumSamples ||
            !g_sampleReady[index].load(std::memory_order_acquire))
            return "ERROR No completed addon sample at that index.";
        const sample_t sample = g_samples[index];
        std::ostringstream output;
        output << "OK row=" << index
            << " tick=" << sample.tick
            << " thread=" << sample.threadId
            << " rcx=0x" << std::hex << sample.rcx
            << " object=0x" << sample.addonObject
            << " descriptor=0x" << sample.descriptor
            << " namePtr=0x" << sample.windowNamePointer
            << " index32=0x" << sample.descriptorIndex32
            << " index64=0x" << sample.descriptorIndex64
            << " arg5=0x" << sample.stackArguments[0]
            << " arg6=0x" << sample.stackArguments[1]
            << " arg7=0x" << sample.stackArguments[2]
            << " arg8=0x" << sample.stackArguments[3]
            << " output=0x" << sample.outputHandle << std::dec
            << " name=" << (sample.windowName[0]
                ? sample.windowName : "<unreadable>");
        return output.str();
    }

    void dump_samples()
    {
        const uint32_t count = (std::min)(
            g_sampleCount.load(std::memory_order_acquire), kMaximumSamples);
        diagnostic_log::writef_important(
            "addon", "Addon display capture dump: completed=%u dropped=%u.",
            g_completedSamples.load(std::memory_order_acquire),
            g_droppedSamples.load(std::memory_order_acquire));
        for (uint32_t index = 0; index < count; ++index)
        {
            if (!g_sampleReady[index].load(std::memory_order_acquire))
                continue;
            const sample_t sample = g_samples[index];
            diagnostic_log::writef_important(
                "addon",
                "row=%u tick=%llu thread=%u rcx=0x%llX object=0x%llX "
                "descriptor=0x%llX descriptor+160={u32=0x%X,u64=0x%llX} "
                "namePtr=0x%llX name='%s' stack={0x%llX,0x%llX,0x%llX,0x%llX} "
                "output=0x%llX",
                index,
                static_cast<unsigned long long>(sample.tick),
                sample.threadId,
                static_cast<unsigned long long>(sample.rcx),
                static_cast<unsigned long long>(sample.addonObject),
                static_cast<unsigned long long>(sample.descriptor),
                sample.descriptorIndex32,
                static_cast<unsigned long long>(sample.descriptorIndex64),
                static_cast<unsigned long long>(sample.windowNamePointer),
                sample.windowName[0] ? sample.windowName : "<unreadable>",
                static_cast<unsigned long long>(sample.stackArguments[0]),
                static_cast<unsigned long long>(sample.stackArguments[1]),
                static_cast<unsigned long long>(sample.stackArguments[2]),
                static_cast<unsigned long long>(sample.stackArguments[3]),
                static_cast<unsigned long long>(sample.outputHandle));
        }
    }
}

namespace addon_display_probe
{
    std::string command(const std::string& arguments)
    {
        std::istringstream input(trim(arguments));
        std::string action;
        input >> action;
        action = lowercase(action);
        if (action.empty() || action == "status")
        {
            tick();
            return status_text();
        }

        if (action == "arm")
        {
            uint64_t seconds = kDefaultDurationSeconds;
            std::string duration, extra;
            input >> duration;
            if (!duration.empty())
            {
                try
                {
                    size_t consumed{};
                    seconds = std::stoull(duration, &consumed, 10);
                    if (consumed != duration.size())
                        return "ERROR Use addon arm [10..300 seconds].";
                }
                catch (...)
                {
                    return "ERROR Use addon arm [10..300 seconds].";
                }
            }
            if ((input >> extra) || seconds < kMinimumDurationSeconds ||
                seconds > kMaximumDurationSeconds)
                return "ERROR Use addon arm [10..300 seconds].";
            if (g_active.load(std::memory_order_acquire))
                return "ERROR Addon capture is already active.";
            if (g_inFlightCallbacks.load(std::memory_order_acquire) != 0)
                return "ERROR The previous addon call is still completing; retry.";
            if (!resolve_and_install())
                return std::string("ERROR ") + g_validationDetail +
                    "; live bytes=" + g_callBytes;

            clear_samples();
            g_deadline.store(GetTickCount64() + seconds * 1000,
                std::memory_order_release);
            g_active.store(true, std::memory_order_release);
            diagnostic_log::writef_important(
                "addon",
                "Addon display capture armed for %llu seconds; native addon "
                "processing remains unchanged.",
                static_cast<unsigned long long>(seconds));
            return "OK Addon capture armed for " +
                std::to_string(seconds) + " seconds.";
        }

        if (action == "row")
        {
            uint64_t index{};
            std::string text, extra;
            if (!(input >> text) || (input >> extra))
                return "ERROR Use addon row <index>.";
            try
            {
                size_t consumed{};
                index = std::stoull(text, &consumed, 0);
                if (consumed != text.size() || index >= kMaximumSamples)
                    return "ERROR Use addon row <0..511>.";
            }
            catch (...)
            {
                return "ERROR Use addon row <0..511>.";
            }
            return sample_text(static_cast<uint32_t>(index));
        }

        if (action == "dump")
        {
            std::string extra;
            if (input >> extra)
                return "ERROR Use addon dump.";
            dump_samples();
            return "OK Addon samples written to PrismMedia.log.";
        }

        if (action == "cancel" || action == "stop")
        {
            g_active.store(false, std::memory_order_release);
            g_deadline.store(0, std::memory_order_release);
            return "OK Addon capture stopped.";
        }

        if (action == "clear")
        {
            if (g_active.load(std::memory_order_acquire) ||
                g_inFlightCallbacks.load(std::memory_order_acquire) != 0)
                return "ERROR Stop the addon capture before clearing it.";
            clear_samples();
            return "OK Addon capture samples cleared.";
        }

        return "ERROR Use addon arm [10..300], status, row <index>, dump, "
            "cancel, or clear.";
    }

    void tick()
    {
        if (!g_active.load(std::memory_order_acquire))
            return;
        const uint64_t deadline = g_deadline.load(std::memory_order_acquire);
        const bool full = g_sampleCount.load(std::memory_order_acquire) >=
            kMaximumSamples;
        if (full || (deadline != 0 && GetTickCount64() >= deadline))
        {
            g_active.store(false, std::memory_order_release);
            diagnostic_log::writef_important(
                "addon",
                "Addon display capture ended (%s): completed=%u dropped=%u. "
                "Use 'addon dump' to write every row.",
                full ? "sample limit" : "timeout",
                g_completedSamples.load(std::memory_order_acquire),
                g_droppedSamples.load(std::memory_order_acquire));
        }
    }

    bool active()
    {
        tick();
        return g_active.load(std::memory_order_acquire);
    }

    void shutdown()
    {
        g_active.store(false, std::memory_order_release);
        const uint64_t started = GetTickCount64();
        while (g_inFlightCallbacks.load(std::memory_order_acquire) != 0 &&
            GetTickCount64() - started < 250)
            Sleep(0);
        if (g_hookInstalled.exchange(false, std::memory_order_acq_rel) &&
            g_target)
        {
            MH_DisableHook(g_target);
            MH_RemoveHook(g_target);
        }
        g_original = nullptr;
        g_target = nullptr;
    }
}
