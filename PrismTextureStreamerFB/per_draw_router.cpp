#define NOMINMAX
#include "per_draw_router.h"

#include "custom_render_probe.h"
#include "diagnostic_log.h"
#include "dx11/CreateTexture2D.h"
#include "screens.h"

#include <Windows.h>
#include <d3d11.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace
{
    constexpr UINT kTextureSlot = 6;
    constexpr uint64_t kTrainingTimeoutMs = 12000;

    struct draw_signature_t
    {
        UINT indexCount{};
        UINT startIndex{};
        INT baseVertex{};
        uintptr_t indexBuffer{};
        uintptr_t vertexBuffer{};
        UINT vertexStride{};
        UINT vertexOffset{};
        DXGI_FORMAT indexFormat{ DXGI_FORMAT_UNKNOWN };
        UINT indexOffset{};
        D3D11_PRIMITIVE_TOPOLOGY topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
        uintptr_t pixelShader{};
        bool valid{};
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
    ID3D11Texture2D* g_targetTexture{};
    IUnknown* g_targetIdentity{};
    ID3D11ShaderResourceView* g_customView{};
    draw_signature_t g_signature;
    struct candidate_t { draw_signature_t signature; uint32_t hits{}; };
    std::vector<candidate_t> g_candidates;
    bool g_hooksRequested{};
    bool g_hooksTraining{};
    bool g_forceRetrain{};
    std::atomic<int> g_fastPhase{
        static_cast<int>(per_draw_router::phase_t::waiting_texture) };
    std::atomic<UINT> g_fastCount{}, g_fastStart{};
    std::atomic<INT> g_fastBase{};

    thread_local bool t_slot6IsTarget{};

    void release_target_locked()
    {
        if (g_customView) g_customView->Release();
        if (g_targetIdentity) g_targetIdentity->Release();
        if (g_targetTexture) g_targetTexture->Release();
        g_customView = nullptr;
        g_targetIdentity = nullptr;
        g_targetTexture = nullptr;
        g_signature = {};
        g_candidates.clear();
        t_slot6IsTarget = false;
    }

    void set_phase_locked(per_draw_router::phase_t phase)
    {
        g_phase = phase;
        g_fastPhase.store(static_cast<int>(phase), std::memory_order_release);
    }

    void request_hooks_locked(bool enabled, bool training = false)
    {
        if (g_hooksRequested == enabled &&
            (!enabled || g_hooksTraining == training)) return;
        if (dx11::create_texture_2d::set_per_draw_router_hooks_enabled(
                enabled, enabled && training))
        {
            g_hooksRequested = enabled;
            g_hooksTraining = enabled && training;
        }
        else
            diagnostic_log::write("error", "Per-display router could not change its D3D hook lease.");
    }

    bool same_identity(ID3D11ShaderResourceView* view, IUnknown* target)
    {
        if (!view || !target) return false;
        ID3D11Resource* resource{};
        view->GetResource(&resource);
        if (!resource) return false;
        IUnknown* identity{};
        const bool ok = SUCCEEDED(resource->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&identity))) && identity;
        resource->Release();
        if (!ok) return false;
        const bool equal = identity == target;
        identity->Release();
        return equal;
    }

    draw_signature_t capture_signature(
        ID3D11DeviceContext* context, UINT count, UINT start, INT base)
    {
        draw_signature_t result{};
        result.indexCount = count;
        result.startIndex = start;
        result.baseVertex = base;

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
        result.valid = result.indexBuffer != 0 && result.vertexBuffer != 0;
        return result;
    }

    bool matches_signature(
        ID3D11DeviceContext* context, const draw_signature_t& expected,
        UINT count, UINT start, INT base)
    {
        if (!expected.valid || count != expected.indexCount ||
            start != expected.startIndex || base != expected.baseVertex)
            return false;
        const draw_signature_t current = capture_signature(context, count, start, base);
        return current.valid &&
            current.indexBuffer == expected.indexBuffer &&
            current.vertexBuffer == expected.vertexBuffer &&
            current.vertexStride == expected.vertexStride &&
            current.vertexOffset == expected.vertexOffset &&
            current.indexFormat == expected.indexFormat &&
            current.indexOffset == expected.indexOffset &&
            current.topology == expected.topology &&
            current.pixelShader == expected.pixelShader;
    }

    bool equal_signature(
        const draw_signature_t& left, const draw_signature_t& right)
    {
        return left.valid && right.valid &&
            left.indexCount == right.indexCount &&
            left.startIndex == right.startIndex &&
            left.baseVertex == right.baseVertex &&
            left.indexBuffer == right.indexBuffer &&
            left.vertexBuffer == right.vertexBuffer &&
            left.vertexStride == right.vertexStride &&
            left.vertexOffset == right.vertexOffset &&
            left.indexFormat == right.indexFormat &&
            left.indexOffset == right.indexOffset &&
            left.topology == right.topology &&
            left.pixelShader == right.pixelShader;
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
            g_detail = "Multiple custom displays require the compatibility fallback.";
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::automatic);
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
            ID3D11Device* device{};
            g_targetTexture->GetDevice(&device);
            const HRESULT viewResult = device
                ? device->CreateShaderResourceView(g_targetTexture, nullptr, &g_customView)
                : E_POINTER;
            if (device) device->Release();
            g_displayId = displayId;
            g_routeSequence = routeSequence;
            g_trainingStarted = GetTickCount64();
            g_signature = {};
            g_candidates.clear();
            t_slot6IsTarget = false;
            if (!g_targetIdentity || FAILED(viewResult) || !g_customView)
            {
                set_phase_locked(phase_t::fallback);
                g_detail = "Could not create the custom display SRV; compatibility fallback retained.";
                request_hooks_locked(false);
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::automatic);
                diagnostic_log::writef("error", "Per-display router setup failed for '%s' (HRESULT 0x%08X).",
                    g_displayId.c_str(), static_cast<unsigned>(viewResult));
            }
            else
            {
                set_phase_locked(phase_t::training);
                g_detail = "Learning the configured display draw.";
                custom_render_probe::set_fallback_mode(
                    custom_render_probe::fallback_mode_t::forced_on);
                request_hooks_locked(true, true);
                diagnostic_log::writef("route", "Per-display router training started for '%s'.",
                    g_displayId.c_str());
            }
        }
        else if (texture)
        {
            texture->Release();
        }

        if (g_phase == phase_t::training && g_signature.valid)
        {
            g_fastCount.store(g_signature.indexCount, std::memory_order_relaxed);
            g_fastStart.store(g_signature.startIndex, std::memory_order_relaxed);
            g_fastBase.store(g_signature.baseVertex, std::memory_order_relaxed);
            set_phase_locked(phase_t::active);
            g_detail = "Routing media only on the learned configured-display draw.";
            request_hooks_locked(true, false);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::forced_off);
            diagnostic_log::writef("route",
                "Per-display router active for '%s': DrawIndexed count=%u start=%u base=%d; global fallback disabled.",
                g_displayId.c_str(), g_signature.indexCount,
                g_signature.startIndex, g_signature.baseVertex);
        }
        else if (g_phase == phase_t::training &&
            GetTickCount64() - g_trainingStarted >= kTrainingTimeoutMs)
        {
            set_phase_locked(phase_t::fallback);
            g_detail = "Training timed out; compatibility fallback restored safely.";
            request_hooks_locked(false);
            custom_render_probe::set_fallback_mode(
                custom_render_probe::fallback_mode_t::automatic);
            diagnostic_log::write("error",
                "Per-display router could not observe the configured display within 12 seconds; compatibility fallback restored.");
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
        return { g_phase, g_enabled, g_displayId, g_trainedDraws,
            g_routedDraws, std::string(phase_name(g_phase)) + ": " + g_detail };
    }

    void notify_pixel_shader_resources(
        ID3D11DeviceContext*, UINT startSlot, UINT viewCount,
        ID3D11ShaderResourceView* const* views)
    {
        if (g_fastPhase.load(std::memory_order_acquire) !=
                static_cast<int>(phase_t::training) ||
            startSlot > kTextureSlot || startSlot + viewCount <= kTextureSlot)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_phase != phase_t::training || !g_targetIdentity)
            return;
        const UINT offset = kTextureSlot - startSlot;
        t_slot6IsTarget = views && offset < viewCount &&
            same_identity(views[offset], g_targetIdentity);
    }

    bool draw_indexed(
        ID3D11DeviceContext* context, UINT count, UINT start, INT base,
        draw_indexed_t original)
    {
        const int fastPhase = g_fastPhase.load(std::memory_order_acquire);
        if (fastPhase != static_cast<int>(phase_t::training) &&
            fastPhase != static_cast<int>(phase_t::active))
            return false;
        if (fastPhase == static_cast<int>(phase_t::active) &&
            (count != g_fastCount.load(std::memory_order_relaxed) ||
             start != g_fastStart.load(std::memory_order_relaxed) ||
             base != g_fastBase.load(std::memory_order_relaxed)))
            return false;

        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_phase == phase_t::training && t_slot6IsTarget)
        {
            const draw_signature_t candidate = capture_signature(context, count, start, base);
            if (candidate.valid)
            {
                auto found = g_candidates.end();
                for (auto it = g_candidates.begin(); it != g_candidates.end(); ++it)
                {
                    if (equal_signature(it->signature, candidate)) { found = it; break; }
                }
                if (found == g_candidates.end() && g_candidates.size() < 32)
                {
                    g_candidates.push_back({ candidate, 1 });
                    found = g_candidates.end() - 1;
                }
                else if (found != g_candidates.end())
                {
                    ++found->hits;
                }
                // A screen surface is a six-index quad. Requiring three
                // observations rejects one-off setup draws without delaying
                // the handoff by more than a few frames.
                if (found != g_candidates.end() && count == 6 && found->hits >= 3)
                {
                    g_signature = found->signature;
                    ++g_trainedDraws;
                    g_detail = "Draw learned; waiting for telemetry to disable the global fallback.";
                }
            }
            return false;
        }
        if (g_phase != phase_t::active || !g_customView ||
            !matches_signature(context, g_signature, count, start, base))
            return false;

        ID3D11ShaderResourceView* custom = g_customView;
        custom->AddRef();
        lock.unlock();

        ID3D11ShaderResourceView* nativeView{};
        context->PSGetShaderResources(kTextureSlot, 1, &nativeView);
        context->PSSetShaderResources(kTextureSlot, 1, &custom);
        original(context, count, start, base);
        context->PSSetShaderResources(kTextureSlot, 1, &nativeView);
        if (nativeView) nativeView->Release();
        custom->Release();
        lock.lock();
        ++g_routedDraws;
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        request_hooks_locked(false);
        release_target_locked();
        set_phase_locked(phase_t::disabled);
    }
}
