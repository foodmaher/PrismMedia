#define NOMINMAX
#include "runtime_draw_probe.h"
#include "diagnostic_options.h"
#include "diagnostic_log.h"
#include "custom_render_probe.h"
#include "dx11/CreateTexture2D.h"
#include <Windows.h>
#include <d3d11.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>
#include <type_traits>
#include <unordered_map>

namespace
{
    using clock_type = std::chrono::steady_clock;
    template<class T> struct held {
        T* p{};
        ~held() { if (p) p->Release(); }
        T** put() { return &p; }
        uintptr_t id() const { return reinterpret_cast<uintptr_t>(p); }
    };
    template<class T, size_t N> struct held_array {
        T* p[N]{};
        ~held_array() { for (auto* v : p) if (v) v->Release(); }
    };
    struct texture_info {
        uintptr_t view{}, resource{}, resourceInterface{};
        unsigned dimension{}, width{}, height{}, format{}, viewFormat{}, viewDimension{}, bindFlags{}, samples{};
    };
    struct snapshot {
        uintptr_t context{}, caller{}, vs{}, ps{}, gs{}, hs{}, ds{}, layout{};
        uintptr_t ib{}, vb{}, indirect{}, blend{}, depth{}, raster{};
        uintptr_t samplers[16]{}, constants[14]{};
        uintptr_t srvs[128]{};
        unsigned contextType{}, kind{}, count{}, start{}, instances{}, firstInstance{};
        int base{};
        unsigned stride{}, vbOffset{}, ibOffset{}, ibFormat{}, topology{}, slot{};
        unsigned viewportCount{}, scissorCount{}, sampleMask{}, stencilRef{};
        float blendFactor[4]{};
        texture_info texture{}, targets[8]{}, depthTarget{};
        D3D11_BLEND_DESC blendDesc{};
        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        D3D11_RASTERIZER_DESC rasterDesc{};
        D3D11_VIEWPORT viewports[16]{};
        D3D11_RECT scissors[16]{};
    };
    struct row { snapshot state{}; uint64_t hits{}, firstUs{}, lastUs{}; };
    std::mutex g_control, g_data, g_start;
    std::atomic<bool> g_active{};
    bool g_hooks{};
    diagnostic_options::capture g_options;
    clock_type::time_point g_begin, g_end;
    std::vector<row> g_rows;
    std::unordered_multimap<uint64_t, size_t> g_index;
    std::atomic<uint64_t> g_seen{}, g_busy{};
    uint64_t g_inspected{}, g_matched{}, g_overflow{};
    std::string g_reason{"never-started"};
    uintptr_t g_gameBase{};
    std::string g_fallback;

