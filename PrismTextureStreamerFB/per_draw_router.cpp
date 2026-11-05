#define NOMINMAX
#include "per_draw_router.h"

#include "custom_render_probe.h"
#include "diagnostic_log.h"
#include "dx11/CreateTexture2D.h"
#include "screens.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
    constexpr UINT kTrackedSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    constexpr uint64_t kTrainingTimeoutMs = 12000;
    constexpr uint32_t kStableObservationCount = 3;
    constexpr size_t kMaximumIntermediates = 16;
    constexpr size_t kMaximumCandidates = 64;

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
    UINT g_targetWidth{};
    UINT g_targetHeight{};
    ID3D11Texture2D* g_targetTexture{};
    IUnknown* g_targetIdentity{};
    ID3D11ShaderResourceView* g_customView{};
    draw_signature_t g_signature;
    std::vector<intermediate_t> g_intermediates;
    std::vector<candidate_t> g_candidates;
    std::vector<uint64_t> g_observedOutputSizes;
    bool g_hooksRequested{};
    bool g_hooksTraining{};
    per_draw_router::draw_kind_t g_hookKind{ per_draw_router::draw_kind_t::none };
    bool g_forceRetrain{};
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
        if (g_customView) g_customView->Release();
        if (g_targetIdentity) g_targetIdentity->Release();
        if (g_targetTexture) g_targetTexture->Release();
        g_customView = nullptr;
        g_targetIdentity = nullptr;
        g_targetTexture = nullptr;
        g_targetWidth = 0;
        g_targetHeight = 0;
        g_signature = {};
        release_intermediates_locked();
        g_candidates.clear();
        g_observedOutputSizes.clear();
        g_directObservations = 0;
        g_derivedObservations = 0;
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
                }
            }
        }

        std::lock_guard<std::mutex> lock(g_mutex);
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
                g_detail = "Tracing the configured media through the UI compositor.";
                // Training no longer relies on the version-specific global
                // branch, so native GPS/smartphone displays remain untouched.
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_off);
                request_hooks_locked(true, true, draw_kind_t::none);
                diagnostic_log::writef(
                    "route",
                    "Per-display compositor tracing started for '%s' (%ux%u).",
                    g_displayId.c_str(), g_targetWidth, g_targetHeight);
            }
        }
        else if (texture)
        {
            texture->Release();
        }

        if (g_phase == phase_t::training && g_signature.valid)
        {
            g_fastKind.store(
                static_cast<unsigned>(g_signature.call.kind),
                std::memory_order_relaxed);
            g_fastCount.store(g_signature.call.count, std::memory_order_relaxed);
            g_fastStart.store(g_signature.call.start, std::memory_order_relaxed);
            g_fastBase.store(g_signature.call.base, std::memory_order_relaxed);
            set_phase_locked(phase_t::active);
            std::ostringstream detail;
            detail << "Routing only " << draw_kind_name(g_signature.call.kind)
                << " slot=" << g_signature.sourceSlot
                << " after one compositor hop.";
            g_detail = detail.str();
            request_hooks_locked(true, false, g_signature.call.kind);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            diagnostic_log::writef(
                "route",
                "Per-display router active for '%s': %s count=%u start=%u "
                "base=%d slot=%u; native displays retained.",
                g_displayId.c_str(), draw_kind_name(g_signature.call.kind),
                g_signature.call.count, g_signature.call.start,
                g_signature.call.base, g_signature.sourceSlot);
        }
        else if (g_phase == phase_t::training &&
            GetTickCount64() - g_trainingStarted >= kTrainingTimeoutMs)
        {
            set_phase_locked(phase_t::fallback);
            g_detail = "Compositor tracing timed out; original displays retained.";
            request_hooks_locked(false);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            diagnostic_log::write(
                "error",
                "Per-display router found no stable one-hop compositor route "
                "within 12 seconds; original displays were retained.");
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
                << " candidates=" << g_candidates.size();
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
            ensure_thread_bindings(context);
            UINT sourceSlot{};
            uintptr_t sourceIdentity{};
            unsigned hop{};
            if (!find_source_locked(sourceSlot, sourceIdentity, hop))
                return false;
            if (hop == 0)
            {
                ++g_directObservations;
                remember_matching_outputs_locked(context);
            }
            else
            {
                ++g_derivedObservations;
                observe_candidate_locked(
                    context, call, sourceSlot, sourceIdentity);
            }
            return false;
        }

        if (g_phase != phase_t::active || !g_customView ||
            !matches_signature(context, g_signature, call))
            return false;

        ID3D11ShaderResourceView* custom = g_customView;
        custom->AddRef();
        const UINT slot = g_signature.sourceSlot;
        lock.unlock();

        ID3D11ShaderResourceView* nativeView{};
        context->PSGetShaderResources(slot, 1, &nativeView);
        context->PSSetShaderResources(slot, 1, &custom);
        custom->Release();
        scope.slot = slot;
        scope.nativeView = nativeView;
        scope.active = true;
        return true;
    }

    void end_draw(ID3D11DeviceContext* context, route_scope_t& scope)
    {
        if (!scope.active) return;
        context->PSSetShaderResources(scope.slot, 1, &scope.nativeView);
        if (scope.nativeView) scope.nativeView->Release();
        scope = {};
        std::lock_guard<std::mutex> lock(g_mutex);
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
