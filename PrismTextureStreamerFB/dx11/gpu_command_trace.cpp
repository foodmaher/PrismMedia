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
		execute_command_list
	};

#pragma pack(push, 1)
	struct trace_header
	{
		uint32_t magic{0x54554750}; // "PGUT"
		uint32_t version{1};
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

	constexpr uint32_t kMaximumEvents = 131072;
	std::array<trace_event, kMaximumEvents> g_events{};
	std::atomic<uint32_t> g_eventCount{};
	std::atomic<uint64_t> g_dropped{};
	std::atomic<bool> g_active{};
	std::atomic<uint32_t> g_writers{};
	std::atomic<uint64_t> g_endTick{};
	uint64_t g_startQpc{};
	uint64_t g_qpcFrequency{};
	std::mutex g_hookMutex;

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
				"Format version: 1\r\nEvents: %u\r\nDropped: %llu\r\n"
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
		record(event_type::vs_constant_buffers, context, startSlot, count,
			count && buffers ? pointer_value(buffers[0]) : 0,
			count > 1 && buffers ? pointer_value(buffers[1]) : 0,
			count > 2 && buffers ? pointer_value(buffers[2]) : 0);
		g_originalVsConstantBuffers(context, startSlot, count, buffers);
	}

	void STDMETHODCALLTYPE hooked_draw_indexed(ID3D11DeviceContext* context,
		UINT indexCount, UINT startIndex, INT baseVertex)
	{
		record(event_type::draw_indexed, context, indexCount, startIndex,
			static_cast<uint32_t>(baseVertex));
		g_originalDrawIndexed(context, indexCount, startIndex, baseVertex);
	}

	void STDMETHODCALLTYPE hooked_draw(ID3D11DeviceContext* context,
		UINT vertexCount, UINT startVertex)
	{
		record(event_type::draw, context, vertexCount, startVertex);
		g_originalDraw(context, vertexCount, startVertex);
	}

	void STDMETHODCALLTYPE hooked_draw_indexed_instanced(
		ID3D11DeviceContext* context, UINT indexCount, UINT instanceCount,
		UINT startIndex, INT baseVertex, UINT startInstance)
	{
		record(event_type::draw_indexed_instanced, context, indexCount,
			instanceCount, startIndex, static_cast<uint32_t>(baseVertex),
			startInstance);
		g_originalDrawIndexedInstanced(context, indexCount, instanceCount,
			startIndex, baseVertex, startInstance);
	}

	void STDMETHODCALLTYPE hooked_draw_instanced(ID3D11DeviceContext* context,
		UINT vertexCount, UINT instanceCount, UINT startVertex,
		UINT startInstance)
	{
		record(event_type::draw_instanced, context, vertexCount, instanceCount,
			startVertex, startInstance);
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
		record(event_type::rs_viewports, context, count, size, depth);
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
		return a && b && c && d && e && f && g;
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
	}

	void begin(uint32_t durationMilliseconds)
	{
		if (g_active.load())
			end();
		g_eventCount.store(0);
		g_dropped.store(0);
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

	void note_render_targets(ID3D11DeviceContext* context, uint32_t count,
		ID3D11RenderTargetView* const* views,
		ID3D11DepthStencilView* depthView, bool withUavs)
	{
		if (!g_active.load(std::memory_order_relaxed))
			return;
		const uint64_t firstResource = count && views
			? view_resource_identity(views[0]) : 0;
		const uint64_t secondResource = count > 1 && views
			? view_resource_identity(views[1]) : 0;
		const uint64_t depthResource = view_resource_identity(depthView);
		record(withUavs ? event_type::om_render_targets_uav :
			event_type::om_render_targets, context, count, firstResource,
			secondResource, depthResource,
			count && views ? pointer_value(views[0]) : 0);
	}

	void note_copy_region(ID3D11DeviceContext* context,
		ID3D11Resource* destination, uint32_t destinationSubresource,
		ID3D11Resource* source, uint32_t sourceSubresource,
		const D3D11_BOX*)
	{
		record(event_type::copy_region, context, pointer_value(destination),
			destinationSubresource, pointer_value(source), sourceSubresource);
	}

	void note_copy_resource(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source)
	{
		record(event_type::copy_resource, context, pointer_value(destination),
			pointer_value(source));
	}

	void note_resolve(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source, uint32_t format)
	{
		record(event_type::resolve, context, pointer_value(destination),
			pointer_value(source), format);
	}

	void note_execute_command_list(ID3D11DeviceContext* context,
		ID3D11CommandList* commandList)
	{
		record(event_type::execute_command_list, context,
			pointer_value(commandList));
	}
}
