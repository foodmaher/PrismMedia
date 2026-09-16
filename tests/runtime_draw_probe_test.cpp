#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <cassert>
#include <iostream>
#include "../PrismTextureStreamerFB/runtime_draw_probe.h"
#include "../PrismTextureStreamerFB/custom_render_probe.h"
#include "../PrismTextureStreamerFB/diagnostic_log.h"

// WARP exercises real D3D11 getters and COM ownership. Hook installation is
// stubbed here: interception and the game-specific branch still need ETS2.
bool hooks{};
namespace dx11::create_texture_2d {
    bool set_live_probe_hooks_enabled(bool value) { hooks=value; return true; }
}
namespace custom_render_probe {
    status_t status() { return {}; }
    fallback_mode_t fallback_mode() { return fallback_mode_t::forced_on; }
}
namespace diagnostic_log {
    void write(const char*,const char*) {}
    void writef(const char*,const char*,...) {}
}
ULONG references(IUnknown* p) { ULONG n=p->AddRef(); p->Release(); return n; }
int main()
{
    ID3D11Device* device{}; ID3D11DeviceContext* context{};
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=64; desc.Height=32; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* texture{}; ID3D11ShaderResourceView* srv{};
    assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)));
    assert(SUCCEEDED(device->CreateShaderResourceView(texture,nullptr,&srv)));
    context->PSSetShaderResources(6,1,&srv); // Bound BEFORE capture starts.
    D3D11_VIEWPORT viewport{0,0,64,32,0,1}; context->RSSetViewports(1,&viewport);
    auto beforeTexture=references(texture), beforeView=references(srv);
    assert(runtime_draw_probe::command("start bad 0").rfind("ERROR",0)==0 && !hooks);
    assert(runtime_draw_probe::command("start warp 5 slot=6 budget=2").rfind("OK",0)==0 && hooks);
    for(int i=0;i<3;++i) runtime_draw_probe::draw(context,"DrawIndexed",0,6,0,0);
    runtime_draw_probe::tick();
    assert(!hooks && !runtime_draw_probe::active());
    const auto row=runtime_draw_probe::command("row 0");
    assert(row.find("\"width\":64")!=std::string::npos);
    assert(row.find("\"height\":32")!=std::string::npos);
    assert(row.find("\"hits\":2")!=std::string::npos);
    assert(row.find("\"blendDesc\":null")!=std::string::npos);
    assert(references(texture)==beforeTexture && references(srv)==beforeView);
    assert(runtime_draw_probe::command("status").find("\"reason\":\"budget\"")!=std::string::npos);
    assert(runtime_draw_probe::command("row 1").rfind("ERROR",0)==0);
    assert(runtime_draw_probe::command("start filter 5 resource=0x1").rfind("OK",0)==0);
    runtime_draw_probe::draw(context,"Draw",0,3,0,0);
    runtime_draw_probe::command("stop");
    assert(runtime_draw_probe::command("status").find("\"rows\":0")!=std::string::npos);
    assert(runtime_draw_probe::command("start timer 1").rfind("OK",0)==0);
    Sleep(1100); runtime_draw_probe::tick();
    assert(!hooks && !runtime_draw_probe::active());
    runtime_draw_probe::shutdown();
    context->ClearState(); srv->Release(); texture->Release(); context->Release(); device->Release();
    std::cout<<row; // Workflow also validates the complete JSON with PowerShell.
}
