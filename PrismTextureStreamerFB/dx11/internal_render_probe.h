#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

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
		bool resourceInitHookInstalled{};
		bool activeMaskHookInstalled{};
		bool mirrorScheduleSeen{};
		bool contextHookInstalled{};
		bool tracing{};
		bool parkActivationRequested{};
		bool parkRenderRequested{};
		bool parkCameraInstalled{};
		bool parkResourcePresent{};
		bool parkMaskForced{};
		bool parkColorTargetReady{};
		bool parkCompositorReady{};
		bool parkReadbackReady{};
		uint32_t timeDateStamp{};
		uint32_t imageSize{};
		uint32_t signatureMatches{};
		uint32_t detectedRva{};
		uint32_t mirrorSlotMask{};
		uint64_t mirrorScheduleCount{};
		uint64_t parkInstallAttempts{};
		uint64_t parkScheduleCount{};
		uint64_t parkOutputFrames{};
		uint64_t parkReadbackBusySkips{};
		uint64_t traceStartedTick{};
		uint64_t traceEndTick{};
		uint64_t frameIndex{};
		size_t candidateCount{};
		uint32_t parkTargetWidth{};
		uint32_t parkTargetHeight{};
		uint32_t parkTargetFormat{};
		uint32_t parkTargetFramerate{};
		uint32_t parkTargetVariant{};
		uint32_t parkTargetCandidateCount{};
		uint32_t parkSelectedCandidate{};
		uint32_t slotWidth[9]{};
		uint32_t slotHeight[9]{};
	};

	bool init();
	void shutdown();
	void on_present_frame(ID3D11DeviceContext* context);
	void begin_trace(uint32_t seconds = 10);
	void end_trace();
	void set_park_activation_requested(bool requested);
	void set_park_render_requested(bool requested);
	void set_park_target_framerate(uint32_t framerate);
	void set_park_target_variant(uint32_t variant);
	void set_park_camera_mount(
		bool kitInstalled,
		bool trailerAware,
		float lateral,
		float height,
		float longitudinal,
		float yawDegrees,
		float pitchDegrees);
	void on_texture_created(ID3D11Texture2D* texture);
	bool copy_park_frame(
		std::vector<uint8_t>& destination,
		uint32_t& width,
		uint32_t& height,
		uint64_t& sequence);
	bool blit_park_texture(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* destination,
		ID3D11RenderTargetView* destinationView,
		bool flipVertical,
		float brightness,
		uint32_t scaleMode,
		uint32_t edgeGuard);

	status_t status();
	std::vector<candidate_t> candidates();
	const char* slot_name(uint32_t slot);
	const char* format_name(uint32_t format);
}
