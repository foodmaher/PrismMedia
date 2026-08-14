#include "internal_render_probe.h"

#include <Windows.h>
#include <d3d11.h>
#include <MinHook/MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "../camera_monitor.h"
#include "../scs_logging.h"
#include "gpu_command_trace.h"

using namespace scs_logging;

namespace
{
	constexpr uint32_t kSupportedTimeDateStamp = 0x6A426DE5;
	constexpr uint32_t kSupportedImageSize = 0x0382D000;
	constexpr uint32_t kExpectedSchedulerRva = 0x00524DB0;
	constexpr uint32_t kExpectedRenderDispatchRva = 0x00722020;
	constexpr uint32_t kExpectedRenderTaskSubmitRva = 0x00722BA0;
	constexpr uint32_t kCameraTokenTableRva = 0x01D1EB30;
	constexpr uint32_t kKnownCameraCount = 9;
	constexpr uint32_t kControlSourceIndex = 5;

	constexpr std::array<uint8_t, 34> kSchedulerSignature = {
		0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x41,
		0x57, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,
		0x48, 0x8B, 0x81, 0x18, 0x01, 0x00, 0x00, 0x41,
		0x8B, 0xE8, 0x4C, 0x8B, 0xFA, 0x48, 0x8B, 0xF1,
		0x48, 0x85
	};
	constexpr std::array<uint8_t, 16> kRenderDispatchSignature = {
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
		0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D
	};
	constexpr std::array<uint8_t, 16> kRenderTaskSubmitSignature = {
		0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
		0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56
	};

	struct fingerprint_t
	{
		uint32_t timeDateStamp{};
		uint32_t imageSize{};
		uint32_t signatureMatches{};
		uint32_t detectedRva{};
		void* schedulerAddress{};
	};

