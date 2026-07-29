#include "internal_render_probe.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook/MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../scs_logging.h"

using namespace scs_logging;

namespace
{
	constexpr uint32_t kSupportedTimeDateStamp = 0x6A426DE5;
	constexpr uint32_t kSupportedImageSize = 0x0382D000;
	constexpr uint32_t kExpectedMirrorScheduleRva = 0x00524DB0;
	constexpr uint32_t kCameraDescriptorOwnerRva = 0x03550398;
	constexpr uint32_t kMirrorSlotCount = 9;
	constexpr uint32_t kMaximumCandidates = 256;

	constexpr std::array<uint8_t, 34> kMirrorScheduleSignature = {
		0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41,
		0x57, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,
		0x48, 0x8B, 0x81, 0x18, 0x01, 0x00, 0x00, 0x41,
		0x8B, 0xE8, 0x4C, 0x8B, 0xFA, 0x48, 0x8B, 0xF1,
		0x48, 0x85
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
	using om_set_render_targets_t =
		void(STDMETHODCALLTYPE*)(ID3D11DeviceContext* context,
			UINT renderTargetViewCount,
			ID3D11RenderTargetView* const* renderTargetViews,
			ID3D11DepthStencilView* depthStencilView);

	std::atomic<bool> g_supportedBuild{};
	std::atomic<bool> g_mirrorHookInstalled{};
	std::atomic<bool> g_contextHookInstalled{};
	std::atomic<bool> g_mirrorScheduleSeen{};
	std::atomic<bool> g_tracing{};
	std::atomic<uint32_t> g_timeDateStamp{};
	std::atomic<uint32_t> g_imageSize{};
	std::atomic<uint32_t> g_signatureMatches{};
	std::atomic<uint32_t> g_detectedRva{};
	std::atomic<uint32_t> g_mirrorSlotMask{};
	std::atomic<uint64_t> g_mirrorScheduleCount{};
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
	void* g_omSetRenderTargetsAddress{};
	mirror_schedule_t g_originalMirrorSchedule{};
	om_set_render_targets_t g_originalOmSetRenderTargets{};

	std::mutex g_candidateMutex;
	std::unordered_map<uintptr_t, candidate_record_t> g_candidates;
	uint32_t g_nextCandidateId = 1;
	thread_local uint32_t g_mirrorScheduleDepth{};

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
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// This is a diagnostic only. A changing engine object must never
			// be allowed to interrupt the game's render scheduler.
		}
	}

	uintptr_t __fastcall hooked_mirror_schedule(
		void* visualInterior,
		void* requestContext,
		uint64_t mode,
		uint64_t fourthArgument)
	{
		const bool tracing =
			g_tracing.load(std::memory_order_relaxed);
		const bool firstObservation =
			!g_mirrorScheduleSeen.load(
				std::memory_order_relaxed);
		if (firstObservation)
		{
			g_mirrorScheduleSeen.store(
				true, std::memory_order_relaxed);
		}
		if (tracing || firstObservation)
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

		if (!tracing)
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

	void record_render_targets(
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews)
	{
		if (!renderTargetViews || renderTargetViewCount == 0)
			return;

		const uint64_t currentFrame =
			g_frameIndex.load(std::memory_order_relaxed);
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
			texture->Release();

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
				if (description.Width ==
						g_slotWidth[slot].load(
							std::memory_order_relaxed) &&
					description.Height ==
						g_slotHeight[slot].load(
							std::memory_order_relaxed))
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
				existing = g_candidates.emplace(
					identity, record).first;
			}

			auto& candidate = existing->second.value;
			candidate.matchingCameraSlotMask |=
				matchingSlotMask;
			++candidate.bindCount;
			candidate.lastFrame = currentFrame;
			if (duringMirrorSchedule)
				++candidate.duringMirrorScheduleBindCount;
			if (nearMirrorSchedule)
				++candidate.nearMirrorBindCount;
		}
	}

	void STDMETHODCALLTYPE hooked_om_set_render_targets(
		ID3D11DeviceContext* context,
		UINT renderTargetViewCount,
		ID3D11RenderTargetView* const* renderTargetViews,
		ID3D11DepthStencilView* depthStencilView)
	{
		if (g_tracing.load(std::memory_order_relaxed))
		{
			record_render_targets(
				renderTargetViewCount,
				renderTargetViews);
		}

		g_originalOmSetRenderTargets(
			context,
			renderTargetViewCount,
			renderTargetViews,
			depthStencilView);
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
			g_omSetRenderTargetsAddress = nullptr;
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
			"render-target candidates=%zu",
			static_cast<unsigned long long>(
				g_mirrorScheduleCount.load() -
				g_traceStartedMirrorScheduleCount.load()),
			g_mirrorSlotMask.load(),
			results.size());

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
				"slot-match=0x%03X "
				"during-scheduler=%llu near-scheduler=%llu "
				"frames=%llu-%llu",
				candidate.id,
				candidate.width,
				candidate.height,
				dx11::internal_render_probe::
					format_name(candidate.format),
				candidate.format,
				candidate.sampleCount,
				static_cast<unsigned long long>(
					candidate.bindCount),
				candidate.matchingCameraSlotMask,
				static_cast<unsigned long long>(
					candidate.duringMirrorScheduleBindCount),
				static_cast<unsigned long long>(
					candidate.nearMirrorBindCount),
				static_cast<unsigned long long>(
					candidate.firstFrame),
				static_cast<unsigned long long>(
					candidate.lastFrame));
		}
	}
}

