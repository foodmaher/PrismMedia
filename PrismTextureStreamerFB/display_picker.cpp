#define NOMINMAX
#include "display_picker.h"

#include "diagnostic_log.h"
#include "per_draw_router.h"
#include "screens.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    enum class stage_t
    {
        idle,
        discovering,
        click,
        probe_white,
        probe_black,
        confirming,
        fallback,
        complete,
        error
    };

    struct colour_t { float r{}, g{}, b{}; };

    std::mutex g_mutex;
    stage_t g_stage{ stage_t::idle };
    std::string g_displayId;
    std::string g_status;
    float g_clickX{ 0.5f };
    float g_clickY{ 0.5f };
    size_t g_candidateCount{};
    size_t g_probeCandidate{};
    size_t g_selectedCandidate{};
    size_t g_manualCandidate{};
    std::vector<float> g_scores;
    colour_t g_whiteSample;
    uint64_t g_stageTick{};
    ID3D11ShaderResourceView* g_whiteView{};
    ID3D11ShaderResourceView* g_blackView{};
    ID3D11ShaderResourceView* g_numberView{};

    void release_view(ID3D11ShaderResourceView*& view)
    {
        if (view) view->Release();
        view = nullptr;
    }

    bool create_solid_view(
        ID3D11Device* device,
        uint32_t bgra,
        ID3D11ShaderResourceView** result)
    {
        if (!device || !result) return false;
        *result = nullptr;
        constexpr UINT size = 16;
        std::vector<uint32_t> pixels(size * size, bgra);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = size;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = size * sizeof(uint32_t);
        ID3D11Texture2D* texture{};
        HRESULT hr = device->CreateTexture2D(
            &description, &data, &texture);
        if (FAILED(hr) || !texture) return false;
        hr = device->CreateShaderResourceView(texture, nullptr, result);
        texture->Release();
        return SUCCEEDED(hr) && *result;
    }

    const char* digit_rows(unsigned digit, unsigned row)
    {
        static const char* rows[10][7] = {
            { "11111", "10001", "10001", "10001", "10001", "10001", "11111" },
            { "00100", "01100", "00100", "00100", "00100", "00100", "01110" },
            { "11111", "00001", "00001", "11111", "10000", "10000", "11111" },
            { "11111", "00001", "00001", "11111", "00001", "00001", "11111" },
            { "10001", "10001", "10001", "11111", "00001", "00001", "00001" },
            { "11111", "10000", "10000", "11111", "00001", "00001", "11111" },
            { "11111", "10000", "10000", "11111", "10001", "10001", "11111" },
            { "11111", "00001", "00010", "00100", "01000", "01000", "01000" },
            { "11111", "10001", "10001", "11111", "10001", "10001", "11111" },
            { "11111", "10001", "10001", "11111", "00001", "00001", "11111" }
        };
        return rows[digit % 10][row % 7];
    }

    bool create_number_view(
        ID3D11Device* device,
        size_t candidate,
        ID3D11ShaderResourceView** result)
    {
        if (!device || !result) return false;
        *result = nullptr;
        constexpr UINT size = 512;
        const uint32_t background = 0xFFFF0060u;
        const uint32_t foreground = 0xFFFFFFFFu;
        std::vector<uint32_t> pixels(size * size, background);
        const unsigned number = static_cast<unsigned>((candidate + 1) % 10);
        constexpr UINT cell = 58;
        constexpr UINT left = (size - 5 * cell) / 2;
        constexpr UINT top = (size - 7 * cell) / 2;
        for (UINT row = 0; row < 7; ++row)
        {
            const char* pattern = digit_rows(number, row);
            for (UINT column = 0; column < 5; ++column)
            {
                if (pattern[column] != '1') continue;
                for (UINT y = 5; y + 5 < cell; ++y)
                    for (UINT x = 5; x + 5 < cell; ++x)
                        pixels[(top + row * cell + y) * size +
                            left + column * cell + x] = foreground;
            }
        }
        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = size;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = size * sizeof(uint32_t);
        ID3D11Texture2D* texture{};
        HRESULT hr = device->CreateTexture2D(
            &description, &data, &texture);
        if (FAILED(hr) || !texture) return false;
        hr = device->CreateShaderResourceView(texture, nullptr, result);
        texture->Release();
        return SUCCEEDED(hr) && *result;
    }

    bool ensure_probe_views(ID3D11Device* device)
    {
        if (!g_whiteView &&
            !create_solid_view(device, 0xFFFFFFFFu, &g_whiteView))
            return false;
        if (!g_blackView &&
            !create_solid_view(device, 0xFF000000u, &g_blackView))
            return false;
        return true;
    }

    void ensure_private_media_textures(
        ID3D11Device* device,
        ID3D11DeviceContext* context)
    {
        if (!device || !context) return;
        std::lock_guard<std::mutex> screensLock(g_screens_mutex);
        for (screen_t& screen : g_screens)
        {
            if (screen.type != screen_type_t::CUSTOM ||
                !screen.enabled || !screen.visualPickerMode ||
                !screen.source || screen.liveTexture)
                continue;

            D3D11_TEXTURE2D_DESC description{};
            description.Width = (std::max)(64U, screen.targetLiveTextureWidth);
            description.Height = (std::max)(64U, screen.targetLiveTextureHeight);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ID3D11Texture2D* texture{};
            const HRESULT result = device->CreateTexture2D(
                &description, nullptr, &texture);
            if (FAILED(result) || !texture)
            {
                diagnostic_log::writef(
                    "error",
                    "Visual picker could not create private media texture "
                    "for %s (HRESULT 0x%08X).",
                    screen.mediaClientId.c_str(),
                    static_cast<unsigned>(result));
                continue;
            }
            screen.liveTexture = texture;
            context->AddRef();
            screen.immediateContext = context;
            screen.liveTextureWidth = description.Width;
            screen.liveTextureHeight = description.Height;
            screen.pluginOwnedLiveTexture = true;
            screen.hasUploadedFrame = false;
            screen.lastTextureMatchTick = GetTickCount64();
            ++screen.textureRouteMatchedSequence;
            diagnostic_log::writef(
                "route",
                "Created private %ux%u visual-picker media texture for %s; "
                "no TOBJ redirect is required.",
                description.Width, description.Height,
                screen.mediaClientId.c_str());
        }
    }

    bool unpack_pixel(
        DXGI_FORMAT format,
        const uint8_t* bytes,
        colour_t& result)
    {
        if (format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            result = { bytes[0] / 255.0f, bytes[1] / 255.0f,
                bytes[2] / 255.0f };
            return true;
        }
        if (format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        {
            result = { bytes[2] / 255.0f, bytes[1] / 255.0f,
                bytes[0] / 255.0f };
            return true;
        }
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            const uint32_t value = *reinterpret_cast<const uint32_t*>(bytes);
            result = { (value & 1023u) / 1023.0f,
                ((value >> 10) & 1023u) / 1023.0f,
                ((value >> 20) & 1023u) / 1023.0f };
            return true;
        }
        return false;
    }

    bool sample_backbuffer(
        IDXGISwapChain* swapChain,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        float normalizedX,
        float normalizedY,
        colour_t& result)
    {
        if (!swapChain || !device || !context) return false;
        ID3D11Texture2D* backBuffer{};
        HRESULT hr = swapChain->GetBuffer(
            0, __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&backBuffer));
        if (FAILED(hr) || !backBuffer) return false;
        D3D11_TEXTURE2D_DESC source{};
        backBuffer->GetDesc(&source);
        constexpr UINT radius = 8;
        const UINT centerX = (std::min)(source.Width - 1,
            static_cast<UINT>((std::clamp)(normalizedX, 0.0f, 1.0f) *
                static_cast<float>(source.Width - 1)));
        const UINT centerY = (std::min)(source.Height - 1,
            static_cast<UINT>((std::clamp)(normalizedY, 0.0f, 1.0f) *
                static_cast<float>(source.Height - 1)));
        const UINT left = centerX > radius ? centerX - radius : 0;
        const UINT top = centerY > radius ? centerY - radius : 0;
        const UINT right = (std::min)(source.Width, centerX + radius + 1);
        const UINT bottom = (std::min)(source.Height, centerY + radius + 1);
        D3D11_TEXTURE2D_DESC staging = source;
        staging.Width = right - left;
        staging.Height = bottom - top;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.SampleDesc.Count = 1;
        staging.SampleDesc.Quality = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        ID3D11Texture2D* readback{};
        hr = device->CreateTexture2D(&staging, nullptr, &readback);
        if (FAILED(hr) || !readback)
        {
            backBuffer->Release();
            return false;
        }
        D3D11_BOX box{ left, top, 0, right, bottom, 1 };
        context->CopySubresourceRegion(readback, 0, 0, 0, 0,
            backBuffer, 0, &box);
        backBuffer->Release();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context->Map(readback, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            readback->Release();
            return false;
        }
        colour_t sum{};
        size_t samples{};
        const UINT bytesPerPixel = 4;
        for (UINT y = 0; y < staging.Height; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                static_cast<size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < staging.Width; ++x)
            {
                colour_t pixel;
                if (!unpack_pixel(source.Format,
                    row + static_cast<size_t>(x) * bytesPerPixel, pixel))
                {
                    context->Unmap(readback, 0);
                    readback->Release();
                    return false;
                }
                sum.r += pixel.r;
                sum.g += pixel.g;
                sum.b += pixel.b;
                ++samples;
            }
        }
        context->Unmap(readback, 0);
        readback->Release();
        if (!samples) return false;
        result = { sum.r / samples, sum.g / samples, sum.b / samples };
        return true;
    }

    float colour_distance(const colour_t& left, const colour_t& right)
    {
        const float r = left.r - right.r;
        const float g = left.g - right.g;
        const float b = left.b - right.b;
        return std::sqrt(r * r + g * g + b * b);
    }

    void enter_fallback_locked(const char* reason)
    {
        per_draw_router::picker_clear_probe();
        g_stage = stage_t::fallback;
        g_manualCandidate = 0;
        g_status = reason;
        g_stageTick = GetTickCount64();
    }

    void begin_number_preview_locked(ID3D11Device* device)
    {
        per_draw_router::picker_clear_probe();
        release_view(g_numberView);
        if (!create_number_view(device, g_manualCandidate, &g_numberView) ||
            !per_draw_router::picker_set_probe(
                g_manualCandidate, g_numberView))
        {
            g_status = "Could not show the numbered fallback. Keep the cockpit visible and retry.";
            return;
        }
        std::ostringstream text;
        text << "Truck screen " << (g_manualCandidate + 1)
            << " is marked with a large number.";
        g_status = text.str();
    }
}