    std::string hex(uintptr_t value) {
        std::ostringstream o; o << "\"0x" << std::hex << value << '"'; return o.str();
    }
    const char* kind_name(unsigned kind) {
        const char* names[] = {"DrawIndexed", "Draw", "DrawIndexedInstanced",
            "DrawInstanced", "DrawAuto", "DrawIndexedIndirect", "DrawInstancedIndirect"};
        return kind < 7 ? names[kind] : "unknown";
    }
    unsigned kind_id(const char* kind) {
        for (unsigned i = 0; i < 7; ++i) if (std::strcmp(kind_name(i), kind) == 0) return i;
        return 7;
    }
    template<class V> texture_info describe(V* view) {
        texture_info result{};
        std::memset(static_cast<void*>(&result),0,sizeof(result));
        if (!view) return result;
        result.view = reinterpret_cast<uintptr_t>(view);
        if constexpr (std::is_same_v<V, ID3D11ShaderResourceView>) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; view->GetDesc(&desc);
            result.viewFormat = desc.Format; result.viewDimension = desc.ViewDimension;
        } else if constexpr (std::is_same_v<V, ID3D11RenderTargetView>) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{}; view->GetDesc(&desc);
            result.viewFormat = desc.Format; result.viewDimension = desc.ViewDimension;
        } else {
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; view->GetDesc(&desc);
            result.viewFormat = desc.Format; result.viewDimension = desc.ViewDimension;
        }
        held<ID3D11Resource> resource;
        view->GetResource(resource.put());
        result.resource = resource.id();
        result.resourceInterface = resource.id();
        if (!resource.p) return result;
        held<IUnknown> identity;
        if (SUCCEEDED(resource.p->QueryInterface(__uuidof(IUnknown),
            reinterpret_cast<void**>(identity.put())))) result.resource = identity.id();
        D3D11_RESOURCE_DIMENSION dimension{};
        resource.p->GetType(&dimension);
        result.dimension = static_cast<unsigned>(dimension);
        if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            held<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.p->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(texture.put())))) {
                D3D11_TEXTURE2D_DESC desc{}; texture.p->GetDesc(&desc);
                result.width = desc.Width; result.height = desc.Height;
                result.format = desc.Format; result.bindFlags = desc.BindFlags;
                result.samples = desc.SampleDesc.Count;
            }
        }
        return result;
    }
    void texture_json(std::ostream& o, const texture_info& t) {
        o << "{\"view\":" << hex(t.view) << ",\"resource\":" << hex(t.resource)
          << ",\"resourceInterface\":" << hex(t.resourceInterface)
          << ",\"dimension\":" << t.dimension << ",\"width\":" << t.width
          << ",\"height\":" << t.height << ",\"format\":" << t.format
          << ",\"viewFormat\":" << t.viewFormat << ",\"viewDimension\":" << t.viewDimension
          << ",\"bindFlags\":" << t.bindFlags << ",\"samples\":" << t.samples << '}';
    }
    template<size_t N> void ids(std::ostream& o, const uintptr_t (&values)[N]) {
        o << '['; for (size_t i = 0; i < N; ++i) { if (i) o << ','; o << hex(values[i]); } o << ']';
    }
    std::string row_json(const row& r) {
        const auto& s = r.state;
        std::ostringstream o; o << std::setprecision(9);
        o << "{\"hits\":" << r.hits << ",\"firstUs\":" << r.firstUs << ",\"lastUs\":" << r.lastUs;
        o << ",\"context\":" << hex(s.context) << ",\"contextType\":" << s.contextType
          << ",\"caller\":" << hex(s.caller) << ",\"callerRva\":" << hex(s.caller >= g_gameBase ? s.caller - g_gameBase : 0)
          << ",\"kind\":\"" << kind_name(s.kind) << "\",\"count\":" << s.count
          << ",\"start\":" << s.start << ",\"base\":" << s.base
          << ",\"instances\":" << s.instances << ",\"firstInstance\":" << s.firstInstance
          << ",\"indirect\":" << hex(s.indirect);
        o << ",\"vs\":" << hex(s.vs) << ",\"ps\":" << hex(s.ps) << ",\"gs\":" << hex(s.gs)
          << ",\"hs\":" << hex(s.hs) << ",\"ds\":" << hex(s.ds) << ",\"layout\":" << hex(s.layout)
          << ",\"ib\":" << hex(s.ib) << ",\"vb0\":" << hex(s.vb)
          << ",\"stride\":" << s.stride << ",\"vbOffset\":" << s.vbOffset
          << ",\"ibOffset\":" << s.ibOffset << ",\"ibFormat\":" << s.ibFormat
          << ",\"topology\":" << s.topology << ",\"slot\":" << s.slot << ",\"texture\":";
        texture_json(o, s.texture);
        o << ",\"targets\":["; for (unsigned i=0;i<8;++i) { if(i) o<<','; texture_json(o,s.targets[i]); } o << ']';
        o << ",\"depthTarget\":"; texture_json(o,s.depthTarget);
        o << ",\"psSrvs\":"; ids(o,s.srvs);
        o << ",\"psSamplers\":"; ids(o,s.samplers);
        o << ",\"psConstantBuffers\":"; ids(o,s.constants);
        o << ",\"blend\":" << hex(s.blend) << ",\"depth\":" << hex(s.depth)
          << ",\"raster\":" << hex(s.raster) << ",\"sampleMask\":" << s.sampleMask
          << ",\"stencilRef\":" << s.stencilRef << ",\"blendFactor\":[";
        for(unsigned i=0;i<4;++i) { if(i)o<<','; o<<s.blendFactor[i]; } o<<']';
        o << ",\"blendDesc\":";
        if (!s.blend) o << "null";
        else {
        o << "{\"alphaToCoverage\":" << s.blendDesc.AlphaToCoverageEnable
          << ",\"independent\":" << s.blendDesc.IndependentBlendEnable << ",\"targets\":[";
        for(unsigned i=0;i<8;++i) { const auto& t=s.blendDesc.RenderTarget[i]; if(i)o<<',';
            o << "{\"enabled\":" << t.BlendEnable << ",\"src\":" << t.SrcBlend << ",\"dst\":" << t.DestBlend
              << ",\"op\":" << t.BlendOp << ",\"srcAlpha\":" << t.SrcBlendAlpha << ",\"dstAlpha\":" << t.DestBlendAlpha
              << ",\"opAlpha\":" << t.BlendOpAlpha << ",\"writeMask\":" << unsigned(t.RenderTargetWriteMask) << '}'; }
        o << "]}";
        }
        o << ",\"depthDesc\":";
        if (!s.depth) o << "null";
        else {
        o << "{\"enabled\":" << s.depthDesc.DepthEnable << ",\"writeMask\":" << s.depthDesc.DepthWriteMask
          << ",\"func\":" << s.depthDesc.DepthFunc << ",\"stencilEnabled\":" << s.depthDesc.StencilEnable
          << ",\"readMask\":" << unsigned(s.depthDesc.StencilReadMask) << ",\"writeStencilMask\":" << unsigned(s.depthDesc.StencilWriteMask);
        const auto& front=s.depthDesc.FrontFace; const auto& back=s.depthDesc.BackFace;
        o << ",\"front\":[" << front.StencilFailOp << ',' << front.StencilDepthFailOp << ',' << front.StencilPassOp << ',' << front.StencilFunc << ']'
          << ",\"back\":[" << back.StencilFailOp << ',' << back.StencilDepthFailOp << ',' << back.StencilPassOp << ',' << back.StencilFunc << "]}";
        }
        o << ",\"rasterDesc\":";
        if (!s.raster) o << "null";
        else {
        o << "{\"fill\":" << s.rasterDesc.FillMode << ",\"cull\":" << s.rasterDesc.CullMode
          << ",\"frontCCW\":" << s.rasterDesc.FrontCounterClockwise << ",\"depthClip\":" << s.rasterDesc.DepthClipEnable
          << ",\"scissor\":" << s.rasterDesc.ScissorEnable << ",\"multisample\":" << s.rasterDesc.MultisampleEnable
          << ",\"depthBias\":" << s.rasterDesc.DepthBias << ",\"depthBiasClamp\":" << s.rasterDesc.DepthBiasClamp
          << ",\"slopeBias\":" << s.rasterDesc.SlopeScaledDepthBias << ",\"antialiasedLine\":" << s.rasterDesc.AntialiasedLineEnable << '}';
        }
        o << ",\"viewports\":[";
        for(unsigned i=0;i<s.viewportCount;++i) { const auto& v=s.viewports[i]; if(i)o<<',';
            o<<'['<<v.TopLeftX<<','<<v.TopLeftY<<','<<v.Width<<','<<v.Height<<','<<v.MinDepth<<','<<v.MaxDepth<<']'; } o<<']';
        o << ",\"scissors\":[";
        for(unsigned i=0;i<s.scissorCount;++i) { const auto& v=s.scissors[i]; if(i)o<<',';
            o<<'['<<v.left<<','<<v.top<<','<<v.right<<','<<v.bottom<<']'; } o<<"]}";
        return o.str();
    }
    // g_control serializes hook installation/removal. Never hold g_data while
    // MinHook suspends threads: a render callback may be using that mutex.
    void stop_locked(const char* reason) {
        { std::lock_guard<std::mutex> lock(g_data);
          g_active.store(false, std::memory_order_release);
          if (g_reason == "running") g_reason = reason; }
        if (g_hooks) {
            if (!dx11::create_texture_2d::set_live_probe_hooks_enabled(false)) {
                diagnostic_log::write("error", "Live capture ended but hook cleanup failed; watchdog will retry.");
                return;
            }
            g_hooks = false;
            diagnostic_log::write("capture", "Live draw capture stopped; results available through capture status/row.");
        }
    }
    std::string status_locked() {
        std::ostringstream o;
        o << "{\"schema\":1,\"version\":\"4.0.0\",\"name\":\"" << g_options.name
          << "\",\"active\":" << (g_active.load() ? "true" : "false")
          << ",\"reason\":\"" << g_reason << "\",\"fallback\":\"" << g_fallback
          << "\",\"gameBase\":" << hex(g_gameBase) << ",\"seen\":" << g_seen.load()
          << ",\"busySkipped\":" << g_busy.load() << ",\"inspected\":" << g_inspected
          << ",\"matched\":" << g_matched << ",\"rows\":" << g_rows.size()
          << ",\"overflow\":" << g_overflow << ",\"slot\":" << g_options.slot
          << ",\"every\":" << g_options.every << ",\"budget\":" << g_options.budget
          << ",\"limit\":" << g_options.limit << ",\"seconds\":" << g_options.seconds << '}';
        return o.str();
    }
}

