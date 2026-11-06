#pragma once

#include <string>

struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace display_picker
{
    // Starts an in-game visual selection for one custom Prism display.
    void start(const std::string& displayId);
    void cancel();
    bool active();
    std::string status_text();

    // Present calls this after the game has completed its frame and before
    // Prism draws ImGui. The picker therefore samples only the real cockpit.
    void on_present(
        IDXGISwapChain* swapChain,
        ID3D11Device* device,
        ID3D11DeviceContext* context);
    void draw_overlay();
    void shutdown();
}
