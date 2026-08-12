#include "internal_render_probe.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <ShlObj.h>
#include <MinHook/MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../scs_logging.h"
#include "../telemetry_state.h"
#include "../camera_correlation.h"
#include "../camera_monitor.h"

using namespace scs_logging;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	constexpr uint32_t kSupportedTimeDateStamp = 0x6A426DE5;
	constexpr uint32_t kSupportedImageSize = 0x0382D000;
	constexpr uint32_t kExpectedMirrorScheduleRva = 0x00524DB0;
	constexpr uint32_t kExpectedActiveMaskRva = 0x00524170;
	constexpr uint32_t kExpectedResourceInitRva = 0x00533400;
	constexpr uint32_t kExpectedMirrorRenderDispatchRva = 0x00722020;
	constexpr uint32_t kExpectedMirrorWorkerRva = 0x0046CB80;
	constexpr uint32_t kExpectedRenderTaskSubmitRva = 0x00722BA0;
	constexpr uint32_t kCameraDescriptorOwnerRva = 0x03550398;
	constexpr uint32_t kMirrorSlotTokenTableRva = 0x01D1EB30;
	constexpr uint32_t kMirrorCameraVtableRva = 0x02196F90;
	constexpr uint32_t kMirrorCameraCloneRva = 0x00846B30;
	constexpr uint32_t kMirrorSlotCount = 9;
	constexpr uint32_t kParkSlot = 7;
	constexpr uint32_t kCloneSourceSlot = 5;
	constexpr uint32_t kMaximumCandidates = 256;
	constexpr uint32_t kMaximumParkTargetCandidates = 4;
	constexpr uint32_t kNoParkTargetCandidate = UINT32_MAX;
	constexpr uint32_t kMaximumIndependentRenderTargets = 32;
	// The previous experiment proved that slot 7 always supplies the game's
	// park-camera view. It is now hard-disabled: no camera installation, mask
	// scheduling, command cloning, timed copy correlation, GPS upload, or
	// monitor frame may originate from slot 7.
	constexpr bool kSlot7CameraPathEnabled = false;
	// Camera Lab is intentionally separated from the in-game GPS. Even a
	// verified future frame is published only to PrismCameraMonitor.exe.
	constexpr bool kCameraLabViewerOnly = true;
	// Never allow the legacy A/B/C/D slot-7 candidates to become the GPS
	// image in an independent-camera build. They remain observable only so
	// diagnostics can prove that mirrors are active without displaying them.
	constexpr bool kIndependentOutputOnly = true;

	constexpr std::array<uint8_t, 34> kMirrorScheduleSignature = {
		0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41,
		0x57, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,
		0x48, 0x8B, 0x81, 0x18, 0x01, 0x00, 0x00, 0x41,
		0x8B, 0xE8, 0x4C, 0x8B, 0xFA, 0x48, 0x8B, 0xF1,
		0x48, 0x85
	};

	constexpr std::array<uint8_t, 34> kActiveMaskSignature = {
		0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
		0xD9, 0x83, 0xFA, 0x02, 0x74, 0x09, 0x83, 0xFA,
		0x0C, 0x74, 0x04, 0x32, 0xC0, 0xEB, 0x02, 0xB0,
		0x01, 0x84, 0xC0, 0x0F, 0x84, 0xBF, 0x01, 0x00,
		0x00, 0x48
	};

	constexpr std::array<uint8_t, 33> kResourceInitSignature = {
		0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x49,
		0x89, 0x73, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41,
		0x56, 0x41, 0x57, 0x49, 0x8D, 0xAB, 0xA8, 0xFE,
		0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x30, 0x02, 0x00,
		0x00
	};

	constexpr std::array<uint8_t, 16> kMirrorRenderDispatchSignature = {
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
		0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D
	};
	constexpr std::array<uint8_t, 16> kMirrorWorkerSignature = {
		0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
		0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57
	};
	constexpr std::array<uint8_t, 16> kRenderTaskSubmitSignature = {
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
		0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56
	};

	struct executable_fingerprint_t
	{
		uint32_t timeDateStamp{};
		uint32_t imageSize{};
		uint32_t signatureMatches{};
		uint32_t detectedRva{};
		void* signatureAddress{};
	};

	struct candidate_record_t
	{
		dx11::internal_render_probe::candidate_t value;
	};

	using mirror_schedule_t =
		uintptr_t(__fastcall*)(
			void* visualInterior,
			void* requestContext,
			uint64_t mode,
			uint64_t fourthArgument);
	using active_mask_t =
		uint32_t(__fastcall*)(
			void* visualInterior,
			uint32_t state);
	using resource_init_t =
		void(__fastcall*)(void* visualInterior);
	using mirror_render_dispatch_t =
		void(__fastcall*)(void* renderer, void* renderContext,
			void* renderCommand);
	using mirror_worker_t = void(__fastcall*)(
		void* renderer, void* renderContext,
		void* cameraInput, void* renderRequest);
	using render_task_submit_t = void(__fastcall*)(
		void* unused,
		void** taskOutput,
		void* renderContext,
		const void* renderCommand);
	using clone_camera_t =
		void*(__fastcall*)(void* camera);
	using om_set_render_targets_t =
		void(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context,
			UINT renderTargetViewCount,
			ID3D11RenderTargetView* const* renderTargetViews,
			ID3D11DepthStencilView* depthStencilView);
	using om_set_render_targets_uav_t =
		void(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context,
			UINT renderTargetViewCount,
			ID3D11RenderTargetView* const* renderTargetViews,
			ID3D11DepthStencilView* depthStencilView,
			UINT unorderedAccessViewStartSlot,
			UINT unorderedAccessViewCount,
			ID3D11UnorderedAccessView* const* unorderedAccessViews,
			const UINT* initialCounts);
	using finish_command_list_t =
		HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context,
			BOOL restoreDeferredContextState,
			ID3D11CommandList** commandList);
	using execute_command_list_t =
		void(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context,
			ID3D11CommandList* commandList,
			BOOL restoreContextState);
	using copy_subresource_region_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
		ID3D11Resource*, UINT, const D3D11_BOX*);
	using copy_resource_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
	using resolve_subresource_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, UINT,
		ID3D11Resource*, UINT, DXGI_FORMAT);

	std::atomic<bool> g_supportedBuild{};
	std::atomic<bool> g_mirrorHookInstalled{};
	std::atomic<bool> g_resourceInitHookInstalled{};
	std::atomic<bool> g_activeMaskHookInstalled{};
	std::atomic<bool> g_contextHookInstalled{};
	std::atomic<bool> g_mirrorJobHookInstalled{};
	std::atomic<bool> g_commandListHooksInstalled{};
	std::atomic<bool> g_gameContextObserverConfirmed{};
	std::atomic<bool> g_uavTargetHookInstalled{};
	std::atomic<bool> g_mirrorScheduleSeen{};
	std::atomic<bool> g_tracing{};
	std::atomic<bool> g_parkActivationRequested{};
	std::atomic<bool> g_parkRenderRequested{};
	std::atomic<bool> g_parkCameraInstalled{};
	std::atomic<bool> g_parkResourcePresent{};
	std::atomic<bool> g_parkMaskForced{};
	std::atomic<uint32_t> g_timeDateStamp{};
	std::atomic<uint32_t> g_imageSize{};
	std::atomic<uint32_t> g_signatureMatches{};
	std::atomic<uint32_t> g_detectedRva{};
	std::atomic<uint32_t> g_mirrorSlotMask{};
	std::atomic<uint64_t> g_mirrorScheduleCount{};
	std::atomic<uint64_t> g_parkInstallAttempts{};
	std::atomic<uint64_t> g_parkScheduleCount{};
	std::atomic<uint64_t> g_parkOutputFrames{};
	std::atomic<uint64_t> g_parkReadbackBusySkips{};
	std::atomic<uint64_t> g_slot7DispatchCount{};
	std::atomic<uint32_t> g_slot7RenderDepth{};
	std::atomic<uint64_t> g_lastSlot7DispatchEndTick{};
	std::atomic<uint64_t> g_commandListTagCount{};
	std::atomic<uint64_t> g_commandListExecuteCount{};
	std::atomic<uint64_t> g_lastParkScheduleTick{};
	std::atomic<uint64_t> g_lastParkForcedFrame{UINT64_MAX};
	std::atomic<uint32_t> g_parkTargetFramerate{15};
	std::atomic<uint32_t> g_parkTargetVariant{1};
	std::atomic<bool> g_parkCameraKitInstalled{};
	std::atomic<bool> g_parkTrailerAwareMount{true};
	std::atomic<float> g_parkMountLateral{};
	std::atomic<float> g_parkMountHeight{2.6f};
	std::atomic<float> g_parkMountLongitudinal{-0.35f};
	std::atomic<float> g_parkMountYaw{180.0f};
	std::atomic<float> g_parkMountPitch{-8.0f};
	std::atomic<bool> g_parkColorTargetReady{};
	std::atomic<bool> g_parkCompositorReady{};
	std::atomic<uint32_t> g_parkTargetWidth{};
	std::atomic<uint32_t> g_parkTargetHeight{};
	std::atomic<uint32_t> g_parkTargetFormat{};
	std::atomic<uint64_t> g_lastMirrorScheduleFrame{
		UINT64_MAX
	};
	std::atomic<uint64_t> g_frameIndex{};
	std::atomic<uint64_t> g_traceStartedTick{};
	std::atomic<uint64_t> g_traceEndTick{};
	std::atomic<uint64_t> g_traceStartedMirrorScheduleCount{};
	std::array<std::atomic<uint32_t>, kMirrorSlotCount>
		g_slotWidth{};
	std::array<std::atomic<uint32_t>, kMirrorSlotCount>
		g_slotHeight{};
	uint8_t* g_executableBase{};

	void* g_mirrorScheduleAddress{};
	void* g_resourceInitAddress{};
	void* g_activeMaskAddress{};
	void* g_omSetRenderTargetsAddress{};
	void* g_omSetRenderTargetsUavAddress{};
	void* g_gameOmSetRenderTargetsAddress{};
	void* g_gameOmSetRenderTargetsUavAddress{};
	void* g_gameCopySubresourceRegionAddress{};
	void* g_gameCopyResourceAddress{};
	void* g_gameResolveSubresourceAddress{};
	void* g_finishCommandListAddress{};
	void* g_executeCommandListAddress{};
	void* g_mirrorRenderDispatchAddress{};
	void* g_mirrorWorkerAddress{};
	void* g_renderTaskSubmitAddress{};
	mirror_schedule_t g_originalMirrorSchedule{};
	resource_init_t g_originalResourceInit{};
	active_mask_t g_originalActiveMask{};
	om_set_render_targets_t g_originalOmSetRenderTargets{};
	om_set_render_targets_uav_t g_originalOmSetRenderTargetsUav{};
	om_set_render_targets_t g_originalGameOmSetRenderTargets{};
	om_set_render_targets_uav_t g_originalGameOmSetRenderTargetsUav{};
	copy_subresource_region_t g_originalGameCopySubresourceRegion{};
	copy_resource_t g_originalGameCopyResource{};
	resolve_subresource_t g_originalGameResolveSubresource{};
	finish_command_list_t g_originalFinishCommandList{};
	execute_command_list_t g_originalExecuteCommandList{};
	mirror_render_dispatch_t g_originalMirrorRenderDispatch{};
	mirror_worker_t g_originalMirrorWorker{};
	render_task_submit_t g_renderTaskSubmit{};
	thread_local void* g_workerRenderer{};
	thread_local void* g_workerRenderContext{};
	thread_local void* g_workerCameraInput{};
	thread_local void* g_workerRenderRequest{};
	std::atomic<uint32_t> g_independentCameraSnapshots{};
	std::atomic<uint32_t> g_independentWorkerSnapshots{};
	std::atomic<uint64_t> g_lastIndependentCommandSnapshotTick{};
	std::atomic<uint64_t> g_lastIndependentWorkerSnapshotTick{};
	std::atomic<uint32_t> g_independentTargetEvents{};
	std::atomic<bool> g_independentSubmitAttempted{};
	std::atomic<bool> g_independentSubmitInProgress{};
	std::atomic<bool> g_independentSubmitSucceeded{};
	std::atomic<uint32_t> g_independentDispatchCount{};
	std::atomic<uint32_t> g_independentRenderDepth{};
	std::atomic<uint32_t> g_independentTaggedTargetCount{};
	std::atomic<void*> g_independentRenderTask{};
	std::atomic<bool> g_independentTemplateReady{};
	std::atomic<void*> g_independentRenderContext{};
	std::atomic<bool> g_independentExclusiveWindow{};
	std::atomic<uint64_t> g_independentSubmitDueTick{};
	std::atomic<bool> g_independentOutputCapturePending{};
	std::atomic<bool> g_independentOutputCaptured{};
	std::atomic<bool> g_customCameraStateVerified{};
	std::atomic<uint64_t> g_cameraLabObservedJobs{};
	std::atomic<uint64_t> g_cameraLabRejectedSlot7Jobs{};
	std::atomic<uint64_t> g_independentOutputArmedTick{};
	std::atomic<uint32_t> g_independentOutputCopyCount{};
	std::atomic<uint32_t> g_independentCorrelatedCopyCount{};
	alignas(16) std::array<uint8_t, 0xF0>
		g_independentCommandTemplate{};
	std::mutex g_independentCameraDiagnosticMutex;
	std::mutex g_independentRenderTargetMutex;
	std::array<uintptr_t, kMaximumIndependentRenderTargets>
		g_independentRenderTargets{};
	uint32_t g_independentRenderTargetCount{};
	std::array<uint64_t, 16> g_independentCameraSnapshotHashes{};
	std::atomic<void*> g_parkVisualInterior{};
	std::atomic<void*> g_parkCamera{};
	std::array<float, 4> g_parkSourcePosition{};
	std::array<float, 4> g_parkSourceOrientation{};
	bool g_parkSourcePositionValid{};

	std::mutex g_candidateMutex;
	std::mutex g_gameContextHookMutex;
	std::unordered_map<uintptr_t, candidate_record_t> g_candidates;
	struct lineage_key_t
	{
		uintptr_t source{};
		uintptr_t destination{};
		bool operator==(const lineage_key_t& other) const
		{
			return source == other.source && destination == other.destination;
		}
	};
	struct lineage_key_hash_t
	{
		size_t operator()(const lineage_key_t& key) const
		{
			return std::hash<uintptr_t>{}(key.source) ^
				(std::hash<uintptr_t>{}(key.destination) << 1);
		}
	};
	struct lineage_record_t
	{
		D3D11_TEXTURE2D_DESC source{};
		D3D11_TEXTURE2D_DESC destination{};
		uint64_t copyRegionCount{};
		uint64_t copyResourceCount{};
		uint64_t resolveCount{};
		uint64_t firstFrame{};
		uint64_t lastFrame{};
		uint32_t threadId{};
	};
	std::unordered_map<lineage_key_t, lineage_record_t,
		lineage_key_hash_t> g_lineage;
	uint32_t g_nextCandidateId = 1;
	thread_local uint32_t g_mirrorScheduleDepth{};
	thread_local int32_t g_renderingMirrorSlot{-1};
	thread_local bool g_capturingParkResourceInit{};

	struct deferred_context_record_t
	{
		int32_t slot{-1};
		ID3D11Texture2D* lastParkSizedTarget{};
	};
	struct command_list_record_t
	{
		int32_t slot{-1};
		ID3D11Texture2D* lastParkSizedTarget{};
	};
	std::mutex g_commandListMutex;
	std::unordered_map<ID3D11DeviceContext*, deferred_context_record_t>
		g_deferredContexts;
	std::unordered_map<ID3D11CommandList*, command_list_record_t>
		g_commandLists;

	std::mutex g_parkTextureMutex;
	ID3D11Texture2D* g_parkColorTexture{};
	ID3D11Texture2D* g_independentColorTexture{};
	uint32_t g_parkColorTextureScore{};
	struct park_target_candidate_t
	{
		ID3D11Texture2D* texture{};
		uintptr_t sourceIdentity{};
		uint64_t observationCount{};
		uint64_t lastObservedFrame{UINT64_MAX};
		uint64_t lastObservedTick{};
	};
	std::array<
		park_target_candidate_t,
		kMaximumParkTargetCandidates> g_parkTargetCandidates{};
	uint32_t g_parkTargetCandidateCount{};
	uint32_t g_parkSelectedCandidate{
		kNoParkTargetCandidate
	};
	ID3D11Device* g_compositorDevice{};
	ID3D11Texture2D* g_parkSampleTexture{};
	ID3D11ShaderResourceView* g_parkSampleView{};
	ID3D11VertexShader* g_parkVertexShader{};
	ID3D11PixelShader* g_parkPixelShader{};
	ID3D11SamplerState* g_parkSampler{};
	ID3D11Buffer* g_parkConstants{};
	ID3D11Device* g_parkReadbackDevice{};
	struct park_staging_slot_t
	{
		ID3D11Texture2D* texture{};
		bool pending{};
		uint64_t submissionOrder{};
	};
	std::array<park_staging_slot_t, 3> g_parkStaging{};
	uint32_t g_nextParkStaging{};
	uint64_t g_parkObservedFrame{UINT64_MAX};
	uint64_t g_parkSubmittedFrame{UINT64_MAX};
	std::vector<uint8_t> g_parkReadbackPixels;
	uint32_t g_parkReadbackWidth{};
	uint32_t g_parkReadbackHeight{};
	uint64_t g_parkReadbackSequence{};
	uint64_t g_parkNextSubmissionOrder{};
	uint64_t g_parkLastDecodedSubmissionOrder{};
	bool g_parkReadbackReady{};
	std::atomic<bool> g_parkDiagnosticImageSaved{};
	std::array<uint8_t, 2048> g_parkFloat11Lut{};
	std::array<uint8_t, 1024> g_parkFloat10Lut{};
	bool g_parkToneMapLutReady{};

	struct park_compositor_constants_t
	{
		float brightness{};
		float flipVertical{};
		float edgeGuardX{};
		float edgeGuardY{};
		float sourceAspect{};
		float destinationAspect{};
		uint32_t scaleMode{};
		float padding{};
	};

	constexpr const char* kParkCompositorShader = R"(
Texture2D ParkTexture : register(t0);
SamplerState ParkSampler : register(s0);

cbuffer ParkConstants : register(b0)
{
	float Brightness;
	float FlipVertical;
	float EdgeGuardX;
	float EdgeGuardY;
	float SourceAspect;
	float DestinationAspect;
	uint ScaleMode;
	float Padding;
};

struct PixelInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD0;
};

PixelInput vs_main(uint vertexId : SV_VertexID)
{
	PixelInput output;
	float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
	output.position = float4(
		uv.x * 2.0f - 1.0f,
		1.0f - uv.y * 2.0f,
		0.0f,
		1.0f);
	output.uv = uv;
	return output;
}

