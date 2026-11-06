#define NOMINMAX
#include "per_draw_router.h"

#include "custom_render_probe.h"
#include "diagnostic_log.h"
#include "dx11/CreateTexture2D.h"
#include "screens.h"
#include "settings.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr UINT kTrackedSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    constexpr uint64_t kTrainingTimeoutMs = 12000;
    constexpr uint32_t kStableObservationCount = 3;
    constexpr size_t kMaximumIntermediates = 16;
    constexpr size_t kMaximumCandidates = 64;
    // Confirmed in the ETS2 1.62 RenderDoc capture: both addon GPS surfaces
    // sample independent 1024x1024 R8G8B8A8_SRGB targets from PS slot 6.
    constexpr UINT kFinalDrawSourceSlot = 6;
    constexpr UINT kFinalDrawWidth = 1024;
    constexpr UINT kFinalDrawHeight = 1024;
    constexpr uint64_t kPreviewMinimumMs = 3000;
    constexpr uint64_t kPreviewMaximumMs = 60000;

    struct draw_signature_t
    {
        per_draw_router::draw_call_t call;
        uintptr_t indexBuffer{};
        uintptr_t vertexBuffer{};
        UINT vertexStride{};
        UINT vertexOffset{};
        DXGI_FORMAT indexFormat{ DXGI_FORMAT_UNKNOWN };
        UINT indexOffset{};
        D3D11_PRIMITIVE_TOPOLOGY topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
        uintptr_t pixelShader{};
        UINT sourceSlot{};
        uintptr_t sourceIdentity{};
        bool valid{};
    };

    struct intermediate_t
    {
        IUnknown* identity{};
        UINT width{};
        UINT height{};
    };

    struct candidate_t
    {
        draw_signature_t signature;
        uint32_t hits{};
        UINT width{};
        UINT height{};
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        bool nativeFinalDraw{};
    };

    struct stable_selector_t
    {
        per_draw_router::draw_kind_t kind{
            per_draw_router::draw_kind_t::none };
        UINT count{};
        UINT start{};
        INT base{};
        UINT sourceSlot{};
        bool valid{};
    };

    struct tracked_bindings_t
    {
        ID3D11DeviceContext* context{};
        uint64_t generation{};
        std::array<uintptr_t, kTrackedSlots> identities{};
    };

    std::mutex g_mutex;
    bool g_enabled = true;
    per_draw_router::phase_t g_phase = per_draw_router::phase_t::waiting_texture;
    std::string g_displayId;
    std::string g_detail = "Waiting for one enabled custom display.";
    uint64_t g_routeSequence{};
    uint64_t g_trainingStarted{};
    uint64_t g_trainedDraws{};
    uint64_t g_routedDraws{};
    uint64_t g_directObservations{};
    uint64_t g_derivedObservations{};
    uint64_t g_finalDrawObservations{};
    UINT g_targetWidth{};
    UINT g_targetHeight{};
    ID3D11Texture2D* g_targetTexture{};
    IUnknown* g_targetIdentity{};
    ID3D11ShaderResourceView* g_customView{};
    draw_signature_t g_signature;
	// One physical GPS can be presented by multiple shader passes. All passes
	// sharing the selected native screen resource must be replaced together;
	// otherwise a later native pass can draw the GPS over Prism media.
	std::vector<draw_signature_t> g_routeSignatures;
    std::vector<intermediate_t> g_intermediates;
    std::vector<candidate_t> g_candidates;
    std::vector<uint64_t> g_observedOutputSizes;
    bool g_hooksRequested{};
    bool g_hooksTraining{};
    per_draw_router::draw_kind_t g_hookKind{ per_draw_router::draw_kind_t::none };
    bool g_forceRetrain{};
    stable_selector_t g_savedSelector;
	std::vector<draw_signature_t> g_previewSignatures;
    std::vector<draw_signature_t> g_pickerProbeSignatures;
    ID3D11ShaderResourceView* g_pickerProbeView{};
    uint64_t g_previewDeadline{};
    size_t g_previewIndex{};
    uint64_t g_previewHits{};
    bool g_candidateReadyLogged{};
    std::atomic<int> g_fastPhase{
        static_cast<int>(per_draw_router::phase_t::waiting_texture) };
    std::atomic<unsigned> g_fastKind{
        static_cast<unsigned>(per_draw_router::draw_kind_t::none) };
    std::atomic<UINT> g_fastCount{}, g_fastStart{};
    std::atomic<INT> g_fastBase{};
    std::atomic<uint64_t> g_bindingGeneration{ 1 };
    thread_local tracked_bindings_t t_bindings;

    const char* draw_kind_name(per_draw_router::draw_kind_t kind)
    {
        switch (kind)
        {
        case per_draw_router::draw_kind_t::indexed: return "DrawIndexed";
        case per_draw_router::draw_kind_t::draw: return "Draw";
        case per_draw_router::draw_kind_t::indexed_instanced: return "DrawIndexedInstanced";
        case per_draw_router::draw_kind_t::instanced: return "DrawInstanced";
        case per_draw_router::draw_kind_t::draw_auto: return "DrawAuto";
        case per_draw_router::draw_kind_t::indexed_indirect: return "DrawIndexedIndirect";
        case per_draw_router::draw_kind_t::instanced_indirect: return "DrawInstancedIndirect";
        default: return "none";
        }
    }

    bool indexed_kind(per_draw_router::draw_kind_t kind)
    {
        return kind == per_draw_router::draw_kind_t::indexed ||
            kind == per_draw_router::draw_kind_t::indexed_instanced ||
            kind == per_draw_router::draw_kind_t::indexed_indirect;
    }

    uintptr_t resource_identity(ID3D11ShaderResourceView* view)
    {
        if (!view) return 0;
        ID3D11Resource* resource{};
        view->GetResource(&resource);
        if (!resource) return 0;
        IUnknown* identity{};
        const bool ok = SUCCEEDED(resource->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&identity))) && identity;
        resource->Release();
        if (!ok) return 0;
        const uintptr_t result = reinterpret_cast<uintptr_t>(identity);
        identity->Release();
        return result;
    }

    bool same_identity(ID3D11ShaderResourceView* view, uintptr_t target)
    {
        return target != 0 && resource_identity(view) == target;
    }

    void release_intermediates_locked()
    {
        for (auto& intermediate : g_intermediates)
            if (intermediate.identity) intermediate.identity->Release();
        g_intermediates.clear();
    }

    void invalidate_thread_bindings()
    {
        g_bindingGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void ensure_thread_bindings(ID3D11DeviceContext* context)
    {
        const uint64_t generation = g_bindingGeneration.load(
            std::memory_order_acquire);
        if (t_bindings.context == context &&
            t_bindings.generation == generation)
            return;
        t_bindings.context = context;
        t_bindings.generation = generation;
        t_bindings.identities.fill(0);
    }

    void release_target_locked()
    {
        if (g_pickerProbeView) g_pickerProbeView->Release();
        g_pickerProbeView = nullptr;
        g_pickerProbeSignatures.clear();
        if (g_customView) g_customView->Release();
        if (g_targetIdentity) g_targetIdentity->Release();
        if (g_targetTexture) g_targetTexture->Release();
        g_customView = nullptr;
        g_targetIdentity = nullptr;
        g_targetTexture = nullptr;
        g_targetWidth = 0;
        g_targetHeight = 0;
        g_signature = {};
		g_routeSignatures.clear();
        release_intermediates_locked();
        g_candidates.clear();
        g_observedOutputSizes.clear();
        g_directObservations = 0;
        g_derivedObservations = 0;
        g_finalDrawObservations = 0;
		g_previewSignatures.clear();
        g_previewDeadline = 0;
        g_previewIndex = 0;
        g_previewHits = 0;
        g_candidateReadyLogged = false;
        invalidate_thread_bindings();
    }

    void set_phase_locked(per_draw_router::phase_t phase)
    {
        g_phase = phase;
        g_fastPhase.store(static_cast<int>(phase), std::memory_order_release);
    }

    void request_hooks_locked(
        bool enabled,
        bool training = false,
        per_draw_router::draw_kind_t kind = per_draw_router::draw_kind_t::none)
    {
        if (g_hooksRequested == enabled &&
            (!enabled || (g_hooksTraining == training && g_hookKind == kind)))
            return;
        if (dx11::create_texture_2d::set_per_draw_router_hooks_enabled(
                enabled, enabled && training, static_cast<unsigned>(kind)))
        {
            g_hooksRequested = enabled;
            g_hooksTraining = enabled && training;
            g_hookKind = enabled ? kind : per_draw_router::draw_kind_t::none;
        }
        else
        {
            diagnostic_log::write(
                "error", "Per-display router could not change its D3D hook lease.");
        }
    }

    bool same_call(
        const per_draw_router::draw_call_t& left,
        const per_draw_router::draw_call_t& right)
    {
        return left.kind == right.kind && left.count == right.count &&
            left.start == right.start && left.base == right.base &&
            left.instances == right.instances &&
            left.firstInstance == right.firstInstance &&
            left.indirectBuffer == right.indirectBuffer;
    }

    draw_signature_t capture_signature(
        ID3D11DeviceContext* context,
        const per_draw_router::draw_call_t& call,
        UINT sourceSlot,
        uintptr_t sourceIdentity)
    {
        draw_signature_t result{};
        result.call = call;
        result.sourceSlot = sourceSlot;
        result.sourceIdentity = sourceIdentity;

        ID3D11Buffer* ib{};
        context->IAGetIndexBuffer(&ib, &result.indexFormat, &result.indexOffset);
        result.indexBuffer = reinterpret_cast<uintptr_t>(ib);
        if (ib) ib->Release();

        ID3D11Buffer* vb{};
        context->IAGetVertexBuffers(
            0, 1, &vb, &result.vertexStride, &result.vertexOffset);
        result.vertexBuffer = reinterpret_cast<uintptr_t>(vb);
        if (vb) vb->Release();

        context->IAGetPrimitiveTopology(&result.topology);
        ID3D11PixelShader* shader{};
        context->PSGetShader(&shader, nullptr, nullptr);
        result.pixelShader = reinterpret_cast<uintptr_t>(shader);
        if (shader) shader->Release();

        result.valid = result.vertexBuffer != 0 && result.pixelShader != 0 &&
            (!indexed_kind(call.kind) || result.indexBuffer != 0);
        return result;
    }

    bool equal_signature(
        const draw_signature_t& left,
        const draw_signature_t& right,
        bool compareSource = true)
    {
        return left.valid && right.valid && same_call(left.call, right.call) &&
            left.indexBuffer == right.indexBuffer &&
            left.vertexBuffer == right.vertexBuffer &&
            left.vertexStride == right.vertexStride &&
            left.vertexOffset == right.vertexOffset &&
            left.indexFormat == right.indexFormat &&
            left.indexOffset == right.indexOffset &&
            left.topology == right.topology &&
            left.pixelShader == right.pixelShader &&
            left.sourceSlot == right.sourceSlot &&
            (!compareSource || left.sourceIdentity == right.sourceIdentity);
    }

    bool matches_signature(
        ID3D11DeviceContext* context,
        const draw_signature_t& expected,
        const per_draw_router::draw_call_t& call)
    {
        if (!expected.valid || !same_call(expected.call, call)) return false;
        const draw_signature_t current = capture_signature(
            context, call, expected.sourceSlot, expected.sourceIdentity);
        if (!equal_signature(current, expected, false)) return false;
        ID3D11ShaderResourceView* view{};
        context->PSGetShaderResources(expected.sourceSlot, 1, &view);
        const bool sourceMatches = same_identity(view, expected.sourceIdentity);
        if (view) view->Release();
        return sourceMatches;
    }

    bool describe_final_draw_source(
        ID3D11ShaderResourceView* view,
        UINT& width,
        UINT& height,
        DXGI_FORMAT& format,
        uintptr_t& identity)
    {
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
        identity = 0;
        if (!view) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        view->GetDesc(&viewDescription);

        ID3D11Resource* resource{};
        view->GetResource(&resource);
        if (!resource) return false;
        ID3D11Texture2D* texture{};
        const HRESULT result = resource->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        if (SUCCEEDED(result) && texture)
        {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            width = description.Width;
            height = description.Height;
            format = description.Format;
            texture->Release();
        }
        IUnknown* canonical{};
        if (SUCCEEDED(resource->QueryInterface(
                __uuidof(IUnknown), reinterpret_cast<void**>(&canonical))) &&
            canonical)
        {
            identity = reinterpret_cast<uintptr_t>(canonical);
            canonical->Release();
        }
        resource->Release();

        const bool rgbaResource = format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool rgbaView = viewDescription.Format == DXGI_FORMAT_UNKNOWN ||
            viewDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            viewDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        return identity != 0 && width == kFinalDrawWidth &&
            height == kFinalDrawHeight && rgbaResource && rgbaView;
    }

    bool selector_matches(
        const stable_selector_t& selector,
        const candidate_t& candidate)
    {
        return selector.valid && candidate.nativeFinalDraw &&
            candidate.signature.call.kind == selector.kind &&
            candidate.signature.call.count == selector.count &&
            candidate.signature.call.start == selector.start &&
            candidate.signature.call.base == selector.base &&
            candidate.signature.sourceSlot == selector.sourceSlot;
    }

	bool same_physical_group(
		const candidate_t& left,
		const candidate_t& right)
	{
		return left.nativeFinalDraw && right.nativeFinalDraw &&
			left.signature.sourceIdentity == right.signature.sourceIdentity &&
			left.signature.call.kind == right.signature.call.kind &&
			left.signature.call.count == right.signature.call.count &&
			left.signature.call.start == right.signature.call.start &&
			left.signature.call.base == right.signature.call.base &&
			left.signature.sourceSlot == right.signature.sourceSlot;
	}

    void restore_saved_selector_if_unique_locked()
    {
        if (!g_savedSelector.valid || g_signature.valid) return;
		candidate_t* firstMatch{};
		uintptr_t selectedSource{};
        for (auto& candidate : g_candidates)
        {
            if (candidate.hits < kStableObservationCount ||
                !selector_matches(g_savedSelector, candidate))
                continue;
			if (!firstMatch)
			{
				firstMatch = &candidate;
				selectedSource = candidate.signature.sourceIdentity;
			}
			else if (candidate.signature.sourceIdentity != selectedSource)
			{
				// The stable selector now identifies more than one physical
				// screen. Do not guess after a game/mod update.
				return;
			}
        }
		if (firstMatch)
        {
			g_routeSignatures.clear();
			for (const auto& candidate : g_candidates)
			{
				if (candidate.hits >= kStableObservationCount &&
					selector_matches(g_savedSelector, candidate) &&
					candidate.signature.sourceIdentity == selectedSource)
					g_routeSignatures.push_back(candidate.signature);
			}
			g_signature = firstMatch->signature;
            ++g_trainedDraws;
			std::ostringstream detail;
			detail << "Restored " << g_routeSignatures.size()
				<< " saved final-draw pass(es) for one physical screen.";
			g_detail = detail.str();
        }
    }

    void observe_native_final_draw_locked(
        ID3D11DeviceContext* context,
        const per_draw_router::draw_call_t& call)
    {
        if (call.kind != per_draw_router::draw_kind_t::indexed)
            return;

        ID3D11ShaderResourceView* view{};
        context->PSGetShaderResources(kFinalDrawSourceSlot, 1, &view);
        UINT width{}, height{};
        DXGI_FORMAT format{ DXGI_FORMAT_UNKNOWN };
        uintptr_t identity{};
        const bool matches = describe_final_draw_source(
            view, width, height, format, identity);
        if (view) view->Release();
        if (!matches) return;

        const draw_signature_t signature = capture_signature(
            context, call, kFinalDrawSourceSlot, identity);
        if (!signature.valid) return;
        ++g_finalDrawObservations;

        auto found = std::find_if(
            g_candidates.begin(), g_candidates.end(),
            [&signature](const candidate_t& item) {
                return item.nativeFinalDraw &&
                    equal_signature(item.signature, signature);
            });
        if (found == g_candidates.end())
        {
            if (g_candidates.size() >= kMaximumCandidates) return;
            g_candidates.push_back(
                { signature, 1, width, height, format, true });
        }
        else
        {
            ++found->hits;
        }
        restore_saved_selector_if_unique_locked();
    }

    bool bind_custom_view_locked(
        ID3D11DeviceContext* context,
        UINT slot,
        per_draw_router::route_scope_t& scope,
        bool preview,
        std::unique_lock<std::mutex>& lock)
    {
        if (!g_customView) return false;
        ID3D11ShaderResourceView* custom = g_customView;
        custom->AddRef();
        lock.unlock();

        ID3D11ShaderResourceView* nativeView{};
        context->PSGetShaderResources(slot, 1, &nativeView);
        context->PSSetShaderResources(slot, 1, &custom);
        custom->Release();
        scope.slot = slot;
        scope.nativeView = nativeView;
        scope.active = true;
        scope.preview = preview;
        return true;
    }

    bool bind_view_locked(
        ID3D11DeviceContext* context,
        UINT slot,
        ID3D11ShaderResourceView* replacement,
        per_draw_router::route_scope_t& scope,
        bool preview,
        std::unique_lock<std::mutex>& lock)
    {
        if (!replacement) return false;
        replacement->AddRef();
        lock.unlock();
        ID3D11ShaderResourceView* nativeView{};
        context->PSGetShaderResources(slot, 1, &nativeView);
        context->PSSetShaderResources(slot, 1, &replacement);
        replacement->Release();
        scope.slot = slot;
        scope.nativeView = nativeView;
        scope.active = true;
        scope.preview = preview;
        return true;
    }

    bool find_source_locked(UINT& slot, uintptr_t& identity, unsigned& hop)
    {
        if (!g_targetIdentity) return false;
        const uintptr_t target = reinterpret_cast<uintptr_t>(g_targetIdentity);
        for (UINT index = 0; index < kTrackedSlots; ++index)
        {
            const uintptr_t bound = t_bindings.identities[index];
            if (!bound) continue;
            for (const auto& intermediate : g_intermediates)
            {
                if (bound == reinterpret_cast<uintptr_t>(intermediate.identity))
                {
                    slot = index;
                    identity = bound;
                    hop = 1;
                    return true;
                }
            }
        }
        for (UINT index = 0; index < kTrackedSlots; ++index)
        {
            if (t_bindings.identities[index] == target)
            {
                slot = index;
                identity = target;
                hop = 0;
                return true;
            }
        }
        return false;
    }

    void remember_matching_outputs_locked(ID3D11DeviceContext* context)
    {
        if (!g_targetWidth || !g_targetHeight ||
            g_intermediates.size() >= kMaximumIntermediates)
            return;

        ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth{};
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth);
        if (depth) depth->Release();

        for (ID3D11RenderTargetView* target : targets)
        {
            if (!target) continue;
            ID3D11Resource* resource{};
            target->GetResource(&resource);
            target->Release();
            if (!resource) continue;

            ID3D11Texture2D* texture{};
            D3D11_TEXTURE2D_DESC description{};
            if (SUCCEEDED(resource->QueryInterface(
                    __uuidof(ID3D11Texture2D),
                    reinterpret_cast<void**>(&texture))) && texture)
            {
                texture->GetDesc(&description);
                texture->Release();
            }
            IUnknown* identity{};
            resource->QueryInterface(
                __uuidof(IUnknown), reinterpret_cast<void**>(&identity));
            resource->Release();
            if (description.Width && description.Height &&
                g_observedOutputSizes.size() < 16)
            {
                const uint64_t size =
                    (static_cast<uint64_t>(description.Width) << 32) |
                    description.Height;
                if (std::find(
                        g_observedOutputSizes.begin(),
                        g_observedOutputSizes.end(), size) ==
                    g_observedOutputSizes.end())
                    g_observedOutputSizes.push_back(size);
            }
            if (!identity || description.Width != g_targetWidth ||
                description.Height != g_targetHeight)
            {
                if (identity) identity->Release();
                continue;
            }

            const uintptr_t value = reinterpret_cast<uintptr_t>(identity);
            const bool exists = value == reinterpret_cast<uintptr_t>(g_targetIdentity) ||
                std::any_of(
                    g_intermediates.begin(), g_intermediates.end(),
                    [value](const intermediate_t& item) {
                        return value == reinterpret_cast<uintptr_t>(item.identity);
                    });
            if (exists)
            {
                identity->Release();
                continue;
            }
            g_intermediates.push_back(
                { identity, description.Width, description.Height });
            diagnostic_log::writef(
                "route",
                "Per-display router traced compositor target %p (%ux%u).",
                static_cast<void*>(identity), description.Width, description.Height);
            if (g_intermediates.size() >= kMaximumIntermediates) break;
        }
    }

    bool quad_like(const per_draw_router::draw_call_t& call)
    {
        if (call.kind == per_draw_router::draw_kind_t::draw_auto ||
            call.kind == per_draw_router::draw_kind_t::indexed_indirect ||
            call.kind == per_draw_router::draw_kind_t::instanced_indirect)
            return true;
        return call.count == 3 || call.count == 4 || call.count == 6;
    }

    void observe_candidate_locked(
        ID3D11DeviceContext* context,
        const per_draw_router::draw_call_t& call,
        UINT sourceSlot,
        uintptr_t sourceIdentity)
    {
        if (!quad_like(call)) return;
        const draw_signature_t candidate = capture_signature(
            context, call, sourceSlot, sourceIdentity);
        if (!candidate.valid) return;

        auto found = std::find_if(
            g_candidates.begin(), g_candidates.end(),
            [&candidate](const candidate_t& item) {
                return equal_signature(item.signature, candidate);
            });
        if (found == g_candidates.end())
        {
            if (g_candidates.size() >= kMaximumCandidates) return;
            g_candidates.push_back({ candidate, 1 });
            found = g_candidates.end() - 1;
        }
        else
        {
            ++found->hits;
        }

        if (found->hits >= kStableObservationCount && !g_signature.valid)
        {
            g_signature = found->signature;
            ++g_trainedDraws;
            std::ostringstream detail;
            detail << "Compositor route learned: "
                << draw_kind_name(g_signature.call.kind)
                << " slot=" << g_signature.sourceSlot
                << "; waiting for telemetry handoff.";
            g_detail = detail.str();
        }
    }

    const char* phase_name(per_draw_router::phase_t phase)
    {
        switch (phase)
        {
        case per_draw_router::phase_t::disabled: return "disabled";
        case per_draw_router::phase_t::waiting_texture: return "waiting-texture";
        case per_draw_router::phase_t::training: return "training";
        case per_draw_router::phase_t::active: return "active";
        case per_draw_router::phase_t::fallback: return "fallback";
        }
        return "unknown";
    }
}

