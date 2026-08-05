#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

// The plugin is injected into the game process, so changing process affinity
// would also restrict ETS2/ATS. These helpers only give soft ideal-processor
// hints to threads that are owned by the plugin. Windows remains free to move
// them whenever the preferred logical processor is busy.
namespace thread_scheduling
{
	constexpr DWORD kUnassignedProcessor = (std::numeric_limits<DWORD>::max)();
	constexpr size_t kTrackedProcessors = 64;

	inline std::array<std::atomic<uint32_t>, kTrackedProcessors>
		g_render_processor_hits{};
	inline std::atomic<uint64_t> g_last_analysis_tick{};
	inline std::atomic<DWORD> g_background_processor_0{
		kUnassignedProcessor };
	inline std::atomic<DWORD> g_background_processor_1{
		kUnassignedProcessor };
	inline std::atomic<DWORD> g_background_processor_2{
		kUnassignedProcessor };
	inline std::atomic<uint32_t> g_background_assignment{};

	inline void insert_candidate(
		DWORD processor,
		uint32_t hits,
		DWORD (&processors)[3],
		uint32_t (&processorHits)[3])
	{
		for (size_t index = 0; index < 3; ++index)
		{
			if (hits >= processorHits[index])
				continue;
			for (size_t move = 2; move > index; --move)
			{
				processors[move] = processors[move - 1];
				processorHits[move] = processorHits[move - 1];
			}
			processors[index] = processor;
			processorHits[index] = hits;
			break;
		}
	}

	inline void observe_render_thread()
	{
		// Sampling one out of eight Presents is enough to find the logical
		// processors favoured by the render thread without adding per-frame
		// timer or topology work.
		static thread_local uint32_t sampleDivider{};
		if ((++sampleDivider & 7U) != 0)
			return;

		const DWORD currentProcessor = GetCurrentProcessorNumber();
		if (currentProcessor < kTrackedProcessors)
		{
			g_render_processor_hits[currentProcessor].fetch_add(
				1, std::memory_order_relaxed);
		}

		const uint64_t now = GetTickCount64();
		uint64_t previous = g_last_analysis_tick.load(
			std::memory_order_relaxed);
		if (previous == 0)
		{
			g_last_analysis_tick.compare_exchange_strong(
				previous, now, std::memory_order_relaxed);
			return;
		}
		if (now >= previous && now - previous < 2000)
			return;
		if (!g_last_analysis_tick.compare_exchange_strong(
			previous, now, std::memory_order_relaxed))
		{
			return;
		}

		DWORD_PTR processMask{};
		DWORD_PTR systemMask{};
		if (!GetProcessAffinityMask(
			GetCurrentProcess(), &processMask, &systemMask))
		{
			return;
		}

		SYSTEM_INFO systemInfo{};
		GetSystemInfo(&systemInfo);
		const DWORD processorCount = (std::min)(
			static_cast<DWORD>((std::min)(
				kTrackedProcessors, sizeof(DWORD_PTR) * 8)),
			systemInfo.dwNumberOfProcessors);
		DWORD processors[3]{
			kUnassignedProcessor,
			kUnassignedProcessor,
			kUnassignedProcessor };
		uint32_t processorHits[3]{
			(std::numeric_limits<uint32_t>::max)(),
			(std::numeric_limits<uint32_t>::max)(),
			(std::numeric_limits<uint32_t>::max)() };

		for (DWORD processor = 0; processor < processorCount; ++processor)
		{
			const uint32_t hits =
				g_render_processor_hits[processor].exchange(
					0, std::memory_order_relaxed);
			const DWORD_PTR bit = static_cast<DWORD_PTR>(1) << processor;
			if ((processMask & bit) == 0)
				continue;
			insert_candidate(
				processor, hits, processors, processorHits);
		}

		g_background_processor_0.store(
			processors[0], std::memory_order_relaxed);
		g_background_processor_1.store(
			processors[1], std::memory_order_relaxed);
		g_background_processor_2.store(
			processors[2], std::memory_order_relaxed);
	}

	inline DWORD preferred_processor(uint32_t slot)
	{
		switch (slot % 3)
		{
		case 0:
			return g_background_processor_0.load(
				std::memory_order_relaxed);
		case 1:
			return g_background_processor_1.load(
				std::memory_order_relaxed);
		default:
			return g_background_processor_2.load(
				std::memory_order_relaxed);
		}
	}

	inline void apply_thread_preference(HANDLE thread, uint32_t slot)
	{
		const DWORD processor = preferred_processor(slot);
		if (processor != kUnassignedProcessor)
			SetThreadIdealProcessor(thread, processor);
	}

	inline void refresh_current_thread_preference()
	{
		static thread_local const uint32_t slot =
			g_background_assignment.fetch_add(
				1, std::memory_order_relaxed);
		static thread_local DWORD appliedProcessor =
			kUnassignedProcessor;
		static thread_local uint64_t lastRefresh{};

		const uint64_t now = GetTickCount64();
		if (lastRefresh != 0 && now >= lastRefresh &&
			now - lastRefresh < 2000)
		{
			return;
		}
		lastRefresh = now;

		const DWORD processor = preferred_processor(slot);
		if (processor == kUnassignedProcessor ||
			processor == appliedProcessor)
		{
			return;
		}
		if (SetThreadIdealProcessor(
			GetCurrentThread(), processor) != kUnassignedProcessor)
		{
			appliedProcessor = processor;
		}
	}
}