namespace display_picker
{
    void start(const std::string& displayId)
    {
        // Do not nest the picker mutex around router operations. The picker is
        // started from the Present/ImGui path while the router also runs from
        // DrawIndexed, and overlay hook chains can re-enter either path.
        try
        {
            per_draw_router::picker_clear_probe();
            per_draw_router::retrain();
        }
        catch (const std::exception& error)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stage = stage_t::error;
            g_status = std::string(
                "Picker start was stopped safely after a synchronization "
                "error: ") + error.what();
            diagnostic_log::writef(
                "error", "Visual picker start was stopped safely: %s.",
                error.what());
            return;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stage = stage_t::error;
            g_status = "Picker start was stopped safely after an unknown plugin error.";
            diagnostic_log::write(
                "error", "Visual picker start was stopped after an unknown plugin error.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_displayId = displayId;
            g_candidateCount = 0;
            g_probeCandidate = 0;
            g_scores.clear();
            release_view(g_numberView);
            g_status = "Discovering independent truck-screen draws...";
            g_stage = stage_t::discovering;
            g_stageTick = GetTickCount64();
        }
        diagnostic_log::writef(
            "picker", "Visual screen picker started for %s.",
            displayId.c_str());
    }

    void cancel()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        per_draw_router::picker_clear_probe();
        g_stage = stage_t::idle;
        g_status.clear();
    }

    bool active()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_stage != stage_t::idle;
    }

    std::string status_text()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    void on_present(
        IDXGISwapChain* swapChain,
        ID3D11Device* device,
        ID3D11DeviceContext* context)
    {
        ensure_private_media_textures(device, context);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stage == stage_t::idle) return;
        if (!ensure_probe_views(device))
        {
            g_stage = stage_t::error;
            g_status = "The picker could not create its temporary GPU probes.";
            return;
        }

        if (g_stage == stage_t::discovering)
        {
            g_candidateCount = per_draw_router::picker_candidate_count();
            if (g_candidateCount > 0)
            {
                g_stage = stage_t::click;
                g_status = "Double-click inside the truck screen that should show this media.";
                g_stageTick = GetTickCount64();
            }
            else if (GetTickCount64() - g_stageTick > 15000)
            {
                g_stage = stage_t::error;
                g_status = "No independent GPS screen draws were found. Keep the cockpit and target screens visible, then retry.";
            }
            return;
        }

        if (g_stage == stage_t::fallback && !g_numberView &&
            g_candidateCount > 0)
        {
            begin_number_preview_locked(device);
            return;
        }

        if (g_stage != stage_t::probe_white &&
            g_stage != stage_t::probe_black)
            return;

        colour_t sample;
        if (!sample_backbuffer(
                swapChain, device, context,
                g_clickX, g_clickY, sample))
        {
            enter_fallback_locked(
                "Automatic pixel sampling is unavailable for this backbuffer format; use the numbered fallback.");
            begin_number_preview_locked(device);
            return;
        }

        if (g_stage == stage_t::probe_white)
        {
            g_whiteSample = sample;
            if (!per_draw_router::picker_set_probe(
                    g_probeCandidate, g_blackView))
            {
                enter_fallback_locked(
                    "The automatic probe lost its screen candidate; use the numbered fallback.");
                begin_number_preview_locked(device);
                return;
            }
            g_stage = stage_t::probe_black;
            return;
        }

        g_scores.push_back(colour_distance(g_whiteSample, sample));
        ++g_probeCandidate;
        if (g_probeCandidate < g_candidateCount)
        {
            if (!per_draw_router::picker_set_probe(
                    g_probeCandidate, g_whiteView))
            {
                enter_fallback_locked(
                    "The automatic probe lost its screen candidate; use the numbered fallback.");
                begin_number_preview_locked(device);
                return;
            }
            g_stage = stage_t::probe_white;
            return;
        }

        per_draw_router::picker_clear_probe();
        size_t best{};
        size_t second{};
        for (size_t index = 0; index < g_scores.size(); ++index)
        {
            if (g_scores[index] > g_scores[best])
            {
                second = best;
                best = index;
            }
            else if (index != best &&
                (second == best || g_scores[index] > g_scores[second]))
                second = index;
        }
        const float bestScore = g_scores.empty() ? 0.0f : g_scores[best];
        const float secondScore = g_scores.size() > 1 ? g_scores[second] : 0.0f;
        const bool confident = bestScore >= 0.10f &&
            (g_scores.size() == 1 ||
             bestScore >= secondScore * 1.35f + 0.025f);
        if (!confident)
        {
            std::ostringstream reason;
            reason << "The click response was ambiguous (best "
                << bestScore << ", next " << secondScore
                << "); no route was selected. Use the numbered fallback.";
            enter_fallback_locked(reason.str().c_str());
            begin_number_preview_locked(device);
            return;
        }

        g_selectedCandidate = best;
        std::ostringstream preview;
        preview << "preview " << best << " 3";
        const std::string response = per_draw_router::command(preview.str());
        if (response.rfind("OK", 0) != 0)
        {
            enter_fallback_locked(
                "The automatic match was found but its media preview could not start; use the numbered fallback.");
            begin_number_preview_locked(device);
            return;
        }
        g_stage = stage_t::confirming;
        g_stageTick = GetTickCount64();
        g_status = "Automatic match found. Confirm the screen showing Prism media during this 3-second preview.";
    }

    void draw_overlay()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_stage == stage_t::idle || !ImGui::GetCurrentContext()) return;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowBgAlpha(0.06f);
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav;
        ImGui::Begin("##prism_visual_picker", nullptr, flags);
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
        ImGui::InvisibleButton("##picker_surface", viewport->Size);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 panelMin(
            viewport->Pos.x + viewport->Size.x * 0.17f,
            viewport->Pos.y + 28.0f);
        const ImVec2 panelMax(
            viewport->Pos.x + viewport->Size.x * 0.83f,
            viewport->Pos.y + 132.0f);
        draw->AddRectFilled(panelMin, panelMax,
            IM_COL32(8, 18, 27, 238), 12.0f);
        draw->AddRect(panelMin, panelMax,
            IM_COL32(35, 211, 239, 255), 12.0f, 0, 2.0f);
        draw->AddText(ImVec2(panelMin.x + 20.0f, panelMin.y + 17.0f),
            IM_COL32(255, 255, 255, 255), "PrismMedia - Pick truck screen");
        draw->AddText(ImVec2(panelMin.x + 20.0f, panelMin.y + 49.0f),
            IM_COL32(185, 215, 230, 255), g_status.c_str());
        draw->AddText(ImVec2(panelMin.x + 20.0f, panelMin.y + 76.0f),
            IM_COL32(145, 164, 177, 255), "Escape cancels without changing any native screen.");

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            per_draw_router::picker_clear_probe();
            g_stage = stage_t::idle;
            ImGui::End();
            return;
        }

        if (g_stage == stage_t::click &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            g_clickX = viewport->Size.x > 0.0f
                ? (io.MousePos.x - viewport->Pos.x) / viewport->Size.x : 0.5f;
            g_clickY = viewport->Size.y > 0.0f
                ? (io.MousePos.y - viewport->Pos.y) / viewport->Size.y : 0.5f;
            g_probeCandidate = 0;
            g_scores.clear();
            if (per_draw_router::picker_set_probe(0, g_whiteView))
            {
                g_stage = stage_t::probe_white;
                g_status = "Testing each independent screen at the clicked point...";
            }
            else
            {
                g_stage = stage_t::fallback;
                g_status = "Automatic probing could not start. Use the numbered fallback.";
            }
        }

        if (g_stage == stage_t::confirming &&
            GetTickCount64() - g_stageTick >= 3000)
        {
            ImGui::SetCursorScreenPos(ImVec2(
                panelMin.x + 20.0f, panelMax.y + 18.0f));
            ImGui::BeginGroup();
            if (ImGui::Button("Use this screen", ImVec2(190.0f, 42.0f)))
            {
                std::ostringstream select;
                select << "select " << g_selectedCandidate;
                const std::string response = per_draw_router::command(select.str());
                g_status = response.rfind("OK", 0) == 0
                    ? "Screen selected and saved. Other truck screens remain native."
                    : "Selection could not be saved; use the fallback or retry.";
                g_stage = response.rfind("OK", 0) == 0
                    ? stage_t::complete : stage_t::fallback;
                g_stageTick = GetTickCount64();
            }
            ImGui::SameLine();
            if (ImGui::Button("Numbered fallback", ImVec2(190.0f, 42.0f)))
            {
                g_stage = stage_t::fallback;
                g_manualCandidate = 0;
                g_status = "Numbered fallback started.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Try click again", ImVec2(170.0f, 42.0f)))
            {
                per_draw_router::picker_clear_probe();
                g_stage = stage_t::click;
                g_status = "Double-click near the center of the desired truck screen.";
            }
            ImGui::EndGroup();
        }

        if (g_stage == stage_t::fallback)
        {
            ImGui::SetCursorScreenPos(ImVec2(
                panelMin.x + 20.0f, panelMax.y + 18.0f));
            if (!g_numberView)
            {
                // on_present creates the numbered texture on the next frame.
                g_status = "Preparing numbered screen 1...";
            }
            ImGui::BeginDisabled(g_manualCandidate == 0);
            if (ImGui::Button("Previous number", ImVec2(170.0f, 42.0f)))
            {
                --g_manualCandidate;
                release_view(g_numberView);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(g_manualCandidate + 1 >= g_candidateCount);
            if (ImGui::Button("Next number", ImVec2(170.0f, 42.0f)))
            {
                ++g_manualCandidate;
                release_view(g_numberView);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Use numbered screen", ImVec2(210.0f, 42.0f)))
            {
                per_draw_router::picker_clear_probe();
                std::ostringstream select;
                select << "select " << g_manualCandidate;
                const std::string response = per_draw_router::command(select.str());
                g_status = response.rfind("OK", 0) == 0
                    ? "Numbered screen selected and saved."
                    : "The numbered selection could not be saved.";
                g_stage = response.rfind("OK", 0) == 0
                    ? stage_t::complete : stage_t::error;
                g_stageTick = GetTickCount64();
            }
        }

        if (g_stage == stage_t::complete &&
            GetTickCount64() - g_stageTick >= 2200)
            g_stage = stage_t::idle;
        ImGui::End();
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        per_draw_router::picker_clear_probe();
        release_view(g_whiteView);
        release_view(g_blackView);
        release_view(g_numberView);
        g_stage = stage_t::idle;
    }
}