namespace dx11::internal_render_probe
{
	bool init()
	{
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
		}

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
			"D3D11 target hook=%s",
			g_mirrorHookInstalled.load()
				? "ready" : "unavailable",
			g_contextHookInstalled.load()
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
		if (g_omSetRenderTargetsAddress)
		{
			MH_DisableHook(
				g_omSetRenderTargetsAddress);
			MH_RemoveHook(
				g_omSetRenderTargetsAddress);
			g_omSetRenderTargetsAddress = nullptr;
		}

		g_mirrorHookInstalled.store(false);
		g_contextHookInstalled.store(false);
	}

	void on_present_frame()
	{
		g_frameIndex.fetch_add(
			1, std::memory_order_relaxed);

		if (!g_tracing.load(std::memory_order_relaxed))
			return;

		const uint64_t now = GetTickCount64();
		if (now >=
			g_traceEndTick.load(std::memory_order_relaxed))
		{
			end_trace();
		}
	}

	void begin_trace(uint32_t seconds)
	{
		if (!g_supportedBuild.load() ||
			!g_mirrorHookInstalled.load() ||
			!g_contextHookInstalled.load())
		{
			return;
		}

		seconds = (std::clamp)(seconds, 3U, 30U);
		{
			std::lock_guard<std::mutex> lock(
				g_candidateMutex);
			g_candidates.clear();
			g_nextCandidateId = 1;
		}

		const uint64_t now = GetTickCount64();
		g_traceStartedTick.store(now);
		g_traceStartedMirrorScheduleCount.store(
			g_mirrorScheduleCount.load());
		g_traceEndTick.store(
			now + static_cast<uint64_t>(seconds) * 1000);
		g_tracing.store(true, std::memory_order_release);
		scs_log(
			0,
			"[RTT probe] Starting %u-second read-only "
			"render-target trace",
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
	}

	status_t status()
	{
		status_t result{};
		result.supportedBuild = g_supportedBuild.load();
		result.mirrorHookInstalled =
			g_mirrorHookInstalled.load();
		result.mirrorScheduleSeen =
			g_mirrorScheduleSeen.load();
		result.contextHookInstalled =
			g_contextHookInstalled.load();
		result.tracing = g_tracing.load();
		result.timeDateStamp = g_timeDateStamp.load();
		result.imageSize = g_imageSize.load();
		result.signatureMatches =
			g_signatureMatches.load();
		result.detectedRva = g_detectedRva.load();
		result.mirrorSlotMask = g_mirrorSlotMask.load();
		result.mirrorScheduleCount =
			g_mirrorScheduleCount.load();
		result.traceStartedTick =
			g_traceStartedTick.load();
		result.traceEndTick = g_traceEndTick.load();
		result.frameIndex = g_frameIndex.load();
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