namespace runtime_draw_probe
{
    std::mutex& start_mutex() { return g_start; }
    bool active() { return g_active.load(std::memory_order_acquire); }
    void tick() {
        std::lock_guard<std::mutex> control(g_control);
        if (g_hooks && (!active() || clock_type::now() >= g_end)) stop_locked("deadline");
    }
    void shutdown() {
        std::lock_guard<std::mutex> control(g_control); stop_locked("aborted");
    }
    std::string command(const std::string& text) {
        std::lock_guard<std::mutex> control(g_control);
        std::istringstream input(text); std::string verb; input >> verb;
        if (verb == "stop" || verb == "abort") { stop_locked("stopped"); return "OK Capture stopped."; }
        if (verb == "start") {
            std::lock_guard<std::mutex> startLock(g_start);
            diagnostic_options::capture options; std::string error;
            if (!diagnostic_options::parse(input, options, error)) return "ERROR " + error;
            const auto legacy = custom_render_probe::status();
            if (active() || legacy.active || legacy.waitingForTexture)
                return "ERROR A capture is already active; stop/abort it first.";
            stop_locked("replaced");
            if (g_hooks) return "ERROR Previous hook cleanup has not completed.";
            {
                std::lock_guard<std::mutex> lock(g_data);
                g_options = options; g_rows.clear(); g_rows.reserve(options.limit);
                g_index.clear(); g_index.reserve(options.limit);
                g_seen = 0; g_busy = 0; g_inspected = g_matched = g_overflow = 0;
                g_gameBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                const auto mode = custom_render_probe::fallback_mode();
                g_fallback = mode == custom_render_probe::fallback_mode_t::forced_on ? "on" :
                    mode == custom_render_probe::fallback_mode_t::forced_off ? "off" : "auto";
                g_begin = clock_type::now(); g_end = g_begin + std::chrono::seconds(options.seconds);
                g_reason = "running";
            }
            // Publish only after enabling: no draw inspects partially initialized state.
            if (!dx11::create_texture_2d::set_live_probe_hooks_enabled(true)) {
                std::lock_guard<std::mutex> lock(g_data); g_reason = "hook-error";
                return "ERROR Could not enable live draw hooks.";
            }
            g_hooks = true; g_active.store(true, std::memory_order_release);
            diagnostic_log::writef("capture", "Live capture '%s' started (%us, slot=%u, every=%u). No reload requested.",
                options.name.c_str(), options.seconds, options.slot, options.every);
            return "OK Capture started. Use capture status, then capture row <index>.";
        }
        std::lock_guard<std::mutex> lock(g_data);
        if (verb == "status") return status_locked();
        if (verb == "row") {
            std::string value, extra; uint64_t index{};
            if (!(input >> value) || (input >> extra) || !diagnostic_options::number(value,index) || index >= g_rows.size())
                return "ERROR Row index outside stored results.";
            if (active()) return "ERROR Stop the capture before exporting rows.";
            return row_json(g_rows[static_cast<size_t>(index)]);
        }
        return "ERROR Use capture start/status/stop/row. Enter help for options.";
    }
    void draw(ID3D11DeviceContext* c, const char* kind, uintptr_t caller,
        unsigned count, unsigned start, int base, unsigned instances,
        unsigned firstInstance, uintptr_t indirectBuffer)
    {
        if (!active()) return;
        // No disk IO, hook mutation or game memory reads in the draw callback.
        std::unique_lock<std::mutex> lock(g_data, std::try_to_lock);
        if (!lock.owns_lock()) { g_busy.fetch_add(1); return; }
        if (!active()) return;
        const auto now = clock_type::now();
        if (now >= g_end) { g_active = false; g_reason = "deadline"; return; }
        const uint64_t ordinal = g_seen.fetch_add(1);
        if ((g_options.caller && caller != g_gameBase + g_options.caller) ||
            (g_options.count && count != g_options.count) || ordinal % g_options.every) return;
        if (g_inspected >= g_options.budget) { g_reason="budget"; g_active=false; return; }
        ++g_inspected;
        try {
            snapshot s{};
            std::memset(static_cast<void*>(&s),0,sizeof(s));
            s.context = reinterpret_cast<uintptr_t>(c); s.contextType = c->GetType();
            s.caller=caller; s.kind=kind_id(kind); s.count=count; s.start=start; s.base=base;
            s.instances=instances; s.firstInstance=firstInstance; s.indirect=indirectBuffer; s.slot=g_options.slot;
            held<ID3D11PixelShader> ps; c->PSGetShader(ps.put(), nullptr, nullptr); s.ps=ps.id();
            if (g_options.ps && s.ps != g_options.ps) return;
            held<ID3D11Buffer> ib; DXGI_FORMAT format{};
            c->IAGetIndexBuffer(ib.put(), &format, &s.ibOffset); s.ib=ib.id(); s.ibFormat=format;
            if (g_options.ib && s.ib != g_options.ib) return;
            held_array<ID3D11ShaderResourceView,128> views;
            c->PSGetShaderResources(0,128,views.p);
            for(unsigned i=0;i<128;++i) s.srvs[i]=reinterpret_cast<uintptr_t>(views.p[i]);
            s.texture=describe(views.p[s.slot]);
            if ((g_options.resource && s.texture.resource != g_options.resource) ||
                (g_options.width && s.texture.width != g_options.width) ||
                (g_options.height && s.texture.height != g_options.height)) return;
            held<ID3D11VertexShader> vs; c->VSGetShader(vs.put(),nullptr,nullptr); s.vs=vs.id();
            held<ID3D11GeometryShader> gs; c->GSGetShader(gs.put(),nullptr,nullptr); s.gs=gs.id();
            held<ID3D11HullShader> hs; c->HSGetShader(hs.put(),nullptr,nullptr); s.hs=hs.id();
            held<ID3D11DomainShader> ds; c->DSGetShader(ds.put(),nullptr,nullptr); s.ds=ds.id();
            held<ID3D11InputLayout> layout; c->IAGetInputLayout(layout.put()); s.layout=layout.id();
            held<ID3D11Buffer> vb; c->IAGetVertexBuffers(0,1,vb.put(),&s.stride,&s.vbOffset); s.vb=vb.id();
            D3D11_PRIMITIVE_TOPOLOGY topology{}; c->IAGetPrimitiveTopology(&topology); s.topology=topology;
            held<ID3D11BlendState> blend; c->OMGetBlendState(blend.put(),s.blendFactor,&s.sampleMask); s.blend=blend.id();
            if(blend.p) blend.p->GetDesc(&s.blendDesc);
            held<ID3D11DepthStencilState> depth; c->OMGetDepthStencilState(depth.put(),&s.stencilRef); s.depth=depth.id();
            if(depth.p) depth.p->GetDesc(&s.depthDesc);
            held<ID3D11RasterizerState> raster; c->RSGetState(raster.put()); s.raster=raster.id();
            if(raster.p) raster.p->GetDesc(&s.rasterDesc);
            s.viewportCount=16; c->RSGetViewports(&s.viewportCount,s.viewports);
            s.scissorCount=16; c->RSGetScissorRects(&s.scissorCount,s.scissors);
            held_array<ID3D11RenderTargetView,8> targets; held<ID3D11DepthStencilView> dsv;
            c->OMGetRenderTargets(8,targets.p,dsv.put());
            for(unsigned i=0;i<8;++i) s.targets[i]=describe(targets.p[i]);
            s.depthTarget=describe(dsv.p);
            held_array<ID3D11SamplerState,16> samplers; c->PSGetSamplers(0,16,samplers.p);
            for(unsigned i=0;i<16;++i) s.samplers[i]=reinterpret_cast<uintptr_t>(samplers.p[i]);
            held_array<ID3D11Buffer,14> constants; c->PSGetConstantBuffers(0,14,constants.p);
            for(unsigned i=0;i<14;++i) s.constants[i]=reinterpret_cast<uintptr_t>(constants.p[i]);
            ++g_matched;
            const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now-g_begin).count();
            uint64_t hash=14695981039346656037ULL;
            const auto* bytes=reinterpret_cast<const unsigned char*>(&s);
            for(size_t i=0;i<sizeof(s);++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
            const auto range=g_index.equal_range(hash);
            for(auto it=range.first;it!=range.second;++it) {
                auto& r=g_rows[it->second];
                if(std::memcmp(&r.state,&s,sizeof(s))==0) { ++r.hits; r.lastUs=us; return; }
            }
            if(g_rows.size() >= g_options.limit) { ++g_overflow; return; }
            g_rows.push_back({s,1,us,us});
            g_index.emplace(hash,g_rows.size()-1);
        } catch (...) {
            g_reason="capture-error"; g_active=false;
        }
    }
}
