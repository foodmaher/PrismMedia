#include "gpu_command_trace.h"

#include <Windows.h>
#include <ShlObj.h>
#include <d3d11.h>
#include <MinHook/MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "../scs_logging.h"

using namespace scs_logging;

namespace
{
	enum class event_type : uint32_t
	{
		trace_begin = 1,
		trace_end,
		job_enter,
		job_exit,
		probe_job_enter,
		probe_job_exit,
		om_render_targets,
		om_render_targets_uav,
		vs_constant_buffers,
		rs_viewports,
		draw,
		draw_indexed,
		draw_instanced,
		draw_indexed_instanced,
		update_subresource,
		copy_region,
		copy_resource,
		resolve,
		execute_command_list,
		worker_enter,
		worker_exit,
		scheduler_enter,
		scheduler_exit,
		submit_enter,
		submit_exit,
		frame_marker,
		object_digest,
		stack_frame,
		draw_batch,
		gpu_thread_selected
	};

#pragma pack(push, 1)
	struct trace_header
	{
		uint32_t magic{0x54554750}; // "PGUT"
		uint32_t version{2};
		uint32_t headerBytes{};
		uint32_t eventBytes{};
		uint64_t qpcFrequency{};
		uint64_t startQpc{};
		uint64_t eventCount{};
		uint64_t droppedEvents{};
	};

	struct trace_event
	{
		uint64_t qpc{};
		uint32_t threadId{};
		uint32_t type{};
		uint64_t context{};
		uint64_t a{};
		uint64_t b{};
		uint64_t c{};
		uint64_t d{};
		uint64_t e{};
	};
#pragma pack(pop)

	constexpr uint32_t kMaximumEvents = 262144;
	constexpr uint32_t kDrawBatchSize = 256;
	std::array<trace_event, kMaximumEvents> g_events{};
	std::atomic<uint32_t> g_eventCount{};
	std::atomic<uint64_t> g_dropped{};
	std::atomic<bool> g_active{};
	std::atomic<uint32_t> g_writers{};
	std::atomic<uint64_t> g_endTick{};
	std::atomic<uint32_t> g_primaryGpuThread{};
	std::atomic<uint32_t> g_traceGeneration{};
	std::atomic<bool> g_deepHooksReady{};
	uint64_t g_startQpc{};
	uint64_t g_qpcFrequency{};
	std::mutex g_hookMutex;

	struct draw_batch_state
	{
		uint32_t generation{};
		uint32_t discoveryDraws{};
		uint32_t draws{};
		uint32_t indexed{};
		uint32_t instanced{};
		uint64_t hash{1469598103934665603ULL};
		ID3D11DeviceContext* context{};
		bool targetSeen{};
		bool targetWithUavs{};
		uint32_t targetCount{};
		uint64_t targetFirst{};
		uint64_t targetSecond{};
		uint64_t targetDepth{};
		bool viewportSeen{};
		uint32_t viewportCount{};
		uint64_t viewportSize{};
		uint64_t viewportDepth{};
		bool constantBuffersSeen{};
		uint32_t constantStart{};
		uint32_t constantCount{};
		uint64_t constantFirst{};
		uint64_t constantSecond{};
		uint64_t constantThird{};
	};

	thread_local draw_batch_state g_drawBatch{};

