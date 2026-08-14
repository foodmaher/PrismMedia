#pragma once

#include <cstdint>

struct ID3D11Buffer;
struct ID3D11CommandList;
struct ID3D11DepthStencilView;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct D3D11_BOX;

namespace dx11::gpu_command_trace
{
	bool observe_context(ID3D11DeviceContext* context);
	void shutdown();
	void begin(uint32_t durationMilliseconds = 8000);
	void end();
	void tick();
	bool active();

	void mark_job(bool entering, bool submittedProbe, void* owner,
		void* renderer, void* renderContext, void* renderCommand,
		int32_t sourceIndex);
	void note_render_targets(ID3D11DeviceContext* context,
		uint32_t count, ID3D11RenderTargetView* const* views,
		ID3D11DepthStencilView* depthView, bool withUavs);
	void note_copy_region(ID3D11DeviceContext* context,
		ID3D11Resource* destination, uint32_t destinationSubresource,
		ID3D11Resource* source, uint32_t sourceSubresource,
		const D3D11_BOX* sourceBox);
	void note_copy_resource(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source);
	void note_resolve(ID3D11DeviceContext* context,
		ID3D11Resource* destination, ID3D11Resource* source,
		uint32_t format);
	void note_execute_command_list(ID3D11DeviceContext* context,
		ID3D11CommandList* commandList);
}