namespace per_draw_router
{
    void update(bool driving)
    {
        ID3D11Texture2D* texture{};
        uint64_t routeSequence{};
        std::string displayId;
		stable_selector_t persistedSelector;
        size_t eligible{};
        {
            std::lock_guard<std::mutex> screensLock(g_screens_mutex);
            for (const screen_t& screen : g_screens)
            {
                if (screen.type != screen_type_t::CUSTOM || !screen.enabled ||
                    !screen.liveTexture || !screen.source)
                    continue;
                ++eligible;
                if (eligible == 1)
                {
                    texture = screen.liveTexture;
                    texture->AddRef();
                    routeSequence = screen.textureRouteMatchedSequence;
                    displayId = screen.mediaClientId;
					if (screen.finalDrawRouteValid &&
						screen.finalDrawRouteKind <
							static_cast<uint32_t>(draw_kind_t::none) &&
						screen.finalDrawRouteSlot < kTrackedSlots)
					{
						persistedSelector.kind = static_cast<draw_kind_t>(
							screen.finalDrawRouteKind);
						persistedSelector.count = screen.finalDrawRouteCount;
						persistedSelector.start = screen.finalDrawRouteStart;
						persistedSelector.base = screen.finalDrawRouteBase;
						persistedSelector.sourceSlot = screen.finalDrawRouteSlot;
						persistedSelector.valid = true;
					}
                }
            }
        }

        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_enabled)
        {
            if (texture) texture->Release();
            request_hooks_locked(false);
            release_target_locked();
            set_phase_locked(phase_t::disabled);
            g_detail = "Per-display routing was disabled from the console.";
            return;
        }
        if (!driving || eligible == 0)
        {
            if (texture) texture->Release();
            request_hooks_locked(false);
            release_target_locked();
            set_phase_locked(phase_t::waiting_texture);
            g_detail = "Waiting for one enabled custom display in the truck.";
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::automatic);
            return;
        }
        if (eligible != 1)
        {
            if (texture) texture->Release();
            request_hooks_locked(false);
            release_target_locked();
            set_phase_locked(phase_t::fallback);
            g_detail = "Enable exactly one custom display while learning a route.";
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            return;
        }

        const bool changed = g_forceRetrain || texture != g_targetTexture ||
            routeSequence != g_routeSequence || displayId != g_displayId;
        if (changed)
        {
            g_forceRetrain = false;
			g_savedSelector = persistedSelector;
            release_target_locked();
            g_targetTexture = texture;
            texture = nullptr;
            g_targetTexture->QueryInterface(
                __uuidof(IUnknown), reinterpret_cast<void**>(&g_targetIdentity));
            D3D11_TEXTURE2D_DESC targetDescription{};
            g_targetTexture->GetDesc(&targetDescription);
            g_targetWidth = targetDescription.Width;
            g_targetHeight = targetDescription.Height;

            ID3D11Device* device{};
            g_targetTexture->GetDevice(&device);
            const HRESULT viewResult = device
                ? device->CreateShaderResourceView(
                    g_targetTexture, nullptr, &g_customView)
                : E_POINTER;
            if (device) device->Release();
            g_displayId = displayId;
            g_routeSequence = routeSequence;
            g_trainingStarted = GetTickCount64();
            g_signature = {};
            g_candidates.clear();
            invalidate_thread_bindings();

            if (!g_targetIdentity || FAILED(viewResult) || !g_customView)
            {
                set_phase_locked(phase_t::fallback);
                g_detail = "Could not create the custom display SRV; original displays retained.";
                request_hooks_locked(false);
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_off);
                diagnostic_log::writef(
                    "error",
                    "Per-display router setup failed for '%s' (HRESULT 0x%08X).",
                    g_displayId.c_str(), static_cast<unsigned>(viewResult));
            }
            else
            {
                set_phase_locked(phase_t::training);
                g_detail = "Discovering independent 1024x1024 GPS final draws at PS slot 6.";
                // Training no longer relies on the version-specific global
                // branch, so native GPS/smartphone displays remain untouched.
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_off);
				// RenderDoc proved both target surfaces use DrawIndexed, so the
				// calibration never enables the other six draw detours or the
				// high-frequency PSSetShaderResources detour.
                request_hooks_locked(true, false, draw_kind_t::indexed);
                diagnostic_log::writef(
                    "route",
                    "Per-display final-draw discovery started for '%s' (%ux%u media).",
                    g_displayId.c_str(), g_targetWidth, g_targetHeight);
            }
        }
        else if (texture)
        {
            texture->Release();
        }

        if (g_phase == phase_t::training && g_signature.valid)
        {
			if (g_routeSignatures.empty())
				g_routeSignatures.push_back(g_signature);
            g_fastKind.store(
                static_cast<unsigned>(g_signature.call.kind),
                std::memory_order_relaxed);
            g_fastCount.store(g_signature.call.count, std::memory_order_relaxed);
            g_fastStart.store(g_signature.call.start, std::memory_order_relaxed);
            g_fastBase.store(g_signature.call.base, std::memory_order_relaxed);
            set_phase_locked(phase_t::active);
            std::ostringstream detail;
			detail << "Routing " << g_routeSignatures.size() << " "
				<< draw_kind_name(g_signature.call.kind)
				<< " pass(es) for one physical screen"
                << " slot=" << g_signature.sourceSlot
				<< "; unmatched screens retain native resources.";
            g_detail = detail.str();
            request_hooks_locked(true, false, g_signature.call.kind);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            diagnostic_log::writef(
                "route",
                "Per-display router active for '%s': %s count=%u start=%u "
				"base=%d slot=%u passes=%llu; native displays retained.",
                g_displayId.c_str(), draw_kind_name(g_signature.call.kind),
                g_signature.call.count, g_signature.call.start,
				g_signature.call.base, g_signature.sourceSlot,
				static_cast<unsigned long long>(g_routeSignatures.size()));
        }
        else if (g_phase == phase_t::training &&
            GetTickCount64() - g_trainingStarted >= kTrainingTimeoutMs)
        {
            const bool hasFinalDrawCandidate = std::any_of(
                g_candidates.begin(), g_candidates.end(),
                [](const candidate_t& candidate) {
                    return candidate.nativeFinalDraw &&
                        candidate.hits >= kStableObservationCount;
                });
            if (hasFinalDrawCandidate)
            {
                g_detail = "Final-draw candidates are ready; use route candidates, "
                    "route preview, then route select.";
                if (!g_candidateReadyLogged)
                {
                    g_candidateReadyLogged = true;
                    diagnostic_log::write(
                        "route",
                        "Independent GPS final draws discovered. Select one "
                        "through the diagnostic console; native displays remain active.");
                }
            }
            else
            {
                set_phase_locked(phase_t::fallback);
                g_detail = "Final-draw discovery timed out; original displays retained.";
                request_hooks_locked(false);
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_off);
                diagnostic_log::write(
                    "error",
                    "Per-display router found no stable 1024x1024 slot-6 GPS "
                    "draw within 12 seconds; original displays were retained.");
            }
        }
    }

    void set_enabled(bool enabled)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_enabled = enabled;
        if (enabled) g_forceRetrain = true;
    }

    void retrain()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_enabled = true;
        g_forceRetrain = true;
    }

    status_t status()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::ostringstream detail;
        detail << phase_name(g_phase) << ": " << g_detail;
        if (g_phase == phase_t::training || g_phase == phase_t::fallback)
        {
            detail << " traceDirect=" << g_directObservations
                << " traceTargets=" << g_intermediates.size()
                << " traceDerived=" << g_derivedObservations
                << " finalDraws=" << g_finalDrawObservations
                << " candidates=" << g_candidates.size();
            if (g_previewDeadline != 0)
            {
                const uint64_t now = GetTickCount64();
                detail << " preview=" << g_previewIndex
                    << " previewHits=" << g_previewHits
                    << " previewMs="
                    << (now < g_previewDeadline
                        ? g_previewDeadline - now : 0);
            }
            if (!g_observedOutputSizes.empty())
            {
                detail << " outputs=";
                for (size_t index = 0;
                    index < g_observedOutputSizes.size(); ++index)
                {
                    if (index) detail << ',';
                    detail << (g_observedOutputSizes[index] >> 32) << 'x'
                        << (g_observedOutputSizes[index] & 0xFFFFFFFFu);
                }
            }
        }
        return { g_phase, g_enabled, g_displayId, g_trainedDraws,
            g_routedDraws, detail.str() };
    }

    std::string command(const std::string& text)
    {
        std::istringstream input(text);
        std::string action;
        input >> action;
        std::transform(action.begin(), action.end(), action.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });

        std::unique_lock<std::mutex> lock(g_mutex);
        auto stable_candidate = [](size_t requested) -> candidate_t* {
            size_t ordinal{};
			for (size_t index = 0; index < g_candidates.size(); ++index)
            {
				auto& candidate = g_candidates[index];
                if (!candidate.nativeFinalDraw ||
                    candidate.hits < kStableObservationCount)
                    continue;
				bool alreadyRepresented{};
				for (size_t prior = 0; prior < index; ++prior)
				{
					if (g_candidates[prior].hits >= kStableObservationCount &&
						same_physical_group(g_candidates[prior], candidate))
					{
						alreadyRepresented = true;
						break;
					}
				}
				if (alreadyRepresented) continue;
                if (ordinal++ == requested)
                    return &candidate;
            }
            return nullptr;
        };

        if (action.empty() || action == "candidates")
        {
            std::ostringstream rows;
            size_t ordinal{};
			for (size_t index = 0; index < g_candidates.size(); ++index)
            {
				const auto& candidate = g_candidates[index];
                if (!candidate.nativeFinalDraw ||
                    candidate.hits < kStableObservationCount)
                    continue;
				bool alreadyRepresented{};
				for (size_t prior = 0; prior < index; ++prior)
				{
					if (g_candidates[prior].hits >= kStableObservationCount &&
						same_physical_group(g_candidates[prior], candidate))
					{
						alreadyRepresented = true;
						break;
					}
				}
				if (alreadyRepresented) continue;
				size_t passes{};
				for (const auto& grouped : g_candidates)
					if (grouped.hits >= kStableObservationCount &&
						same_physical_group(candidate, grouped)) ++passes;
                rows << "\n" << ordinal
                    << " kind=" << draw_kind_name(candidate.signature.call.kind)
                    << " count=" << candidate.signature.call.count
                    << " start=" << candidate.signature.call.start
                    << " base=" << candidate.signature.call.base
                    << " slot=" << candidate.signature.sourceSlot
                    << " hits=" << candidate.hits
                    << " resource=0x" << std::hex
                    << candidate.signature.sourceIdentity
                    << " ps=0x" << candidate.signature.pixelShader
                    << std::dec
                    << " size=" << candidate.width << 'x' << candidate.height
					<< " format=" << static_cast<unsigned>(candidate.format)
					<< " passes=" << passes;
                ++ordinal;
            }
            std::ostringstream output;
            output << "OK candidates=" << ordinal << rows.str();
            if (ordinal == 0)
                output << "\nnone; keep the cockpit visible and run route retrain";
            return output.str();
        }

        if (action == "preview")
        {
            std::string selection;
            input >> selection;
            std::string extra;
            if (selection == "off" || selection == "stop")
            {
                if (input >> extra)
                    return "ERROR Use route preview off.";
				g_previewSignatures.clear();
                g_previewDeadline = 0;
                return "OK Final-draw preview stopped; native GPS binding restored.";
            }

            uint64_t index{};
            uint64_t seconds{ 8 };
            try
            {
                size_t used{};
                index = std::stoull(selection, &used, 10);
                if (used != selection.size()) throw std::invalid_argument("index");
                std::string duration;
                if (input >> duration)
                {
                    used = 0;
                    seconds = std::stoull(duration, &used, 10);
                    if (used != duration.size())
                        throw std::invalid_argument("seconds");
                }
                if (input >> extra)
                    throw std::invalid_argument("extra");
            }
            catch (...)
            {
                return "ERROR Use route preview <candidate> [3..60 seconds], or route preview off.";
            }
            if (seconds * 1000 < kPreviewMinimumMs ||
                seconds * 1000 > kPreviewMaximumMs)
                return "ERROR Preview duration must be 3..60 seconds.";
            if (!g_customView)
                return "ERROR The enabled custom display has no live media SRV yet.";
            if (g_phase == phase_t::active)
                return "ERROR A route is already active; use route retrain before previewing another candidate.";
            candidate_t* candidate = stable_candidate(static_cast<size_t>(index));
            if (!candidate)
                return "ERROR Candidate index is not in route candidates.";

			g_previewSignatures.clear();
			for (const auto& grouped : g_candidates)
				if (grouped.hits >= kStableObservationCount &&
					same_physical_group(*candidate, grouped))
					g_previewSignatures.push_back(grouped.signature);
            g_previewIndex = static_cast<size_t>(index);
            g_previewHits = 0;
            g_previewDeadline = GetTickCount64() + seconds * 1000;
            set_phase_locked(phase_t::training);
            request_hooks_locked(true, false, draw_kind_t::indexed);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            std::ostringstream response;
            response << "OK Previewing candidate " << index << " for "
				<< seconds << " seconds using " << g_previewSignatures.size()
				<< " pass(es); unmatched screens remain native.";
            return response.str();
        }

        if (action == "select")
        {
            std::string selection, extra;
            if (!(input >> selection) || (input >> extra))
                return "ERROR Use route select <candidate>.";
            uint64_t index{};
            try
            {
                size_t used{};
                index = std::stoull(selection, &used, 10);
                if (used != selection.size())
                    throw std::invalid_argument("index");
            }
            catch (...)
            {
                return "ERROR Use route select <candidate>.";
            }
            candidate_t* candidate = stable_candidate(static_cast<size_t>(index));
            if (!candidate)
                return "ERROR Candidate index is not in route candidates.";
            g_signature = candidate->signature;
			g_routeSignatures.clear();
			for (const auto& grouped : g_candidates)
			{
				if (grouped.nativeFinalDraw &&
					grouped.hits >= kStableObservationCount &&
					grouped.signature.sourceIdentity ==
						candidate->signature.sourceIdentity &&
					grouped.signature.call.kind == candidate->signature.call.kind &&
					grouped.signature.call.count == candidate->signature.call.count &&
					grouped.signature.call.start == candidate->signature.call.start &&
					grouped.signature.call.base == candidate->signature.call.base &&
					grouped.signature.sourceSlot == candidate->signature.sourceSlot)
					g_routeSignatures.push_back(grouped.signature);
			}
			if (g_routeSignatures.empty())
				g_routeSignatures.push_back(candidate->signature);
            g_savedSelector = {
                candidate->signature.call.kind,
                candidate->signature.call.count,
                candidate->signature.call.start,
                candidate->signature.call.base,
                candidate->signature.sourceSlot,
                true };
			g_previewSignatures.clear();
            g_previewDeadline = 0;
            g_trainingStarted = GetTickCount64();
			++g_trainedDraws;
            set_phase_locked(phase_t::training);
            request_hooks_locked(true, false, draw_kind_t::indexed);
            diagnostic_log::writef(
                "route",
                "Final-draw candidate %llu selected: %s count=%u start=%u "
				"base=%d slot=%u resource=%p passes=%llu.",
                static_cast<unsigned long long>(index),
                draw_kind_name(candidate->signature.call.kind),
                candidate->signature.call.count,
                candidate->signature.call.start,
                candidate->signature.call.base,
                candidate->signature.sourceSlot,
				reinterpret_cast<void*>(candidate->signature.sourceIdentity),
				static_cast<unsigned long long>(g_routeSignatures.size()));

			const std::string selectedDisplay = g_displayId;
			const stable_selector_t saved = g_savedSelector;
			lock.unlock();
			bool screenFound{};
			{
				std::lock_guard<std::mutex> screensLock(g_screens_mutex);
				for (screen_t& screen : g_screens)
				{
					if (screen.type != screen_type_t::CUSTOM ||
						screen.mediaClientId != selectedDisplay)
						continue;
					screen.finalDrawRouteValid = true;
					screen.finalDrawRouteKind =
						static_cast<uint32_t>(saved.kind);
					screen.finalDrawRouteCount = saved.count;
					screen.finalDrawRouteStart = saved.start;
					screen.finalDrawRouteBase = saved.base;
					screen.finalDrawRouteSlot = saved.sourceSlot;
					screenFound = true;
					break;
				}
			}
			const bool savedConfiguration = screenFound && settings::save();
			return savedConfiguration
				? "OK Candidate selected and saved; route becomes active on the next telemetry update."
				: "OK Candidate selected for this session, but its persistent setting could not be saved.";
        }

        if (action == "clear")
        {
            std::string extra;
            if (input >> extra)
                return "ERROR Use route clear.";
            g_savedSelector = {};
			g_previewSignatures.clear();
            g_previewDeadline = 0;
            g_forceRetrain = true;
			const std::string selectedDisplay = g_displayId;
			lock.unlock();
			bool screenFound{};
			{
				std::lock_guard<std::mutex> screensLock(g_screens_mutex);
				for (screen_t& screen : g_screens)
				{
					if (screen.type != screen_type_t::CUSTOM ||
						screen.mediaClientId != selectedDisplay)
						continue;
					screen.finalDrawRouteValid = false;
					screenFound = true;
					break;
				}
			}
			const bool savedConfiguration = screenFound && settings::save();
			return savedConfiguration
				? "OK Saved final-draw selection cleared; route retraining requested."
				: "OK Session selection cleared, but its persistent setting could not be saved.";
        }

        return "ERROR Use route candidates, preview <candidate> [seconds], "
            "preview off, select <candidate>, or clear.";
    }

    size_t picker_candidate_count()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        size_t result{};
        for (size_t index = 0; index < g_candidates.size(); ++index)
        {
            const auto& candidate = g_candidates[index];
            if (!candidate.nativeFinalDraw ||
                candidate.hits < kStableObservationCount)
                continue;
            bool represented{};
            for (size_t prior = 0; prior < index; ++prior)
            {
                if (g_candidates[prior].hits >= kStableObservationCount &&
                    same_physical_group(g_candidates[prior], candidate))
                {
                    represented = true;
                    break;
                }
            }
            if (!represented) ++result;
        }
        return result;
    }

    bool picker_set_probe(
        size_t requested,
        ID3D11ShaderResourceView* probeView)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        candidate_t* selected{};
        size_t ordinal{};
        for (size_t index = 0; index < g_candidates.size(); ++index)
        {
            auto& candidate = g_candidates[index];
            if (!candidate.nativeFinalDraw ||
                candidate.hits < kStableObservationCount)
                continue;
            bool represented{};
            for (size_t prior = 0; prior < index; ++prior)
            {
                if (g_candidates[prior].hits >= kStableObservationCount &&
                    same_physical_group(g_candidates[prior], candidate))
                {
                    represented = true;
                    break;
                }
            }
            if (represented) continue;
            if (ordinal++ == requested)
            {
                selected = &candidate;
                break;
            }
        }
        if (!selected || !probeView) return false;

        if (g_pickerProbeView) g_pickerProbeView->Release();
        g_pickerProbeView = probeView;
        g_pickerProbeView->AddRef();
        g_pickerProbeSignatures.clear();
        for (const auto& grouped : g_candidates)
        {
            if (grouped.hits >= kStableObservationCount &&
                same_physical_group(*selected, grouped))
                g_pickerProbeSignatures.push_back(grouped.signature);
        }
        if (g_pickerProbeSignatures.empty())
        {
            g_pickerProbeView->Release();
            g_pickerProbeView = nullptr;
            return false;
        }
        set_phase_locked(phase_t::training);
        request_hooks_locked(true, false, draw_kind_t::indexed);
        custom_render_probe::set_fallback_mode(
            custom_render_probe::fallback_mode_t::forced_off);
        return true;
    }

    void picker_clear_probe()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pickerProbeView) g_pickerProbeView->Release();
        g_pickerProbeView = nullptr;
        g_pickerProbeSignatures.clear();
    }

    void notify_pixel_shader_resources(
        ID3D11DeviceContext* context,
        UINT startSlot,
        UINT viewCount,
        ID3D11ShaderResourceView* const* views)
    {
        if (g_fastPhase.load(std::memory_order_acquire) !=
                static_cast<int>(phase_t::training) ||
            startSlot >= kTrackedSlots || viewCount == 0)
            return;

        ensure_thread_bindings(context);
        const UINT end = (std::min)(kTrackedSlots, startSlot + viewCount);
        for (UINT slot = startSlot; slot < end; ++slot)
        {
            const UINT offset = slot - startSlot;
            t_bindings.identities[slot] = views && offset < viewCount
                ? resource_identity(views[offset])
                : 0;
        }
    }

    bool begin_draw(
        ID3D11DeviceContext* context,
        const draw_call_t& call,
        route_scope_t& scope)
    {
        scope = {};
        const int fastPhase = g_fastPhase.load(std::memory_order_acquire);
        if (fastPhase != static_cast<int>(phase_t::training) &&
            fastPhase != static_cast<int>(phase_t::active))
            return false;

        if (fastPhase == static_cast<int>(phase_t::active) &&
            (static_cast<unsigned>(call.kind) !=
                    g_fastKind.load(std::memory_order_relaxed) ||
             call.count != g_fastCount.load(std::memory_order_relaxed) ||
             call.start != g_fastStart.load(std::memory_order_relaxed) ||
             call.base != g_fastBase.load(std::memory_order_relaxed)))
            return false;

        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_phase == phase_t::training)
        {
            observe_native_final_draw_locked(context, call);

            if (g_pickerProbeView)
            {
                for (const auto& signature : g_pickerProbeSignatures)
                {
                    if (!matches_signature(context, signature, call))
                        continue;
                    return bind_view_locked(
                        context, signature.sourceSlot, g_pickerProbeView,
                        scope, true, lock);
                }
            }

            if (g_previewDeadline != 0)
            {
                const uint64_t now = GetTickCount64();
                if (now >= g_previewDeadline)
                {
					g_previewSignatures.clear();
                    g_previewDeadline = 0;
                }
				else
                {
					for (const auto& signature : g_previewSignatures)
					{
						if (!matches_signature(context, signature, call))
							continue;
						return bind_custom_view_locked(
							context, signature.sourceSlot,
							scope, true, lock);
					}
                }
            }

			// The older compositor trace could activate a shared intermediate
			// and reproduce the black-screen bug. RenderDoc identified the
			// independent native screen SRVs, so only an explicit preview and
			// selection may activate routing now.
            return false;
        }

		if (g_phase != phase_t::active || !g_customView)
            return false;
		for (const auto& signature : g_routeSignatures)
		{
			if (!matches_signature(context, signature, call))
				continue;
			return bind_custom_view_locked(
				context, signature.sourceSlot, scope, false, lock);
		}
		return false;
    }

    void end_draw(ID3D11DeviceContext* context, route_scope_t& scope)
    {
        if (!scope.active) return;
        context->PSSetShaderResources(scope.slot, 1, &scope.nativeView);
        if (scope.nativeView) scope.nativeView->Release();
        const bool preview = scope.preview;
        scope = {};
        std::lock_guard<std::mutex> lock(g_mutex);
        if (preview)
            ++g_previewHits;
        else
            ++g_routedDraws;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request_hooks_locked(false);
        release_target_locked();
        set_phase_locked(phase_t::disabled);
    }
}
