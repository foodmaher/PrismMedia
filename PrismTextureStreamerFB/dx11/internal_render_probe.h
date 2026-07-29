#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dx11::internal_render_probe
{
	struct candidate_t
	{
		uint32_t id{};
		uint32_t width{};
		uint32_t height{};
		uint32_t format{};
		uint32_t sampleCount{};
		uint32_t matchingCameraSlotMask{};
		uint64_t bindCount{};
		uint64_t duringMirrorScheduleBindCount{};
		uint64_t nearMirrorBindCount{};
		uint64_t firstFrame{};
		uint64_t lastFrame{};
	};

	struct status_t
	{
		bool supportedBuild{};
		bool mirrorHookInstalled{};
		bool mirrorScheduleSeen{};
		bool contextHookInstalled{};
		bool tracing{};
		uint32_t timeDateStamp{};
		uint32_t imageSize{};
		uint32_t signatureMatches{};
		uint32_t detectedRva{};
		uint32_t mirrorSlotMask{};
		uint64_t mirrorScheduleCount{};
		uint64_t traceStartedTick{};
		uint64_t traceEndTick{};
		uint64_t frameIndex{};
		size_t candidateCount{};
		uint32_t slotWidth[9]{};
		uint32_t slotHeight[9]{};
	};

	bool init();
	void shutdown();
	void on_present_frame();
	void begin_trace(uint32_t seconds = 10);
	void end_trace();

	status_t status();
	std::vector<candidate_t> candidates();
	const char* slot_name(uint32_t slot);
	const char* format_name(uint32_t format);
}