	using scheduler_t = uintptr_t(__fastcall*)(
		void*, void*, uint64_t, uint64_t);
	using render_dispatch_t = void(__fastcall*)(void*, void*, void*);
	using render_task_submit_t = void(__fastcall*)(
		void*, void**, void*, const void*);
	using om_set_render_targets_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
		ID3D11DepthStencilView*);
	using om_set_render_targets_uav_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
		ID3D11DepthStencilView*, UINT, UINT,
		ID3D11UnorderedAccessView* const*, const UINT*);
	using copy_subresource_region_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
		ID3D11Resource*, UINT, const D3D11_BOX*);
	using copy_resource_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
	using resolve_subresource_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, UINT,
		ID3D11Resource*, UINT, DXGI_FORMAT);
	using execute_command_list_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11CommandList*, BOOL);

	uint8_t* g_executableBase{};
	std::atomic<bool> g_supported{};
	std::atomic<bool> g_schedulerHookReady{};
	std::atomic<bool> g_dispatchHookReady{};
	std::atomic<bool> g_contextHooksReady{};
	std::atomic<bool> g_actualContextSeen{};
	std::atomic<bool> g_tracing{};
	std::atomic<bool> g_schedulerSeen{};
	std::atomic<uint32_t> g_timeDateStamp{};
	std::atomic<uint32_t> g_imageSize{};
	std::atomic<uint32_t> g_signatureMatches{};
	std::atomic<uint32_t> g_detectedRva{};
	std::atomic<uint64_t> g_schedulerCalls{};
	std::atomic<uint64_t> g_observedJobs{};
	std::atomic<uint64_t> g_submittedProbeJobs{};
	std::atomic<uint64_t> g_frameIndex{};
	std::atomic<uint64_t> g_traceStartTick{};
	std::atomic<uint64_t> g_traceEndTick{};

	void* g_schedulerAddress{};
	void* g_dispatchAddress{};
	void* g_submitAddress{};
	void* g_omAddress{};
	void* g_omUavAddress{};
	void* g_copyRegionAddress{};
	void* g_copyResourceAddress{};
	void* g_resolveAddress{};
	void* g_executeAddress{};
	scheduler_t g_originalScheduler{};
	render_dispatch_t g_originalDispatch{};
	render_task_submit_t g_submit{};
	om_set_render_targets_t g_originalOm{};
	om_set_render_targets_uav_t g_originalOmUav{};
	copy_subresource_region_t g_originalCopyRegion{};
	copy_resource_t g_originalCopyResource{};
	resolve_subresource_t g_originalResolve{};
	execute_command_list_t g_originalExecute{};
	std::mutex g_contextHookMutex;

	std::array<uint8_t, 0xF0> g_controlTemplate{};
	std::atomic<bool> g_templateReady{};
	std::atomic<bool> g_submitAttempted{};
	std::atomic<bool> g_submitInProgress{};
	std::atomic<void*> g_controlRenderContext{};
	std::atomic<void*> g_probeTask{};

	fingerprint_t fingerprint_executable()
	{
		fingerprint_t result{};
		auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
		if (!module)
			return result;
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return result;
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
			module + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE ||
			nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
			return result;

		result.timeDateStamp = nt->FileHeader.TimeDateStamp;
		result.imageSize = nt->OptionalHeader.SizeOfImage;
		auto* section = IMAGE_FIRST_SECTION(nt);
		for (uint16_t index = 0;
			index < nt->FileHeader.NumberOfSections; ++index, ++section)
		{
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
				continue;
			const uint32_t rva = section->VirtualAddress;
			if (rva >= result.imageSize)
				continue;
			const size_t size = (std::min)(
				static_cast<size_t>(section->Misc.VirtualSize),
				static_cast<size_t>(result.imageSize - rva));
			if (size < kSchedulerSignature.size())
				continue;
			for (size_t offset = 0;
				offset <= size - kSchedulerSignature.size(); ++offset)
			{
				if (std::memcmp(module + rva + offset,
					kSchedulerSignature.data(),
					kSchedulerSignature.size()) != 0)
					continue;
				++result.signatureMatches;
				if (!result.schedulerAddress)
				{
					result.schedulerAddress = module + rva + offset;
					result.detectedRva = rva +
						static_cast<uint32_t>(offset);
				}
			}
		}
		return result;
	}

	template <size_t Size>
	bool matches_bytes(uint32_t rva,
		const std::array<uint8_t, Size>& signature)
	{
		if (!g_executableBase || rva > g_imageSize.load() ||
			Size > static_cast<size_t>(g_imageSize.load() - rva))
			return false;
		bool matches{};
		__try
		{
			matches = std::memcmp(g_executableBase + rva,
				signature.data(), Size) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			matches = false;
		}
		return matches;
	}

	int32_t resolve_source_index(void* renderCommand)
	{
		if (!renderCommand || !g_executableBase)
			return -1;
		__try
		{
			auto* tokens = reinterpret_cast<uint64_t*>(
				g_executableBase + kCameraTokenTableRva);
			const uint64_t token = *reinterpret_cast<uint64_t*>(
				static_cast<uint8_t*>(renderCommand) + 0x38);
			for (uint32_t index = 0; index < kKnownCameraCount; ++index)
				if (token != 0 && token == tokens[index])
					return static_cast<int32_t>(index);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
		return -1;
	}

	bool copy_command(void* source, void* destination, size_t size)
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
			return false;
		}
	}

	void prepare_control_job(void* renderContext, void* renderCommand)
	{
		if (!g_tracing.load(std::memory_order_acquire) || !g_submit ||
			!renderContext || !renderCommand)
			return;
		bool expected = false;
		if (!g_templateReady.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
			return;
		if (!copy_command(renderCommand, g_controlTemplate.data(),
			g_controlTemplate.size()))
		{
			g_templateReady.store(false, std::memory_order_release);
			return;
		}
		g_controlRenderContext.store(renderContext,
			std::memory_order_release);
		scs_log(0,
			"[GPU trace] Non-park control command captured; separate "
			"task submission armed.");
	}

	void submit_control_job()
	{
		if (!g_tracing.load(std::memory_order_acquire) || !g_submit ||
			!g_templateReady.load(std::memory_order_acquire))
			return;
		bool expected = false;
		if (!g_submitAttempted.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
			return;
		void* task{};
		g_submitInProgress.store(true, std::memory_order_release);
		__try
		{
			g_submit(nullptr, &task,
				g_controlRenderContext.load(std::memory_order_acquire),
				g_controlTemplate.data());
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			task = nullptr;
		}
		g_probeTask.store(task, std::memory_order_release);
		g_submitInProgress.store(false, std::memory_order_release);
		if (task)
			scs_log(0,
				"[GPU trace] Separate control job submitted: task=%p.", task);
		else
			scs_log(2,
				"[GPU trace] Control-job submission returned no task.");
	}

	uintptr_t __fastcall hooked_scheduler(void* owner, void* request,
		uint64_t mode, uint64_t fourth)
	{
		g_schedulerSeen.store(true, std::memory_order_relaxed);
		g_schedulerCalls.fetch_add(1, std::memory_order_relaxed);
		if (g_templateReady.load(std::memory_order_acquire) &&
			!g_submitAttempted.load(std::memory_order_acquire))
			submit_control_job();
		return g_originalScheduler(owner, request, mode, fourth);
	}

	void __fastcall hooked_dispatch(void* renderer, void* renderContext,
		void* renderCommand)
	{
		void* owner = renderCommand
			? static_cast<uint8_t*>(renderCommand) - 0x38 : nullptr;
		const bool probe = owner &&
			(owner == g_probeTask.load(std::memory_order_acquire) ||
				g_submitInProgress.load(std::memory_order_acquire));
		const int32_t sourceIndex = resolve_source_index(renderCommand);
		g_observedJobs.fetch_add(1, std::memory_order_relaxed);
		if (probe)
			g_submittedProbeJobs.fetch_add(1, std::memory_order_relaxed);
		dx11::gpu_command_trace::mark_job(true, probe, owner, renderer,
			renderContext, renderCommand, sourceIndex);
		g_originalDispatch(renderer, renderContext, renderCommand);
		dx11::gpu_command_trace::mark_job(false, probe, owner, renderer,
			renderContext, renderCommand, sourceIndex);
		if (!probe && sourceIndex == static_cast<int32_t>(kControlSourceIndex))
			prepare_control_job(renderContext, renderCommand);
	}

	void STDMETHODCALLTYPE hooked_om(ID3D11DeviceContext* context,
		UINT count, ID3D11RenderTargetView* const* views,
		ID3D11DepthStencilView* depth)
	{
		dx11::gpu_command_trace::observe_context(context);
		dx11::gpu_command_trace::note_render_targets(
			context, count, views, depth, false);
		g_originalOm(context, count, views, depth);
	}

	void STDMETHODCALLTYPE hooked_om_uav(ID3D11DeviceContext* context,
		UINT count, ID3D11RenderTargetView* const* views,
		ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
		ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
	{
		dx11::gpu_command_trace::observe_context(context);
		dx11::gpu_command_trace::note_render_targets(
			context, count, views, depth, true);
		g_originalOmUav(context, count, views, depth, uavStart, uavCount,
			uavs, initialCounts);
	}

	void STDMETHODCALLTYPE hooked_copy_region(ID3D11DeviceContext* context,
		ID3D11Resource* destination, UINT destinationSubresource,
		UINT destinationX, UINT destinationY, UINT destinationZ,
		ID3D11Resource* source, UINT sourceSubresource,
		const D3D11_BOX* sourceBox)
	{
		dx11::gpu_command_trace::note_copy_region(context, destination,
			destinationSubresource, source, sourceSubresource, sourceBox);
		g_originalCopyRegion(context, destination, destinationSubresource,
			destinationX, destinationY, destinationZ, source,
			sourceSubresource, sourceBox);
	}

	void STDMETHODCALLTYPE hooked_copy_resource(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source)
	{
		dx11::gpu_command_trace::note_copy_resource(
			context, destination, source);
		g_originalCopyResource(context, destination, source);
	}

	void STDMETHODCALLTYPE hooked_resolve(ID3D11DeviceContext* context,
		ID3D11Resource* destination, UINT destinationSubresource,
		ID3D11Resource* source, UINT sourceSubresource, DXGI_FORMAT format)
	{
		dx11::gpu_command_trace::note_resolve(context, destination, source,
			static_cast<uint32_t>(format));
		g_originalResolve(context, destination, destinationSubresource,
			source, sourceSubresource, format);
	}

	void STDMETHODCALLTYPE hooked_execute(ID3D11DeviceContext* context,
		ID3D11CommandList* commandList, BOOL restoreState)
	{
		dx11::gpu_command_trace::note_execute_command_list(
			context, commandList);
		g_originalExecute(context, commandList, restoreState);
	}

	template <typename Function>
	bool install_hook(void* address, void* hook, Function& original,
		void*& stored)
	{
		if (!address)
			return false;
		if (stored)
			return stored == address;
		const MH_STATUS status = MH_CreateHook(address, hook,
			reinterpret_cast<void**>(&original));
		if (status != MH_OK || MH_EnableHook(address) != MH_OK)
		{
			if (status == MH_OK)
				MH_RemoveHook(address);
			original = nullptr;
			return false;
		}
		stored = address;
		return true;
	}

	void remove_hook(void*& address)
	{
		if (!address)
			return;
		MH_DisableHook(address);
		MH_RemoveHook(address);
		address = nullptr;
	}
}

namespace dx11::internal_render_probe
{
	bool init()
	{
		camera_monitor::initialize();
		g_executableBase = reinterpret_cast<uint8_t*>(
			GetModuleHandleW(nullptr));
		const fingerprint_t fingerprint = fingerprint_executable();
		g_timeDateStamp.store(fingerprint.timeDateStamp);
		g_imageSize.store(fingerprint.imageSize);
		g_signatureMatches.store(fingerprint.signatureMatches);
		g_detectedRva.store(fingerprint.detectedRva);
		const bool supported =
			fingerprint.timeDateStamp == kSupportedTimeDateStamp &&
			fingerprint.imageSize == kSupportedImageSize &&
			fingerprint.signatureMatches == 1 &&
			fingerprint.detectedRva == kExpectedSchedulerRva &&
			matches_bytes(kExpectedRenderDispatchRva,
				kRenderDispatchSignature) &&
			matches_bytes(kExpectedRenderTaskSubmitRva,
				kRenderTaskSubmitSignature);
		g_supported.store(supported);
		if (supported)
		{
			install_hook(fingerprint.schedulerAddress,
				reinterpret_cast<void*>(&hooked_scheduler),
				g_originalScheduler, g_schedulerAddress);
			g_schedulerHookReady.store(g_schedulerAddress != nullptr);
			install_hook(g_executableBase + kExpectedRenderDispatchRva,
				reinterpret_cast<void*>(&hooked_dispatch),
				g_originalDispatch, g_dispatchAddress);
			g_dispatchHookReady.store(g_dispatchAddress != nullptr);
			g_submitAddress = g_executableBase +
				kExpectedRenderTaskSubmitRva;
			g_submit = reinterpret_cast<render_task_submit_t>(
				g_submitAddress);
		}
		camera_monitor::publish(
			prism_camera_monitor::Stage::GpuTraceReady,
			prism_camera_monitor::kPluginConnected |
				prism_camera_monitor::kLegacyCameraPathRemoved,
			"GPU command trace ready",
			"Legacy internal-camera rendering is not compiled into this "
			"module. The diagnostic records one non-park control job only.");
		scs_log(0,
			"[GPU trace] executable timestamp=0x%08X image=0x%08X "
			"matches=%u scheduler-rva=0x%08X supported=%s.",
			fingerprint.timeDateStamp, fingerprint.imageSize,
			fingerprint.signatureMatches, fingerprint.detectedRva,
			supported ? "yes" : "no");
		return supported;
	}

	void shutdown()
	{
		if (g_tracing.load())
			end_trace();
		dx11::gpu_command_trace::shutdown();
		remove_hook(g_executeAddress);
		remove_hook(g_resolveAddress);
		remove_hook(g_copyResourceAddress);
		remove_hook(g_copyRegionAddress);
		remove_hook(g_omUavAddress);
		remove_hook(g_omAddress);
		remove_hook(g_dispatchAddress);
		remove_hook(g_schedulerAddress);
		g_submitAddress = nullptr;
		g_submit = nullptr;
		g_contextHooksReady.store(false);
		g_actualContextSeen.store(false);
		camera_monitor::shutdown();
	}

	void on_game_context_available(ID3D11DeviceContext* context)
	{
		if (!context || !g_supported.load())
			return;
		void** vtable = *reinterpret_cast<void***>(context);
		if (!vtable)
			return;
		std::lock_guard<std::mutex> lock(g_contextHookMutex);
		const bool om = install_hook(vtable[33],
			reinterpret_cast<void*>(&hooked_om), g_originalOm, g_omAddress);
		const bool uav = install_hook(vtable[34],
			reinterpret_cast<void*>(&hooked_om_uav),
			g_originalOmUav, g_omUavAddress);
		const bool region = install_hook(vtable[46],
			reinterpret_cast<void*>(&hooked_copy_region),
			g_originalCopyRegion, g_copyRegionAddress);
		const bool copy = install_hook(vtable[47],
			reinterpret_cast<void*>(&hooked_copy_resource),
			g_originalCopyResource, g_copyResourceAddress);
		const bool resolve = install_hook(vtable[57],
			reinterpret_cast<void*>(&hooked_resolve),
			g_originalResolve, g_resolveAddress);
		const bool execute = install_hook(vtable[58],
			reinterpret_cast<void*>(&hooked_execute),
			g_originalExecute, g_executeAddress);
		const bool deep = dx11::gpu_command_trace::observe_context(context);
		const bool ready = om && uav && region && copy && resolve &&
			execute && deep;
		g_contextHooksReady.store(ready);
		g_actualContextSeen.store(true);
		scs_log(ready ? 0 : 2,
			ready
				? "[GPU trace] D3D11 target/viewport/buffer/draw/copy hooks ready."
				: "[GPU trace] One or more D3D11 trace hooks failed.");
	}

	void on_present_frame(ID3D11DeviceContext*)
	{
		camera_monitor::heartbeat();
		if (camera_monitor::consume_run_request())
			begin_trace(10);
		dx11::gpu_command_trace::tick();
		g_frameIndex.fetch_add(1, std::memory_order_relaxed);
		if (g_tracing.load(std::memory_order_acquire) &&
			GetTickCount64() >= g_traceEndTick.load(
				std::memory_order_relaxed))
			end_trace();
	}

	void begin_trace(uint32_t seconds)
	{
		camera_monitor::launch_viewer();
		if (g_tracing.load())
			end_trace();
		if (!g_supported.load() || !g_schedulerHookReady.load() ||
			!g_dispatchHookReady.load() || !g_contextHooksReady.load())
		{
			camera_monitor::begin_run(
				"GPU tracing could not start because a required observer "
				"is unavailable.");
			camera_monitor::publish(
				prism_camera_monitor::Stage::Failed,
				prism_camera_monitor::kPluginConnected |
					prism_camera_monitor::kLegacyCameraPathRemoved,
				"Required observer unavailable",
				!g_supported.load()
					? "The executable fingerprint is not supported."
					: "The actual D3D11 context was not observed or a hook failed.");
			return;
		}
		seconds = (std::clamp)(seconds, 5U, 15U);
		g_observedJobs.store(0);
		g_submittedProbeJobs.store(0);
		g_templateReady.store(false);
		g_submitAttempted.store(false);
		g_submitInProgress.store(false);
		g_controlRenderContext.store(nullptr);
		g_probeTask.store(nullptr);
		g_controlTemplate.fill(0);
		const uint64_t now = GetTickCount64();
		g_traceStartTick.store(now);
		g_traceEndTick.store(now + static_cast<uint64_t>(seconds) * 1000);
		camera_monitor::begin_run(
			"Recording a bounded API-only trace. No texture pixels are "
			"captured, read back, displayed, or uploaded to the GPS.");
		g_tracing.store(true, std::memory_order_release);
		dx11::gpu_command_trace::begin(seconds * 1000);
		scs_log(0, "[GPU trace] Started %u-second control-job trace.",
			seconds);
	}

	void end_trace()
	{
		if (!g_tracing.exchange(false, std::memory_order_acq_rel))
			return;
		dx11::gpu_command_trace::end();
		camera_monitor::publish(
			prism_camera_monitor::Stage::CorrelationReady,
			prism_camera_monitor::kPluginConnected |
				prism_camera_monitor::kLegacyCameraPathRemoved,
			"GPU command trace saved",
			"Send Documents\\ETS2\\PrismIndependentGpuTrace.bin and "
			"PrismIndependentGpuTrace.txt for analysis.",
			0, g_observedJobs.load(), g_submittedProbeJobs.load());
		scs_log(0,
			"[GPU trace] Finished: observed-jobs=%llu probe-entries=%llu.",
			static_cast<unsigned long long>(g_observedJobs.load()),
			static_cast<unsigned long long>(g_submittedProbeJobs.load()));
	}

	void set_park_activation_requested(bool) {}
	void set_park_render_requested(bool) {}
	void set_park_target_framerate(uint32_t) {}
	void set_park_target_variant(uint32_t) {}
	void set_park_camera_mount(bool, bool, float, float, float, float, float) {}
	void on_texture_created(ID3D11Texture2D*) {}

	bool copy_park_frame(std::vector<uint8_t>&, uint32_t&, uint32_t&,
		uint64_t&)
	{
		return false;
	}

	bool blit_park_texture(ID3D11DeviceContext*, ID3D11Texture2D*,
		ID3D11RenderTargetView*, bool, float, uint32_t, uint32_t)
	{
		return false;
	}

	status_t status()
	{
		status_t result{};
		result.supportedBuild = g_supported.load();
		result.mirrorHookInstalled = g_schedulerHookReady.load();
		result.mirrorScheduleSeen = g_schedulerSeen.load();
		result.contextHookInstalled = g_contextHooksReady.load();
		result.actualContextObserverReady = g_actualContextSeen.load();
		result.uavTargetHookInstalled = g_contextHooksReady.load();
		result.mirrorJobHookInstalled = g_dispatchHookReady.load();
		result.tracing = g_tracing.load();
		result.timeDateStamp = g_timeDateStamp.load();
		result.imageSize = g_imageSize.load();
		result.signatureMatches = g_signatureMatches.load();
		result.detectedRva = g_detectedRva.load();
		result.mirrorScheduleCount = g_schedulerCalls.load();
		result.traceStartedTick = g_traceStartTick.load();
		result.traceEndTick = g_traceEndTick.load();
		result.frameIndex = g_frameIndex.load();
		return result;
	}

	std::vector<candidate_t> candidates()
	{
		return {};
	}

	const char* slot_name(uint32_t)
	{
		return "native view";
	}

	const char* format_name(uint32_t format)
	{
		switch (static_cast<DXGI_FORMAT>(format))
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
		case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
		default: return "unknown";
		}
	}
}