	using vs_set_constant_buffers_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
	using draw_indexed_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, INT);
	using draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using draw_indexed_instanced_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using draw_instanced_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
	using rs_set_viewports_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
	using update_subresource_t = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
		const void*, UINT, UINT);

	void* g_vsConstantBuffersAddress{};
	void* g_drawIndexedAddress{};
	void* g_drawAddress{};
	void* g_drawIndexedInstancedAddress{};
	void* g_drawInstancedAddress{};
	void* g_viewportsAddress{};
	void* g_updateSubresourceAddress{};
	vs_set_constant_buffers_t g_originalVsConstantBuffers{};
	draw_indexed_t g_originalDrawIndexed{};
	draw_t g_originalDraw{};
	draw_indexed_instanced_t g_originalDrawIndexedInstanced{};
	draw_instanced_t g_originalDrawInstanced{};
	rs_set_viewports_t g_originalViewports{};
	update_subresource_t g_originalUpdateSubresource{};

	uint64_t pointer_value(const void* value)
	{
		return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
	}

	uint64_t view_resource_identity(ID3D11View* view)
	{
		if (!view)
			return 0;
		ID3D11Resource* resource{};
		view->GetResource(&resource);
		const uint64_t identity = pointer_value(resource);
		if (resource)
			resource->Release();
		return identity;
	}

	uint64_t bounded_hash(const void* data, size_t size)
	{
		if (!data || size == 0 || size > 4096)
			return 0;
		uint64_t hash = 1469598103934665603ULL;
		__try
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
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

	uint64_t mix_hash(uint64_t hash, uint64_t value)
	{
		hash ^= value;
		hash *= 1099511628211ULL;
		return hash;
	}

	uint64_t qpc_now()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return static_cast<uint64_t>(value.QuadPart);
	}

	uint64_t pack_float_pair(float first, float second)
	{
		uint32_t a{};
		uint32_t b{};
		std::memcpy(&a, &first, sizeof(a));
		std::memcpy(&b, &second, sizeof(b));
		return static_cast<uint64_t>(a) |
			(static_cast<uint64_t>(b) << 32);
	}

	void record(event_type type, ID3D11DeviceContext* context,
		uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
		uint64_t d = 0, uint64_t e = 0)
	{
		if (!g_active.load(std::memory_order_relaxed))
			return;
		g_writers.fetch_add(1, std::memory_order_acq_rel);
		if (!g_active.load(std::memory_order_acquire))
		{
			g_writers.fetch_sub(1, std::memory_order_release);
			return;
		}
		const uint32_t index = g_eventCount.fetch_add(
			1, std::memory_order_relaxed);
		if (index >= kMaximumEvents)
		{
			g_dropped.fetch_add(1, std::memory_order_relaxed);
			g_writers.fetch_sub(1, std::memory_order_release);
			return;
		}
		auto& event = g_events[index];
		event.qpc = qpc_now();
		event.threadId = GetCurrentThreadId();
		event.type = static_cast<uint32_t>(type);
		event.context = pointer_value(context);
		event.a = a;
		event.b = b;
		event.c = c;
		event.d = d;
		event.e = e;
		g_writers.fetch_sub(1, std::memory_order_release);
	}

	bool primary_gpu_thread()
	{
		const uint32_t selected = g_primaryGpuThread.load(
			std::memory_order_acquire);
		return selected != 0 && selected == GetCurrentThreadId();
	}

	void reset_draw_batch_if_needed()
	{
		const uint32_t generation = g_traceGeneration.load(
			std::memory_order_relaxed);
		if (g_drawBatch.generation == generation)
			return;
		g_drawBatch = {};
		g_drawBatch.generation = generation;
		g_drawBatch.hash = 1469598103934665603ULL;
	}

	void flush_draw_batch()
	{
		reset_draw_batch_if_needed();
		if (g_drawBatch.draws == 0 || !primary_gpu_thread())
			return;
		record(event_type::draw_batch, g_drawBatch.context,
			g_drawBatch.draws, g_drawBatch.indexed,
			g_drawBatch.instanced, g_drawBatch.hash);
		g_drawBatch.draws = 0;
		g_drawBatch.indexed = 0;
		g_drawBatch.instanced = 0;
		g_drawBatch.hash = 1469598103934665603ULL;
	}

	void aggregate_draw(ID3D11DeviceContext* context, bool indexed,
		bool instanced, uint64_t first, uint64_t second, uint64_t third)
	{
		if (!g_active.load(std::memory_order_relaxed))
			return;
		reset_draw_batch_if_needed();
		const uint32_t threadId = GetCurrentThreadId();
		uint32_t selected = g_primaryGpuThread.load(
			std::memory_order_acquire);
		if (selected == 0)
		{
			if (++g_drawBatch.discoveryDraws < 64)
				return;
			uint32_t expected = 0;
			if (g_primaryGpuThread.compare_exchange_strong(
				expected, threadId, std::memory_order_acq_rel))
			{
				record(event_type::gpu_thread_selected, context, threadId,
					g_drawBatch.discoveryDraws);
			}
			selected = g_primaryGpuThread.load(std::memory_order_acquire);
		}
		if (selected != threadId)
			return;
		g_drawBatch.context = context;
		++g_drawBatch.draws;
		if (indexed)
			++g_drawBatch.indexed;
		if (instanced)
			++g_drawBatch.instanced;
		g_drawBatch.hash = mix_hash(g_drawBatch.hash,
			(indexed ? 1ULL : 0ULL) | (instanced ? 2ULL : 0ULL));
		g_drawBatch.hash = mix_hash(g_drawBatch.hash, first);
		g_drawBatch.hash = mix_hash(g_drawBatch.hash, second);
		g_drawBatch.hash = mix_hash(g_drawBatch.hash, third);
		if (g_drawBatch.draws >= kDrawBatchSize)
			flush_draw_batch();
	}

	std::wstring output_directory()
	{
		PWSTR documents{};
		if (FAILED(SHGetKnownFolderPath(
			FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents)))
			return {};
		std::wstring result(documents);
		CoTaskMemFree(documents);
		result += L"\\ETS2";
		CreateDirectoryW(result.c_str(), nullptr);
		return result;
	}

	const char* event_name(uint32_t value)
	{
		switch (static_cast<event_type>(value))
		{
		case event_type::trace_begin: return "trace_begin";
		case event_type::trace_end: return "trace_end";
		case event_type::job_enter: return "job_enter";
		case event_type::job_exit: return "job_exit";
		case event_type::probe_job_enter: return "probe_job_enter";
		case event_type::probe_job_exit: return "probe_job_exit";
		case event_type::om_render_targets: return "om_render_targets";
		case event_type::om_render_targets_uav: return "om_render_targets_uav";
		case event_type::vs_constant_buffers: return "vs_constant_buffers";
		case event_type::rs_viewports: return "rs_viewports";
		case event_type::draw: return "draw";
		case event_type::draw_indexed: return "draw_indexed";
		case event_type::draw_instanced: return "draw_instanced";
		case event_type::draw_indexed_instanced: return "draw_indexed_instanced";
		case event_type::update_subresource: return "update_subresource";
		case event_type::copy_region: return "copy_region";
		case event_type::copy_resource: return "copy_resource";
		case event_type::resolve: return "resolve";
		case event_type::execute_command_list: return "execute_command_list";
		case event_type::worker_enter: return "worker_enter";
		case event_type::worker_exit: return "worker_exit";
		case event_type::scheduler_enter: return "scheduler_enter";
		case event_type::scheduler_exit: return "scheduler_exit";
		case event_type::submit_enter: return "submit_enter";
		case event_type::submit_exit: return "submit_exit";
		case event_type::frame_marker: return "frame_marker";
		case event_type::object_digest: return "object_digest";
		case event_type::stack_frame: return "stack_frame";
		case event_type::draw_batch: return "draw_batch";
		case event_type::gpu_thread_selected: return "gpu_thread_selected";
		default: return "unknown";
		}
	}

	void save_trace()
	{
		const std::wstring directory = output_directory();
		if (directory.empty())
		{
			scs_log(2, "[GPU trace] Documents folder was unavailable.");
			return;
		}
		const uint32_t count = (std::min)(
			g_eventCount.load(std::memory_order_acquire), kMaximumEvents);
		trace_header header{};
		header.headerBytes = sizeof(trace_header);
		header.eventBytes = sizeof(trace_event);
		header.qpcFrequency = g_qpcFrequency;
		header.startQpc = g_startQpc;
		header.eventCount = count;
		header.droppedEvents = g_dropped.load(std::memory_order_relaxed);

		const std::wstring binaryPath = directory +
			L"\\PrismIndependentGpuTrace.bin";
		HANDLE binary = CreateFileW(binaryPath.c_str(), GENERIC_WRITE, 0,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (binary != INVALID_HANDLE_VALUE)
		{
			DWORD written{};
			WriteFile(binary, &header, sizeof(header), &written, nullptr);
			if (count != 0)
				WriteFile(binary, g_events.data(),
					count * static_cast<DWORD>(sizeof(trace_event)),
					&written, nullptr);
			CloseHandle(binary);
		}

		std::array<uint64_t, 32> totals{};
		for (uint32_t index = 0; index < count; ++index)
			if (g_events[index].type < totals.size())
				++totals[g_events[index].type];
		const std::wstring summaryPath = directory +
			L"\\PrismIndependentGpuTrace.txt";
		FILE* summary{};
		_wfopen_s(&summary, summaryPath.c_str(), L"wb");
		if (summary)
		{
			std::fprintf(summary,
				"Prism independent GPU-command trace\r\n"
				"Format version: 2\r\nEvents: %u\r\nDropped: %llu\r\n"
				"Binary: PrismIndependentGpuTrace.bin\r\n\r\n",
				count, static_cast<unsigned long long>(header.droppedEvents));
			for (uint32_t type = 1; type < totals.size(); ++type)
				if (totals[type] != 0)
					std::fprintf(summary, "%s: %llu\r\n", event_name(type),
						static_cast<unsigned long long>(totals[type]));
			std::fclose(summary);
		}
		scs_log(0,
			"[GPU trace] Saved %u events (%llu dropped) to Documents\\ETS2.",
			count, static_cast<unsigned long long>(header.droppedEvents));
	}

	void STDMETHODCALLTYPE hooked_vs_constant_buffers(
		ID3D11DeviceContext* context, UINT startSlot, UINT count,
		ID3D11Buffer* const* buffers)
	{
		if (primary_gpu_thread())
		{
			reset_draw_batch_if_needed();
			const uint64_t first = count && buffers
				? pointer_value(buffers[0]) : 0;
			const uint64_t second = count > 1 && buffers
				? pointer_value(buffers[1]) : 0;
			const uint64_t third = count > 2 && buffers
				? pointer_value(buffers[2]) : 0;
			if (!g_drawBatch.constantBuffersSeen ||
				g_drawBatch.constantStart != startSlot ||
				g_drawBatch.constantCount != count ||
				g_drawBatch.constantFirst != first ||
				g_drawBatch.constantSecond != second ||
				g_drawBatch.constantThird != third)
			{
				record(event_type::vs_constant_buffers, context,
					startSlot, count, first, second, third);
				g_drawBatch.constantBuffersSeen = true;
				g_drawBatch.constantStart = startSlot;
				g_drawBatch.constantCount = count;
				g_drawBatch.constantFirst = first;
				g_drawBatch.constantSecond = second;
				g_drawBatch.constantThird = third;
			}
		}
		g_originalVsConstantBuffers(context, startSlot, count, buffers);
	}

	void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* context,
		UINT indexCount, UINT startIndex, INT baseVertex)
	{
		aggregate_draw(context, true, false, indexCount, startIndex,
			static_cast<uint32_t>(baseVertex));
		g_originalDrawIndexed(context, indexCount, startIndex, baseVertex);
	}

	void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* context,
		UINT vertexCount, UINT startVertex)
	{
		aggregate_draw(context, false, false, vertexCount, startVertex, 0);
		g_originalDraw(context, vertexCount, startVertex);
	}

	void STDMETHODCALLTYPE hooked_draw_indexed_instanced(
		ID3D11DeviceContext* context, UINT indexCount, UINT instanceCount,
		UINT startIndex, INT baseVertex, UINT startInstance)
	{
		aggregate_draw(context, true, true, indexCount, instanceCount,
			startIndex ^ (static_cast<uint64_t>(startInstance) << 32));
		g_originalDrawIndexedInstanced(context, indexCount, instanceCount,
			startIndex, baseVertex, startInstance);
	}

	void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* context,
		UINT vertexCount, UINT instanceCount, UINT startVertex,
		UINT startInstance)
	{
		aggregate_draw(context, false, true, vertexCount, instanceCount,
			startVertex ^ (static_cast<uint64_t>(startInstance) << 32));
		g_originalDrawInstanced(context, vertexCount, instanceCount,
			startVertex, startInstance);
	}

	void STDMETHODCALLTYPE hooked_viewports(ID3D11DeviceContext* context,
		UINT count, const D3D11_VIEWPORT* viewports)
	{
		uint64_t size{};
		uint64_t depth{};
		if (count && viewports)
		{
			size = pack_float_pair(viewports[0].Width, viewports[0].Height);
			depth = pack_float_pair(viewports[0].MinDepth,
				viewports[0].MaxDepth);
		}
		if (primary_gpu_thread())
		{
			reset_draw_batch_if_needed();
			if (!g_drawBatch.viewportSeen ||
				g_drawBatch.viewportCount != count ||
				g_drawBatch.viewportSize != size ||
				g_drawBatch.viewportDepth != depth)
			{
				record(event_type::rs_viewports, context, count, size, depth);
				g_drawBatch.viewportSeen = true;
				g_drawBatch.viewportCount = count;
				g_drawBatch.viewportSize = size;
				g_drawBatch.viewportDepth = depth;
			}
		}
		g_originalViewports(context, count, viewports);
	}

	void STDMETHODCALLTYPE hooked_update_subresource(
		ID3D11DeviceContext* context, ID3D11Resource* destination,
		UINT subresource, const D3D11_BOX* box, const void* sourceData,
		UINT rowPitch, UINT depthPitch)
	{
		uint32_t byteWidth{};
		uint64_t dataHash{};
		ID3D11Buffer* buffer{};
		if (g_active.load(std::memory_order_relaxed) && destination &&
			SUCCEEDED(destination->QueryInterface(
			__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
			buffer)
		{
			D3D11_BUFFER_DESC description{};
			buffer->GetDesc(&description);
			byteWidth = description.ByteWidth;
			dataHash = bounded_hash(sourceData, byteWidth);
			buffer->Release();
		}
		if (primary_gpu_thread())
			record(event_type::update_subresource, context,
				pointer_value(destination), subresource, byteWidth,
				dataHash, rowPitch);
		g_originalUpdateSubresource(context, destination, subresource, box,
			sourceData, rowPitch, depthPitch);
	}

	template <typename Function>
	bool install_one(void* address, void* hook, Function& original,
		void*& storedAddress)
	{
		if (storedAddress)
			return storedAddress == address;
		const MH_STATUS status = MH_CreateHook(address, hook,
			reinterpret_cast<void**>(&original));
		if (status != MH_OK || MH_EnableHook(address) != MH_OK)
		{
			if (status == MH_OK)
				MH_RemoveHook(address);
			original = nullptr;
			return false;
		}
		storedAddress = address;
		return true;
	}

	void remove_one(void*& address)
	{
		if (!address)
			return;
		MH_DisableHook(address);
		MH_RemoveHook(address);
		address = nullptr;
	}
}

namespace dx11::gpu_command_trace
{
	bool observe_context(ID3D11DeviceContext* context)
	{
		if (!context)
			return false;
		if (g_deepHooksReady.load(std::memory_order_acquire))
			return true;
		void** vtable = *reinterpret_cast<void***>(context);
		if (!vtable)
			return false;
		std::lock_guard<std::mutex> lock(g_hookMutex);
		const bool a = install_one(vtable[7],
			reinterpret_cast<void*>(&hooked_vs_constant_buffers),
			g_originalVsConstantBuffers, g_vsConstantBuffersAddress);
		const bool b = install_one(vtable[12],
			reinterpret_cast<void*>(&hooked_draw_indexed),
			g_originalDrawIndexed, g_drawIndexedAddress);
		const bool c = install_one(vtable[13],
			reinterpret_cast<void*>(&hooked_draw),
			g_originalDraw, g_drawAddress);
		const bool d = install_one(vtable[20],
			reinterpret_cast<void*>(&hooked_draw_indexed_instanced),
			g_originalDrawIndexedInstanced, g_drawIndexedInstancedAddress);
		const bool e = install_one(vtable[21],
			reinterpret_cast<void*>(&hooked_draw_instanced),
			g_originalDrawInstanced, g_drawInstancedAddress);
		const bool f = install_one(vtable[44],
			reinterpret_cast<void*>(&hooked_viewports),
			g_originalViewports, g_viewportsAddress);
		const bool g = install_one(vtable[48],
			reinterpret_cast<void*>(&hooked_update_subresource),
			g_originalUpdateSubresource, g_updateSubresourceAddress);
		const bool ready = a && b && c && d && e && f && g;
		g_deepHooksReady.store(ready, std::memory_order_release);
		return ready;
	}

	void shutdown()
	{
		if (g_active.load())
			end();
		std::lock_guard<std::mutex> lock(g_hookMutex);
		remove_one(g_vsConstantBuffersAddress);
		remove_one(g_drawIndexedAddress);
		remove_one(g_drawAddress);
		remove_one(g_drawIndexedInstancedAddress);
		remove_one(g_drawInstancedAddress);
		remove_one(g_viewportsAddress);
		remove_one(g_updateSubresourceAddress);
		g_deepHooksReady.store(false, std::memory_order_release);
	}

	void begin(uint32_t durationMilliseconds)
	{
		if (g_active.load())
			end();
		g_eventCount.store(0);
		g_dropped.store(0);
		g_primaryGpuThread.store(0);
		g_traceGeneration.fetch_add(1, std::memory_order_relaxed);
		LARGE_INTEGER frequency{};
		QueryPerformanceFrequency(&frequency);
		g_qpcFrequency = static_cast<uint64_t>(frequency.QuadPart);
		g_startQpc = qpc_now();
		g_endTick.store(GetTickCount64() +
			(std::clamp)(durationMilliseconds, 1000U, 15000U));
		g_active.store(true, std::memory_order_release);
		record(event_type::trace_begin, nullptr, durationMilliseconds);
		scs_log(0, "[GPU trace] Recording the bounded D3D11 command window.");
	}

	void end()
	{
		flush_draw_batch();
		if (!g_active.exchange(false, std::memory_order_acq_rel))
			return;
		const uint64_t waitUntil = GetTickCount64() + 100;
		while (g_writers.load(std::memory_order_acquire) != 0 &&
			GetTickCount64() < waitUntil)
			SwitchToThread();
		// Add the terminal marker manually because recording is now disabled.
		const uint32_t index = g_eventCount.fetch_add(1);
		if (index < kMaximumEvents)
		{
			auto& event = g_events[index];
			event.qpc = qpc_now();
			event.threadId = GetCurrentThreadId();
			event.type = static_cast<uint32_t>(event_type::trace_end);
		}
		save_trace();
	}

	void tick()
	{
		if (g_active.load(std::memory_order_acquire) &&
			GetTickCount64() >= g_endTick.load(std::memory_order_relaxed))
			end();
	}

	bool active()
	{
		return g_active.load(std::memory_order_acquire);
	}

	void mark_job(bool entering, bool submittedProbe, void* owner,
		void* renderer, void* renderContext, void* renderCommand,
		int32_t sourceIndex)
	{
		const event_type type = submittedProbe
			? (entering ? event_type::probe_job_enter :
				event_type::probe_job_exit)
			: (entering ? event_type::job_enter : event_type::job_exit);
		record(type, nullptr, pointer_value(owner), pointer_value(renderer),
			pointer_value(renderContext), pointer_value(renderCommand),
			static_cast<uint32_t>(sourceIndex));
	}

	void mark_worker(bool entering, void* renderer, void* renderContext,
		void* cameraInput, void* renderRequest)
	{
		record(entering ? event_type::worker_enter : event_type::worker_exit,
			nullptr, pointer_value(renderer), pointer_value(renderContext),
			pointer_value(cameraInput), pointer_value(renderRequest));
	}

	void mark_scheduler(bool entering, void* owner, void* request,
		uint64_t mode, uint64_t fourth)
	{
		record(entering ? event_type::scheduler_enter :
			event_type::scheduler_exit, nullptr, pointer_value(owner),
			pointer_value(request), mode, fourth);
	}

	void mark_submit(bool entering, bool pluginSubmit, void* task,
		void* renderContext, const void* renderCommand)
	{
		record(entering ? event_type::submit_enter : event_type::submit_exit,
			nullptr, pluginSubmit ? 1 : 0, pointer_value(task),
			pointer_value(renderContext), pointer_value(renderCommand));
	}

	void mark_frame(uint64_t frameIndex)
	{
		flush_draw_batch();
		record(event_type::frame_marker, nullptr, frameIndex,
			g_primaryGpuThread.load(std::memory_order_relaxed));
	}

	void mark_object_digest(path_role role, void* owner,
		void* renderContext, void* renderCommand, int32_t sourceIndex)
	{
		const uint64_t commandHash = bounded_hash(renderCommand, 0xF0);
		const uint64_t ownerHash = bounded_hash(owner, 0x180);
		const uint64_t contextHash = bounded_hash(renderContext, 0x100);
		record(event_type::object_digest,
			reinterpret_cast<ID3D11DeviceContext*>(renderCommand),
			static_cast<uint32_t>(role),
			static_cast<uint32_t>(sourceIndex), commandHash, ownerHash,
			contextHash);
	}

	void capture_stack(path_role role, uint32_t framesToSkip)
	{
		if (!g_active.load(std::memory_order_relaxed))
			return;
		void* frames[16]{};
		const USHORT count = RtlCaptureStackBackTrace(
			static_cast<ULONG>(framesToSkip + 1),
			static_cast<ULONG>(_countof(frames)), frames, nullptr);
		const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(
			GetModuleHandleW(nullptr));
		for (USHORT index = 0; index < count; ++index)
		{
			const uintptr_t address = reinterpret_cast<uintptr_t>(frames[index]);
			const uint64_t rva = moduleBase && address >= moduleBase
				? static_cast<uint64_t>(address - moduleBase) : UINT64_MAX;
			record(event_type::stack_frame, nullptr,
				static_cast<uint32_t>(role), index, rva, address);
		}
	}

	void note_render_targets(ID3D11DeviceContext* context, uint32_t count,
		ID3D11RenderTargetView* const* views,
		ID3D11DepthStencilView* depthView, bool withUavs)
	{
		if (!g_active.load(std::memory_order_relaxed) ||
			!primary_gpu_thread())
			return;
		flush_draw_batch();
		const uint64_t firstResource = count && views
			? view_resource_identity(views[0]) : 0;
		const uint64_t secondResource = count > 1 && views
			? view_resource_identity(views[1]) : 0;
		const uint64_t depthResource = view_resource_identity(depthView);
		reset_draw_batch_if_needed();
		if (g_drawBatch.targetSeen &&
			g_drawBatch.targetWithUavs == withUavs &&
			g_drawBatch.targetCount == count &&
			g_drawBatch.targetFirst == firstResource &&
			g_drawBatch.targetSecond == secondResource &&
			g_drawBatch.targetDepth == depthResource)
			return;
		record(withUavs ? event_type::om_render_targets_uav :
			event_type::om_render_targets, context, count, firstResource,
			secondResource, depthResource,
			count && views ? pointer_value(views[0]) : 0);
		g_drawBatch.targetSeen = true;
		g_drawBatch.targetWithUavs = withUavs;
		g_drawBatch.targetCount = count;
		g_drawBatch.targetFirst = firstResource;
		g_drawBatch.targetSecond = secondResource;
		g_drawBatch.targetDepth = depthResource;
	}

	void note_copy_region(ID3D11DeviceContext* context,
		ID3D11Resource* destination, uint32_t destinationSubresource,
		ID3D11Resource* source, uint32_t sourceSubresource,
		const D3D11_BOX*)
	{
		if (primary_gpu_thread())
			record(event_type::copy_region, context,
				pointer_value(destination), destinationSubresource,
				pointer_value(source), sourceSubresource);
	}

	void note_copy_resource(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source)
	{
		if (primary_gpu_thread())
			record(event_type::copy_resource, context,
				pointer_value(destination), pointer_value(source));
	}

	void note_resolve(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source, uint32_t format)
	{
		if (primary_gpu_thread())
			record(event_type::resolve, context, pointer_value(destination),
				pointer_value(source), format);
	}

	void note_execute_command_list(ID3D11DeviceContext* context,
		ID3D11CommandList* commandList)
	{
		if (primary_gpu_thread())
			record(event_type::execute_command_list, context,
				pointer_value(commandList));
	}
}