float4 ps_main(PixelInput input) : SV_TARGET
{
	float2 displayUv = input.uv;
	if (displayUv.x < EdgeGuardX ||
		displayUv.x > 1.0f - EdgeGuardX ||
		displayUv.y < EdgeGuardY ||
		displayUv.y > 1.0f - EdgeGuardY)
	{
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	float2 uv = displayUv;
	if (ScaleMode == 1)
	{
		if (SourceAspect > DestinationAspect)
		{
			float imageHeight = DestinationAspect / SourceAspect;
			float top = (1.0f - imageHeight) * 0.5f;
			if (uv.y < top || uv.y > top + imageHeight)
				return float4(0.0f, 0.0f, 0.0f, 1.0f);
			uv.y = (uv.y - top) / imageHeight;
		}
		else
		{
			float imageWidth = SourceAspect / DestinationAspect;
			float left = (1.0f - imageWidth) * 0.5f;
			if (uv.x < left || uv.x > left + imageWidth)
				return float4(0.0f, 0.0f, 0.0f, 1.0f);
			uv.x = (uv.x - left) / imageWidth;
		}
	}
	else if (ScaleMode == 2)
	{
		if (SourceAspect > DestinationAspect)
		{
			float sourceWidth = DestinationAspect / SourceAspect;
			uv.x = 0.5f + (uv.x - 0.5f) * sourceWidth;
		}
		else
		{
			float sourceHeight = SourceAspect / DestinationAspect;
			uv.y = 0.5f + (uv.y - 0.5f) * sourceHeight;
		}
	}

	if (FlipVertical > 0.5f)
		uv.y = 1.0f - uv.y;

	float3 colour = max(
		ParkTexture.Sample(ParkSampler, uv).rgb,
		float3(0.0f, 0.0f, 0.0f));
	colour = colour / (1.0f + colour);
	colour = pow(saturate(colour), 1.0f / 2.2f);
	colour = saturate(colour * Brightness);
	return float4(colour, 1.0f);
}
)";

	template <typename T>
	void release_com_object(T*& object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	void release_park_sample_resources_locked()
	{
		release_com_object(g_parkSampleView);
		release_com_object(g_parkSampleTexture);
	}

	void release_park_readback_resources_locked()
	{
		for (auto& slot : g_parkStaging)
		{
			release_com_object(slot.texture);
			slot.pending = false;
			slot.submissionOrder = 0;
		}
		release_com_object(g_parkReadbackDevice);
		g_nextParkStaging = 0;
		g_parkObservedFrame = UINT64_MAX;
		g_parkSubmittedFrame = UINT64_MAX;
		g_parkReadbackPixels.clear();
		g_parkReadbackWidth = 0;
		g_parkReadbackHeight = 0;
		g_parkReadbackSequence = 0;
		g_parkNextSubmissionOrder = 0;
		g_parkLastDecodedSubmissionOrder = 0;
		g_parkReadbackReady = false;
	}

	void release_park_compositor_locked()
	{
		release_park_sample_resources_locked();
		release_com_object(g_parkConstants);
		release_com_object(g_parkSampler);
		release_com_object(g_parkPixelShader);
		release_com_object(g_parkVertexShader);
		release_com_object(g_compositorDevice);
		g_parkCompositorReady.store(
			false, std::memory_order_relaxed);
	}

	void release_park_color_target_locked()
	{
		release_com_object(g_parkColorTexture);
		release_com_object(g_independentColorTexture);
		for (auto& candidate : g_parkTargetCandidates)
		{
			release_com_object(candidate.texture);
			candidate.sourceIdentity = 0;
			candidate.observationCount = 0;
			candidate.lastObservedFrame = UINT64_MAX;
			candidate.lastObservedTick = 0;
		}
		g_parkTargetCandidateCount = 0;
		g_parkSelectedCandidate = kNoParkTargetCandidate;
		g_parkColorTextureScore = 0;
		g_parkColorTargetReady.store(
			false, std::memory_order_relaxed);
		g_parkTargetWidth.store(0, std::memory_order_relaxed);
		g_parkTargetHeight.store(0, std::memory_order_relaxed);
		g_parkTargetFormat.store(0, std::memory_order_relaxed);
		g_independentOutputCaptured.store(
			false, std::memory_order_relaxed);
		g_independentOutputCapturePending.store(
			false, std::memory_order_relaxed);
		release_park_sample_resources_locked();
		release_park_readback_resources_locked();
	}

	void release_legacy_park_sources_locked()
	{
		// A visual-interior/mirror resource rebuild invalidates the engine's
		// A/B/C/D targets, but it does not invalidate our plugin-owned snapshot
		// on the same D3D device. Drop only engine-owned observations here.
		release_com_object(g_parkColorTexture);
		for (auto& candidate : g_parkTargetCandidates)
		{
			release_com_object(candidate.texture);
			candidate = {};
		}
		g_parkTargetCandidateCount = 0;
		g_parkSelectedCandidate = kNoParkTargetCandidate;
		g_parkColorTextureScore = 0;

		if (g_independentColorTexture &&
			g_independentOutputCaptured.load(
				std::memory_order_acquire))
		{
			// Keep the compatibility alias and readiness/status fields coherent;
			// actual display/readback paths use g_independentColorTexture directly.
			g_parkColorTexture = g_independentColorTexture;
			g_parkColorTexture->AddRef();
			D3D11_TEXTURE2D_DESC description{};
			g_independentColorTexture->GetDesc(&description);
			g_parkColorTextureScore = UINT32_MAX;
			g_parkTargetWidth.store(
				description.Width, std::memory_order_relaxed);
			g_parkTargetHeight.store(
				description.Height, std::memory_order_relaxed);
			g_parkTargetFormat.store(
				static_cast<uint32_t>(description.Format),
				std::memory_order_relaxed);
			g_parkColorTargetReady.store(
				true, std::memory_order_release);
			scs_log(0,
				"[RTT custom] Game mirror resources rebuilt; preserved "
				"the plugin-owned independent snapshot.");
			return;
		}

		g_parkColorTargetReady.store(
			false, std::memory_order_relaxed);
		g_parkTargetWidth.store(0, std::memory_order_relaxed);
		g_parkTargetHeight.store(0, std::memory_order_relaxed);
		g_parkTargetFormat.store(0, std::memory_order_relaxed);
		release_park_sample_resources_locked();
		release_park_readback_resources_locked();
	}

	void release_legacy_park_sources()
	{
		std::lock_guard<std::mutex> lock(g_parkTextureMutex);
		release_legacy_park_sources_locked();
	}

	void release_park_color_target()
	{
		std::lock_guard<std::mutex> lock(g_parkTextureMutex);
		release_park_color_target_locked();
	}

	void select_park_target_locked(
		uint32_t candidateIndex,
		uint64_t currentFrame)
	{
		if (candidateIndex >= g_parkTargetCandidateCount)
			return;

		auto& candidate =
			g_parkTargetCandidates[candidateIndex];
		if (!candidate.texture)
			return;

		const bool changed =
			g_parkColorTexture != candidate.texture;
		g_parkSelectedCandidate = candidateIndex;
		if (changed)
		{
			release_com_object(g_parkColorTexture);
			g_parkColorTexture = candidate.texture;
			g_parkColorTexture->AddRef();
			g_parkSubmittedFrame = UINT64_MAX;
		}

		D3D11_TEXTURE2D_DESC description{};
		g_parkColorTexture->GetDesc(&description);
		g_parkObservedFrame = currentFrame;
		g_parkTargetWidth.store(
			description.Width, std::memory_order_relaxed);
		g_parkTargetHeight.store(
			description.Height, std::memory_order_relaxed);
		g_parkTargetFormat.store(
			static_cast<uint32_t>(description.Format),
			std::memory_order_relaxed);
		const bool firstTarget =
			!g_parkColorTargetReady.exchange(
				true, std::memory_order_acq_rel);
		if (changed || firstTarget)
		{
			scs_log(
				0,
				"[RTT park] Selected target candidate %c: "
				"%ux%u %s.",
				static_cast<char>('A' + candidateIndex),
				description.Width,
				description.Height,
				dx11::internal_render_probe::format_name(
					static_cast<uint32_t>(
						description.Format)));
		}
	}

	float decode_unsigned_float(
		uint32_t bits,
		uint32_t mantissaBits)
	{
		const uint32_t mantissaMask =
			(1U << mantissaBits) - 1U;
		const uint32_t mantissa = bits & mantissaMask;
		const uint32_t exponent =
			(bits >> mantissaBits) & 0x1FU;
		if (exponent == 0)
		{
			return std::ldexp(
				static_cast<float>(mantissa),
				1 - 15 - static_cast<int>(mantissaBits));
		}
		if (exponent == 0x1FU)
			return mantissa == 0 ? 65504.0f : 0.0f;
		return std::ldexp(
			1.0f +
				static_cast<float>(mantissa) /
					static_cast<float>(1U << mantissaBits),
			static_cast<int>(exponent) - 15);
	}

	uint8_t tone_map_channel(float value)
	{
		value = (std::max)(0.0f, value);
		value = value / (1.0f + value);
		value = std::pow(
			(std::clamp)(value, 0.0f, 1.0f),
			1.0f / 2.2f);
		return static_cast<uint8_t>(
			std::lround(
				(std::clamp)(value, 0.0f, 1.0f) *
				255.0f));
	}

	float decode_half_float(uint16_t bits)
	{
		const uint32_t sign = (bits >> 15) & 1U;
		const uint32_t exponent = (bits >> 10) & 0x1FU;
		const uint32_t mantissa = bits & 0x3FFU;
		float value{};
		if (exponent == 0)
			value = std::ldexp(static_cast<float>(mantissa), -24);
		else if (exponent == 0x1FU)
			value = mantissa == 0 ? 65504.0f : 0.0f;
		else
			value = std::ldexp(
				1.0f + static_cast<float>(mantissa) / 1024.0f,
				static_cast<int>(exponent) - 15);
		return sign ? -value : value;
	}

	void ensure_park_tone_map_lut()
	{
		if (g_parkToneMapLutReady)
			return;
		for (uint32_t value = 0;
			value < g_parkFloat11Lut.size(); ++value)
		{
			g_parkFloat11Lut[value] =
				tone_map_channel(
					decode_unsigned_float(value, 6));
		}
		for (uint32_t value = 0;
			value < g_parkFloat10Lut.size(); ++value)
		{
			g_parkFloat10Lut[value] =
				tone_map_channel(
					decode_unsigned_float(value, 5));
		}
		g_parkToneMapLutReady = true;
	}

	void clear_previous_park_diagnostic_bmp()
	{
		PWSTR documentsPath{};
		if (FAILED(SHGetKnownFolderPath(
				FOLDERID_Documents,
				KF_FLAG_DEFAULT,
				nullptr,
				&documentsPath)) ||
			!documentsPath)
		{
			if (documentsPath)
				CoTaskMemFree(documentsPath);
			return;
		}

		const std::wstring filePath =
			std::wstring(documentsPath) +
			L"\\ETS2\\PrismParkCapture.bmp";
		CoTaskMemFree(documentsPath);
		if (!DeleteFileW(filePath.c_str()))
		{
			const DWORD error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND &&
				error != ERROR_PATH_NOT_FOUND)
			{
				scs_log(2,
					"[RTT custom] Could not clear the previous park "
					"capture (Win32 error %lu).",
					error);
			}
		}
	}

	bool save_park_diagnostic_bmp_locked()
	{
		if (g_parkReadbackPixels.empty() ||
			g_parkReadbackWidth == 0 ||
			g_parkReadbackHeight == 0)
		{
			return false;
		}

		PWSTR documentsPath{};
		const HRESULT folderResult = SHGetKnownFolderPath(
			FOLDERID_Documents,
			KF_FLAG_DEFAULT,
			nullptr,
			&documentsPath);
		if (FAILED(folderResult) || !documentsPath)
		{
			scs_log(2,
				"[RTT custom] Failed to locate the Documents folder "
				"for the park capture (HRESULT 0x%08X).",
				static_cast<unsigned>(folderResult));
			if (documentsPath)
				CoTaskMemFree(documentsPath);
			return false;
		}

		std::wstring directory(documentsPath);
		CoTaskMemFree(documentsPath);
		directory += L"\\ETS2";
		if (!CreateDirectoryW(directory.c_str(), nullptr) &&
			GetLastError() != ERROR_ALREADY_EXISTS)
		{
			scs_log(2,
				"[RTT custom] Failed to create Documents\\ETS2 for "
				"the park capture (Win32 error %lu).",
				GetLastError());
			return false;
		}

		const std::wstring filePath =
			directory + L"\\PrismParkCapture.bmp";
		HANDLE file = CreateFileW(
			filePath.c_str(),
			GENERIC_WRITE,
			FILE_SHARE_READ,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			scs_log(2,
				"[RTT custom] Failed to open the park capture image "
				"in Documents\\ETS2 (Win32 error %lu).",
				GetLastError());
			return false;
		}

		const uint64_t pixelBytes64 =
			static_cast<uint64_t>(g_parkReadbackWidth) *
			g_parkReadbackHeight * 4ULL;
		if (pixelBytes64 > MAXDWORD)
		{
			CloseHandle(file);
			return false;
		}
		const DWORD pixelBytes =
			static_cast<DWORD>(pixelBytes64);
		BITMAPFILEHEADER fileHeader{};
		BITMAPINFOHEADER infoHeader{};
		fileHeader.bfType = 0x4D42;
		fileHeader.bfOffBits =
			sizeof(fileHeader) + sizeof(infoHeader);
		fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
		infoHeader.biSize = sizeof(infoHeader);
		infoHeader.biWidth =
			static_cast<LONG>(g_parkReadbackWidth);
		// A negative BMP height preserves the D3D top-to-bottom row order.
		infoHeader.biHeight =
			-static_cast<LONG>(g_parkReadbackHeight);
		infoHeader.biPlanes = 1;
		infoHeader.biBitCount = 32;
		infoHeader.biCompression = BI_RGB;
		infoHeader.biSizeImage = pixelBytes;

		DWORD written{};
		bool saved =
			WriteFile(file, &fileHeader, sizeof(fileHeader),
				&written, nullptr) &&
			written == sizeof(fileHeader);
		if (saved)
		{
			saved = WriteFile(file, &infoHeader, sizeof(infoHeader),
				&written, nullptr) &&
				written == sizeof(infoHeader);
		}
		if (saved)
		{
			saved = WriteFile(file, g_parkReadbackPixels.data(),
				pixelBytes, &written, nullptr) &&
				written == pixelBytes;
		}
		CloseHandle(file);

		if (saved)
		{
			scs_log(0,
				"[RTT custom] Saved independent park capture to "
				"Documents\\ETS2\\PrismParkCapture.bmp (%ux%u).",
				g_parkReadbackWidth,
				g_parkReadbackHeight);
		}
		else
		{
			DeleteFileW(filePath.c_str());
			scs_log(2,
				"[RTT custom] Failed while writing "
				"Documents\\ETS2\\PrismParkCapture.bmp.");
		}
		return saved;
	}

	bool decode_park_readback_locked(
		const D3D11_MAPPED_SUBRESOURCE& mapped,
		const D3D11_TEXTURE2D_DESC& description)
	{
		if (!mapped.pData ||
			(description.Format != DXGI_FORMAT_R11G11B10_FLOAT &&
				description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
		{
			return false;
		}

		const size_t pixelCount =
			static_cast<size_t>(description.Width) *
			description.Height;
		if (description.Format == DXGI_FORMAT_R11G11B10_FLOAT)
			ensure_park_tone_map_lut();
		g_parkReadbackPixels.resize(pixelCount * 4);
		const auto* sourceBase =
			static_cast<const uint8_t*>(mapped.pData);
		for (uint32_t y = 0;
			y < description.Height; ++y)
		{
			const uint8_t* sourceRow = sourceBase +
				static_cast<size_t>(y) * mapped.RowPitch;
			uint8_t* destinationRow =
				g_parkReadbackPixels.data() +
				static_cast<size_t>(y) *
					description.Width * 4;
			for (uint32_t x = 0;
				x < description.Width; ++x)
			{
				if (description.Format == DXGI_FORMAT_R11G11B10_FLOAT)
				{
					const uint32_t packed =
						reinterpret_cast<const uint32_t*>(sourceRow)[x];
					destinationRow[x * 4 + 0] =
						g_parkFloat10Lut[(packed >> 22) & 0x3FFU];
					destinationRow[x * 4 + 1] =
						g_parkFloat11Lut[(packed >> 11) & 0x7FFU];
					destinationRow[x * 4 + 2] =
						g_parkFloat11Lut[packed & 0x7FFU];
				}
				else
				{
					const auto* rgba =
						reinterpret_cast<const uint16_t*>(sourceRow) + x * 4;
					destinationRow[x * 4 + 0] =
						tone_map_channel(decode_half_float(rgba[2]));
					destinationRow[x * 4 + 1] =
						tone_map_channel(decode_half_float(rgba[1]));
					destinationRow[x * 4 + 2] =
						tone_map_channel(decode_half_float(rgba[0]));
				}
				destinationRow[x * 4 + 3] = 255;
			}
		}

		g_parkReadbackWidth = description.Width;
		g_parkReadbackHeight = description.Height;
		++g_parkReadbackSequence;
		g_parkReadbackReady = true;
		scs_log(0,
			"[RTT custom] Independent CPU readback decoded: "
			"%ux%u sequence=%llu.",
			g_parkReadbackWidth,
			g_parkReadbackHeight,
			static_cast<unsigned long long>(
				g_parkReadbackSequence));
		if (kIndependentOutputOnly &&
			g_independentOutputCaptured.load(
				std::memory_order_acquire) &&
			!g_parkDiagnosticImageSaved.load(
				std::memory_order_relaxed) &&
			save_park_diagnostic_bmp_locked())
		{
			g_parkDiagnosticImageSaved.store(
				true, std::memory_order_release);
		}
		if (g_customCameraStateVerified.load(
				std::memory_order_acquire))
		{
			camera_monitor::publish_frame(
				g_parkReadbackPixels.data(),
				g_parkReadbackWidth,
				g_parkReadbackHeight,
				g_parkReadbackWidth * 4,
				"Frame accepted because both the custom camera state and "
				"plugin-owned render target were uniquely verified.");
		}
		g_parkOutputFrames.fetch_add(
			1, std::memory_order_relaxed);
		return true;
	}

	bool ensure_park_staging_locked(
		ID3D11Device* device,
		const D3D11_TEXTURE2D_DESC& sourceDescription)
	{
		if (!device ||
			sourceDescription.SampleDesc.Count != 1 ||
			(sourceDescription.Format != DXGI_FORMAT_R11G11B10_FLOAT &&
				sourceDescription.Format != DXGI_FORMAT_R16G16B16A16_FLOAT))
		{
			return false;
		}

		if (g_parkReadbackDevice == device &&
			g_parkStaging[0].texture)
		{
			D3D11_TEXTURE2D_DESC existing{};
			g_parkStaging[0].texture->GetDesc(&existing);
			if (existing.Width == sourceDescription.Width &&
				existing.Height == sourceDescription.Height &&
				existing.Format == sourceDescription.Format)
			{
				return true;
			}
		}

		release_park_readback_resources_locked();
		g_parkReadbackDevice = device;
		g_parkReadbackDevice->AddRef();

		D3D11_TEXTURE2D_DESC stagingDescription =
			sourceDescription;
		stagingDescription.MipLevels = 1;
		stagingDescription.ArraySize = 1;
		stagingDescription.Usage = D3D11_USAGE_STAGING;
		stagingDescription.BindFlags = 0;
		stagingDescription.CPUAccessFlags =
			D3D11_CPU_ACCESS_READ;
		stagingDescription.MiscFlags = 0;

		for (auto& slot : g_parkStaging)
		{
			const HRESULT result = device->CreateTexture2D(
				&stagingDescription,
				nullptr,
				&slot.texture);
			if (FAILED(result) || !slot.texture)
			{
				scs_log(
					2,
					"[RTT park] Could not create a safe "
					"staging texture (hr=0x%08X).",
					result);
				release_park_readback_resources_locked();
				return false;
			}
		}

		scs_log(
			0,
			"[RTT park] Safe staged readback initialized: "
			"%ux%u %s.",
			sourceDescription.Width,
			sourceDescription.Height,
			dx11::internal_render_probe::format_name(
				static_cast<uint32_t>(
					sourceDescription.Format)));
		return true;
	}

	bool observe_park_colour_target(
		ID3D11Texture2D* texture,
		const D3D11_TEXTURE2D_DESC& description,
		uint64_t currentFrame,
		uintptr_t sourceIdentity = 0)
	{
		if (!texture ||
			sourceIdentity == 0 ||
			!g_parkRenderRequested.load(
				std::memory_order_relaxed) ||
			description.SampleDesc.Count != 1)
		{
			return false;
		}
		switch (description.Format)
		{
		case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			break;
		default:
			return false;
		}

		const uint32_t width =
			g_slotWidth[kParkSlot].load(
				std::memory_order_relaxed);
		const uint32_t height =
			g_slotHeight[kParkSlot].load(
				std::memory_order_relaxed);
		const bool exactSize =
			description.Width == width &&
			description.Height == height;
		const bool scaledSize =
			width <= UINT32_MAX / 2 && height <= UINT32_MAX / 2 &&
			description.Width == width * 2 &&
			description.Height == height * 2;
		if (width == 0 || height == 0 ||
			(!exactSize && !scaledSize))
		{
			return false;
		}

		const uint64_t now = GetTickCount64();
		const uint64_t staleAfterMs = 1000;
		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		uint32_t candidateIndex =
			kNoParkTargetCandidate;
		for (uint32_t index = 0;
			index < g_parkTargetCandidateCount; ++index)
		{
			if (g_parkTargetCandidates[index].texture == texture &&
				g_parkTargetCandidates[index].sourceIdentity ==
					sourceIdentity)
			{
				candidateIndex = index;
				break;
			}
		}

		if (candidateIndex == kNoParkTargetCandidate)
		{
			if (g_parkTargetCandidateCount >=
				kMaximumParkTargetCandidates)
			{
				uint32_t oldestIndex = 0;
				uint64_t oldestTick = UINT64_MAX;
				for (uint32_t index = 0;
					index < g_parkTargetCandidateCount; ++index)
				{
					if (g_parkTargetCandidates[index].lastObservedTick <
						oldestTick)
					{
						oldestTick =
							g_parkTargetCandidates[index].lastObservedTick;
						oldestIndex = index;
					}
				}
				if (oldestTick != 0 &&
					now - oldestTick < staleAfterMs)
					return false;

				candidateIndex = oldestIndex;
				auto& stale =
					g_parkTargetCandidates[candidateIndex];
				if (g_parkSelectedCandidate == candidateIndex)
				{
					release_com_object(g_parkColorTexture);
					g_parkSelectedCandidate =
						kNoParkTargetCandidate;
					g_parkColorTargetReady.store(
						false, std::memory_order_relaxed);
				}
				release_com_object(stale.texture);
				stale = {};
				scs_log(
					0,
					"[RTT park] Recycled stale copy path %c.",
					static_cast<char>('A' + candidateIndex));
			}
			else
			{
				candidateIndex = g_parkTargetCandidateCount++;
			}
			auto& candidate =
				g_parkTargetCandidates[candidateIndex];
			candidate.texture = texture;
			candidate.texture->AddRef();
			candidate.sourceIdentity = sourceIdentity;
			candidate.observationCount = 0;
			candidate.lastObservedFrame = currentFrame;
			candidate.lastObservedTick = now;
			scs_log(
				0,
				"[RTT park] Discovered copy path %c: source=%p, "
				"destination=%p, %ux%u %s.",
				static_cast<char>('A' + candidateIndex),
				reinterpret_cast<void*>(sourceIdentity),
				texture,
				description.Width,
				description.Height,
				dx11::internal_render_probe::format_name(
					static_cast<uint32_t>(
						description.Format)));
		}

		auto& candidate =
			g_parkTargetCandidates[candidateIndex];
		++candidate.observationCount;
		candidate.lastObservedFrame = currentFrame;
		candidate.lastObservedTick = now;

		const uint32_t requestedVariant =
			g_parkTargetVariant.load(std::memory_order_relaxed);
		if (!kIndependentOutputOnly &&
			((requestedVariant == 0 &&
				g_parkSelectedCandidate == kNoParkTargetCandidate) ||
			requestedVariant == candidateIndex + 1))
		{
			select_park_target_locked(candidateIndex, currentFrame);
		}
		if (kIndependentOutputOnly)
			return false;
		return g_parkSelectedCandidate == candidateIndex;
	}

	void capture_selected_park_copy(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* texture,
		const D3D11_TEXTURE2D_DESC& description,
		uint64_t currentFrame,
		uintptr_t sourceIdentity)
	{
		if (!observe_park_colour_target(
			texture, description, currentFrame, sourceIdentity))
			return;

		std::lock_guard<std::mutex> lock(g_parkTextureMutex);
		ID3D11Device* device{};
		texture->GetDevice(&device);
		if (!device || !ensure_park_staging_locked(device, description))
		{
			release_com_object(device);
			return;
		}
		release_com_object(device);

		for (uint32_t offset = 0;
			offset < g_parkStaging.size(); ++offset)
		{
			const uint32_t index =
				(g_nextParkStaging + offset) %
				static_cast<uint32_t>(g_parkStaging.size());
			auto& slot = g_parkStaging[index];
			if (slot.pending || !slot.texture)
				continue;

			// Snapshot this exact source path now. The engine may write a
			// left/right mirror or player view into the destination later.
			g_originalGameCopyResource(
				context, slot.texture, texture);
			slot.pending = true;
			slot.submissionOrder = ++g_parkNextSubmissionOrder;
			g_nextParkStaging =
				(index + 1) %
				static_cast<uint32_t>(g_parkStaging.size());
			g_parkObservedFrame = currentFrame;
			g_parkSubmittedFrame = currentFrame;
			break;
		}
	}

	bool compile_park_shader(
		const char* entryPoint,
		const char* target,
		ID3DBlob** byteCode)
	{
		ID3DBlob* errors{};
		const HRESULT result = D3DCompile(
			kParkCompositorShader,
			std::strlen(kParkCompositorShader),
			"PrismParkCompositor",
			nullptr,
			nullptr,
			entryPoint,
			target,
			D3DCOMPILE_OPTIMIZATION_LEVEL3,
			0,
			byteCode,
			&errors);
		if (FAILED(result))
		{
			const char* message = errors
				? static_cast<const char*>(
					errors->GetBufferPointer())
				: "no compiler details";
			scs_log(
				2,
				"[RTT park] Shader compilation failed for "
				"%s: %s",
				entryPoint,
				message);
		}
		release_com_object(errors);
		return SUCCEEDED(result);
	}

	bool ensure_park_compositor_locked(ID3D11Device* device)
	{
		if (!device)
			return false;
		if (g_compositorDevice == device &&
			g_parkVertexShader &&
			g_parkPixelShader &&
			g_parkSampler &&
			g_parkConstants)
		{
			return true;
		}

		release_park_compositor_locked();
		g_compositorDevice = device;
		g_compositorDevice->AddRef();

		ID3DBlob* vertexCode{};
		ID3DBlob* pixelCode{};
		if (!compile_park_shader(
				"vs_main", "vs_5_0", &vertexCode) ||
			!compile_park_shader(
				"ps_main", "ps_5_0", &pixelCode))
		{
			release_com_object(vertexCode);
			release_com_object(pixelCode);
			release_park_compositor_locked();
			return false;
		}

		HRESULT result = device->CreateVertexShader(
			vertexCode->GetBufferPointer(),
			vertexCode->GetBufferSize(),
			nullptr,
			&g_parkVertexShader);
		if (SUCCEEDED(result))
		{
			result = device->CreatePixelShader(
				pixelCode->GetBufferPointer(),
				pixelCode->GetBufferSize(),
				nullptr,
				&g_parkPixelShader);
		}
		release_com_object(vertexCode);
		release_com_object(pixelCode);
		if (FAILED(result))
		{
			release_park_compositor_locked();
			return false;
		}

		D3D11_SAMPLER_DESC samplerDescription{};
		samplerDescription.Filter =
			D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDescription.AddressU =
			D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDescription.AddressV =
			D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDescription.AddressW =
			D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
		result = device->CreateSamplerState(
			&samplerDescription, &g_parkSampler);

		D3D11_BUFFER_DESC constantDescription{};
		constantDescription.ByteWidth =
			sizeof(park_compositor_constants_t);
		constantDescription.Usage = D3D11_USAGE_DEFAULT;
		constantDescription.BindFlags =
			D3D11_BIND_CONSTANT_BUFFER;
		if (SUCCEEDED(result))
		{
			result = device->CreateBuffer(
				&constantDescription,
				nullptr,
				&g_parkConstants);
		}
		if (FAILED(result))
		{
			release_park_compositor_locked();
			return false;
		}

		g_parkCompositorReady.store(
			true, std::memory_order_relaxed);
		scs_log(
			0,
			"[RTT park] GPU compositor initialized.");
		return true;
	}

	bool ensure_park_sample_texture_locked(
		ID3D11Device* device,
		const D3D11_TEXTURE2D_DESC& sourceDescription)
	{
		if (g_parkSampleTexture)
		{
			D3D11_TEXTURE2D_DESC existing{};
			g_parkSampleTexture->GetDesc(&existing);
			if (existing.Width == sourceDescription.Width &&
				existing.Height == sourceDescription.Height &&
				existing.Format == sourceDescription.Format &&
				existing.SampleDesc.Count ==
					sourceDescription.SampleDesc.Count)
			{
				return g_parkSampleView != nullptr;
			}
			release_park_sample_resources_locked();
		}

		D3D11_TEXTURE2D_DESC sampleDescription =
			sourceDescription;
		sampleDescription.MipLevels = 1;
		sampleDescription.ArraySize = 1;
		sampleDescription.Usage = D3D11_USAGE_DEFAULT;
		sampleDescription.BindFlags =
			D3D11_BIND_SHADER_RESOURCE;
		sampleDescription.CPUAccessFlags = 0;
		sampleDescription.MiscFlags = 0;
		HRESULT result = device->CreateTexture2D(
			&sampleDescription,
			nullptr,
			&g_parkSampleTexture);
		if (SUCCEEDED(result))
		{
			result = device->CreateShaderResourceView(
				g_parkSampleTexture,
				nullptr,
				&g_parkSampleView);
		}
		if (FAILED(result))
		{
			release_park_sample_resources_locked();
			scs_log(
				2,
				"[RTT park] Could not create the park-camera "
				"sampling texture (hr=0x%08X).",
				result);
			return false;
		}
		return true;
	}

	executable_fingerprint_t fingerprint_executable()
	{
		executable_fingerprint_t result{};
		auto* module = reinterpret_cast<uint8_t*>(
			GetModuleHandleW(nullptr));
		if (!module)
			return result;

		auto* dosHeader =
			reinterpret_cast<IMAGE_DOS_HEADER*>(module);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
			return result;

		auto* ntHeader = reinterpret_cast<IMAGE_NT_HEADERS64*>(
			module + dosHeader->e_lfanew);
		if (ntHeader->Signature != IMAGE_NT_SIGNATURE ||
			ntHeader->OptionalHeader.Magic !=
				IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			return result;
		}

		result.timeDateStamp =
			ntHeader->FileHeader.TimeDateStamp;
		result.imageSize =
			ntHeader->OptionalHeader.SizeOfImage;

		auto* section = IMAGE_FIRST_SECTION(ntHeader);
		for (uint16_t index = 0;
			index < ntHeader->FileHeader.NumberOfSections;
			++index, ++section)
		{
			if ((section->Characteristics &
				IMAGE_SCN_MEM_EXECUTE) == 0)
			{
				continue;
			}

			const uint32_t sectionRva =
				static_cast<uint32_t>(
					section->VirtualAddress);
			if (sectionRva >= result.imageSize)
				continue;

			const size_t virtualSize =
				static_cast<size_t>(
					section->Misc.VirtualSize);
			const size_t remainingImageSize =
				static_cast<size_t>(
					result.imageSize - sectionRva);
			const size_t sectionSize =
				virtualSize < remainingImageSize
					? virtualSize
					: remainingImageSize;
			if (sectionSize < kMirrorScheduleSignature.size())
				continue;

			uint8_t* sectionStart =
				module + section->VirtualAddress;
			for (size_t offset = 0;
				offset <=
					sectionSize -
						kMirrorScheduleSignature.size();
				++offset)
			{
				if (std::memcmp(
					sectionStart + offset,
					kMirrorScheduleSignature.data(),
					kMirrorScheduleSignature.size()) != 0)
				{
					continue;
				}

				++result.signatureMatches;
				if (!result.signatureAddress)
				{
					result.signatureAddress =
						sectionStart + offset;
					result.detectedRva =
						section->VirtualAddress +
						static_cast<uint32_t>(offset);
				}
			}
		}

		return result;
	}

	template <size_t Size>
	bool matches_expected_bytes(
		uint32_t rva,
		const std::array<uint8_t, Size>& signature)
	{
		if (!g_executableBase ||
			rva > g_imageSize.load() ||
			Size > static_cast<size_t>(
				g_imageSize.load() - rva))
		{
			return false;
		}

		bool matches{};
		__try
		{
			matches =
				std::memcmp(
					g_executableBase + rva,
					signature.data(),
					Size) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			matches = false;
		}
		return matches;
	}

	uint32_t capture_mirror_slots(void* visualInterior)
	{
		if (!visualInterior)
			return 0;

		uint32_t mask{};
		bool valid = true;
		__try
		{
			auto* objectBytes =
				static_cast<uint8_t*>(visualInterior);
			auto** cameraSlots =
				*reinterpret_cast<void***>(
					objectBytes + 0x13B0);
			const uint64_t cameraCount =
				*reinterpret_cast<uint64_t*>(
					objectBytes + 0x13B8);
			if (!cameraSlots || cameraCount > 64)
			{
				valid = false;
			}
			else
			{
				const uint32_t readableCount =
					(static_cast<uint32_t>(
						(std::min)(
							cameraCount,
							static_cast<uint64_t>(
								kMirrorSlotCount))));
				for (uint32_t index = 0;
					index < readableCount; ++index)
				{
					if (cameraSlots[index])
						mask |= 1U << index;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			valid = false;
		}
		return valid ? mask : 0;
	}

	void capture_camera_slot_descriptors()
	{
		if (!g_executableBase)
			return;

		__try
		{
			auto* descriptorOwner =
				*reinterpret_cast<uint8_t**>(
					g_executableBase +
					kCameraDescriptorOwnerRva);
			if (!descriptorOwner)
				return;

			uint8_t* descriptorArray =
				descriptorOwner + 0xA8;
			auto** descriptorSlots =
				*reinterpret_cast<void***>(
					descriptorArray + 0x08);
			const uint64_t descriptorCount =
				*reinterpret_cast<uint64_t*>(
					descriptorArray + 0x10);
			if (!descriptorSlots || descriptorCount > 64)
				return;

			const uint32_t readableCount =
				static_cast<uint32_t>((std::min)(
					descriptorCount,
					static_cast<uint64_t>(
						kMirrorSlotCount)));
			for (uint32_t slot = 0;
				slot < readableCount; ++slot)
			{
				auto* descriptor = static_cast<uint8_t*>(
					descriptorSlots[slot]);
				if (!descriptor)
					continue;

				const uint32_t width =
					*reinterpret_cast<uint32_t*>(
						descriptor + 0x08);
				const uint32_t height =
					*reinterpret_cast<uint32_t*>(
						descriptor + 0x0C);
				if (width >= 16 && width <= 8192 &&
					height >= 16 && height <= 8192)
				{
					g_slotWidth[slot].store(
						width,
						std::memory_order_relaxed);
					g_slotHeight[slot].store(
						height,
						std::memory_order_relaxed);
					if (slot == kParkSlot)
					{
						g_parkResourcePresent.store(
							true,
							std::memory_order_relaxed);
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// This is a diagnostic only. A changing engine object must never
			// be allowed to interrupt the game's render scheduler.
		}
	}

	bool try_install_park_camera(void* visualInterior)
	{
		if (!kSlot7CameraPathEnabled)
		{
			g_parkCameraInstalled.store(
				false, std::memory_order_relaxed);
			g_parkResourcePresent.store(
				false, std::memory_order_relaxed);
			g_parkCamera.store(nullptr, std::memory_order_relaxed);
			return false;
		}
		if (!visualInterior ||
			!g_parkActivationRequested.load(
				std::memory_order_relaxed))
		{
			return false;
		}

		g_parkInstallAttempts.fetch_add(
			1, std::memory_order_relaxed);

		bool installed{};
		bool alreadyPresent{};
		bool invalidLayout{};
		__try
		{
			auto* objectBytes =
				static_cast<uint8_t*>(visualInterior);
			auto** cameraSlots =
				*reinterpret_cast<void***>(
					objectBytes + 0x13B0);
			const uint64_t cameraCount =
				*reinterpret_cast<uint64_t*>(
					objectBytes + 0x13B8);
			if (!cameraSlots ||
				cameraCount <= kParkSlot ||
				cameraCount > 64)
			{
				invalidLayout = true;
			}
			else if (cameraSlots[kParkSlot])
			{
				alreadyPresent = true;
				g_parkCamera.store(
					cameraSlots[kParkSlot],
					std::memory_order_relaxed);
				std::memcpy(
					g_parkSourcePosition.data(),
					static_cast<uint8_t*>(
						cameraSlots[kParkSlot]) + 0x4A0,
					sizeof(g_parkSourcePosition));
				std::memcpy(
					g_parkSourceOrientation.data(),
					static_cast<uint8_t*>(
						cameraSlots[kParkSlot]) + 0x4B0,
					sizeof(g_parkSourceOrientation));
				g_parkSourcePositionValid = true;
				installed = true;
			}
			else
			{
				void* sourceCamera =
					cameraSlots[kCloneSourceSlot];
				if (!sourceCamera)
				{
					invalidLayout = true;
				}
				else
				{
					auto** vtable =
						*reinterpret_cast<void***>(
							sourceCamera);
					void* expectedVtable =
						g_executableBase +
						kMirrorCameraVtableRva;
					void* expectedClone =
						g_executableBase +
						kMirrorCameraCloneRva;
					if (vtable != expectedVtable ||
						vtable[2] != expectedClone)
					{
						invalidLayout = true;
					}
					else
					{
						auto clone =
							reinterpret_cast<
								clone_camera_t>(
								vtable[2]);
						void* parkCamera =
							clone(sourceCamera);
						if (parkCamera &&
							*reinterpret_cast<void***>(
								parkCamera) ==
								expectedVtable)
						{
							// The clone is a newly-owned engine object.
							// Slot 7 participates in the visual-interior
							// destructor just like slots 0-6, so ownership
							// is deliberately transferred to the engine.
							cameraSlots[kParkSlot] =
								parkCamera;
							g_parkCamera.store(
								parkCamera,
								std::memory_order_relaxed);
							std::memcpy(
								g_parkSourcePosition.data(),
								static_cast<uint8_t*>(
									parkCamera) + 0x4A0,
								sizeof(g_parkSourcePosition));
							std::memcpy(
								g_parkSourceOrientation.data(),
								static_cast<uint8_t*>(
									parkCamera) + 0x4B0,
								sizeof(g_parkSourceOrientation));
							g_parkSourcePositionValid = true;
							installed = true;
						}
						else
						{
							invalidLayout = true;
						}
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			invalidLayout = true;
			installed = false;
		}

		g_parkVisualInterior.store(
			installed ? visualInterior : nullptr,
			std::memory_order_relaxed);
		g_parkCameraInstalled.store(
			installed, std::memory_order_relaxed);

		if (installed)
		{
			scs_log(
				0,
				alreadyPresent
					? "[RTT park] Slot 7 already exists; "
						"using the engine-owned camera."
					: "[RTT park] Cloned the initialized "
						"front mirror camera into dormant slot 7.");
		}
		else if (invalidLayout)
		{
			scs_log(
				2,
				"[RTT park] Park-camera activation was "
				"refused because the checked engine layout "
				"did not match.");
		}
		return installed;
	}

	void apply_park_camera_mount()
	{
		auto* camera = static_cast<uint8_t*>(
			g_parkCamera.load(std::memory_order_relaxed));
		if (!camera)
			return;
		if (!g_parkCameraKitInstalled.load(
				std::memory_order_relaxed))
		{
			if (g_parkSourcePositionValid)
			{
				__try
				{
					std::memcpy(camera + 0x4A0,
						g_parkSourcePosition.data(),
						sizeof(g_parkSourcePosition));
					std::memcpy(camera + 0x4B0,
						g_parkSourceOrientation.data(),
						sizeof(g_parkSourceOrientation));
				}
				__except (EXCEPTION_EXECUTE_HANDLER)
				{
				}
			}
			return;
		}

		const float lateral = g_parkMountLateral.load();
		const float height = g_parkMountHeight.load();
		const float longitudinal =
			g_parkMountLongitudinal.load();
		const float yawOffset =
			g_parkMountYaw.load() * 0.01745329251994329577f;
		const float pitch =
			g_parkMountPitch.load() * 0.01745329251994329577f;
		float position[4]{
			lateral, height, longitudinal, 1.0f
		};
		float relativeHeading{};
		bool trailerPose{};

		if (g_parkTrailerAwareMount.load() &&
			g_camera_bridge_trailer_valid.load() &&
			g_camera_bridge_truck_valid.load())
		{
			const double dx =
				g_last_trailer_world_x.load() -
				g_bridge_truck_world_x.load();
			const double dy =
				g_last_trailer_world_y.load() -
				g_bridge_truck_world_y.load();
			const double dz =
				g_last_trailer_world_z.load() -
				g_bridge_truck_world_z.load();
			const double truckHeading =
				g_bridge_truck_heading.load() *
				6.28318530717958647692;
			const double cosine = std::cos(truckHeading);
			const double sine = std::sin(truckHeading);
			const double localX = cosine * dx + sine * dz;
			const double localZ = -sine * dx + cosine * dz;
			const double distanceSquared =
				dx * dx + dy * dy + dz * dz;
			if (std::isfinite(localX) &&
				std::isfinite(localZ) &&
				distanceSquared < 40000.0)
			{
				position[0] =
					static_cast<float>(localX) + lateral;
				position[1] =
					static_cast<float>(dy) + height;
				position[2] =
					static_cast<float>(localZ) +
					longitudinal;
				relativeHeading = static_cast<float>(
					(g_last_trailer_heading.load() -
						g_bridge_truck_heading.load()) *
					6.28318530717958647692);
				trailerPose = true;
			}
		}
		if (!trailerPose && g_parkSourcePositionValid)
		{
			position[0] =
				g_parkSourcePosition[0] + lateral;
			position[1] =
				g_parkSourcePosition[1] + height;
			position[2] =
				g_parkSourcePosition[2] + longitudinal;
			position[3] = g_parkSourcePosition[3];
		}

		const float halfYaw =
			(relativeHeading + yawOffset) * 0.5f;
		const float halfPitch = pitch * 0.5f;
		const float sy = std::sin(halfYaw);
		const float cy = std::cos(halfYaw);
		const float sp = std::sin(halfPitch);
		const float cp = std::cos(halfPitch);
		const float orientation[4]{
			cy * sp,
			sy * cp,
			-sy * sp,
			cy * cp
		};

		for (float value : position)
			if (!std::isfinite(value))
				return;
		for (float value : orientation)
			if (!std::isfinite(value))
				return;

		__try
		{
			std::memcpy(camera + 0x4A0,
				position, sizeof(position));
			std::memcpy(camera + 0x4B0,
				orientation, sizeof(orientation));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// A transient engine object must never take down the game.
		}
	}

	void submit_independent_render_validation(
		void* renderContext, void* renderCommand);

	void __fastcall hooked_resource_init(void* visualInterior)
	{
		if (visualInterior !=
			g_parkVisualInterior.load(
				std::memory_order_relaxed))
		{
			for (uint32_t slot = 0;
				slot < kMirrorSlotCount; ++slot)
			{
				g_slotWidth[slot].store(
					0, std::memory_order_relaxed);
				g_slotHeight[slot].store(
					0, std::memory_order_relaxed);
			}
			g_parkCameraInstalled.store(
				false, std::memory_order_relaxed);
			g_parkResourcePresent.store(
				false, std::memory_order_relaxed);
			g_parkCamera.store(
				nullptr, std::memory_order_relaxed);
			g_parkSourcePositionValid = false;
		}

		const bool parkInstalled =
			try_install_park_camera(visualInterior);
		if (parkInstalled)
		{
			// The descriptor exists before the engine allocates the slot's
			// GPU resources. Capture it now so the CreateTexture2D hook can
			// distinguish slot 7 from the scaled side mirrors.
			capture_camera_slot_descriptors();
			release_legacy_park_sources();
		}
		g_capturingParkResourceInit = parkInstalled;
		g_originalResourceInit(visualInterior);
		g_capturingParkResourceInit = false;

		if (parkInstalled)
		{
			capture_camera_slot_descriptors();
			const bool resourcePresent =
				g_slotWidth[kParkSlot].load(
					std::memory_order_relaxed) != 0 &&
				g_slotHeight[kParkSlot].load(
					std::memory_order_relaxed) != 0;
			g_parkResourcePresent.store(
				resourcePresent,
				std::memory_order_relaxed);
			scs_log(
				resourcePresent ? 0 : 2,
				"[RTT park] Internal park resource: %s "
				"(descriptor %ux%u).",
				resourcePresent
					? "created"
					: "not found after initialization",
				g_slotWidth[kParkSlot].load(),
				g_slotHeight[kParkSlot].load());
		}
	}

	uint32_t __fastcall hooked_active_mask(
		void* visualInterior,
		uint32_t state)
	{
		uint32_t result =
			g_originalActiveMask(visualInterior, state);
		if (!kSlot7CameraPathEnabled)
		{
			g_parkMaskForced.store(
				false, std::memory_order_relaxed);
			return result;
		}
		const bool parkEligible =
			(state == 2 || state == 12) &&
			g_parkRenderRequested.load(
				std::memory_order_relaxed) &&
			g_parkCameraInstalled.load(
				std::memory_order_relaxed) &&
			g_parkResourcePresent.load(
				std::memory_order_relaxed) &&
			visualInterior ==
				g_parkVisualInterior.load(
					std::memory_order_relaxed);
		bool forcePark{};
		if (parkEligible)
		{
			const uint64_t now = GetTickCount64();
			const uint32_t targetFramerate =
				(std::clamp)(
					g_parkTargetFramerate.load(
						std::memory_order_relaxed),
					1U,
					60U);
			const uint64_t interval =
				(std::max)(1ULL, 1000ULL / targetFramerate);
			const uint64_t previous =
				g_lastParkScheduleTick.load(
					std::memory_order_relaxed);
			forcePark =
				previous == 0 ||
				now < previous ||
				now - previous >= interval;
			if (forcePark)
			{
				g_lastParkScheduleTick.store(
					now, std::memory_order_relaxed);
			}
		}
		if (forcePark)
		{
			result |= 1U << kParkSlot;
			g_lastParkForcedFrame.store(
				g_frameIndex.load(
					std::memory_order_relaxed),
				std::memory_order_relaxed);
			g_parkScheduleCount.fetch_add(
				1, std::memory_order_relaxed);
		}
		g_parkMaskForced.store(
			forcePark, std::memory_order_relaxed);
		return result;
	}

	uintptr_t __fastcall hooked_mirror_schedule(
		void* visualInterior,
		void* requestContext,
		uint64_t mode,
		uint64_t fourthArgument)
	{
		if (g_tracing.load(std::memory_order_acquire) &&
			g_independentExclusiveWindow.load(
				std::memory_order_acquire))
		{
			const uint64_t now = GetTickCount64();
			const uint64_t due = g_independentSubmitDueTick.load(
				std::memory_order_acquire);
			if (due != 0 && now >= due &&
				!g_independentSubmitAttempted.load(
					std::memory_order_acquire))
			{
				submit_independent_render_validation(
					g_independentRenderContext.load(
						std::memory_order_acquire),
					g_independentCommandTemplate.data());
			}
			if (due != 0 && now > due + 2500 &&
				!g_independentOutputCaptured.load(
					std::memory_order_acquire))
			{
				g_independentExclusiveWindow.store(
					false, std::memory_order_release);
				g_independentOutputCapturePending.store(
					false, std::memory_order_release);
				scs_log(2,
					"[RTT custom] Independent output capture timed out; "
					"tagged render targets=%u. Legacy slot-7 output remains "
					"blocked.",
					g_independentTaggedTargetCount.load(
						std::memory_order_relaxed));
			}
		}

		const bool tracing =
			g_tracing.load(std::memory_order_relaxed);
		const bool observingPark =
			g_parkRenderRequested.load(
				std::memory_order_relaxed);
		const bool firstObservation =
			!g_mirrorScheduleSeen.load(
				std::memory_order_relaxed);
		if (firstObservation)
		{
			g_mirrorScheduleSeen.store(
				true, std::memory_order_relaxed);
		}
		if (tracing || observingPark || firstObservation)
		{
			g_mirrorScheduleCount.fetch_add(
				1, std::memory_order_relaxed);
			g_lastMirrorScheduleFrame.store(
				g_frameIndex.load(std::memory_order_relaxed),
				std::memory_order_relaxed);
			g_mirrorSlotMask.store(
				capture_mirror_slots(visualInterior),
				std::memory_order_relaxed);
			capture_camera_slot_descriptors();
		}

		if (observingPark)
			apply_park_camera_mount();

		if (!tracing && !observingPark)
		{
			return g_originalMirrorSchedule(
				visualInterior,
				requestContext,
				mode,
				fourthArgument);
		}

		++g_mirrorScheduleDepth;
		const uintptr_t result =
			g_originalMirrorSchedule(
				visualInterior,
				requestContext,
				mode,
				fourthArgument);
		--g_mirrorScheduleDepth;
		return result;
	}

	int32_t resolve_render_command_slot(void* renderCommand)
	{
		if (!renderCommand || !g_executableBase)
			return -1;

		__try
		{
			auto* slotTokens = reinterpret_cast<uint64_t*>(
				g_executableBase + kMirrorSlotTokenTableRva);
			auto* bytes = static_cast<uint8_t*>(renderCommand);
			// The scheduler writes the exact slot token to command +0x38.
			// Hooking the common render dispatch receives that command directly,
			// including paths which bypass the queued-job wrapper.
			const uint64_t jobToken =
				*reinterpret_cast<uint64_t*>(bytes + 0x38);
			for (uint32_t slot = 0; slot < kMirrorSlotCount; ++slot)
			{
				if (jobToken != 0 && jobToken == slotTokens[slot])
					return static_cast<int32_t>(slot);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
		return -1;
	}

	bool safe_copy_diagnostic_bytes(
		void* source, uint8_t* destination, size_t size)
	{
		if (!source || !destination || size == 0)
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

	uint64_t diagnostic_hash(
		const uint8_t* bytes, size_t size, uint64_t seed)
	{
		uint64_t value = seed;
		for (size_t index = 0; index < size; ++index)
		{
			value ^= bytes[index];
			value *= 1099511628211ULL;
		}
		return value;
	}

	void log_diagnostic_block(
		uint32_t snapshot, const char* name,
		void* address, size_t size)
	{
		std::array<uint8_t, 256> bytes{};
		size = (std::min)(size, bytes.size());
		if (!safe_copy_diagnostic_bytes(
			address, bytes.data(), size))
		{
			scs_log(0,
				"[RTT custom] snapshot=%u %s=%p unreadable",
				snapshot, name, address);
			return;
		}
		for (size_t offset = 0; offset < size; offset += 16)
		{
			scs_log(0,
				"[RTT custom] snapshot=%u %s=%p +%03X: "
				"%02X %02X %02X %02X %02X %02X %02X %02X "
				"%02X %02X %02X %02X %02X %02X %02X %02X",
				snapshot, name, address,
				static_cast<unsigned>(offset),
				bytes[offset + 0], bytes[offset + 1],
				bytes[offset + 2], bytes[offset + 3],
				bytes[offset + 4], bytes[offset + 5],
				bytes[offset + 6], bytes[offset + 7],
				bytes[offset + 8], bytes[offset + 9],
				bytes[offset + 10], bytes[offset + 11],
				bytes[offset + 12], bytes[offset + 13],
				bytes[offset + 14], bytes[offset + 15]);
		}
	}

	void capture_independent_camera_diagnostic(
		void* renderer, void* renderContext, void* renderCommand)
	{
		if (!g_tracing.load(std::memory_order_relaxed))
			return;
		const uint64_t now = GetTickCount64();
		uint64_t previousTick =
			g_lastIndependentCommandSnapshotTick.load(
				std::memory_order_relaxed);
		if (previousTick != 0 && now - previousTick < 1000)
			return;
		if (!g_lastIndependentCommandSnapshotTick.compare_exchange_strong(
			previousTick, now,
			std::memory_order_acq_rel))
			return;

		std::array<uint8_t, 256> commandBytes{};
		std::array<uint8_t, 128> cameraBytes{};
		if (!safe_copy_diagnostic_bytes(
			renderCommand, commandBytes.data(), commandBytes.size()))
			return;
		safe_copy_diagnostic_bytes(
			g_workerCameraInput, cameraBytes.data(), cameraBytes.size());
		uint64_t hash = diagnostic_hash(
			commandBytes.data(), commandBytes.size(),
			1469598103934665603ULL);
		hash = diagnostic_hash(
			cameraBytes.data(), cameraBytes.size(), hash);

		uint32_t snapshot{};
		{
			std::lock_guard<std::mutex> lock(
				g_independentCameraDiagnosticMutex);
			const uint32_t count =
				g_independentCameraSnapshots.load(
					std::memory_order_relaxed);
			if (count >= 16)
				return;
			snapshot = count + 1;
			g_independentCameraSnapshotHashes[count] = hash;
			g_independentCameraSnapshots.store(
				snapshot, std::memory_order_relaxed);
		}

		scs_log(0,
			"[RTT custom] snapshot=%u thread=%u hash=%016llX "
			"renderer=%p context=%p command=%p worker={%p,%p,%p,%p}",
			snapshot, GetCurrentThreadId(),
			static_cast<unsigned long long>(hash),
			renderer, renderContext, renderCommand,
			g_workerRenderer, g_workerRenderContext,
			g_workerCameraInput, g_workerRenderRequest);
		void* stackFrames[16]{};
		const USHORT stackCount = RtlCaptureStackBackTrace(
			0, static_cast<ULONG>(std::size(stackFrames)),
			stackFrames, nullptr);
		for (USHORT index = 0; index < stackCount; ++index)
		{
			const uintptr_t address =
				reinterpret_cast<uintptr_t>(stackFrames[index]);
			const uintptr_t base =
				reinterpret_cast<uintptr_t>(g_executableBase);
			const uint32_t imageSize = g_imageSize.load(
				std::memory_order_relaxed);
			if (address >= base && address < base + imageSize)
			{
				scs_log(0,
					"[RTT custom stack] snapshot=%u frame=%u "
					"address=%p exe-rva=0x%08llX",
					snapshot, index, stackFrames[index],
					static_cast<unsigned long long>(address - base));
			}
			else
			{
				scs_log(0,
					"[RTT custom stack] snapshot=%u frame=%u "
					"address=%p external",
					snapshot, index, stackFrames[index]);
			}
		}
		log_diagnostic_block(snapshot, "command", renderCommand, 256);
		void* commandOwner{};
		if (safe_copy_diagnostic_bytes(
			static_cast<uint8_t*>(renderCommand) + 0xF8,
			reinterpret_cast<uint8_t*>(&commandOwner),
			sizeof(commandOwner)) && commandOwner)
		{
			log_diagnostic_block(
				snapshot, "command-owner", commandOwner, 256);
		}
		log_diagnostic_block(snapshot, "camera", g_workerCameraInput, 128);
		log_diagnostic_block(snapshot, "request", g_workerRenderRequest, 128);
		log_diagnostic_block(snapshot, "context", renderContext, 128);
	}

	void capture_independent_worker_diagnostic(
		void* renderer, void* renderContext,
		void* cameraInput, void* renderRequest)
	{
		if (!g_tracing.load(std::memory_order_relaxed))
			return;
		const uint64_t now = GetTickCount64();
		uint64_t previous =
			g_lastIndependentWorkerSnapshotTick.load(
				std::memory_order_relaxed);
		if (previous != 0 && now - previous < 2000)
			return;
		if (!g_lastIndependentWorkerSnapshotTick.compare_exchange_strong(
			previous, now, std::memory_order_acq_rel))
			return;
		const uint32_t snapshot =
			g_independentWorkerSnapshots.fetch_add(
				1, std::memory_order_relaxed) + 1;
		if (snapshot > 12)
			return;

		scs_log(0,
			"[RTT custom worker] snapshot=%u thread=%u "
			"renderer=%p context=%p camera=%p request=%p",
			snapshot, GetCurrentThreadId(), renderer,
			renderContext, cameraInput, renderRequest);
		log_diagnostic_block(snapshot, "worker-renderer", renderer, 128);
		log_diagnostic_block(snapshot, "worker-context", renderContext, 128);
		log_diagnostic_block(snapshot, "worker-camera", cameraInput, 256);
		log_diagnostic_block(snapshot, "worker-request", renderRequest, 256);
	}

	void __fastcall hooked_mirror_worker(
		void* renderer, void* renderContext,
		void* cameraInput, void* renderRequest)
	{
		void* previousRenderer = g_workerRenderer;
		void* previousContext = g_workerRenderContext;
		void* previousCamera = g_workerCameraInput;
		void* previousRequest = g_workerRenderRequest;
		g_workerRenderer = renderer;
		g_workerRenderContext = renderContext;
		g_workerCameraInput = cameraInput;
		g_workerRenderRequest = renderRequest;
		capture_independent_worker_diagnostic(
			renderer, renderContext, cameraInput, renderRequest);
		g_originalMirrorWorker(
			renderer, renderContext, cameraInput, renderRequest);
		g_workerRenderer = previousRenderer;
		g_workerRenderContext = previousContext;
		g_workerCameraInput = previousCamera;
		g_workerRenderRequest = previousRequest;
	}

	void prepare_independent_render_validation(
		void* renderContext, void* renderCommand)
	{
		if (!g_tracing.load(std::memory_order_acquire) ||
			!g_renderTaskSubmit || !renderContext || !renderCommand)
		{
			return;
		}

		bool expected = false;
		if (!g_independentTemplateReady.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
		{
			return;
		}

		if (!safe_copy_diagnostic_bytes(
			renderCommand,
			g_independentCommandTemplate.data(),
			g_independentCommandTemplate.size()))
		{
			g_independentTemplateReady.store(
				false, std::memory_order_release);
			return;
		}

		const uint64_t now = GetTickCount64();
		g_independentRenderContext.store(
			renderContext, std::memory_order_release);
		// Submit from the normal scheduler thread without pausing mirrors.
		// Output identification is based on render targets bound by this exact
		// independent task, so mirror scheduling cannot deadlock its final copy.
		g_independentSubmitDueTick.store(
			now + 1, std::memory_order_release);
		g_independentExclusiveWindow.store(
			true, std::memory_order_release);
		scs_log(0,
			"[RTT custom] Independent command template captured; "
			"submission armed without pausing normal mirrors.");
	}

	void submit_independent_render_validation(
		void* renderContext, void* renderCommand)
	{
		if (!g_tracing.load(std::memory_order_acquire) ||
			!g_renderTaskSubmit || !renderContext || !renderCommand)
		{
			return;
		}

		bool expected = false;
		if (!g_independentSubmitAttempted.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
		{
			return;
		}

		// The engine constructor at 0x722BA0 copies exactly the first 0xF0
		// bytes into a new 0x28590-byte render task, initializes that task's
		// private renderer, and queues it through the normal Prism3D job
		// system. Keep the source on our stack only for the synchronous copy.
		alignas(16) std::array<uint8_t, 0xF0> commandCopy{};
		if (!safe_copy_diagnostic_bytes(
			renderCommand, commandCopy.data(), commandCopy.size()))
		{
			scs_log(2,
				"[RTT custom] Independent render submission aborted: "
				"the live command could not be copied.");
			return;
		}

		void* task{};
		g_independentSubmitInProgress.store(
			true, std::memory_order_release);
		__try
		{
			g_renderTaskSubmit(
				nullptr, &task, renderContext, commandCopy.data());
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			task = nullptr;
		}

		g_independentRenderTask.store(task, std::memory_order_release);
		g_independentSubmitSucceeded.store(
			task != nullptr, std::memory_order_release);
		g_independentSubmitInProgress.store(
			false, std::memory_order_release);
		if (task)
		{
			scs_log(0,
				"[RTT custom] Independent Prism3D render task submitted: "
				"task=%p command=%p context=%p. This is a separate "
				"engine job, not a second call on the live slot-7 task.",
				task, static_cast<uint8_t*>(task) + 0x38,
				renderContext);
		}
		else
		{
			scs_log(2,
				"[RTT custom] Independent Prism3D render task submission "
				"returned no task.");
		}
	}

	void __fastcall hooked_mirror_render_dispatch(
		void* renderer,
		void* renderContext,
		void* renderCommand)
	{
		const int32_t previousSlot = g_renderingMirrorSlot;
		g_renderingMirrorSlot =
			resolve_render_command_slot(renderCommand);
		void* commandOwner = renderCommand
			? static_cast<uint8_t*>(renderCommand) - 0x38
			: nullptr;
		const bool independentTask =
			commandOwner != nullptr &&
			(commandOwner == g_independentRenderTask.load(
				std::memory_order_acquire) ||
			g_independentSubmitInProgress.load(
				std::memory_order_acquire));
		const bool renderingParkSlot =
			g_renderingMirrorSlot == static_cast<int32_t>(kParkSlot);
		if (!kSlot7CameraPathEnabled)
		{
			g_cameraLabObservedJobs.fetch_add(
				1, std::memory_order_relaxed);
			if (renderingParkSlot)
				g_cameraLabRejectedSlot7Jobs.fetch_add(
					1, std::memory_order_relaxed);

			if (!renderingParkSlot)
			{
				camera_correlation::observe(
					g_workerCameraInput,
					g_workerRenderRequest,
					renderCommand);
			}

			g_originalMirrorRenderDispatch(
				renderer, renderContext, renderCommand);
			g_renderingMirrorSlot = previousSlot;
			return;
		}
		if (renderingParkSlot)
		{
			g_slot7DispatchCount.fetch_add(1, std::memory_order_relaxed);
			if (independentTask)
			{
				const uint32_t count =
					g_independentDispatchCount.fetch_add(
						1, std::memory_order_relaxed) + 1;
				if (count == 1)
				{
					g_independentOutputArmedTick.store(
						GetTickCount64(), std::memory_order_release);
					g_independentOutputCapturePending.store(
						true, std::memory_order_release);
					scs_log(0,
						"[RTT custom] Independent Prism3D render task "
						"entered the engine renderer: task=%p renderer=%p "
						"context=%p.",
						commandOwner, renderer, renderContext);
				}
			}
			else
			{
				capture_independent_camera_diagnostic(
					renderer, renderContext, renderCommand);
			}
			// The engine can bind the target from its D3D worker rather than
			// this dispatch thread. Publish a narrowly scoped cross-thread
			// marker for the duration of the confirmed slot-7 render call.
			g_slot7RenderDepth.fetch_add(1, std::memory_order_release);
		}
		if (independentTask)
			g_independentRenderDepth.fetch_add(
				1, std::memory_order_release);
		g_originalMirrorRenderDispatch(
			renderer, renderContext, renderCommand);
		if (independentTask)
			g_independentRenderDepth.fetch_sub(
				1, std::memory_order_release);
		if (renderingParkSlot)
		{
			g_lastSlot7DispatchEndTick.store(
				GetTickCount64(), std::memory_order_release);
			g_slot7RenderDepth.fetch_sub(1, std::memory_order_release);
		}
		if (renderingParkSlot && !independentTask)
			prepare_independent_render_validation(
				renderContext, renderCommand);
		g_renderingMirrorSlot = previousSlot;
	}

	void remember_independent_render_target(
		ID3D11Texture2D* texture,
		const D3D11_TEXTURE2D_DESC& description)
	{
		if (!texture ||
			g_independentRenderDepth.load(
				std::memory_order_acquire) == 0)
		{
			return;
		}
		const uint32_t width = g_slotWidth[kParkSlot].load(
			std::memory_order_relaxed);
		const uint32_t height = g_slotHeight[kParkSlot].load(
			std::memory_order_relaxed);
		if (width == 0 || height == 0 ||
			description.Width != width ||
			description.Height != height ||
			description.SampleDesc.Count != 1 ||
			description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			return;
		}

		const uintptr_t identity = reinterpret_cast<uintptr_t>(texture);
		std::lock_guard<std::mutex> lock(
			g_independentRenderTargetMutex);
		for (uint32_t index = 0;
			index < g_independentRenderTargetCount; ++index)
		{
			if (g_independentRenderTargets[index] == identity)
				return;
		}
		if (g_independentRenderTargetCount <
			kMaximumIndependentRenderTargets)
		{
			g_independentRenderTargets[
				g_independentRenderTargetCount++] = identity;
			g_independentTaggedTargetCount.store(
				g_independentRenderTargetCount,
				std::memory_order_release);
		}
	}

	bool is_independent_render_source(uintptr_t identity)
	{
		if (identity == 0)
			return false;
		std::lock_guard<std::mutex> lock(
			g_independentRenderTargetMutex);
		for (uint32_t index = 0;
			index < g_independentRenderTargetCount; ++index)
		{
			if (g_independentRenderTargets[index] == identity)
				return true;
		}
		return false;
	}

	void record_render_targets(
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		bool recordCandidates,
		uint32_t apiPath,
		bool duringSlot7,
		bool afterSlot7,
		bool allowParkSelection)
	{
		if (!renderTargetViews || renderTargetViewCount == 0)
			return;

		const uint64_t currentFrame =
			g_frameIndex.load(std::memory_order_relaxed);
		if (!recordCandidates &&
			g_renderingMirrorSlot !=
				static_cast<int32_t>(kParkSlot))
		{
			return;
		}
		const uint64_t mirrorFrame =
			g_lastMirrorScheduleFrame.load(
				std::memory_order_relaxed);
		const bool nearMirrorSchedule =
			mirrorFrame != UINT64_MAX &&
			currentFrame >= mirrorFrame &&
			currentFrame - mirrorFrame <= 2;
		const bool duringMirrorSchedule =
			g_mirrorScheduleDepth != 0;

		for (UINT index = 0;
			index < renderTargetViewCount; ++index)
		{
			ID3D11RenderTargetView* view =
				renderTargetViews[index];
			if (!view)
				continue;

			ID3D11Resource* resource{};
			view->GetResource(&resource);
			if (!resource)
				continue;

			ID3D11Texture2D* texture{};
			const HRESULT queryResult =
				resource->QueryInterface(
					__uuidof(ID3D11Texture2D),
					reinterpret_cast<void**>(&texture));
			resource->Release();
			if (FAILED(queryResult) || !texture)
				continue;

			const uintptr_t identity =
				reinterpret_cast<uintptr_t>(texture);
			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			remember_independent_render_target(
				texture, description);
			if (allowParkSelection && g_renderingMirrorSlot ==
				static_cast<int32_t>(kParkSlot))
			{
				observe_park_colour_target(
					texture,
					description,
					currentFrame);
			}
			texture->Release();
			if (!recordCandidates)
				continue;

			if (description.Width < 64 ||
				description.Height < 64 ||
				description.Width > 8192 ||
				description.Height > 8192 ||
				(description.BindFlags &
					D3D11_BIND_RENDER_TARGET) == 0)
			{
				continue;
			}

			uint32_t matchingSlotMask{};
			for (uint32_t slot = 0;
				slot < kMirrorSlotCount; ++slot)
			{
				const uint32_t slotWidth =
					g_slotWidth[slot].load(
						std::memory_order_relaxed);
				const uint32_t slotHeight =
					g_slotHeight[slot].load(
						std::memory_order_relaxed);
				const bool nominalMatch =
					description.Width == slotWidth &&
					description.Height == slotHeight;
				const bool mirrorScaleTwoMatch =
					slotWidth <= UINT32_MAX / 2 &&
					slotHeight <= UINT32_MAX / 2 &&
					description.Width == slotWidth * 2 &&
					description.Height == slotHeight * 2;
				if (slotWidth != 0 && slotHeight != 0 &&
					(nominalMatch || mirrorScaleTwoMatch))
				{
					matchingSlotMask |= 1U << slot;
				}
			}

			std::lock_guard<std::mutex> lock(
				g_candidateMutex);
			auto existing = g_candidates.find(identity);
			if (existing == g_candidates.end())
			{
				if (g_candidates.size() >=
					kMaximumCandidates)
				{
					continue;
				}

				candidate_record_t record{};
				record.value.id = g_nextCandidateId++;
				record.value.width = description.Width;
				record.value.height = description.Height;
				record.value.format =
					static_cast<uint32_t>(
						description.Format);
				record.value.sampleCount =
					description.SampleDesc.Count;
				record.value.matchingCameraSlotMask =
					matchingSlotMask;
				record.value.firstFrame = currentFrame;
				record.value.lastFrame = currentFrame;
				record.value.firstThreadId = GetCurrentThreadId();
				existing = g_candidates.emplace(
					identity, record).first;
			}

			auto& candidate = existing->second.value;
			candidate.matchingCameraSlotMask |=
				matchingSlotMask;
			++candidate.bindCount;
			if ((apiPath & 1U) != 0)
				++candidate.omSetRenderTargetsBindCount;
			if ((apiPath & 2U) != 0)
				++candidate.omSetRenderTargetsUavBindCount;
			if (duringSlot7)
				++candidate.duringSlot7BindCount;
			if (afterSlot7)
				++candidate.afterSlot7BindCount;
			candidate.lastFrame = currentFrame;
			candidate.lastThreadId = GetCurrentThreadId();
			if (duringMirrorSchedule)
				++candidate.duringMirrorScheduleBindCount;
			if (nearMirrorSchedule)
				++candidate.nearMirrorBindCount;
		}
	}

	bool is_park_sized_hdr_target(ID3D11Texture2D* texture)
	{
		if (!texture)
			return false;
		D3D11_TEXTURE2D_DESC description{};
		texture->GetDesc(&description);
		const uint32_t width = g_slotWidth[kParkSlot].load(
			std::memory_order_relaxed);
		const uint32_t height = g_slotHeight[kParkSlot].load(
			std::memory_order_relaxed);
		const bool exactSize = description.Width == width &&
			description.Height == height;
		const bool scaledSize = width <= UINT32_MAX / 2 &&
			height <= UINT32_MAX / 2 &&
			description.Width == width * 2 &&
			description.Height == height * 2;
		return description.SampleDesc.Count == 1 &&
			(exactSize || scaledSize);
	}

	void remember_context_target(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews)
	{
		if (!context || !renderTargetViews ||
			!g_parkRenderRequested.load(std::memory_order_relaxed))
			return;

		ID3D11Texture2D* selected{};
		for (UINT index = 0; index < renderTargetViewCount; ++index)
		{
			ID3D11RenderTargetView* view = renderTargetViews[index];
			if (!view)
				continue;
			ID3D11Resource* resource{};
			view->GetResource(&resource);
			if (!resource)
				continue;
			ID3D11Texture2D* texture{};
			const HRESULT result = resource->QueryInterface(
				__uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(&texture));
			resource->Release();
			if (SUCCEEDED(result) && texture)
			{
				if (is_park_sized_hdr_target(texture))
				{
					if (selected)
						selected->Release();
					selected = texture;
				}
				else
				{
					texture->Release();
				}
			}
		}

		std::lock_guard<std::mutex> lock(g_commandListMutex);
		auto& record = g_deferredContexts[context];
		if (g_renderingMirrorSlot >= 0)
		{
			if (record.slot != g_renderingMirrorSlot)
				release_com_object(record.lastParkSizedTarget);
			record.slot = g_renderingMirrorSlot;
		}
		if (selected)
		{
			release_com_object(record.lastParkSizedTarget);
			record.lastParkSizedTarget = selected;
		}
	}

	void observe_and_forward_render_targets(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView,
		om_set_render_targets_t original,
		uint32_t apiPath = 1U)
	{
		remember_context_target(
			context, renderTargetViewCount, renderTargetViews);

		const bool duringSlot7 =
			g_slot7RenderDepth.load(std::memory_order_acquire) != 0;
		const uint64_t lastSlot7End =
			g_lastSlot7DispatchEndTick.load(std::memory_order_acquire);
		const uint64_t now = GetTickCount64();
		const bool afterSlot7 = !duringSlot7 && lastSlot7End != 0 &&
			now >= lastSlot7End && now - lastSlot7End <= 100;
		const bool exactSlot7 =
			g_renderingMirrorSlot == static_cast<int32_t>(kParkSlot);
		int32_t effectiveSlot = g_renderingMirrorSlot;
		if (effectiveSlot < 0 && duringSlot7)
		{
			effectiveSlot = static_cast<int32_t>(kParkSlot);
		}
		else if (effectiveSlot < 0 && afterSlot7 &&
			g_tracing.load(std::memory_order_relaxed))
		{
			// Correlate late worker binds for diagnostics only. These binds
			// are never eligible to become the live park-camera target.
			effectiveSlot = static_cast<int32_t>(kParkSlot);
		}
		if (effectiveSlot < 0)
		{
			std::lock_guard<std::mutex> lock(g_commandListMutex);
			const auto found = g_deferredContexts.find(context);
			if (found != g_deferredContexts.end())
				effectiveSlot = found->second.slot;
		}
		const int32_t previousSlot = g_renderingMirrorSlot;
		g_renderingMirrorSlot = effectiveSlot;
		const bool tracing =
			g_tracing.load(std::memory_order_relaxed);
		if (tracing ||
			g_parkRenderRequested.load(
				std::memory_order_relaxed))
		{
			record_render_targets(
				renderTargetViewCount,
				renderTargetViews,
				tracing,
				apiPath,
				duringSlot7 || exactSlot7,
				afterSlot7,
				duringSlot7 || exactSlot7);
		}
		g_renderingMirrorSlot = previousSlot;

		original(
			context,
			renderTargetViewCount,
			renderTargetViews,
			depthStencilView);
	}

	void observe_and_forward_render_targets_uav(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView,
		UINT unorderedAccessViewStartSlot,
		UINT unorderedAccessViewCount,
		ID3D11UnorderedAccessView* const* unorderedAccessViews,
		const UINT* initialCounts,
		om_set_render_targets_uav_t original)
	{
		// Reuse the complete correlation path without forwarding through the
		// standard OM method, then invoke the actual UAV-capable entry point.
		remember_context_target(
			context, renderTargetViewCount, renderTargetViews);
		const bool duringSlot7 =
			g_slot7RenderDepth.load(std::memory_order_acquire) != 0;
		const uint64_t lastSlot7End =
			g_lastSlot7DispatchEndTick.load(std::memory_order_acquire);
		const uint64_t now = GetTickCount64();
		const bool afterSlot7 = !duringSlot7 && lastSlot7End != 0 &&
			now >= lastSlot7End && now - lastSlot7End <= 100;
		const bool exactSlot7 =
			g_renderingMirrorSlot == static_cast<int32_t>(kParkSlot);
		const int32_t previousSlot = g_renderingMirrorSlot;
		if (g_renderingMirrorSlot < 0 &&
			(duringSlot7 || (afterSlot7 && g_tracing.load())))
		{
			g_renderingMirrorSlot = static_cast<int32_t>(kParkSlot);
		}
		const bool tracing = g_tracing.load(std::memory_order_relaxed);
		if (tracing || g_parkRenderRequested.load(std::memory_order_relaxed))
		{
			record_render_targets(
				renderTargetViewCount, renderTargetViews, tracing, 2U,
				duringSlot7 || exactSlot7, afterSlot7,
				duringSlot7 || exactSlot7);
		}
		g_renderingMirrorSlot = previousSlot;
		original(context, renderTargetViewCount, renderTargetViews,
			depthStencilView, unorderedAccessViewStartSlot,
			unorderedAccessViewCount, unorderedAccessViews, initialCounts);
	}

	void STDMETHODCALLTYPE hooked_om_set_render_targets(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView)
	{
		observe_and_forward_render_targets(
			context, renderTargetViewCount, renderTargetViews,
			depthStencilView, g_originalOmSetRenderTargets);
	}

	void STDMETHODCALLTYPE hooked_game_om_set_render_targets(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView)
	{
		observe_and_forward_render_targets(
			context, renderTargetViewCount, renderTargetViews,
			depthStencilView, g_originalGameOmSetRenderTargets);
	}

	void STDMETHODCALLTYPE hooked_om_set_render_targets_uav(
		ID3D11DeviceContext* context, UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView,
		UINT unorderedAccessViewStartSlot, UINT unorderedAccessViewCount,
		ID3D11UnorderedAccessView* const* unorderedAccessViews,
		const UINT* initialCounts)
	{
		observe_and_forward_render_targets_uav(
			context, renderTargetViewCount, renderTargetViews,
			depthStencilView, unorderedAccessViewStartSlot,
			unorderedAccessViewCount, unorderedAccessViews, initialCounts,
			g_originalOmSetRenderTargetsUav);
	}

	void STDMETHODCALLTYPE hooked_game_om_set_render_targets_uav(
		ID3D11DeviceContext* context, UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView,
		UINT unorderedAccessViewStartSlot, UINT unorderedAccessViewCount,
		ID3D11UnorderedAccessView* const* unorderedAccessViews,
		const UINT* initialCounts)
	{
		observe_and_forward_render_targets_uav(
			context, renderTargetViewCount, renderTargetViews,
			depthStencilView, unorderedAccessViewStartSlot,
			unorderedAccessViewCount, unorderedAccessViews, initialCounts,
			g_originalGameOmSetRenderTargetsUav);
	}

	bool get_texture_description(
		ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& description,
		uintptr_t& identity)
	{
		if (!resource)
			return false;
		ID3D11Texture2D* texture{};
		const HRESULT result = resource->QueryInterface(
			__uuidof(ID3D11Texture2D),
			reinterpret_cast<void**>(&texture));
		if (FAILED(result) || !texture)
			return false;
		texture->GetDesc(&description);
		identity = reinterpret_cast<uintptr_t>(texture);
		texture->Release();
		return true;
	}

	bool matches_camera_dimensions(const D3D11_TEXTURE2D_DESC& description)
	{
		for (uint32_t slot = 0; slot < kMirrorSlotCount; ++slot)
		{
			const uint32_t width = g_slotWidth[slot].load();
			const uint32_t height = g_slotHeight[slot].load();
			if (width == 0 || height == 0)
				continue;
			if ((description.Width == width &&
				description.Height == height) ||
				(width <= UINT32_MAX / 2 && height <= UINT32_MAX / 2 &&
					description.Width == width * 2 &&
					description.Height == height * 2))
			{
				return true;
			}
		}
		return false;
	}

	void record_resource_lineage(
		ID3D11Resource* source, ID3D11Resource* destination,
		uint32_t operation)
	{
		if (!g_tracing.load(std::memory_order_relaxed))
			return;
		D3D11_TEXTURE2D_DESC sourceDescription{};
		D3D11_TEXTURE2D_DESC destinationDescription{};
		uintptr_t sourceIdentity{};
		uintptr_t destinationIdentity{};
		if (!get_texture_description(source, sourceDescription, sourceIdentity) ||
			!get_texture_description(destination, destinationDescription,
				destinationIdentity) ||
			(!matches_camera_dimensions(sourceDescription) &&
				!matches_camera_dimensions(destinationDescription)))
		{
			return;
		}

		std::lock_guard<std::mutex> lock(g_candidateMutex);
		lineage_key_t key{ sourceIdentity, destinationIdentity };
		auto found = g_lineage.find(key);
		if (found == g_lineage.end())
		{
			if (g_lineage.size() >= 512)
				return;
			lineage_record_t record{};
			record.source = sourceDescription;
			record.destination = destinationDescription;
			record.firstFrame = g_frameIndex.load();
			found = g_lineage.emplace(key, record).first;
		}
		auto& record = found->second;
		if (operation == 1U) ++record.copyRegionCount;
		if (operation == 2U) ++record.copyResourceCount;
		if (operation == 3U) ++record.resolveCount;
		record.lastFrame = g_frameIndex.load();
		record.threadId = GetCurrentThreadId();
	}

	void STDMETHODCALLTYPE hooked_game_copy_subresource_region(
		ID3D11DeviceContext* context, ID3D11Resource* destination,
		UINT destinationSubresource, UINT destinationX, UINT destinationY,
		UINT destinationZ, ID3D11Resource* source, UINT sourceSubresource,
		const D3D11_BOX* sourceBox)
	{
		record_resource_lineage(source, destination, 1U);
		g_originalGameCopySubresourceRegion(
			context, destination, destinationSubresource, destinationX,
			destinationY, destinationZ, source, sourceSubresource, sourceBox);
	}

	bool capture_independent_output_copy(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* texture,
		const D3D11_TEXTURE2D_DESC& description,
		uintptr_t sourceIdentity)
	{
		if (!context || !texture ||
			!g_independentOutputCapturePending.load(
				std::memory_order_acquire))
		{
			return false;
		}

		const uint64_t now = GetTickCount64();
		const uint64_t armed = g_independentOutputArmedTick.load(
			std::memory_order_acquire);
		if (armed == 0 || now < armed || now - armed > 2000)
			return false;
		const uint64_t elapsed = now - armed;
		const bool taggedSource =
			is_independent_render_source(sourceIdentity);
		// Prism3D queues the actual D3D work after the engine dispatch returns
		// and can reuse the shared slot target instead of binding a task-owned
		// texture. In the captured build the independent result is the first
		// final park-sized copy after dispatch (16-82 ms in verified runs).
		// Accept that one narrowly bounded copy, then immediately freeze it in
		// our owned texture. Legacy A/B/C/D paths remain unable to display.
		// Timing correlation was the reason a slot-7 image was accepted. The
		// new path accepts only a uniquely tagged target from a verified custom
		// camera state. There is deliberately no shared-texture fallback.
		const bool boundedPostDispatchCopy = false;
		if (!taggedSource ||
			!g_customCameraStateVerified.load(
				std::memory_order_acquire))
			return false;

		const uint32_t parkWidth = g_slotWidth[kParkSlot].load(
			std::memory_order_relaxed);
		const uint32_t parkHeight = g_slotHeight[kParkSlot].load(
			std::memory_order_relaxed);
		if (description.Width != parkWidth ||
			description.Height != parkHeight ||
			description.SampleDesc.Count != 1 ||
			description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(g_parkTextureMutex);
		if (!g_independentOutputCapturePending.load(
			std::memory_order_relaxed))
		{
			return false;
		}

		ID3D11Device* device{};
		texture->GetDevice(&device);
		if (!device)
			return false;

		bool createTexture = g_independentColorTexture == nullptr;
		if (g_independentColorTexture)
		{
			D3D11_TEXTURE2D_DESC existing{};
			g_independentColorTexture->GetDesc(&existing);
			createTexture =
				existing.Width != description.Width ||
				existing.Height != description.Height ||
				existing.Format != description.Format ||
				existing.SampleDesc.Count != description.SampleDesc.Count;
		}
		if (createTexture)
		{
			release_com_object(g_independentColorTexture);
			D3D11_TEXTURE2D_DESC ownedDescription = description;
			ownedDescription.MipLevels = 1;
			ownedDescription.ArraySize = 1;
			ownedDescription.Usage = D3D11_USAGE_DEFAULT;
			ownedDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			ownedDescription.CPUAccessFlags = 0;
			ownedDescription.MiscFlags = 0;
			const HRESULT result = device->CreateTexture2D(
				&ownedDescription, nullptr,
				&g_independentColorTexture);
			if (FAILED(result) || !g_independentColorTexture)
			{
				device->Release();
				scs_log(2,
					"[RTT custom] Could not create the plugin-owned "
					"independent target (hr=0x%08X).", result);
				return false;
			}
		}
		device->Release();

		// Snapshot immediately after the engine's final copy. Subsequent left
		// or right mirror work cannot modify this plugin-owned texture.
		g_originalGameCopyResource(
			context, g_independentColorTexture, texture);
		release_com_object(g_parkColorTexture);
		g_parkColorTexture = g_independentColorTexture;
		g_parkColorTexture->AddRef();
		release_park_sample_resources_locked();
		release_park_readback_resources_locked();
		g_parkSelectedCandidate = kNoParkTargetCandidate;
		g_parkColorTextureScore = UINT32_MAX;
		g_parkObservedFrame = g_frameIndex.load(
			std::memory_order_relaxed);
		g_parkSubmittedFrame = UINT64_MAX;
		g_parkTargetWidth.store(
			description.Width, std::memory_order_relaxed);
		g_parkTargetHeight.store(
			description.Height, std::memory_order_relaxed);
		g_parkTargetFormat.store(
			static_cast<uint32_t>(description.Format),
			std::memory_order_relaxed);
		g_parkColorTargetReady.store(true, std::memory_order_release);
		g_independentOutputCopyCount.fetch_add(
			1, std::memory_order_relaxed);
		if (boundedPostDispatchCopy)
			g_independentCorrelatedCopyCount.fetch_add(
				1, std::memory_order_relaxed);
		g_independentOutputCaptured.store(
			true, std::memory_order_release);
		g_independentOutputCapturePending.store(
			false, std::memory_order_release);
		g_independentExclusiveWindow.store(
			false, std::memory_order_release);
		scs_log(0,
			"[RTT custom] Independent output isolated into the "
			"plugin-owned target: source=%p final=%p owned=%p, "
			"%ux%u %s, delay=%llu ms, correlation=%s. "
			"Legacy slot-7 output remains blocked.",
			reinterpret_cast<void*>(sourceIdentity), texture,
			g_independentColorTexture,
			description.Width, description.Height,
			dx11::internal_render_probe::format_name(
				static_cast<uint32_t>(description.Format)),
			static_cast<unsigned long long>(elapsed),
			taggedSource ? "tagged-target" : "bounded-post-dispatch");
		return true;
	}

	void STDMETHODCALLTYPE hooked_game_copy_resource(
		ID3D11DeviceContext* context, ID3D11Resource* destination,
		ID3D11Resource* source)
	{
		record_resource_lineage(source, destination, 2U);
		g_originalGameCopyResource(context, destination, source);

		if (!g_parkRenderRequested.load(std::memory_order_relaxed) ||
			!destination)
		{
			return;
		}
		ID3D11Texture2D* texture{};
		if (FAILED(destination->QueryInterface(
			__uuidof(ID3D11Texture2D),
			reinterpret_cast<void**>(&texture))) || !texture)
		{
			return;
		}
		D3D11_TEXTURE2D_DESC description{};
		texture->GetDesc(&description);
		const uint32_t parkWidth = g_slotWidth[kParkSlot].load();
		const uint32_t parkHeight = g_slotHeight[kParkSlot].load();
		const bool stableFinalDestination =
			texture != g_parkSampleTexture &&
			description.Usage == D3D11_USAGE_DEFAULT &&
			description.Width == parkWidth &&
			description.Height == parkHeight &&
			description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
		if (stableFinalDestination)
		{
			ID3D11Texture2D* sourceTexture{};
			if (source && SUCCEEDED(source->QueryInterface(
				__uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(&sourceTexture))) &&
				sourceTexture)
			{
				const uintptr_t sourceIdentity =
					reinterpret_cast<uintptr_t>(sourceTexture);
				const bool isolated = capture_independent_output_copy(
					context, texture, description, sourceIdentity);
				if (!isolated &&
					!g_independentOutputCaptured.load(
						std::memory_order_acquire))
				{
					capture_selected_park_copy(
						context,
						texture,
						description,
						g_frameIndex.load(std::memory_order_relaxed),
						sourceIdentity);
				}
				sourceTexture->Release();
			}
		}
		texture->Release();
	}

	void STDMETHODCALLTYPE hooked_game_resolve_subresource(
		ID3D11DeviceContext* context, ID3D11Resource* destination,
		UINT destinationSubresource, ID3D11Resource* source,
		UINT sourceSubresource, DXGI_FORMAT format)
	{
		record_resource_lineage(source, destination, 3U);
		g_originalGameResolveSubresource(
			context, destination, destinationSubresource,
			source, sourceSubresource, format);
	}

	HRESULT STDMETHODCALLTYPE hooked_finish_command_list(
		ID3D11DeviceContext* context,
		BOOL restoreDeferredContextState,
		ID3D11CommandList** commandList)
	{
		const HRESULT result = g_originalFinishCommandList(
			context, restoreDeferredContextState, commandList);

		deferred_context_record_t deferred{};
		{
			std::lock_guard<std::mutex> lock(g_commandListMutex);
			const auto found = g_deferredContexts.find(context);
			if (found != g_deferredContexts.end())
			{
				deferred = found->second;
				g_deferredContexts.erase(found);
			}
		}
		if (deferred.slot < 0)
			deferred.slot = g_renderingMirrorSlot;

		if (SUCCEEDED(result) && commandList && *commandList &&
			deferred.slot >= 0)
		{
			std::lock_guard<std::mutex> lock(g_commandListMutex);
			auto& record = g_commandLists[*commandList];
			release_com_object(record.lastParkSizedTarget);
			record.slot = deferred.slot;
			record.lastParkSizedTarget = deferred.lastParkSizedTarget;
			deferred.lastParkSizedTarget = nullptr;
			g_commandListTagCount.fetch_add(1, std::memory_order_relaxed);
		}
		release_com_object(deferred.lastParkSizedTarget);
		return result;
	}

	void STDMETHODCALLTYPE hooked_execute_command_list(
		ID3D11DeviceContext* context,
		ID3D11CommandList* commandList,
		BOOL restoreContextState)
	{
		command_list_record_t record{};
		{
			std::lock_guard<std::mutex> lock(g_commandListMutex);
			const auto found = g_commandLists.find(commandList);
			if (found != g_commandLists.end())
			{
				record = found->second;
				g_commandLists.erase(found);
			}
		}

		const int32_t previousSlot = g_renderingMirrorSlot;
		g_renderingMirrorSlot = record.slot;
		if (record.slot == static_cast<int32_t>(kParkSlot))
		{
			g_commandListExecuteCount.fetch_add(1, std::memory_order_relaxed);
			if (record.lastParkSizedTarget)
			{
				D3D11_TEXTURE2D_DESC description{};
				record.lastParkSizedTarget->GetDesc(&description);
				observe_park_colour_target(
					record.lastParkSizedTarget,
					description,
					g_frameIndex.load(std::memory_order_relaxed));
			}
		}
		g_originalExecuteCommandList(
			context, commandList, restoreContextState);
		g_renderingMirrorSlot = previousSlot;
		release_com_object(record.lastParkSizedTarget);
	}

	bool install_context_hook()
	{
		ID3D11Device* device{};
		ID3D11DeviceContext* context{};
		D3D_FEATURE_LEVEL featureLevel{};

		HRESULT result = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_HARDWARE,
			nullptr,
			0,
			nullptr,
			0,
			D3D11_SDK_VERSION,
			&device,
			&featureLevel,
			&context);
		if (FAILED(result))
		{
			result = D3D11CreateDevice(
				nullptr,
				D3D_DRIVER_TYPE_WARP,
				nullptr,
				0,
				nullptr,
				0,
				D3D11_SDK_VERSION,
				&device,
				&featureLevel,
				&context);
		}

		if (FAILED(result) || !device || !context)
		{
			if (context)
				context->Release();
			if (device)
				device->Release();
			return false;
		}

		void** vtable =
			*reinterpret_cast<void***>(context);
		g_omSetRenderTargetsAddress = vtable[33];
		g_omSetRenderTargetsUavAddress = vtable[34];
		g_executeCommandListAddress = vtable[58];
		g_finishCommandListAddress = vtable[114];

		const MH_STATUS createStatus = MH_CreateHook(
			g_omSetRenderTargetsAddress,
			&hooked_om_set_render_targets,
			reinterpret_cast<void**>(
				&g_originalOmSetRenderTargets));
		bool installed = false;
		if (createStatus == MH_OK)
		{
			installed =
				MH_EnableHook(
					g_omSetRenderTargetsAddress) == MH_OK;
			if (!installed)
			{
				MH_RemoveHook(
					g_omSetRenderTargetsAddress);
			}
		}

		context->Release();
		device->Release();
		if (!installed)
		{
			g_omSetRenderTargetsAddress = nullptr;
			g_executeCommandListAddress = nullptr;
			g_finishCommandListAddress = nullptr;
			return false;
		}

		const MH_STATUS uavCreateStatus = MH_CreateHook(
			g_omSetRenderTargetsUavAddress,
			&hooked_om_set_render_targets_uav,
			reinterpret_cast<void**>(
				&g_originalOmSetRenderTargetsUav));
		if (uavCreateStatus != MH_OK ||
			MH_EnableHook(g_omSetRenderTargetsUavAddress) != MH_OK)
		{
			if (uavCreateStatus == MH_OK)
				MH_RemoveHook(g_omSetRenderTargetsUavAddress);
			g_omSetRenderTargetsUavAddress = nullptr;
			g_originalOmSetRenderTargetsUav = nullptr;
		}
		else
		{
			g_uavTargetHookInstalled.store(true);
		}

		const MH_STATUS finishCreate = MH_CreateHook(
			g_finishCommandListAddress,
			&hooked_finish_command_list,
			reinterpret_cast<void**>(&g_originalFinishCommandList));
		const bool finishInstalled =
			finishCreate == MH_OK &&
			MH_EnableHook(g_finishCommandListAddress) == MH_OK;
		const MH_STATUS executeCreate = MH_CreateHook(
			g_executeCommandListAddress,
			&hooked_execute_command_list,
			reinterpret_cast<void**>(&g_originalExecuteCommandList));
		const bool executeInstalled =
			executeCreate == MH_OK &&
			MH_EnableHook(g_executeCommandListAddress) == MH_OK;

		if (!finishInstalled || !executeInstalled)
		{
			if (finishCreate == MH_OK)
			{
				MH_DisableHook(g_finishCommandListAddress);
				MH_RemoveHook(g_finishCommandListAddress);
			}
			if (executeCreate == MH_OK)
			{
				MH_DisableHook(g_executeCommandListAddress);
				MH_RemoveHook(g_executeCommandListAddress);
			}
			g_finishCommandListAddress = nullptr;
			g_executeCommandListAddress = nullptr;
		}
		g_commandListHooksInstalled.store(
			finishInstalled && executeInstalled);
		return installed;
	}

	void log_trace_results(
		const std::vector<
			dx11::internal_render_probe::candidate_t>&
			results)
	{
		scs_log(
			0,
			"[RTT probe] Trace finished. "
			"mirror calls=%llu, slot mask=0x%03X, "
			"park schedules=%llu, "
			"render-target candidates=%zu, slot7 dispatches=%llu, "
			"command lists tagged=%llu executed=%llu",
			static_cast<unsigned long long>(
				g_mirrorScheduleCount.load() -
				g_traceStartedMirrorScheduleCount.load()),
			g_mirrorSlotMask.load(),
			static_cast<unsigned long long>(
				g_parkScheduleCount.load()),
			results.size(),
			static_cast<unsigned long long>(
				g_slot7DispatchCount.load()),
			static_cast<unsigned long long>(
				g_commandListTagCount.load()),
			static_cast<unsigned long long>(
				g_commandListExecuteCount.load()));

		for (uint32_t slot = 0;
			slot < kMirrorSlotCount; ++slot)
		{
			if ((g_mirrorSlotMask.load() &
				(1U << slot)) != 0)
			{
				scs_log(
					0,
					"[RTT probe] camera slot %u (%s) "
					"is present, descriptor=%ux%u",
					slot,
					dx11::internal_render_probe::
						slot_name(slot),
					g_slotWidth[slot].load(),
					g_slotHeight[slot].load());
			}
		}

		const size_t logCount = (std::min)(
			results.size(), static_cast<size_t>(40));
		for (size_t index = 0; index < logCount; ++index)
		{
			const auto& candidate = results[index];
			scs_log(
				0,
				"[RTT probe] target #%u: %ux%u "
				"format=%s(%u) samples=%u binds=%llu "
				"api-om/uav=%llu/%llu slot7-during/after=%llu/%llu "
				"slot-match=0x%03X "
				"during-scheduler=%llu near-scheduler=%llu "
				"frames=%llu-%llu threads=%u-%u",
				candidate.id,
				candidate.width,
				candidate.height,
				dx11::internal_render_probe::
					format_name(candidate.format),
				candidate.format,
				candidate.sampleCount,
				static_cast<unsigned long long>(
					candidate.bindCount),
				static_cast<unsigned long long>(
					candidate.omSetRenderTargetsBindCount),
				static_cast<unsigned long long>(
					candidate.omSetRenderTargetsUavBindCount),
				static_cast<unsigned long long>(
					candidate.duringSlot7BindCount),
				static_cast<unsigned long long>(
					candidate.afterSlot7BindCount),
				candidate.matchingCameraSlotMask,
				static_cast<unsigned long long>(
					candidate.duringMirrorScheduleBindCount),
				static_cast<unsigned long long>(
					candidate.nearMirrorBindCount),
				static_cast<unsigned long long>(
					candidate.firstFrame),
				static_cast<unsigned long long>(
					candidate.lastFrame),
				candidate.firstThreadId,
				candidate.lastThreadId);
		}

		std::vector<std::pair<lineage_key_t, lineage_record_t>> lineage;
		{
			std::lock_guard<std::mutex> lock(g_candidateMutex);
			lineage.reserve(g_lineage.size());
			for (const auto& item : g_lineage)
				lineage.push_back(item);
		}
		std::sort(lineage.begin(), lineage.end(),
			[](const auto& left, const auto& right)
			{
				const uint64_t leftCount = left.second.copyRegionCount +
					left.second.copyResourceCount + left.second.resolveCount;
				const uint64_t rightCount = right.second.copyRegionCount +
					right.second.copyResourceCount + right.second.resolveCount;
				return leftCount > rightCount;
			});
		scs_log(0,
			"[RTT lineage] Resource-copy paths=%zu "
			"(source -> destination).",
			lineage.size());
		const size_t lineageLogCount = (std::min)(
			lineage.size(), static_cast<size_t>(80));
		for (size_t index = 0; index < lineageLogCount; ++index)
		{
			const auto& key = lineage[index].first;
			const auto& record = lineage[index].second;
			scs_log(0,
				"[RTT lineage] #%zu %p %ux%u %s(%u) -> "
				"%p %ux%u %s(%u) region/copy/resolve=%llu/%llu/%llu "
				"frames=%llu-%llu thread=%u",
				index + 1,
				reinterpret_cast<void*>(key.source),
				record.source.Width, record.source.Height,
				dx11::internal_render_probe::format_name(
					static_cast<uint32_t>(record.source.Format)),
				static_cast<uint32_t>(record.source.Format),
				reinterpret_cast<void*>(key.destination),
				record.destination.Width, record.destination.Height,
				dx11::internal_render_probe::format_name(
					static_cast<uint32_t>(record.destination.Format)),
				static_cast<uint32_t>(record.destination.Format),
				static_cast<unsigned long long>(record.copyRegionCount),
				static_cast<unsigned long long>(record.copyResourceCount),
				static_cast<unsigned long long>(record.resolveCount),
				static_cast<unsigned long long>(record.firstFrame),
				static_cast<unsigned long long>(record.lastFrame),
				record.threadId);
		}
	}
}

namespace dx11::internal_render_probe
{
	bool init()
	{
		camera_monitor::initialize();
		g_executableBase = reinterpret_cast<uint8_t*>(
			GetModuleHandleW(nullptr));
		const executable_fingerprint_t fingerprint =
			fingerprint_executable();
		g_timeDateStamp.store(fingerprint.timeDateStamp);
		g_imageSize.store(fingerprint.imageSize);
		g_signatureMatches.store(
			fingerprint.signatureMatches);
		g_detectedRva.store(fingerprint.detectedRva);

		const bool supported =
			fingerprint.timeDateStamp ==
				kSupportedTimeDateStamp &&
			fingerprint.imageSize == kSupportedImageSize &&
			fingerprint.signatureMatches == 1 &&
			fingerprint.detectedRva ==
				kExpectedMirrorScheduleRva;
		g_supportedBuild.store(supported);

		if (supported)
		{
			g_mirrorScheduleAddress =
				fingerprint.signatureAddress;
			const MH_STATUS createStatus = MH_CreateHook(
				g_mirrorScheduleAddress,
				&hooked_mirror_schedule,
				reinterpret_cast<void**>(
					&g_originalMirrorSchedule));
			if (createStatus == MH_OK &&
				MH_EnableHook(
					g_mirrorScheduleAddress) == MH_OK)
			{
				g_mirrorHookInstalled.store(true);
			}
			else
			{
				MH_RemoveHook(g_mirrorScheduleAddress);
				g_mirrorScheduleAddress = nullptr;
			}

			if (matches_expected_bytes(
				kExpectedResourceInitRva,
				kResourceInitSignature))
			{
				g_resourceInitAddress =
					g_executableBase +
					kExpectedResourceInitRva;
				const MH_STATUS createStatus =
					MH_CreateHook(
						g_resourceInitAddress,
						&hooked_resource_init,
						reinterpret_cast<void**>(
							&g_originalResourceInit));
				if (createStatus == MH_OK &&
					MH_EnableHook(
						g_resourceInitAddress) == MH_OK)
				{
					g_resourceInitHookInstalled.store(
						true);
				}
				else
				{
					MH_RemoveHook(
						g_resourceInitAddress);
					g_resourceInitAddress = nullptr;
				}
			}

			if (matches_expected_bytes(
				kExpectedActiveMaskRva,
				kActiveMaskSignature))
			{
				g_activeMaskAddress =
					g_executableBase +
					kExpectedActiveMaskRva;
				const MH_STATUS createStatus =
					MH_CreateHook(
						g_activeMaskAddress,
						&hooked_active_mask,
						reinterpret_cast<void**>(
							&g_originalActiveMask));
				if (createStatus == MH_OK &&
					MH_EnableHook(
						g_activeMaskAddress) == MH_OK)
				{
					g_activeMaskHookInstalled.store(
						true);
				}
				else
				{
					MH_RemoveHook(
						g_activeMaskAddress);
					g_activeMaskAddress = nullptr;
				}
			}

			if (matches_expected_bytes(
				kExpectedMirrorRenderDispatchRva,
				kMirrorRenderDispatchSignature))
			{
				g_mirrorRenderDispatchAddress =
					g_executableBase +
						kExpectedMirrorRenderDispatchRva;
				const MH_STATUS createStatus = MH_CreateHook(
					g_mirrorRenderDispatchAddress,
					&hooked_mirror_render_dispatch,
					reinterpret_cast<void**>(
						&g_originalMirrorRenderDispatch));
				if (createStatus == MH_OK &&
					MH_EnableHook(
						g_mirrorRenderDispatchAddress) == MH_OK)
				{
					g_mirrorJobHookInstalled.store(true);
				}
				else
				{
					MH_RemoveHook(g_mirrorRenderDispatchAddress);
					g_mirrorRenderDispatchAddress = nullptr;
				}
			}

			if (matches_expected_bytes(
				kExpectedRenderTaskSubmitRva,
				kRenderTaskSubmitSignature))
			{
				g_renderTaskSubmitAddress =
					g_executableBase + kExpectedRenderTaskSubmitRva;
				g_renderTaskSubmit =
					reinterpret_cast<render_task_submit_t>(
						g_renderTaskSubmitAddress);
			}

			if (matches_expected_bytes(
				kExpectedMirrorWorkerRva,
				kMirrorWorkerSignature))
			{
				g_mirrorWorkerAddress =
					g_executableBase + kExpectedMirrorWorkerRva;
				const MH_STATUS createStatus = MH_CreateHook(
					g_mirrorWorkerAddress,
					&hooked_mirror_worker,
					reinterpret_cast<void**>(
						&g_originalMirrorWorker));
				if (createStatus != MH_OK ||
					MH_EnableHook(g_mirrorWorkerAddress) != MH_OK)
				{
					MH_RemoveHook(g_mirrorWorkerAddress);
					g_mirrorWorkerAddress = nullptr;
				}
			}
		}
		camera_monitor::publish(
			prism_camera_monitor::Stage::Slot7Blocked,
			prism_camera_monitor::kPluginConnected |
				prism_camera_monitor::kSlot7Disabled,
			"Slot 7 permanently disabled",
			"The independent camera experiment cannot install, schedule, "
			"clone, capture, read back, or display slot 7. Start a Camera "
			"Lab diagnostic to inspect the new path.");

		// Do not add any D3D11 interception on ATS or a different ETS2
		// executable. The observer is useful only when the exact internal
		// scheduler hook is also active.
		if (g_mirrorHookInstalled.load())
		{
			g_contextHookInstalled.store(
				install_context_hook());
		}

		scs_log(
			0,
			"[RTT probe] ETS2 timestamp=0x%08X "
			"image=0x%08X signature matches=%u "
			"rva=0x%08X supported=%s",
			fingerprint.timeDateStamp,
			fingerprint.imageSize,
			fingerprint.signatureMatches,
			fingerprint.detectedRva,
			supported ? "yes" : "no");
		scs_log(
			0,
			"[RTT probe] mirror hook=%s, "
			"park init hook=%s, park mask hook=%s, slot dispatch hook=%s, "
			"custom-camera worker=%s, independent submit=%s, "
			"D3D11 target hook=%s, command-list hooks=%s",
			g_mirrorHookInstalled.load()
				? "ready" : "unavailable",
			g_resourceInitHookInstalled.load()
				? "ready" : "unavailable",
			g_activeMaskHookInstalled.load()
				? "ready" : "unavailable",
			g_mirrorJobHookInstalled.load()
				? "ready" : "unavailable",
			g_mirrorWorkerAddress
				? "diagnostic-ready" : "unavailable",
			g_renderTaskSubmit
				? "ready" : "unavailable",
			g_contextHookInstalled.load()
				? "ready" : "unavailable",
			g_commandListHooksInstalled.load()
				? "ready" : "unavailable");

		return
			g_mirrorHookInstalled.load() ||
			g_contextHookInstalled.load();
	}

	void shutdown()
	{
		if (g_tracing.load())
			end_trace();

		if (g_mirrorScheduleAddress)
		{
			MH_DisableHook(g_mirrorScheduleAddress);
			MH_RemoveHook(g_mirrorScheduleAddress);
			g_mirrorScheduleAddress = nullptr;
		}
		if (g_resourceInitAddress)
		{
			MH_DisableHook(g_resourceInitAddress);
			MH_RemoveHook(g_resourceInitAddress);
			g_resourceInitAddress = nullptr;
		}
		if (g_activeMaskAddress)
		{
			MH_DisableHook(g_activeMaskAddress);
			MH_RemoveHook(g_activeMaskAddress);
			g_activeMaskAddress = nullptr;
		}
		if (g_mirrorRenderDispatchAddress)
		{
			MH_DisableHook(g_mirrorRenderDispatchAddress);
			MH_RemoveHook(g_mirrorRenderDispatchAddress);
			g_mirrorRenderDispatchAddress = nullptr;
		}
		if (g_mirrorWorkerAddress)
		{
			MH_DisableHook(g_mirrorWorkerAddress);
			MH_RemoveHook(g_mirrorWorkerAddress);
			g_mirrorWorkerAddress = nullptr;
		}
		g_renderTaskSubmitAddress = nullptr;
		g_renderTaskSubmit = nullptr;
		if (g_omSetRenderTargetsAddress)
		{
			MH_DisableHook(
				g_omSetRenderTargetsAddress);
			MH_RemoveHook(
				g_omSetRenderTargetsAddress);
			g_omSetRenderTargetsAddress = nullptr;
		}
		if (g_omSetRenderTargetsUavAddress)
		{
			MH_DisableHook(g_omSetRenderTargetsUavAddress);
			MH_RemoveHook(g_omSetRenderTargetsUavAddress);
			g_omSetRenderTargetsUavAddress = nullptr;
			g_originalOmSetRenderTargetsUav = nullptr;
		}
		if (g_gameOmSetRenderTargetsAddress)
		{
			MH_DisableHook(g_gameOmSetRenderTargetsAddress);
			MH_RemoveHook(g_gameOmSetRenderTargetsAddress);
			g_gameOmSetRenderTargetsAddress = nullptr;
			g_originalGameOmSetRenderTargets = nullptr;
		}
		if (g_gameOmSetRenderTargetsUavAddress)
		{
			MH_DisableHook(g_gameOmSetRenderTargetsUavAddress);
			MH_RemoveHook(g_gameOmSetRenderTargetsUavAddress);
			g_gameOmSetRenderTargetsUavAddress = nullptr;
			g_originalGameOmSetRenderTargetsUav = nullptr;
		}
		if (g_gameCopySubresourceRegionAddress)
		{
			MH_DisableHook(g_gameCopySubresourceRegionAddress);
			MH_RemoveHook(g_gameCopySubresourceRegionAddress);
			g_gameCopySubresourceRegionAddress = nullptr;
			g_originalGameCopySubresourceRegion = nullptr;
		}
		if (g_gameCopyResourceAddress)
		{
			MH_DisableHook(g_gameCopyResourceAddress);
			MH_RemoveHook(g_gameCopyResourceAddress);
			g_gameCopyResourceAddress = nullptr;
			g_originalGameCopyResource = nullptr;
		}
		if (g_gameResolveSubresourceAddress)
		{
			MH_DisableHook(g_gameResolveSubresourceAddress);
			MH_RemoveHook(g_gameResolveSubresourceAddress);
			g_gameResolveSubresourceAddress = nullptr;
			g_originalGameResolveSubresource = nullptr;
		}
		if (g_finishCommandListAddress)
		{
			MH_DisableHook(g_finishCommandListAddress);
			MH_RemoveHook(g_finishCommandListAddress);
			g_finishCommandListAddress = nullptr;
		}
		if (g_executeCommandListAddress)
		{
			MH_DisableHook(g_executeCommandListAddress);
			MH_RemoveHook(g_executeCommandListAddress);
			g_executeCommandListAddress = nullptr;
		}

		g_mirrorHookInstalled.store(false);
		g_resourceInitHookInstalled.store(false);
		g_activeMaskHookInstalled.store(false);
		g_contextHookInstalled.store(false);
		g_mirrorJobHookInstalled.store(false);
		g_commandListHooksInstalled.store(false);
		g_uavTargetHookInstalled.store(false);
		g_gameContextObserverConfirmed.store(false);
		g_parkCameraInstalled.store(false);
		g_parkResourcePresent.store(false);
		g_parkMaskForced.store(false);
		g_parkVisualInterior.store(nullptr);
		g_parkCamera.store(nullptr);
		{
			std::lock_guard<std::mutex> lock(g_commandListMutex);
			for (auto& item : g_deferredContexts)
				release_com_object(item.second.lastParkSizedTarget);
			g_deferredContexts.clear();
			for (auto& item : g_commandLists)
				release_com_object(item.second.lastParkSizedTarget);
			g_commandLists.clear();
		}
		{
			std::lock_guard<std::mutex> lock(
				g_parkTextureMutex);
			release_park_color_target_locked();
			release_park_compositor_locked();
		}
		camera_correlation::stop();
		camera_monitor::shutdown();
	}

	void on_game_context_available(ID3D11DeviceContext* context)
	{
		if (!context || !g_supportedBuild.load())
			return;

		void** vtable = *reinterpret_cast<void***>(context);
		void* address = vtable ? vtable[33] : nullptr;
		void* uavAddress = vtable ? vtable[34] : nullptr;
		void* copyRegionAddress = vtable ? vtable[46] : nullptr;
		void* copyResourceAddress = vtable ? vtable[47] : nullptr;
		void* resolveAddress = vtable ? vtable[57] : nullptr;
		if (!address || !uavAddress || !copyRegionAddress ||
			!copyResourceAddress || !resolveAddress)
			return;

		std::lock_guard<std::mutex> lock(g_gameContextHookMutex);
		bool standardReady =
			address == g_omSetRenderTargetsAddress ||
			address == g_gameOmSetRenderTargetsAddress;
		if (!standardReady && !g_gameOmSetRenderTargetsAddress)
		{
			const MH_STATUS createStatus = MH_CreateHook(
				address, &hooked_game_om_set_render_targets,
				reinterpret_cast<void**>(
					&g_originalGameOmSetRenderTargets));
			standardReady = createStatus == MH_OK &&
				MH_EnableHook(address) == MH_OK;
			if (standardReady)
				g_gameOmSetRenderTargetsAddress = address;
			else
			{
				if (createStatus == MH_OK)
					MH_RemoveHook(address);
				g_originalGameOmSetRenderTargets = nullptr;
			}
		}

		bool uavReady =
			uavAddress == g_omSetRenderTargetsUavAddress ||
			uavAddress == g_gameOmSetRenderTargetsUavAddress;
		if (!uavReady && !g_gameOmSetRenderTargetsUavAddress)
		{
			const MH_STATUS createStatus = MH_CreateHook(
				uavAddress, &hooked_game_om_set_render_targets_uav,
				reinterpret_cast<void**>(
					&g_originalGameOmSetRenderTargetsUav));
			uavReady = createStatus == MH_OK &&
				MH_EnableHook(uavAddress) == MH_OK;
			if (uavReady)
				g_gameOmSetRenderTargetsUavAddress = uavAddress;
			else
			{
				if (createStatus == MH_OK)
					MH_RemoveHook(uavAddress);
				g_originalGameOmSetRenderTargetsUav = nullptr;
			}
		}

		bool copyRegionReady =
			copyRegionAddress == g_gameCopySubresourceRegionAddress;
		if (!copyRegionReady && !g_gameCopySubresourceRegionAddress)
		{
			const MH_STATUS status = MH_CreateHook(
				copyRegionAddress, &hooked_game_copy_subresource_region,
				reinterpret_cast<void**>(
					&g_originalGameCopySubresourceRegion));
			copyRegionReady = status == MH_OK &&
				MH_EnableHook(copyRegionAddress) == MH_OK;
			if (copyRegionReady)
				g_gameCopySubresourceRegionAddress = copyRegionAddress;
			else if (status == MH_OK)
				MH_RemoveHook(copyRegionAddress);
		}

		bool copyResourceReady =
			copyResourceAddress == g_gameCopyResourceAddress;
		if (!copyResourceReady && !g_gameCopyResourceAddress)
		{
			const MH_STATUS status = MH_CreateHook(
				copyResourceAddress, &hooked_game_copy_resource,
				reinterpret_cast<void**>(&g_originalGameCopyResource));
			copyResourceReady = status == MH_OK &&
				MH_EnableHook(copyResourceAddress) == MH_OK;
			if (copyResourceReady)
				g_gameCopyResourceAddress = copyResourceAddress;
			else if (status == MH_OK)
				MH_RemoveHook(copyResourceAddress);
		}

		bool resolveReady = resolveAddress == g_gameResolveSubresourceAddress;
		if (!resolveReady && !g_gameResolveSubresourceAddress)
		{
			const MH_STATUS status = MH_CreateHook(
				resolveAddress, &hooked_game_resolve_subresource,
				reinterpret_cast<void**>(&g_originalGameResolveSubresource));
			resolveReady = status == MH_OK &&
				MH_EnableHook(resolveAddress) == MH_OK;
			if (resolveReady)
				g_gameResolveSubresourceAddress = resolveAddress;
			else if (status == MH_OK)
				MH_RemoveHook(resolveAddress);
		}

		if (standardReady && uavReady && copyRegionReady &&
			copyResourceReady && resolveReady)
		{
			g_uavTargetHookInstalled.store(true);
			if (!g_gameContextObserverConfirmed.exchange(true))
				scs_log(0,
					"[RTT probe] ETS2 actual-context diagnostics ready: "
					"OM/UAV + CopyRegion/CopyResource/Resolve.");
		}
		else
		{
			scs_log(2,
				"[RTT probe] ETS2 actual-context diagnostics incomplete: "
				"standard=%s uav=%s copy-region=%s copy=%s resolve=%s.",
				standardReady ? "ready" : "failed",
				uavReady ? "ready" : "failed",
				copyRegionReady ? "ready" : "failed",
				copyResourceReady ? "ready" : "failed",
				resolveReady ? "ready" : "failed");
		}
	}

	void on_present_frame(ID3D11DeviceContext* context)
	{
		camera_monitor::heartbeat();
		if (camera_monitor::consume_run_request())
			begin_trace(180);
		uint32_t requestedPhase{};
		if (camera_monitor::consume_phase_request(requestedPhase) &&
			g_tracing.load(std::memory_order_acquire))
		{
			if (camera_correlation::capture_phase(requestedPhase))
			{
				end_trace();
			}
			else
			{
				g_traceEndTick.store(
					GetTickCount64() + 180000,
					std::memory_order_relaxed);
			}
		}
		camera_correlation::tick(
			g_cameraLabObservedJobs.load(std::memory_order_relaxed),
			g_cameraLabRejectedSlot7Jobs.load(
				std::memory_order_relaxed));
		g_frameIndex.fetch_add(
			1, std::memory_order_relaxed);

		if (g_tracing.load(std::memory_order_relaxed))
		{
			const uint64_t now = GetTickCount64();
			if (now >=
				g_traceEndTick.load(
					std::memory_order_relaxed))
			{
				camera_correlation::finish(true);
				end_trace();
			}
		}

		if (!context ||
			!g_parkRenderRequested.load(
				std::memory_order_relaxed))
		{
			return;
		}

		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		ID3D11Texture2D* displayTexture = kIndependentOutputOnly
			? g_independentColorTexture
			: g_parkColorTexture;
		if (!displayTexture ||
			(kIndependentOutputOnly &&
				!g_independentOutputCaptured.load(
					std::memory_order_acquire)))
			return;

		D3D11_TEXTURE2D_DESC description{};
		displayTexture->GetDesc(&description);
		ID3D11Device* sourceDevice{};
		ID3D11Device* contextDevice{};
		displayTexture->GetDevice(&sourceDevice);
		context->GetDevice(&contextDevice);
		const bool sameDevice =
			sourceDevice && sourceDevice == contextDevice;
		if (!sameDevice ||
			!ensure_park_staging_locked(
				sourceDevice, description))
		{
			release_com_object(sourceDevice);
			release_com_object(contextDevice);
			return;
		}
		release_com_object(sourceDevice);
		release_com_object(contextDevice);

		// Creating the staging ring clears its old scheduling markers. If the
		// independent snapshot was captured before that first initialization,
		// re-arm the one required GPU-to-staging copy now. Without this, the
		// owned texture remains valid but no CPU readback is ever submitted.
		if (kIndependentOutputOnly &&
			g_independentOutputCaptured.load(
				std::memory_order_acquire) &&
			g_parkObservedFrame == UINT64_MAX &&
			g_parkSubmittedFrame == UINT64_MAX)
		{
			g_parkObservedFrame = g_frameIndex.load(
				std::memory_order_relaxed);
			scs_log(0,
				"[RTT custom] Independent snapshot re-armed after "
				"first staging initialization.");
		}

		for (auto& slot : g_parkStaging)
		{
			if (!slot.pending || !slot.texture)
				continue;

			D3D11_MAPPED_SUBRESOURCE mapped{};
			const HRESULT result = context->Map(
				slot.texture,
				0,
				D3D11_MAP_READ,
				D3D11_MAP_FLAG_DO_NOT_WAIT,
				&mapped);
			if (result == DXGI_ERROR_WAS_STILL_DRAWING)
			{
				g_parkReadbackBusySkips.fetch_add(
					1, std::memory_order_relaxed);
				continue;
			}
			if (FAILED(result))
			{
				slot.pending = false;
				continue;
			}

			if (slot.submissionOrder >
				g_parkLastDecodedSubmissionOrder)
			{
				if (decode_park_readback_locked(
					mapped, description))
				{
					g_parkLastDecodedSubmissionOrder =
						slot.submissionOrder;
				}
			}
			context->Unmap(slot.texture, 0);
			slot.pending = false;
		}

		if (g_parkObservedFrame == UINT64_MAX ||
			g_parkObservedFrame == g_parkSubmittedFrame)
		{
			return;
		}

		for (uint32_t offset = 0;
			offset < g_parkStaging.size(); ++offset)
		{
			const uint32_t index =
				(g_nextParkStaging + offset) %
				static_cast<uint32_t>(
					g_parkStaging.size());
			auto& slot = g_parkStaging[index];
			if (slot.pending || !slot.texture)
				continue;

			context->CopyResource(
				slot.texture,
				displayTexture);
			slot.pending = true;
			slot.submissionOrder =
				++g_parkNextSubmissionOrder;
			g_nextParkStaging =
				(index + 1) %
				static_cast<uint32_t>(
					g_parkStaging.size());
			g_parkSubmittedFrame =
				g_parkObservedFrame;
			scs_log(0,
				"[RTT custom] Independent snapshot submitted to CPU "
				"staging (order=%llu).",
				static_cast<unsigned long long>(
					slot.submissionOrder));
			break;
		}
	}

	void begin_trace(uint32_t seconds)
	{
		camera_monitor::launch_viewer();
		if (g_tracing.load(std::memory_order_acquire))
			end_trace();
		if (!g_supportedBuild.load() ||
			!g_mirrorHookInstalled.load() ||
			!g_mirrorJobHookInstalled.load() ||
			!g_contextHookInstalled.load())
		{
			camera_monitor::begin_run(
				"Camera Lab could not start because one or more required "
				"Prism3D/D3D11 observers are unavailable.");
			camera_monitor::publish(
				prism_camera_monitor::Stage::Failed,
				prism_camera_monitor::kPluginConnected |
					prism_camera_monitor::kSlot7Disabled,
				"Required observer unavailable",
				!g_supportedBuild.load()
					? "This game executable is not the exact supported ETS2 "
						"build, so no internal addresses were used."
					: "The Prism3D scheduler, render-job observer, or D3D11 "
						"context observer failed to install. Check game.log.txt.");
			return;
		}

		seconds = (std::clamp)(seconds, 30U, 180U);
		g_customCameraStateVerified.store(
			false, std::memory_order_release);
		g_cameraLabObservedJobs.store(
			0, std::memory_order_relaxed);
		g_cameraLabRejectedSlot7Jobs.store(
			0, std::memory_order_relaxed);
		g_parkOutputFrames.store(
			0, std::memory_order_relaxed);
		camera_monitor::begin_run(
			"Slot 7 is hard-disabled. The plugin is observing Prism3D "
			"render jobs for a camera state and target that can be owned "
			"independently. No unverified image is sent to the GPS.");
		// Clear any snapshot left by an older build. This diagnostic never
		// routes its output through the GPS texture.
		release_park_color_target();
		g_parkDiagnosticImageSaved.store(
			false, std::memory_order_release);
		clear_previous_park_diagnostic_bmp();
		{
			std::lock_guard<std::mutex> lock(
				g_candidateMutex);
			g_candidates.clear();
			g_lineage.clear();
			g_nextCandidateId = 1;
		}
		{
			std::lock_guard<std::mutex> lock(
				g_independentCameraDiagnosticMutex);
			g_independentCameraSnapshotHashes.fill(0);
			g_independentCameraSnapshots.store(
				0, std::memory_order_relaxed);
			g_independentWorkerSnapshots.store(
				0, std::memory_order_relaxed);
			g_lastIndependentCommandSnapshotTick.store(
				0, std::memory_order_relaxed);
			g_lastIndependentWorkerSnapshotTick.store(
				0, std::memory_order_relaxed);
			g_independentTargetEvents.store(
				0, std::memory_order_relaxed);
			g_independentSubmitAttempted.store(
				false, std::memory_order_relaxed);
			g_independentSubmitInProgress.store(
				false, std::memory_order_relaxed);
			g_independentSubmitSucceeded.store(
				false, std::memory_order_relaxed);
			g_independentDispatchCount.store(
				0, std::memory_order_relaxed);
			g_independentRenderDepth.store(
				0, std::memory_order_relaxed);
			g_independentTaggedTargetCount.store(
				0, std::memory_order_relaxed);
			g_independentRenderTask.store(
				nullptr, std::memory_order_relaxed);
			g_independentTemplateReady.store(
				false, std::memory_order_relaxed);
			g_independentRenderContext.store(
				nullptr, std::memory_order_relaxed);
			g_independentExclusiveWindow.store(
				false, std::memory_order_relaxed);
			g_independentSubmitDueTick.store(
				0, std::memory_order_relaxed);
			g_independentOutputCapturePending.store(
				false, std::memory_order_relaxed);
			g_independentOutputCaptured.store(
				false, std::memory_order_relaxed);
			g_independentOutputArmedTick.store(
				0, std::memory_order_relaxed);
			g_independentOutputCopyCount.store(
				0, std::memory_order_relaxed);
			g_independentCorrelatedCopyCount.store(
				0, std::memory_order_relaxed);
		}
		{
			std::lock_guard<std::mutex> lock(
				g_independentRenderTargetMutex);
			g_independentRenderTargets.fill(0);
			g_independentRenderTargetCount = 0;
		}

		const uint64_t now = GetTickCount64();
		g_traceStartedTick.store(now);
		g_lastSlot7DispatchEndTick.store(0, std::memory_order_relaxed);
		g_traceStartedMirrorScheduleCount.store(
			g_mirrorScheduleCount.load());
		g_parkScheduleCount.store(
			0, std::memory_order_relaxed);
		g_parkMaskForced.store(
			false, std::memory_order_relaxed);
		g_traceEndTick.store(
			now + static_cast<uint64_t>(seconds) * 1000);
		g_tracing.store(true, std::memory_order_release);
		camera_correlation::begin();
		scs_log(
			0,
			"[Camera Lab] Starting %u-second guided camera-memory "
			"correlation. Slot 7 is disabled and will only be counted "
			"as a rejected native job; all correlation reads are bounded.",
			seconds);
	}

	void end_trace()
	{
		if (!g_tracing.exchange(
			false, std::memory_order_acq_rel))
		{
			return;
		}
		const auto results = candidates();
		log_trace_results(results);
		scs_log(
			0,
			"[Camera Lab] Diagnostic finished: observed-jobs=%llu "
			"rejected-slot7=%llu verified-camera=%s tagged-targets=%u "
			"readback-frames=%llu.",
			static_cast<unsigned long long>(
				g_cameraLabObservedJobs.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(
				g_cameraLabRejectedSlot7Jobs.load(
					std::memory_order_relaxed)),
			g_customCameraStateVerified.load(
				std::memory_order_relaxed) ? "yes" : "no",
			g_independentTaggedTargetCount.load(
				std::memory_order_relaxed),
			static_cast<unsigned long long>(
				g_parkOutputFrames.load(std::memory_order_relaxed)));
		g_independentExclusiveWindow.store(
			false, std::memory_order_release);
		g_independentOutputCapturePending.store(
			false, std::memory_order_release);
		g_parkMaskForced.store(
			false, std::memory_order_relaxed);
		camera_correlation::finish(false);
	}

	void set_park_activation_requested(bool requested)
	{
		const bool previous =
			g_parkActivationRequested.exchange(
				requested, std::memory_order_acq_rel);
		if (!requested && previous)
		{
			g_independentExclusiveWindow.store(
				false, std::memory_order_release);
			g_independentOutputCapturePending.store(
				false, std::memory_order_release);
			g_independentSubmitDueTick.store(
				0, std::memory_order_relaxed);
			g_lastParkScheduleTick.store(
				0, std::memory_order_relaxed);
			g_lastParkForcedFrame.store(
				UINT64_MAX, std::memory_order_relaxed);
			g_parkMaskForced.store(
				false, std::memory_order_relaxed);
			release_park_color_target();
			scs_log(0,
				"[RTT custom] Internal park camera disabled; released "
				"the plugin-owned independent snapshot.");
		}
	}

	void set_park_render_requested(bool requested)
	{
		const bool previous =
			g_parkRenderRequested.exchange(
				requested, std::memory_order_acq_rel);
		if (!requested)
		{
			g_independentExclusiveWindow.store(
				false, std::memory_order_release);
			g_independentOutputCapturePending.store(
				false, std::memory_order_release);
			g_independentSubmitDueTick.store(
				0, std::memory_order_relaxed);
			g_lastParkScheduleTick.store(
				0, std::memory_order_relaxed);
			g_lastParkForcedFrame.store(
				UINT64_MAX, std::memory_order_relaxed);
			g_parkMaskForced.store(
				false, std::memory_order_relaxed);
			if (previous)
			{
				if (g_independentOutputCaptured.load(
						std::memory_order_acquire))
				{
					scs_log(0,
						"[RTT custom] Park rendering paused; retained "
						"the plugin-owned independent snapshot.");
				}
				else
				{
					scs_log(0,
						"[RTT custom] Park rendering paused; no "
						"independent snapshot is available yet.");
				}
			}
		}
	}

	void set_park_target_framerate(uint32_t framerate)
	{
		g_parkTargetFramerate.store(
			(std::clamp)(framerate, 1U, 60U),
			std::memory_order_release);
	}

	void set_park_target_variant(uint32_t variant)
	{
		variant = (std::min)(
			variant, kMaximumParkTargetCandidates);
		const uint32_t previous =
			g_parkTargetVariant.exchange(
				variant, std::memory_order_acq_rel);
		if (previous == variant)
			return;
		if (kIndependentOutputOnly)
		{
			scs_log(0,
				"[RTT custom] Legacy target selector ignored; "
				"the GPS accepts only an independent owned target.");
			return;
		}

		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		release_com_object(g_parkColorTexture);
		g_parkSelectedCandidate =
			kNoParkTargetCandidate;
		g_parkObservedFrame = UINT64_MAX;
		g_parkSubmittedFrame = UINT64_MAX;
		g_parkColorTargetReady.store(
			false, std::memory_order_relaxed);
		g_parkTargetWidth.store(
			0, std::memory_order_relaxed);
		g_parkTargetHeight.store(
			0, std::memory_order_relaxed);
		g_parkTargetFormat.store(
			0, std::memory_order_relaxed);

		if (variant != 0 &&
			variant <= g_parkTargetCandidateCount)
		{
			const auto& candidate =
				g_parkTargetCandidates[variant - 1];
			select_park_target_locked(
				variant - 1,
				candidate.lastObservedFrame);
		}

		if (variant == 0)
		{
			scs_log(
				0,
				"[RTT park] Target selector changed to Auto.");
		}
		else
		{
			scs_log(
				0,
				"[RTT park] Target selector changed to "
				"candidate %c.",
				static_cast<char>('A' + variant - 1));
		}
	}

	void set_park_camera_mount(
		bool kitInstalled,
		bool trailerAware,
		float lateral,
		float height,
		float longitudinal,
		float yawDegrees,
		float pitchDegrees)
	{
		g_parkCameraKitInstalled.store(kitInstalled);
		g_parkTrailerAwareMount.store(trailerAware);
		g_parkMountLateral.store(
			(std::clamp)(lateral, -5.0f, 5.0f));
		g_parkMountHeight.store(
			(std::clamp)(height, -2.0f, 8.0f));
		g_parkMountLongitudinal.store(
			(std::clamp)(longitudinal, -8.0f, 8.0f));
		g_parkMountYaw.store(
			(std::clamp)(yawDegrees, -360.0f, 360.0f));
		g_parkMountPitch.store(
			(std::clamp)(pitchDegrees, -89.0f, 89.0f));
	}

	void on_texture_created(ID3D11Texture2D* texture)
	{
		if (kIndependentOutputOnly || !texture ||
			!g_capturingParkResourceInit ||
			!g_parkCameraInstalled.load(
				std::memory_order_relaxed))
		{
			return;
		}

		D3D11_TEXTURE2D_DESC description{};
		texture->GetDesc(&description);
		if (description.SampleDesc.Count != 1 ||
			(description.BindFlags &
				D3D11_BIND_RENDER_TARGET) == 0)
		{
			return;
		}

		const uint32_t parkWidth =
			g_slotWidth[kParkSlot].load(
				std::memory_order_relaxed);
		const uint32_t parkHeight =
			g_slotHeight[kParkSlot].load(
				std::memory_order_relaxed);
		if (parkWidth == 0 || parkHeight == 0)
			return;

		const bool exactSize =
			description.Width == parkWidth &&
			description.Height == parkHeight;
		const bool scaledSize =
			parkWidth <= UINT32_MAX / 2 &&
			parkHeight <= UINT32_MAX / 2 &&
			description.Width == parkWidth * 2 &&
			description.Height == parkHeight * 2;
		if (!exactSize && !scaledSize)
			return;

		uint32_t formatScore{};
		switch (description.Format)
		{
		case DXGI_FORMAT_R11G11B10_FLOAT:
			formatScore = 400;
			break;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			formatScore = 300;
			break;
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			formatScore = 250;
			break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			formatScore = 200;
			break;
		default:
			return;
		}

		const uint32_t score =
			formatScore + (exactSize ? 100U : 0U);
		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		// Resource initialization creates the shared mirror targets before
		// the dedicated park target. Prefer the latest equally good match so
		// slot 7 replaces an earlier same-size mirror allocation.
		if (score < g_parkColorTextureScore)
			return;

		release_com_object(g_parkColorTexture);
		release_park_sample_resources_locked();
		g_parkColorTexture = texture;
		g_parkColorTexture->AddRef();
		g_parkColorTextureScore = score;
		g_parkTargetWidth.store(
			description.Width, std::memory_order_relaxed);
		g_parkTargetHeight.store(
			description.Height, std::memory_order_relaxed);
		g_parkTargetFormat.store(
			static_cast<uint32_t>(description.Format),
			std::memory_order_relaxed);
		g_parkColorTargetReady.store(
			true, std::memory_order_release);
		scs_log(
			0,
			"[RTT park] Captured park colour target: "
			"%ux%u %s(%u), score=%u.",
			description.Width,
			description.Height,
			format_name(
				static_cast<uint32_t>(
					description.Format)),
			static_cast<uint32_t>(description.Format),
			score);
	}

	bool copy_park_frame(
		std::vector<uint8_t>& destination,
		uint32_t& width,
		uint32_t& height,
		uint64_t& sequence)
	{
		if (kCameraLabViewerOnly)
			return false;
		if (!kSlot7CameraPathEnabled &&
			!g_customCameraStateVerified.load(
				std::memory_order_acquire))
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		if (!g_parkReadbackReady ||
			g_parkReadbackPixels.empty() ||
			g_parkReadbackSequence == sequence)
		{
			return false;
		}

		destination = g_parkReadbackPixels;
		width = g_parkReadbackWidth;
		height = g_parkReadbackHeight;
		sequence = g_parkReadbackSequence;
		return true;
	}

	bool blit_park_texture(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* destination,
		ID3D11RenderTargetView* destinationView,
		bool flipVertical,
		float brightness,
		uint32_t scaleMode,
		uint32_t edgeGuard)
	{
		if (kCameraLabViewerOnly)
			return false;
		if (!context || !destination || !destinationView)
			return false;

		std::lock_guard<std::mutex> lock(
			g_parkTextureMutex);
		ID3D11Texture2D* displayTexture = kIndependentOutputOnly
			? g_independentColorTexture
			: g_parkColorTexture;
		if (!displayTexture ||
			(kIndependentOutputOnly &&
				!g_independentOutputCaptured.load(
					std::memory_order_acquire)))
			return false;

		ID3D11Device* device{};
		context->GetDevice(&device);
		if (!device)
			return false;

		D3D11_TEXTURE2D_DESC sourceDescription{};
		D3D11_TEXTURE2D_DESC destinationDescription{};
		displayTexture->GetDesc(&sourceDescription);
		destination->GetDesc(&destinationDescription);
		const bool valid =
			sourceDescription.SampleDesc.Count == 1 &&
			destinationDescription.Width != 0 &&
			destinationDescription.Height != 0 &&
			ensure_park_compositor_locked(device) &&
			ensure_park_sample_texture_locked(
				device, sourceDescription);
		device->Release();
		if (!valid)
			return false;

		park_compositor_constants_t constants{};
		constants.brightness =
			(std::clamp)(brightness, 0.10f, 2.0f);
		constants.flipVertical =
			flipVertical ? 1.0f : 0.0f;
		constants.edgeGuardX =
			static_cast<float>(edgeGuard) /
			static_cast<float>(
				destinationDescription.Width);
		constants.edgeGuardY =
			static_cast<float>(edgeGuard) /
			static_cast<float>(
				destinationDescription.Height);
		constants.sourceAspect =
			static_cast<float>(sourceDescription.Width) /
			static_cast<float>(sourceDescription.Height);
		constants.destinationAspect =
			static_cast<float>(
				destinationDescription.Width) /
			static_cast<float>(
				destinationDescription.Height);
		constants.scaleMode = (std::min)(scaleMode, 2U);

		// Replacing the output target also unbinds the engine's park target,
		// allowing a legal GPU copy without a CPU readback or synchronization
		// stall.
		context->OMSetRenderTargets(
			1, &destinationView, nullptr);
		context->CopyResource(
			g_parkSampleTexture,
			displayTexture);
		context->UpdateSubresource(
			g_parkConstants,
			0,
			nullptr,
			&constants,
			0,
			0);

		D3D11_VIEWPORT viewport{};
		viewport.Width =
			static_cast<float>(
				destinationDescription.Width);
		viewport.Height =
			static_cast<float>(
				destinationDescription.Height);
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(
			D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(
			g_parkVertexShader, nullptr, 0);
		context->PSSetShader(
			g_parkPixelShader, nullptr, 0);
		context->PSSetConstantBuffers(
			0, 1, &g_parkConstants);
		context->PSSetSamplers(
			0, 1, &g_parkSampler);
		context->PSSetShaderResources(
			0, 1, &g_parkSampleView);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		const FLOAT blendFactor[4]{};
		context->OMSetBlendState(
			nullptr, blendFactor, UINT_MAX);
		context->OMSetDepthStencilState(nullptr, 0);
		context->Draw(3, 0);

		ID3D11ShaderResourceView* noView{};
		context->PSSetShaderResources(0, 1, &noView);
		g_parkOutputFrames.fetch_add(
			1, std::memory_order_relaxed);
		return true;
	}

	status_t status()
	{
		status_t result{};
		result.supportedBuild = g_supportedBuild.load();
		result.mirrorHookInstalled =
			g_mirrorHookInstalled.load();
		result.resourceInitHookInstalled =
			g_resourceInitHookInstalled.load();
		result.activeMaskHookInstalled =
			g_activeMaskHookInstalled.load();
		result.mirrorScheduleSeen =
			g_mirrorScheduleSeen.load();
		result.contextHookInstalled =
			g_contextHookInstalled.load();
		result.actualContextObserverReady =
			g_gameContextObserverConfirmed.load();
		result.uavTargetHookInstalled =
			g_uavTargetHookInstalled.load();
		result.mirrorJobHookInstalled =
			g_mirrorJobHookInstalled.load();
		result.commandListHooksInstalled =
			g_commandListHooksInstalled.load();
		result.tracing = g_tracing.load();
		result.parkActivationRequested =
			g_parkActivationRequested.load();
		result.parkRenderRequested =
			g_parkRenderRequested.load();
		result.parkCameraInstalled =
			g_parkCameraInstalled.load();
		result.parkResourcePresent =
			g_parkResourcePresent.load();
		result.parkMaskForced =
			g_parkMaskForced.load();
		result.parkColorTargetReady =
			g_parkColorTargetReady.load();
		result.parkCompositorReady =
			g_parkCompositorReady.load();
		{
			std::lock_guard<std::mutex> lock(
				g_parkTextureMutex);
			result.parkReadbackReady =
				g_parkReadbackReady;
			result.parkTargetCandidateCount =
				g_parkTargetCandidateCount;
			result.parkSelectedCandidate =
				g_parkSelectedCandidate ==
					kNoParkTargetCandidate
				? 0
				: g_parkSelectedCandidate + 1;
		}
		result.timeDateStamp = g_timeDateStamp.load();
		result.imageSize = g_imageSize.load();
		result.signatureMatches =
			g_signatureMatches.load();
		result.detectedRva = g_detectedRva.load();
		result.mirrorSlotMask = g_mirrorSlotMask.load();
		result.mirrorScheduleCount =
			g_mirrorScheduleCount.load();
		result.parkInstallAttempts =
			g_parkInstallAttempts.load();
		result.parkScheduleCount =
			g_parkScheduleCount.load();
		result.parkOutputFrames =
			g_parkOutputFrames.load();
		result.parkReadbackBusySkips =
			g_parkReadbackBusySkips.load();
		result.slot7DispatchCount =
			g_slot7DispatchCount.load();
		result.commandListTagCount =
			g_commandListTagCount.load();
		result.commandListExecuteCount =
			g_commandListExecuteCount.load();
		result.traceStartedTick =
			g_traceStartedTick.load();
		result.traceEndTick = g_traceEndTick.load();
		result.frameIndex = g_frameIndex.load();
		result.parkTargetWidth =
			g_parkTargetWidth.load();
		result.parkTargetHeight =
			g_parkTargetHeight.load();
		result.parkTargetFormat =
			g_parkTargetFormat.load();
		result.parkTargetFramerate =
			g_parkTargetFramerate.load();
		result.parkTargetVariant =
			g_parkTargetVariant.load();
		for (uint32_t slot = 0;
			slot < kMirrorSlotCount; ++slot)
		{
			result.slotWidth[slot] =
				g_slotWidth[slot].load();
			result.slotHeight[slot] =
				g_slotHeight[slot].load();
		}
		{
			std::lock_guard<std::mutex> lock(
				g_candidateMutex);
			result.candidateCount = g_candidates.size();
		}
		return result;
	}

	std::vector<candidate_t> candidates()
	{
		std::vector<candidate_t> result;
		{
			std::lock_guard<std::mutex> lock(
				g_candidateMutex);
			result.reserve(g_candidates.size());
			for (const auto& item : g_candidates)
				result.push_back(item.second.value);
		}

		std::sort(
			result.begin(), result.end(),
			[](const candidate_t& left,
				const candidate_t& right)
			{
				if (left.duringSlot7BindCount !=
					right.duringSlot7BindCount)
				{
					return left.duringSlot7BindCount >
						right.duringSlot7BindCount;
				}
				if (left.afterSlot7BindCount !=
					right.afterSlot7BindCount)
				{
					return left.afterSlot7BindCount >
						right.afterSlot7BindCount;
				}
				const uint32_t leftParkMatch =
					left.matchingCameraSlotMask &
					((1U << 7) | (1U << 8));
				const uint32_t rightParkMatch =
					right.matchingCameraSlotMask &
					((1U << 7) | (1U << 8));
				if ((leftParkMatch != 0) !=
					(rightParkMatch != 0))
				{
					return leftParkMatch != 0;
				}
				if (left.duringMirrorScheduleBindCount !=
					right.duringMirrorScheduleBindCount)
				{
					return left.duringMirrorScheduleBindCount >
						right.duringMirrorScheduleBindCount;
				}
				if (left.nearMirrorBindCount !=
					right.nearMirrorBindCount)
				{
					return left.nearMirrorBindCount >
						right.nearMirrorBindCount;
				}
				if (left.bindCount != right.bindCount)
					return left.bindCount > right.bindCount;
				return left.id < right.id;
			});
		return result;
	}

	const char* slot_name(uint32_t slot)
	{
		static constexpr const char* names[] = {
			"close",
			"close_s",
			"far",
			"far_s",
			"side",
			"front",
			"hood",
			"park",
			"park_360"
		};
		return slot <
			static_cast<uint32_t>(
				sizeof(names) / sizeof(names[0]))
			? names[slot] : "unknown";
	}

	const char* format_name(uint32_t format)
	{
		switch (static_cast<DXGI_FORMAT>(format))
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			return "RGBA8";
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return "RGBA8_SRGB";
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return "BGRA8";
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return "BGRA8_SRGB";
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return "RGB10A2";
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			return "RGBA16F";
		default:
			return "DXGI";
		}
	}
}
