#define NOMINMAX
#include "menu.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <map>
#include <string>
#include <vector>

#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_dx11.h>
#include <ImGui/imgui_impl_win32.h>

#include "../dinput8/dinput8.h"
#include "../dx11/present.h"
#include "../environment_audio.h"
#include "../hotkeys.h"
#include "../misc/imgui_stdlib.h"
#include "../prism/execute_command.h"
#include "../prism/prism.h"
#include "../screens.h"
#include "../settings.h"
#include "../sources/media_client.h"
#include "../sources/native_media.h"
#include "../sources/window.h"
#include "../sources/wgc_window.h"
#include "../telemetry_state.h"
#include "../thread_scheduling.h"
#include "../update_checker.h"
#include "../version.h"

namespace
{
    std::atomic<bool> menuVisible{};
    bool savePending{};
    uint64_t lastChangeTick{};
    bool criticalReloadPending{};
    int hotkeyBindingIndex = -1;
    int selectedScreen{};
    int selectedPage{ 1 };
    float panelAnimation = 1.0f;
    int restoreRequest = -1;
    bool saveNowRequest{};
    std::string restoreStatus;

    struct inspector_t
    {
        const char* title = "Explore any option";
        const char* effect = "Hover a control to see what changes in-game.";
        const char* impact = "No performance impact";
        const char* apply = "Auto-saved";
    } inspector;

    constexpr ImU32 kPanelRaised = IM_COL32(24, 39, 53, 245);
    constexpr ImU32 kBorder = IM_COL32(72, 93, 109, 165);
    constexpr ImU32 kCyan = IM_COL32(28, 215, 245, 255);
    constexpr ImU32 kBlue = IM_COL32(31, 145, 255, 255);
    constexpr ImU32 kGreen = IM_COL32(103, 235, 91, 255);
    constexpr ImU32 kMuted = IM_COL32(159, 174, 190, 255);

    ImFont* ui_font(int index)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if (atlas && index >= 0 && index < atlas->Fonts.Size && atlas->Fonts[index])
            return atlas->Fonts[index];
        return ImGui::GetFont();
    }

    void text_with_font(int fontIndex, const ImVec4& colour, const char* text)
    {
        ImGui::PushFont(ui_font(fontIndex));
        ImGui::TextColored(colour, "%s", text);
        ImGui::PopFont();
    }

    bool slider_percent(const char* label, float& normalized,
        float minimumPercent = 0.0f, float maximumPercent = 100.0f)
    {
        float percent = normalized * 100.0f;
        if (!ImGui::SliderFloat(label, &percent, minimumPercent,
            maximumPercent, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            return false;
        normalized = percent / 100.0f;
        return true;
    }

    bool toggle_switch(const char* label, bool& value)
    {
        ImGui::PushID(label);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 36.0f);
        const bool pressed = ImGui::InvisibleButton("##toggle", size);
        if (pressed) value = !value;
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ui_font(0), 16.0f, ImVec2(start.x, start.y + 8.0f),
            hovered ? IM_COL32_WHITE : IM_COL32(222, 231, 238, 255), label);
        const ImVec2 trackMin(start.x + size.x - 54.0f, start.y + 5.0f);
        const ImVec2 trackMax(start.x + size.x, start.y + 31.0f);
        draw->AddRectFilled(trackMin, trackMax,
            value ? IM_COL32(10, 160, 205, 255) : IM_COL32(59, 73, 86, 255), 13.0f);
        if (value)
            draw->AddRect(trackMin, trackMax, IM_COL32(31, 224, 248, 180), 13.0f, 0, 2.0f);
        const float knobX = value ? trackMax.x - 13.0f : trackMin.x + 13.0f;
        draw->AddCircleFilled(ImVec2(knobX, start.y + 18.0f), 10.0f, IM_COL32(242, 247, 250, 255));
        ImGui::PopID();
        return pressed;
    }

    void draw_card_shadow(ImDrawList* draw, const ImVec2& minimum,
        const ImVec2& maximum, float rounding = 14.0f)
    {
        // ImGui has no blur pass. Layered translucent outlines provide the
        // soft card elevation from the approved mock-up at negligible cost.
        for (int layer = 8; layer >= 1; --layer)
        {
            const float spread = static_cast<float>(layer) * 1.25f;
            const int alpha = 5 + (9 - layer) * 2;
            draw->AddRect(
                ImVec2(minimum.x - spread, minimum.y - spread + 5.0f),
                ImVec2(maximum.x + spread, maximum.y + spread + 5.0f),
                IM_COL32(0, 0, 0, alpha), rounding + spread, 0, 2.0f);
        }
    }

    void draw_nav_icon(ImDrawList* draw, int icon, const ImVec2& c, ImU32 colour)
    {
        if (icon == 0)
        {
            draw->AddTriangleFilled(ImVec2(c.x - 13.0f, c.y - 1.0f),
                ImVec2(c.x, c.y - 13.0f), ImVec2(c.x + 13.0f, c.y - 1.0f), colour);
            draw->AddRect(ImVec2(c.x - 10.0f, c.y - 1.0f), ImVec2(c.x + 10.0f, c.y + 12.0f), colour, 1.0f, 0, 2.0f);
            draw->AddRectFilled(ImVec2(c.x - 3.0f, c.y + 4.0f), ImVec2(c.x + 3.0f, c.y + 12.0f), colour);
        }
        else if (icon == 1)
        {
            draw->AddRect(ImVec2(c.x - 11.0f, c.y - 9.0f), ImVec2(c.x + 11.0f, c.y + 9.0f), colour, 3.0f, 0, 2.0f);
            draw->AddTriangleFilled(ImVec2(c.x - 3.0f, c.y - 5.0f), ImVec2(c.x + 6.0f, c.y), ImVec2(c.x - 3.0f, c.y + 5.0f), colour);
        }
        else if (icon == 2)
        {
            const ImVec2 speaker[] = { ImVec2(c.x - 11.0f, c.y - 5.0f), ImVec2(c.x - 5.0f, c.y - 5.0f), ImVec2(c.x + 2.0f, c.y - 11.0f), ImVec2(c.x + 2.0f, c.y + 11.0f), ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x - 11.0f, c.y + 5.0f) };
            draw->AddConvexPolyFilled(speaker, 6, colour);
            draw->AddBezierCubic(ImVec2(c.x + 6.0f, c.y - 7.0f), ImVec2(c.x + 11.0f, c.y - 4.0f), ImVec2(c.x + 11.0f, c.y + 4.0f), ImVec2(c.x + 6.0f, c.y + 7.0f), colour, 2.0f);
        }
        else if (icon == 3)
        {
            draw->AddRect(ImVec2(c.x - 13.0f, c.y - 8.0f), ImVec2(c.x + 13.0f, c.y + 8.0f), colour, 7.0f, 0, 2.0f);
            draw->AddLine(ImVec2(c.x - 8.0f, c.y), ImVec2(c.x - 2.0f, c.y), colour, 2.0f);
            draw->AddLine(ImVec2(c.x - 5.0f, c.y - 3.0f), ImVec2(c.x - 5.0f, c.y + 3.0f), colour, 2.0f);
            draw->AddCircleFilled(ImVec2(c.x + 6.0f, c.y - 2.5f), 2.0f, colour);
            draw->AddCircleFilled(ImVec2(c.x + 9.0f, c.y + 2.5f), 2.0f, colour);
        }
        else if (icon == 4)
        {
            draw->AddLine(ImVec2(c.x - 9.0f, c.y + 9.0f), ImVec2(c.x + 7.0f, c.y - 7.0f), colour, 4.0f);
            draw->AddCircleFilled(ImVec2(c.x + 9.0f, c.y - 9.0f), 4.0f, colour);
            draw->AddCircle(ImVec2(c.x - 10.0f, c.y + 10.0f), 4.0f, colour, 16, 2.0f);
        }
        else
        {
            draw->AddCircle(c, 8.0f, colour, 24, 2.0f);
            draw->AddCircleFilled(c, 3.0f, colour);
            for (int i = 0; i < 8; ++i)
            {
                const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
                draw->AddLine(ImVec2(c.x + std::cos(angle) * 10.0f, c.y + std::sin(angle) * 10.0f),
                    ImVec2(c.x + std::cos(angle) * 14.0f, c.y + std::sin(angle) * 14.0f), colour, 2.0f);
            }
        }
    }

    bool nav_button(int icon, const char* label, bool selected)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 62.0f);
        ImGui::InvisibleButton(label, size);
        const bool pressed = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (selected || hovered)
            draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
                selected ? IM_COL32(29, 48, 64, 250) : IM_COL32(25, 41, 55, 210), 10.0f);
        if (selected)
        {
            draw->AddRect(ImVec2(start.x - 2.0f, start.y - 2.0f),
                ImVec2(start.x + size.x + 2.0f, start.y + size.y + 2.0f),
                IM_COL32(24, 216, 245, 55), 12.0f, 0, 4.0f);
            draw->AddRectFilled(start, ImVec2(start.x + 4.0f, start.y + size.y), kCyan, 3.0f);
            draw->AddRect(start, ImVec2(start.x + size.x, start.y + size.y), IM_COL32(45, 207, 235, 100), 10.0f);
        }
        const ImVec2 badgeMin(start.x + 18.0f, start.y + 14.0f);
        const ImVec2 badgeMax(start.x + 54.0f, start.y + 50.0f);
        draw->AddRectFilled(badgeMin, badgeMax,
            selected ? IM_COL32(18, 176, 209, 230) : IM_COL32(39, 52, 66, 230), 8.0f);
        draw_nav_icon(draw, icon, ImVec2(start.x + 36.0f, start.y + 32.0f),
            selected ? IM_COL32_WHITE : kMuted);
        draw->AddText(ui_font(1), 19.0f, ImVec2(start.x + 70.0f, start.y + 21.0f),
            selected ? kCyan : IM_COL32(230, 237, 243, 255), label);
        return pressed;
    }

    void draw_status_icon(ImDrawList* draw, int icon, const ImVec2& c, ImU32 colour)
    {
        if (icon == 0)
        {
            draw->AddRectFilled(ImVec2(c.x - 14.0f, c.y - 9.0f),
                ImVec2(c.x + 14.0f, c.y + 9.0f), IM_COL32(235, 35, 42, 255), 5.0f);
            draw->AddTriangleFilled(ImVec2(c.x - 3.0f, c.y - 5.0f),
                ImVec2(c.x + 7.0f, c.y), ImVec2(c.x - 3.0f, c.y + 5.0f), IM_COL32_WHITE);
        }
        else if (icon == 1)
        {
            draw->AddCircleFilled(c, 15.0f, IM_COL32(35, 215, 96, 255));
            for (int wave = 0; wave < 3; ++wave)
            {
                const float y = c.y - 5.0f + wave * 5.0f;
                draw->AddBezierCubic(ImVec2(c.x - 9.0f, y), ImVec2(c.x - 3.0f, y - 2.0f),
                    ImVec2(c.x + 4.0f, y), ImVec2(c.x + 9.0f, y + 2.0f),
                    IM_COL32(4, 31, 19, 255), 1.8f);
            }
        }
        else
        {
            draw->AddCircleFilled(ImVec2(c.x - 5.0f, c.y + 2.0f), 8.0f, colour);
            draw->AddCircleFilled(ImVec2(c.x + 4.0f, c.y - 3.0f), 10.0f, colour);
            draw->AddCircleFilled(ImVec2(c.x + 12.0f, c.y + 3.0f), 7.0f, colour);
            draw->AddRectFilled(ImVec2(c.x - 12.0f, c.y + 2.0f), ImVec2(c.x + 17.0f, c.y + 10.0f), colour, 4.0f);
        }
    }

    void status_card(int icon, const char* title, const char* state, ImU32 accent, float width)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, 58.0f);
        ImGui::Dummy(size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw_card_shadow(draw, start, ImVec2(start.x + size.x, start.y + size.y), 10.0f);
        draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y), kPanelRaised, 10.0f);
        draw->AddRect(start, ImVec2(start.x + size.x, start.y + size.y), kBorder, 10.0f);
        draw_status_icon(draw, icon, ImVec2(start.x + 24.0f, start.y + 29.0f), accent);
        draw->AddText(ui_font(0), 16.0f, ImVec2(start.x + 48.0f, start.y + 10.0f), IM_COL32_WHITE, title);
        draw->AddText(ui_font(0), 16.0f, ImVec2(start.x + 48.0f, start.y + 31.0f), accent, state);
    }

    void mark_changed(bool critical = false)
    {
        savePending = true;
        lastChangeTick = GetTickCount64();
        criticalReloadPending = criticalReloadPending || critical;
    }

    void explain_last_item(const char* title, const char* effect,
        const char* impact = "Negligible",
        const char* apply = "Applies live and auto-saves")
    {
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
            inspector = { title, effect, impact, apply };
    }

    float effective_brightness(const screen_t& screen)
    {
        float value = (std::clamp)(screen.brightness, 0.10f, 2.0f);
        if (screen.autoBrightnessEnabled && g_game_lighting_valid.load())
        {
            float scene = (std::clamp)(
                (g_game_lighting_luminance.load() - 0.06f) / 0.54f,
                0.0f, 1.0f);
            scene = scene * scene * (3.0f - 2.0f * scene);
            value *= screen.autoBrightnessDarkMultiplier +
                (screen.autoBrightnessBrightMultiplier -
                    screen.autoBrightnessDarkMultiplier) * scene;
        }
        return (std::clamp)(value, 0.05f, 2.0f);
    }

    void apply_brightness(screen_t& screen)
    {
        screen.effectiveBrightness = effective_brightness(screen);
        if (screen.source && screen.source->SupportsSourceBrightness())
            screen.source->SetSourceBrightness(screen.effectiveBrightness);
        screen.hasUploadedFrame = false;
    }

    void reset_source_stats(screen_t& screen)
    {
        screen.frameScratch.clear();
        screen.frameScratchWidth = 0;
        screen.frameScratchHeight = 0;
        screen.hasUploadedFrame = false;
        screen.uploadCpuMs = screen.totalPluginCpuMs = 0.0;
        screen.estimatedFpsLoss = screen.deliveredFps = 0.0;
        screen.uploadedFrames = screen.lastUploadTick = 0;
        screen.sourceCreatedTick = screen.lastSourceFrameTick = 0;
        screen.suspiciousMagentaFrame = screen.sourceFrameStale = false;
        screen.consecutiveMapFailures = 0;
    }

    bool rebuild_source(screen_t& screen)
    {
        g_screen_source_creation_in_progress = true;
        screen.source.reset();
        reset_source_stats(screen);
        if (screen.contentMode == content_mode_t::WINDOW_CAPTURE &&
            !screen.source_application_name.empty())
        {
            screen.source = screen.legacyCapture
                ? sources::CreateWindowSource(
                    screen.source_application_name.c_str(),
                    screen.source_application_display_name.empty() ? nullptr : screen.source_application_display_name.c_str(),
                    screen.framerate, screen.targetLiveTextureWidth, screen.targetLiveTextureHeight)
                : sources::CreateWgcWindowSource(
                    screen.source_application_name.c_str(),
                    screen.source_application_display_name.empty() ? nullptr : screen.source_application_display_name.c_str(),
                    screen.framerate, screen.targetLiveTextureWidth, screen.targetLiveTextureHeight);
        }
        else if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA && !screen.mediaUrl.empty())
        {
            screen.source = sources::CreateMediaClientSource(
                screen.mediaUrl, screen.framerate,
                screen.targetLiveTextureWidth, screen.targetLiveTextureHeight,
                screen.mediaService == media_service_t::SPOTIFY);
        }
        else if (screen.contentMode == content_mode_t::NATIVE_DIRECT_MEDIA && !screen.mediaUrl.empty())
        {
            screen.source = sources::CreateNativeMediaSource(
                screen.mediaUrl, screen.framerate,
                screen.targetLiveTextureWidth, screen.targetLiveTextureHeight);
        }
        if (screen.source)
        {
            screen.sourceCreatedTick = GetTickCount64();
            screen.source->SetPaused(screen.paused);
            apply_brightness(screen);
        }
        g_screen_source_creation_in_progress = false;
        return screen.source != nullptr;
    }

    void apply_profile(screen_t& screen)
    {
        switch (screen.performanceProfile)
        {
        case performance_profile_t::ECONOMY: screen.targetLiveTextureWidth = 854; screen.targetLiveTextureHeight = 480; screen.framerate = 20; break;
        case performance_profile_t::BALANCED: screen.targetLiveTextureWidth = 1280; screen.targetLiveTextureHeight = 720; screen.framerate = 30; break;
        case performance_profile_t::QUALITY: screen.targetLiveTextureWidth = 1920; screen.targetLiveTextureHeight = 1080; screen.framerate = 30; break;
        case performance_profile_t::SMOOTH: screen.targetLiveTextureWidth = 1280; screen.targetLiveTextureHeight = 720; screen.framerate = 60; break;
        case performance_profile_t::CUSTOM: break;
        }
        if (screen.source)
        {
            screen.source->SetFramerate(screen.framerate);
            screen.source->SetOutputSize(screen.targetLiveTextureWidth, screen.targetLiveTextureHeight);
        }
    }

    void set_visible(bool visible)
    {
        menuVisible = visible;
        if (!visible) { hotkeyBindingIndex = -1; g_is_binding_hotkey = false; }
        dinput8::set_mouse(visible);
        if (!ImGui::GetCurrentContext()) return;
        auto& io = ImGui::GetIO();
        if (visible)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            io.MouseDrawCursor = true;
        }
        else
        {
            io.MouseDrawCursor = false;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            ::SetCursor(nullptr);
        }
    }

    bool capture_hotkey(hotkey_binding_t& binding)
    {
        if ((GetAsyncKeyState(VK_ESCAPE) & 1) != 0) return true;
        if ((GetAsyncKeyState(VK_BACK) & 1) != 0 || (GetAsyncKeyState(VK_DELETE) & 1) != 0)
        { binding = {}; return true; }
        for (UINT key = 7; key < 256; ++key)
        {
            if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
                key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
                key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) continue;
            if ((GetAsyncKeyState(static_cast<int>(key)) & 1) == 0) continue;
            binding.virtualKey = key;
            binding.control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            binding.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            binding.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            return true;
        }
        return false;
    }

    bool edit_gamepad_binding(const char* label, int id, gamepad_binding_t& binding)
    {
        bool changed = false;
        ImGui::PushID(id);
        ImGui::TextUnformatted(label); ImGui::SameLine(145.0f);
        ImGui::SetNextItemWidth(75.0f);
        if (ImGui::BeginCombo("##modifier", gamepad_modifier_name(binding.modifier)))
        {
            for (int i = 0; i < static_cast<int>(gamepad_modifier_t::COUNT); ++i)
            {
                const auto value = static_cast<gamepad_modifier_t>(i);
                if (ImGui::Selectable(gamepad_modifier_name(value), value == binding.modifier))
                { binding.modifier = value; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##input", gamepad_input_name(binding.input)))
        {
            for (int i = 0; i < static_cast<int>(gamepad_input_t::COUNT); ++i)
            {
                const auto value = static_cast<gamepad_input_t>(i);
                if (ImGui::Selectable(gamepad_input_name(value), value == binding.input))
                { binding.input = value; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return changed;
    }

    std::map<std::string, std::string> enumerate_windows()
    {
        std::map<std::string, std::string> result;
        EnumWindows([](HWND window, LPARAM parameter) -> BOOL
        {
            if (!IsWindowVisible(window) || (GetWindowLongA(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
            char title[512]{}; GetWindowTextA(window, title, sizeof(title));
            DWORD pid{}; GetWindowThreadProcessId(window, &pid);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return TRUE;
            char path[MAX_PATH]{}; DWORD size = MAX_PATH;
            QueryFullProcessImageNameA(process, 0, path, &size); CloseHandle(process);
            std::string executable(path);
            const size_t slash = executable.find_last_of("\\/");
            if (slash != std::string::npos) executable.erase(0, slash + 1);
            if (executable.empty()) return TRUE;
            auto* apps = reinterpret_cast<std::map<std::string, std::string>*>(parameter);
            (*apps)[title[0] ? title : executable] = executable;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    }

    void draw_wave_preview(const screen_t& screen)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 224.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        draw->AddRectFilled(origin, end, IM_COL32(7, 17, 29, 248), 14.0f);
        draw->AddRect(origin, end, IM_COL32(27, 202, 237, 185), 14.0f, 0, 2.0f);
        draw->AddRectFilledMultiColor(
            ImVec2(origin.x + 1.0f, origin.y + 1.0f), ImVec2(end.x - 1.0f, end.y - 1.0f),
            IM_COL32(18, 92, 153, 60), IM_COL32(10, 120, 95, 45),
            IM_COL32(8, 20, 33, 0), IM_COL32(8, 20, 33, 0));
        const float time = static_cast<float>(ImGui::GetTime());
        const float gain = screen.adaptiveAudioEnabled
            ? (std::clamp)(screen.adaptiveAudioInteriorVolume, 0.15f, 1.0f) : 1.0f;
        const float midY = origin.y + 129.0f;
        const float leftStart = origin.x + 24.0f;
        const float leftEnd = origin.x + size.x * 0.43f;
        const float rightStart = origin.x + size.x * 0.57f;
        const float rightEnd = end.x - 24.0f;
        for (int i = 0; i < 78; ++i)
        {
            const float x = static_cast<float>(i) / 77.0f;
            const float activity = 0.42f + 0.30f * std::sin(x * 39.0f + time * 2.1f) +
                0.18f * std::sin(x * 91.0f - time * 1.3f);
            const float leftFade = 1.0f - 0.72f * x;
            const float rightFade = 0.28f + 0.72f * x;
            const float leftAmp = (9.0f + 27.0f * std::abs(activity)) * leftFade * gain;
            const float rightAmp = (8.0f + 24.0f * std::abs(activity)) * rightFade *
                (std::clamp)(screen.adaptiveAudioOutsideVolume + 0.35f, 0.35f, 1.0f);
            const float lx = leftStart + x * (leftEnd - leftStart);
            const float rx = rightStart + x * (rightEnd - rightStart);
            draw->AddLine(ImVec2(lx, midY - leftAmp), ImVec2(lx, midY + leftAmp),
                IM_COL32(30, 148, 255, 230), 2.0f);
            draw->AddLine(ImVec2(rx, midY - rightAmp), ImVec2(rx, midY + rightAmp),
                IM_COL32(42, 216, 172, 225), 2.0f);
        }
        const float bridgeLeft = origin.x + size.x * 0.41f;
        const float bridgeRight = origin.x + size.x * 0.59f;
        draw->AddBezierCubic(ImVec2(bridgeLeft, midY - 7.0f),
            ImVec2(origin.x + size.x * 0.47f, midY - 7.0f),
            ImVec2(origin.x + size.x * 0.53f, midY + 20.0f),
            ImVec2(bridgeRight, midY + 20.0f), kBlue, 3.0f);
        draw->AddBezierCubic(ImVec2(bridgeLeft, midY + 20.0f),
            ImVec2(origin.x + size.x * 0.47f, midY + 20.0f),
            ImVec2(origin.x + size.x * 0.53f, midY - 7.0f),
            ImVec2(bridgeRight, midY - 7.0f), IM_COL32(42, 216, 172, 255), 3.0f);
        draw->AddLine(ImVec2(origin.x + size.x * 0.5f, origin.y + 83.0f),
            ImVec2(origin.x + size.x * 0.5f, origin.y + 181.0f), IM_COL32(191, 211, 225, 115), 1.0f);
        draw->AddText(ui_font(2), 24.0f, ImVec2(origin.x + 22.0f, origin.y + 18.0f),
            IM_COL32(237, 247, 252, 255), "SMOOTH CABIN / OUTSIDE TRANSITION");
        draw->AddText(ui_font(0), 16.0f, ImVec2(origin.x + 24.0f, origin.y + 57.0f),
            kMuted, "Live preview of the crossfade and tone change");
        draw->AddText(ui_font(0), 16.0f, ImVec2(leftStart, origin.y + 91.0f), kBlue, "CABIN");
        const char* fadeLabel = "650 ms  FADE";
        const ImVec2 fadeSize = ui_font(0)->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, fadeLabel);
        draw->AddText(ui_font(0), 16.0f, ImVec2(origin.x + size.x * 0.5f - fadeSize.x * 0.5f, origin.y + 91.0f), kCyan, fadeLabel);
        const char* outsideLabel = "OUTSIDE";
        const ImVec2 outsideSize = ui_font(0)->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, outsideLabel);
        draw->AddText(ui_font(0), 16.0f, ImVec2(rightEnd - outsideSize.x, origin.y + 91.0f),
            IM_COL32(42, 216, 172, 255), outsideLabel);
        draw->AddText(ui_font(0), 16.0f, ImVec2(origin.x + 24.0f, origin.y + 192.0f),
            IM_COL32(151, 174, 190, 255), "No abrupt volume or low-pass jumps");
        draw->AddText(ui_font(0), 16.0f, ImVec2(end.x - 192.0f, origin.y + 192.0f),
            kGreen, "APPLIES INSTANTLY");
        ImGui::Dummy(size);
    }

    void draw_home(screen_t& screen)
    {
        const auto stats = screen.source ? screen.source->GetPerformanceStats() : source_performance_stats_t{};
        const char* screenKind = screen.type == screen_type_t::GPS ? "GPS" :
            (screen.type == screen_type_t::DASHBOARD ? "Dashboard" : "Custom");
        if (ImGui::BeginTable("home_cards", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::BeginChild("home_source", ImVec2(0.0f, 142.0f), true);
            text_with_font(1, ImVec4(0.12f, 0.86f, 0.98f, 1.0f), "ACTIVE DISPLAY");
            ImGui::PushFont(ui_font(2)); ImGui::Text("%s %d", screenKind, selectedScreen + 1); ImGui::PopFont();
            ImGui::TextWrapped("%s", screen.source ? screen.source->GetStatusText().c_str() : "Waiting for source");
            ImGui::TextDisabled("%ux%u @ %u FPS", screen.targetLiveTextureWidth,
                screen.targetLiveTextureHeight, static_cast<unsigned>(screen.framerate));
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginChild("home_performance", ImVec2(0.0f, 142.0f), true);
            text_with_font(1, ImVec4(0.42f, 0.94f, 0.47f, 1.0f), "LIVE PERFORMANCE");
            ImGui::PushFont(ui_font(2)); ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate); ImGui::PopFont();
            ImGui::Text("Upload %.3f ms | worker %.3f ms", screen.uploadCpuMs, stats.workerCpuMs);
            ImGui::TextDisabled("Delivered %.1f FPS | dropped %llu", stats.deliveredFps > 0.0 ? stats.deliveredFps : screen.deliveredFps,
                static_cast<unsigned long long>(stats.droppedFrames));
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginChild("home_audio", ImVec2(0.0f, 142.0f), true);
            text_with_font(1, ImVec4(0.12f, 0.86f, 0.98f, 1.0f), "ADAPTIVE AUDIO");
            ImGui::PushFont(ui_font(2)); ImGui::TextUnformatted(screen.adaptiveAudioEnabled ? "Enabled" : "Disabled"); ImGui::PopFont();
            ImGui::Text("Cabin %.0f%% | outside %.0f%%", screen.adaptiveAudioInteriorVolume * 100.0f,
                screen.adaptiveAudioOutsideVolume * 100.0f);
            ImGui::TextDisabled("Environment output %.0f%%", g_environment_media_gain.load() * 100.0f);
            ImGui::EndChild();

            ImGui::TableNextColumn();
            ImGui::BeginChild("home_display", ImVec2(0.0f, 142.0f), true);
            text_with_font(1, ImVec4(0.12f, 0.86f, 0.98f, 1.0f), "DISPLAY");
            ImGui::PushFont(ui_font(2)); ImGui::Text("%.0f%% brightness", screen.effectiveBrightness * 100.0f); ImGui::PopFont();
            ImGui::Text("Automatic adjustment: %s", screen.autoBrightnessEnabled ? "On" : "Off");
            ImGui::TextDisabled("Engine: %s", g_engine_enabled.load() ? "running" : "off");
            ImGui::EndChild();
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (ImGui::Button("Open media", ImVec2(150.0f, 42.0f))) { selectedPage = 1; panelAnimation = 0.0f; }
        ImGui::SameLine(); if (ImGui::Button("Tune audio", ImVec2(150.0f, 42.0f))) { selectedPage = 2; panelAnimation = 0.0f; }
        ImGui::SameLine(); if (ImGui::Button("Live performance", ImVec2(180.0f, 42.0f))) { selectedPage = 5; panelAnimation = 0.0f; }
    }

    bool audio_feature_card(const char* id, const char* title, int icon, bool selected, float width)
    {
        ImGui::PushID(id);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, 126.0f);
        ImGui::InvisibleButton("##audio_feature", size);
        const bool clicked = ImGui::IsItemClicked();
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw_card_shadow(draw, start, ImVec2(start.x + size.x, start.y + size.y), 12.0f);
        draw->AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y),
            selected ? IM_COL32(17, 41, 60, 255) : IM_COL32(17, 29, 41, 245), 12.0f);
        draw->AddRect(start, ImVec2(start.x + size.x, start.y + size.y),
            selected ? kCyan : (hovered ? IM_COL32(90, 164, 188, 210) : kBorder), 12.0f, 0, selected ? 2.5f : 1.0f);
        const ImVec2 centre(start.x + size.x * 0.5f, start.y + 44.0f);
        if (icon == 0) draw_nav_icon(draw, 2, centre, selected ? kCyan : IM_COL32_WHITE);
        else if (icon == 1)
        {
            draw->AddRectFilled(ImVec2(centre.x - 23.0f, centre.y - 10.0f), ImVec2(centre.x - 2.0f, centre.y + 10.0f), IM_COL32_WHITE, 3.0f);
            draw->AddCircle(ImVec2(centre.x - 13.0f, centre.y + 11.0f), 5.0f, selected ? kCyan : IM_COL32_WHITE, 16, 2.0f);
            draw->AddTriangleFilled(ImVec2(centre.x + 5.0f, centre.y + 10.0f), ImVec2(centre.x + 18.0f, centre.y - 14.0f), ImVec2(centre.x + 31.0f, centre.y + 10.0f), selected ? kCyan : IM_COL32_WHITE);
        }
        else
        {
            for (int bar = 0; bar < 5; ++bar)
            {
                const float height = 14.0f + static_cast<float>((bar * 9) % 25);
                draw->AddRectFilled(ImVec2(centre.x - 30.0f + bar * 15.0f, centre.y - height * 0.5f),
                    ImVec2(centre.x - 22.0f + bar * 15.0f, centre.y + height * 0.5f),
                    bar == 2 ? kCyan : IM_COL32(213, 225, 235, 255), 2.0f);
            }
        }
        const ImVec2 titleSize = ui_font(1)->CalcTextSizeA(19.0f, FLT_MAX, 0.0f, title);
        draw->AddText(ui_font(1), 19.0f, ImVec2(start.x + (size.x - titleSize.x) * 0.5f, start.y + 91.0f),
            selected ? kCyan : IM_COL32(235, 241, 246, 255), title);
        ImGui::PopID();
        return clicked;
    }

    void draw_audio_feature_cards()
    {
        const float gap = 12.0f;
        const float width = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
        if (audio_feature_card("volume", "VOLUME & BALANCE", 0, false, width))
            inspector = { "Volume & balance", "Controls cabin, menu and exterior media levels.", "Very low CPU", "Applies live" };
        ImGui::SameLine(0.0f, gap);
        if (audio_feature_card("transition", "CABIN / OUTSIDE", 1, true, width))
            inspector = { "Cabin / outside", "Smoothly blends volume and tone between camera environments.", "Very low CPU", "Applies live" };
        ImGui::SameLine(0.0f, gap);
        if (audio_feature_card("filters", "FILTERS", 2, false, width))
            inspector = { "Filters", "Controls distance-based low-pass muffling outside the truck.", "Very low CPU", "Applies live" };
    }

    void draw_media(screen_t& screen)
    {
        text_with_font(2, ImVec4(0.93f, 0.97f, 0.99f, 1.0f), "MEDIA SOURCE");
        ImGui::TextDisabled("One live source. YouTube playlist URLs still work; no saved playlist library.");
        ImGui::Dummy(ImVec2(0.0f, 4.0f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0.0f, 4.0f));
        const char* modes[] = { "Window capture", "Integrated YouTube / Spotify", "Native direct media" };
        int mode = static_cast<int>(screen.contentMode);
        ImGui::TextDisabled("PLAYBACK METHOD");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("##playback_method", &mode, modes, IM_ARRAYSIZE(modes)))
        { screen.contentMode = static_cast<content_mode_t>(mode); rebuild_source(screen); mark_changed(); }
        explain_last_item("Playback method", "Chooses how frames reach the truck display. Integrated media is recommended for web services.", "Integrated: low/medium; Native: lowest; Window: medium/high");
        if (toggle_switch("Pause / Freeze", screen.paused))
        {
            if (screen.source) screen.source->SetPaused(screen.paused);
            mark_changed();
        }
        explain_last_item("Pause / Freeze", "Keeps the last image and stops plugin frame processing.", "Reduces resource use", "Applies instantly");

        if (screen.contentMode == content_mode_t::WINDOW_CAPTURE)
        {
            static std::map<std::string, std::string> applications;
            if (applications.empty()) applications = enumerate_windows();
            if (ImGui::Button("Refresh window list")) applications = enumerate_windows();
            explain_last_item("Refresh windows", "Scans visible desktop windows only when requested.");
            const char* preview = screen.source_application_display_name.empty() ? "Choose a visible window" : screen.source_application_display_name.c_str();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("Source window", preview))
            {
                for (const auto& entry : applications)
                    if (ImGui::Selectable(entry.first.c_str()))
                    { screen.source_application_display_name = entry.first; screen.source_application_name = entry.second; rebuild_source(screen); mark_changed(); }
                ImGui::EndCombo();
            }
            explain_last_item("Source window", "Switches capture immediately without reloading the game session.", "Medium/high");
            if (toggle_switch("Compatibility capture", screen.legacyCapture))
            { rebuild_source(screen); mark_changed(); }
            explain_last_item("Compatibility capture", "Uses the older capture path only when Windows Graphics Capture fails.", "Higher CPU/GPU use");
        }
        else
        {
            if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA)
            {
                int service = static_cast<int>(screen.mediaService);
                const char* services[] = { "YouTube", "Spotify Web" };
                ImGui::TextDisabled("SERVICE");
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("##media_service", &service, services, IM_ARRAYSIZE(services)))
                { screen.mediaService = static_cast<media_service_t>(service); rebuild_source(screen); mark_changed(); }
                explain_last_item("Media service", "YouTube uses the focused player; Spotify always uses the official full web player.", "Low/medium");
                if (!sources::IsMediaClientInstalled())
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "PrismMediaClient.exe is missing.");
            }
            ImGui::TextDisabled("MEDIA URL OR LOCAL FILE");
            ImGui::SetNextItemWidth(-108.0f);
            const bool enter = ImGui::InputText("##media_url", &screen.mediaUrl, ImGuiInputTextFlags_EnterReturnsTrue);
            explain_last_item("Media URL", "Paste one video, playlist, Spotify page, local file, or direct stream. Press Enter to load live.", "No game reload", "Loads live, then auto-saves");
            ImGui::SameLine();
            const bool load = ImGui::Button("Load now", ImVec2(98.0f, 0.0f));
            explain_last_item("Load now", "Reuses the helper whenever possible. The game session remains active.", "No game reload");
            if (enter || load)
            {
                bool loaded = screen.source && screen.source->SupportsMediaControls() &&
                    screen.contentMode == content_mode_t::INTEGRATED_MEDIA && screen.source->LoadMedia(screen.mediaUrl);
                if (!loaded) loaded = rebuild_source(screen);
                mark_changed();
                if (!loaded && !screen.mediaUrl.empty()) ImGui::OpenPopup("Source unavailable");
            }
            if (screen.source && screen.source->SupportsMediaControls())
            {
                ImGui::Text("Status: %s", screen.source->GetStatusText().c_str());
                if (ImGui::Button("Play / Pause", ImVec2(124.0f, 0.0f))) dispatch_media_command(screen, media_command_t::PLAY_PAUSE);
                explain_last_item("Play / Pause", "Uses a state-neutral transport control, so a stale icon cannot claim the wrong state.");
                ImGui::SameLine(); if (ImGui::Button("Previous")) dispatch_media_command(screen, media_command_t::PREVIOUS);
                ImGui::SameLine(); if (ImGui::Button("Next")) dispatch_media_command(screen, media_command_t::NEXT);
                ImGui::SameLine(); if (ImGui::Button("Mute")) dispatch_media_command(screen, media_command_t::MUTE);
                ImGui::SameLine(); if (ImGui::Button("Vol -")) dispatch_media_command(screen, media_command_t::VOLUME_DOWN);
                ImGui::SameLine(); if (ImGui::Button("Vol +")) dispatch_media_command(screen, media_command_t::VOLUME_UP);

                bool hotkeyTarget = screen.hotkeyTarget;
                if (toggle_switch("Use this screen for media hotkeys", hotkeyTarget))
                {
                    if (hotkeyTarget)
                        for (auto& other : g_screens) other.hotkeyTarget = false;
                    screen.hotkeyTarget = hotkeyTarget;
                    mark_changed();
                }
            }
            if (screen.mediaService == media_service_t::SPOTIFY && screen.contentMode == content_mode_t::INTEGRATED_MEDIA && screen.source)
            {
                if (ImGui::Button("Open Spotify sign-in")) screen.source->ShowInteractivePlayer(true);
                explain_last_item("Spotify sign-in", "Temporarily shows the official Spotify Web page for authentication.", "No game reload");
                ImGui::SameLine(); if (ImGui::Button("Hide browser")) screen.source->ShowInteractivePlayer(false);
                ImGui::SameLine(); if (ImGui::Button("Clear Spotify session")) screen.source->ClearBrowserSession();
            }
        }
        const bool supportsVehiclePower = screen.source && screen.source->SupportsVehiclePowerControl();
        ImGui::BeginDisabled(!supportsVehiclePower);
        if (toggle_switch("Follow truck engine", screen.followTruckEngine))
        {
            if (screen.source)
            {
                const bool powered = !screen.followTruckEngine || !g_telemetry_driving.load() || g_engine_enabled.load();
                screen.source->SetVehiclePowered(powered);
            }
            apply_brightness(screen);
            mark_changed();
        }
        explain_last_item("Follow truck engine", "Immediately pauses media and shows the standby truck graphic when the engine turns off.");
        if (screen.followTruckEngine && slider_percent("Engine-off display", screen.engineOffBrightness, 5.0f, 100.0f))
        { apply_brightness(screen); mark_changed(); }
        ImGui::EndDisabled();
        if (!supportsVehiclePower)
            ImGui::TextDisabled("Engine-follow playback is available for integrated and native media.");
        else if (!g_telemetry_driving.load())
            ImGui::TextDisabled("Engine control is inactive in menus / before driving.");
        else
            ImGui::TextColored(g_engine_enabled.load() ? ImVec4(0.42f, 0.94f, 0.47f, 1.0f) : ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Truck engine: %s | media power: %s", g_engine_enabled.load() ? "running" : "off",
                (!screen.followTruckEngine || g_engine_enabled.load()) ? "on" : "paused");
        if (ImGui::BeginPopupModal("Source unavailable", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        { ImGui::TextWrapped("The source could not start. Check the URL, runtime files, and plugin log."); if (ImGui::Button("OK", ImVec2(110, 0))) ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
    }

    void draw_audio(screen_t& screen)
    {
        draw_audio_feature_cards();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        draw_wave_preview(screen);
        const bool spatialSupported = screen.source && screen.source->SupportsSpatialAudio();
        if (!spatialSupported)
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Adaptive sound is available with the Integrated Media Client.");
        ImGui::BeginDisabled(!spatialSupported);
        if (toggle_switch("Adaptive cabin audio", screen.adaptiveAudioEnabled))
        {
            if (!screen.adaptiveAudioEnabled && screen.source)
                screen.source->SetSpatialAudio(1.0f, 0.0f, false);
            mark_changed();
        }
        explain_last_item("Adaptive cabin audio", "Blends gain, stereo position and muffling as the camera moves inside or outside.", "Very low CPU", "Applies live with a 650 ms fade");
        ImGui::BeginDisabled(!screen.adaptiveAudioEnabled);
        if (slider_percent("Cabin volume", screen.adaptiveAudioInteriorVolume)) mark_changed();
        explain_last_item("Cabin volume", "Full-volume anchor when the camera is inside the truck.");
        if (slider_percent("Spatial strength", screen.adaptiveAudioStrength)) mark_changed();
        if (ImGui::SliderFloat("Speaker direction", &screen.adaptiveAudioSpeakerAzimuth, -90.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        if (slider_percent("Facing-away floor", screen.adaptiveAudioFacingAwayVolume)) mark_changed();
        if (ImGui::SliderFloat("Outside-cab distance", &screen.adaptiveAudioOutsideDistance, 0.25f, 2.5f, "%.2f m", ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        if (slider_percent("Minimum volume when far away", screen.adaptiveAudioOutsideVolume)) mark_changed();
        explain_last_item("Exterior volume", "Target volume outside before distance attenuation.", "Very low CPU", "Smooth 650 ms crossfade");
        if (slider_percent("Menu volume", screen.adaptiveAudioMenuVolume)) mark_changed();
        if (toggle_switch("Exterior distance filter", screen.adaptiveAudioExternalDistanceEnabled)) mark_changed();
        ImGui::BeginDisabled(!screen.adaptiveAudioExternalDistanceEnabled);
        if (slider_percent("Near exterior volume", screen.adaptiveAudioExternalNearVolume)) mark_changed();
        if (ImGui::SliderFloat("Near cutoff", &screen.adaptiveAudioExternalNearCutoff, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        if (ImGui::SliderFloat("Full-volume distance", &screen.adaptiveAudioExternalFullVolumeDistance, 0.0f, 10.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        if (ImGui::SliderFloat("Mute distance", &screen.adaptiveAudioExternalMuteDistance, 2.0f, 50.0f, "%.1f m", ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        if (toggle_switch("Smooth low-pass", screen.adaptiveAudioExternalLowPassEnabled)) mark_changed();
        if (ImGui::SliderFloat("Minimum cutoff", &screen.adaptiveAudioExternalMinimumCutoff, 20.0f,
            (std::max)(20.0f, screen.adaptiveAudioExternalNearCutoff), "%.0f Hz",
            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) mark_changed();
        ImGui::EndDisabled(); ImGui::EndDisabled();
        if (spatialSupported && screen.adaptiveAudioEnabled)
        {
            const bool driving = g_telemetry_driving.load();
            const uint64_t now = GetTickCount64();
            const uint64_t lastHeadUpdate = g_last_head_update_tick.load();
            const bool headFresh = driving && lastHeadUpdate != 0 && now >= lastHeadUpdate && now - lastHeadUpdate <= 500;
            const bool exactCamera = g_camera_bridge_connected.load();
            const bool externalCamera = driving && (exactCamera
                ? g_camera_type.load() != 2
                : (!g_camera_interior_hint.load() || !headFresh));
            if (!driving)
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                    "Detected state: menu / paused (%.0f%% volume)", screen.adaptiveAudioMenuVolume * 100.0f);
            else if (externalCamera)
            {
                ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Detected state: external camera");
                if (exactCamera && g_head_anchor_calibrated.load())
                    ImGui::Text("Distance %.1f m | gain %.0f%% | low-pass %.0f Hz",
                        g_external_camera_distance.load(), g_adaptive_audio_distance_gain.load() * 100.0f,
                        g_adaptive_audio_lowpass_hz.load());
                else if (exactCamera)
                    ImGui::TextDisabled("Head anchor not calibrated: switch to camera 1 once.");
            }
            else
                ImGui::TextColored(ImVec4(0.42f, 0.94f, 0.47f, 1.0f),
                    "Detected state: interior camera");
        }
        ImGui::Separator();
        std::lock_guard<std::mutex> lock(g_environment_audio_settings_mutex);
        if (toggle_switch("Protect game sounds", g_environment_audio_settings.enabled)) mark_changed();
        explain_last_item("Protect game sounds", "Gently ducks media when road and engine activity increase; it adds no external audio.", "Very low CPU");
        if (slider_percent("Interior reduction", g_environment_audio_settings.interiorEffect)) mark_changed();
        if (slider_percent("Exterior reduction", g_environment_audio_settings.exteriorEffect)) mark_changed();
        ImGui::Text("Live: %.1f km/h | road contact %.0f%% | environment %.0f%%",
            std::fabs(g_truck_speed_mps.load()) * 3.6f,
            g_environment_grounded_ratio.load() * 100.0f,
            g_environment_intensity.load() * 100.0f);
        ImGui::Text("Mode: %s | resulting media volume %.0f%%",
            !g_telemetry_driving.load() ? "menus / before driving" :
                (g_environment_interior.load() ? "interior" : "exterior"),
            g_environment_media_gain.load() * 100.0f);
        ImGui::TextDisabled("Estimator cost: %.1f us/update | capped at 20 Hz",
            g_environment_update_cpu_us.load());
    }

    void draw_controls()
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "MOUSE + GAMEPAD");
        ImGui::TextWrapped("Every card is keyboard/gamepad navigable. D-pad or stick moves; A activates.");
        if (toggle_switch("Enable gamepad controls", g_gamepad_hotkeys_enabled)) mark_changed();
        const char* controllers[] = { "Automatic", "Controller 1", "Controller 2", "Controller 3", "Controller 4" };
        int controller = g_gamepad_controller_index + 1;
        if (ImGui::Combo("Controller", &controller, controllers, IM_ARRAYSIZE(controllers))) { g_gamepad_controller_index = controller - 1; mark_changed(); }
        if (ImGui::SliderFloat("Stick threshold", &g_gamepad_axis_threshold, 0.20f, 0.95f, "%.2f")) mark_changed();
        if (edit_gamepad_binding("Plugin menu", 9000, g_gamepad_menu_hotkey)) mark_changed();
        ImGui::Separator(); ImGui::TextUnformatted("Media bindings");
        for (int i = 0; i < static_cast<int>(g_media_gamepad_hotkeys.size()); ++i)
            if (edit_gamepad_binding(media_command_name(static_cast<media_command_t>(i)), 9100 + i, g_media_gamepad_hotkeys[i])) mark_changed();
        ImGui::Separator(); ImGui::TextUnformatted("Keyboard bindings");
        for (int i = 0; i < static_cast<int>(g_media_hotkeys.size()); ++i)
        {
            ImGui::PushID(9200 + i); ImGui::TextUnformatted(media_command_name(static_cast<media_command_t>(i))); ImGui::SameLine(145.0f);
            const std::string label = hotkeyBindingIndex == i ? "Press a key...##key" : hotkey_name(g_media_hotkeys[i]) + "##key";
            if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f))) { hotkeyBindingIndex = i; g_is_binding_hotkey = true; }
            if (hotkeyBindingIndex == i && capture_hotkey(g_media_hotkeys[i])) { hotkeyBindingIndex = -1; g_is_binding_hotkey = false; mark_changed(); }
            ImGui::PopID();
        }
    }

    void draw_appearance(screen_t& screen)
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "DISPLAY CHARACTER");
        if (slider_percent("Screen brightness", screen.brightness, 10.0f, 200.0f)) { apply_brightness(screen); mark_changed(); }
        explain_last_item("Brightness", "Changes the streamed image without rebuilding the source.", "Negligible", "Instant + auto-save");
        if (toggle_switch("Automatic game-light adaptation", screen.autoBrightnessEnabled)) { apply_brightness(screen); mark_changed(); }
        explain_last_item("Automatic brightness", "Uses a phone-like sensor curve: sunlight brightening settles in about 2.5 seconds and dimming in about 4 seconds, avoiding rapid jumps.", "Negligible", "Smooth live adaptation + auto-save");
        ImGui::BeginDisabled(!screen.autoBrightnessEnabled);
        if (slider_percent("Dark-scene multiplier", screen.autoBrightnessDarkMultiplier, 25.0f, 125.0f)) { apply_brightness(screen); mark_changed(); }
        if (slider_percent("Bright-scene multiplier", screen.autoBrightnessBrightMultiplier, 50.0f, 200.0f)) { apply_brightness(screen); mark_changed(); }
        if (g_game_lighting_valid.load())
            ImGui::TextDisabled("Game lighting %.0f%% | effective screen %.0f%% | phone-like response 2.5 s / 4 s",
                g_game_lighting_luminance.load() * 100.0f, screen.effectiveBrightness * 100.0f);
        else ImGui::TextDisabled("Waiting for the first game-lighting sample...");
        ImGui::EndDisabled();
        const char* scales[] = { "Stretch", "Fit", "Crop" }; int scale = static_cast<int>(screen.scaleMode);
        if (ImGui::Combo("Scaling", &scale, scales, IM_ARRAYSIZE(scales))) { screen.scaleMode = static_cast<scale_mode_t>(scale); screen.hasUploadedFrame = false; mark_changed(); }
        int guard = screen.edgeBleedGuard;
        if (ImGui::SliderInt("Edge guard", &guard, 0, 16, "%d px")) { screen.edgeBleedGuard = static_cast<uint8_t>(guard); screen.hasUploadedFrame = false; mark_changed(); }
        if (toggle_switch("Flip vertically", screen.flipVertical)) { screen.hasUploadedFrame = false; mark_changed(); }
    }

    void release_screen(screen_t& screen)
    {
        screen.source.reset();
        if (screen.liveTexture) screen.liveTexture->Release();
        if (screen.uploadTexture) screen.uploadTexture->Release();
        if (screen.liveTextureRenderTarget) screen.liveTextureRenderTarget->Release();
        if (screen.immediateContext) screen.immediateContext->Release();
        screen.liveTexture = nullptr; screen.uploadTexture = nullptr;
        screen.liveTextureRenderTarget = nullptr; screen.immediateContext = nullptr;
    }

    void draw_system(screen_t& screen, bool& removeCurrent)
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "PERFORMANCE + SYSTEM");
        const char* profiles[] = { "Custom", "Economy - 854x480 @ 20", "Balanced - 1280x720 @ 30", "Quality - 1920x1080 @ 30", "Smooth - 1280x720 @ 60" };
        int profile = static_cast<int>(screen.performanceProfile);
        if (ImGui::Combo("Performance profile", &profile, profiles, IM_ARRAYSIZE(profiles))) { screen.performanceProfile = static_cast<performance_profile_t>(profile); apply_profile(screen); mark_changed(); }
        explain_last_item("Performance profile", "Smooth is the recommended 1280x720 at 60 FPS preset from your configuration.", "Varies by preset", "Applies live + auto-save");
        int width = static_cast<int>(screen.targetLiveTextureWidth);
        int height = static_cast<int>(screen.targetLiveTextureHeight);
        int fps = static_cast<int>(screen.framerate);
        bool dimensionsChanged = false;
        ImGui::SetNextItemWidth(130.0f);
        dimensionsChanged |= ImGui::InputInt("Width", &width, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(130.0f);
        dimensionsChanged |= ImGui::InputInt("Height", &height, 0, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(130.0f);
        dimensionsChanged |= ImGui::InputInt("FPS", &fps, 0, 0);
        if (dimensionsChanged)
        {
            screen.performanceProfile = performance_profile_t::CUSTOM;
            screen.targetLiveTextureWidth = static_cast<uint32_t>((std::clamp)(width, 64, 7680));
            screen.targetLiveTextureHeight = static_cast<uint32_t>((std::clamp)(height, 64, 4320));
            screen.framerate = static_cast<uint8_t>((std::clamp)(fps, 1, 120));
            if (screen.source)
            {
                screen.source->SetOutputSize(screen.targetLiveTextureWidth, screen.targetLiveTextureHeight);
                screen.source->SetFramerate(screen.framerate);
            }
            mark_changed();
        }
        const auto stats = screen.source ? screen.source->GetPerformanceStats() : source_performance_stats_t{};
        const std::string status = screen.source ? screen.source->GetStatusText() : "Waiting for source";
        ImGui::Text("Source: %s", status.c_str());
        if (ImGui::CollapsingHeader("Live performance", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const double gameFps = ImGui::GetIO().Framerate;
            const double observedFrameMs = gameFps > 0.1 ? 1000.0 / gameFps : 0.0;
            if (observedFrameMs > 0.0)
            {
                const double withoutPluginMs = (std::max)(0.1, observedFrameMs - screen.uploadCpuMs);
                const double instantaneousLoss = (std::max)(0.0, 1000.0 / withoutPluginMs - gameFps);
                const double smoothing = 1.0 - std::exp(-observedFrameMs / 2500.0);
                screen.estimatedFpsLoss = screen.estimatedFpsLoss == 0.0 ? instantaneousLoss :
                    screen.estimatedFpsLoss * (1.0 - smoothing) + instantaneousLoss * smoothing;
            }
            const double delivered = stats.deliveredFps > 0.0 ? stats.deliveredFps : screen.deliveredFps;
            ImGui::Text("Current game FPS: %.1f", gameFps);
            const ImVec4 lossColour = screen.estimatedFpsLoss < 1.0 ? ImVec4(0.35f, 0.90f, 0.40f, 1.0f) :
                (screen.estimatedFpsLoss < 3.0 ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
            ImGui::TextColored(lossColour, "Smoothed estimated FPS loss: %.2f", screen.estimatedFpsLoss);
            ImGui::Text("Game-thread texture upload: %.3f ms", screen.uploadCpuMs);
            ImGui::Text("Source worker CPU: %.3f ms/frame", stats.workerCpuMs);
            ImGui::Text("Total measured plugin CPU: %.3f ms/frame", screen.totalPluginCpuMs);
            ImGui::Text("GPU readback portion: %.3f ms/frame", stats.readbackMs);
            ImGui::Text("Delivered source rate: %.1f FPS", delivered);
            ImGui::Text("Dropped/overloaded frames: %llu",
                static_cast<unsigned long long>(stats.droppedFrames));
            ImGui::Text("Frames uploaded to game: %llu",
                static_cast<unsigned long long>(screen.uploadedFrames));
            ImGui::Text("Hardware decode requested: %s", stats.hardwareDecoded ? "Yes" : "No / external");
            ImGui::Text("Window capture bypassed: %s", stats.directMedia ? "Yes" : "No");
            ImGui::Text("Live environment estimator: %.1f us/update", g_environment_update_cpu_us.load());
            const DWORD cpu0 = thread_scheduling::preferred_processor(0);
            DWORD cpu1 = thread_scheduling::preferred_processor(1);
            DWORD cpu2 = thread_scheduling::preferred_processor(2);
            if (cpu0 != thread_scheduling::kUnassignedProcessor)
            {
                if (cpu1 == thread_scheduling::kUnassignedProcessor) cpu1 = cpu0;
                if (cpu2 == thread_scheduling::kUnassignedProcessor) cpu2 = cpu1;
                ImGui::Text("Background CPU hints: LP %lu, %lu, %lu",
                    static_cast<unsigned long>(cpu0), static_cast<unsigned long>(cpu1), static_cast<unsigned long>(cpu2));
            }
            else ImGui::TextDisabled("Background CPU hints: learning render-thread use...");

            if (ImGui::TreeNode("CPU logical-processor activity (sampled)"))
            {
                const ImVec4 gameColour(0.92f, 0.20f, 0.20f, 1.0f);
                const ImVec4 pluginColour(0.20f, 0.85f, 0.30f, 1.0f);
                const ImVec4 sharedColour(1.0f, 0.78f, 0.12f, 1.0f);
                const ImVec4 idleColour(0.30f, 0.30f, 0.30f, 1.0f);
                const ImGuiColorEditFlags colourFlags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;
                ImGui::ColorButton("##game_cpu", gameColour, colourFlags, ImVec2(12.0f, 12.0f));
                ImGui::SameLine(); ImGui::TextUnformatted("Game"); ImGui::SameLine();
                ImGui::ColorButton("##plugin_cpu", pluginColour, colourFlags, ImVec2(12.0f, 12.0f));
                ImGui::SameLine(); ImGui::TextUnformatted("Plugin"); ImGui::SameLine();
                ImGui::ColorButton("##shared_cpu", sharedColour, colourFlags, ImVec2(12.0f, 12.0f));
                ImGui::SameLine(); ImGui::TextUnformatted("Both");
                const DWORD processorCount = thread_scheduling::tracked_processor_count();
                for (DWORD processor = 0; processor < processorCount; ++processor)
                {
                    const uint32_t gameHits = thread_scheduling::sampled_game_hits(processor);
                    const uint32_t pluginHits = thread_scheduling::sampled_plugin_hits(processor);
                    const ImVec4 colour = gameHits && pluginHits ? sharedColour :
                        (gameHits ? gameColour : (pluginHits ? pluginColour : idleColour));
                    ImGui::PushID(12000 + static_cast<int>(processor));
                    ImGui::ColorButton("##processor", colour, colourFlags, ImVec2(14.0f, 14.0f));
                    ImGui::SameLine(); ImGui::Text("LP %lu   game %u   plugin %u",
                        static_cast<unsigned long>(processor), gameHits, pluginHits);
                    ImGui::PopID();
                }
                ImGui::TextWrapped("Low-overhead two-second samples; game affinity is unchanged.");
                ImGui::TreePop();
            }
        }
        if (update_checker::update_available() && !update_checker::is_dismissed())
        {
            ImGui::TextColored(ImVec4(0.42f, 0.94f, 0.47f, 1.0f), "Update available: %s", update_checker::latest_tag().c_str());
            if (ImGui::Button("Open GitHub Releases")) update_checker::open_releases_page();
            ImGui::SameLine(); if (ImGui::Button("Dismiss")) update_checker::dismiss();
        }
        if (ImGui::CollapsingHeader("Critical texture identity"))
        {
            if (ImGui::InputText("Game texture", &screen.original_texture)) mark_changed(true);
            explain_last_item("Game texture", "Changes which Prism3D texture is intercepted.", "Critical", "Requires texture reload");
            if (ImGui::InputText("Override texture", &screen.override_texture)) mark_changed(true);
            if (ImGui::InputScalar("Override width", ImGuiDataType_U32, &screen.override_texture_size_w)) mark_changed(true);
            if (ImGui::InputScalar("Override height", ImGuiDataType_U32, &screen.override_texture_size_h)) mark_changed(true);
        }
        if (criticalReloadPending)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.28f, 1.0f), "Texture identity changed. Reload only when ready.");
            if (ImGui::Button("Reload game textures", ImVec2(-1.0f, 34.0f))) { prism::string command("game"); prism::execute_command::call(&command, -1); criticalReloadPending = false; }
            explain_last_item("Reload game textures", "Runs the game reload only for texture identity changes or restored backups.", "Brief loading interruption", "Manual critical action");
        }
        if (ImGui::CollapsingHeader("Configuration backups"))
        {
            static int backup{};
            const auto history = settings::backup_history();
            const char* labels[] = { history[0].description.c_str(), history[1].description.c_str(), history[2].description.c_str() };
            ImGui::Combo("Restore point", &backup, labels, IM_ARRAYSIZE(labels));
            if (ImGui::Button("Save configuration now"))
            {
                saveNowRequest = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!history[static_cast<size_t>(backup)].available);
            if (ImGui::Button("Restore selected backup"))
                restoreRequest = backup;
            ImGui::EndDisabled(); if (!restoreStatus.empty()) ImGui::TextWrapped("%s", restoreStatus.c_str());
        }
        ImGui::Separator();
        if (ImGui::Button("Remove this screen", ImVec2(-1.0f, 0.0f))) removeCurrent = true;
        explain_last_item("Remove screen", "Stops its source immediately and removes its saved configuration.", "Reduces resource use");
    }

    void add_screen(screen_type_t type)
    {
        screen_t screen; screen.type = type;
        if (type == screen_type_t::GPS)
        { screen.original_texture = "/vehicle/truck/share/gps.tobj"; screen.override_texture = "/home/PrismTextureStreamer/gps.tobj"; screen.override_texture_size_w = 64; screen.override_texture_size_h = 2048; }
        else if (type == screen_type_t::DASHBOARD)
        { screen.original_texture = "/vehicle/truck/share/dashboard.tobj"; screen.override_texture = "/home/PrismTextureStreamer/dashboard.tobj"; screen.override_texture_size_w = 2048; screen.override_texture_size_h = 64; }
        else { screen.original_texture = ".tobj"; screen.override_texture = "/home/PrismTextureStreamer/.tobj"; }
        g_screens.push_back(std::move(screen)); selectedScreen = static_cast<int>(g_screens.size() - 1); mark_changed(true);
    }

    void draw_inspector()
    {
        text_with_font(2, ImVec4(0.93f, 0.97f, 0.99f, 1.0f), "WHAT THIS CHANGES");
        ImGui::TextDisabled("Focus or hover an option for a live explanation.");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushFont(ui_font(1));
        ImGui::TextColored(ImVec4(0.12f, 0.86f, 0.98f, 1.0f), "EFFECT");
        ImGui::PopFont();
        ImGui::PushFont(ui_font(2)); ImGui::TextWrapped("%s", inspector.title); ImGui::PopFont();
        ImGui::TextWrapped("%s", inspector.effect);

        ImGui::Dummy(ImVec2(0.0f, 10.0f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(ui_font(1));
        ImGui::TextColored(ImVec4(0.12f, 0.86f, 0.98f, 1.0f), "PERFORMANCE");
        ImGui::PopFont();
        ImGui::TextWrapped("%s", inspector.impact);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::PushFont(ui_font(1));
        ImGui::TextColored(ImVec4(0.42f, 0.94f, 0.47f, 1.0f), "APPLIES");
        ImGui::PopFont();
        ImGui::TextWrapped("%s", inspector.apply);

        ImGui::Dummy(ImVec2(0.0f, 18.0f));
        const ImVec2 graph = ImGui::GetCursorScreenPos();
        const ImVec2 graphSize(ImGui::GetContentRegionAvail().x, 154.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(graph, ImVec2(graph.x + graphSize.x, graph.y + graphSize.y),
            IM_COL32(7, 16, 27, 235), 10.0f);
        draw->AddRect(graph, ImVec2(graph.x + graphSize.x, graph.y + graphSize.y), kBorder, 10.0f);
        draw->AddText(ui_font(0), 16.0f, ImVec2(graph.x + 14.0f, graph.y + 12.0f), kMuted, "SMOOTH CURVE PREVIEW");
        const ImVec2 a(graph.x + 18.0f, graph.y + 122.0f);
        const ImVec2 b(graph.x + graphSize.x - 18.0f, graph.y + 52.0f);
        draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), IM_COL32(111, 132, 148, 75));
        draw->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), IM_COL32(111, 132, 148, 75));
        draw->AddBezierCubic(a,
            ImVec2(a.x + graphSize.x * 0.24f, a.y),
            ImVec2(b.x - graphSize.x * 0.24f, b.y), b, kCyan, 3.0f);
        draw->AddCircleFilled(a, 4.0f, kCyan); draw->AddCircleFilled(b, 4.0f, kGreen);
        ImGui::Dummy(graphSize);

        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        const ImVec2 live = ImGui::GetCursorScreenPos();
        const ImVec2 liveSize(ImGui::GetContentRegionAvail().x, 72.0f);
        ImGui::Dummy(liveSize);
        draw->AddRectFilled(live, ImVec2(live.x + liveSize.x, live.y + liveSize.y),
            IM_COL32(24, 71, 35, 190), 10.0f);
        draw->AddRect(live, ImVec2(live.x + liveSize.x, live.y + liveSize.y),
            IM_COL32(106, 235, 91, 145), 10.0f);
        draw->AddCircle(ImVec2(live.x + 29.0f, live.y + 36.0f), 14.0f, kGreen, 24, 2.0f);
        draw->AddText(ui_font(1), 19.0f, ImVec2(live.x + 54.0f, live.y + 13.0f), kGreen,
            "NO RESTART REQUIRED");
        draw->AddText(ui_font(0), 16.0f, ImVec2(live.x + 54.0f, live.y + 39.0f),
            IM_COL32(207, 231, 210, 255), "Safe changes apply live");
    }

    void draw_menu()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 wanted((std::min)(1680.0f, viewport->WorkSize.x - 28.0f),
            (std::min)(930.0f, viewport->WorkSize.y - 28.0f));
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(wanted, ImGuiCond_Always);
        ImGuiStyle oldStyle = ImGui::GetStyle();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.ChildRounding = 12.0f; style.FrameRounding = 9.0f; style.PopupRounding = 10.0f;
        style.FramePadding = ImVec2(12.0f, 9.0f); style.ItemSpacing = ImVec2(10.0f, 11.0f);
        style.ItemInnerSpacing = ImVec2(9.0f, 7.0f); style.ScrollbarSize = 10.0f;
        style.ScrollbarRounding = 8.0f; style.GrabRounding = 8.0f; style.GrabMinSize = 15.0f;
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.043f, 0.083f, 0.12f, 0.95f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.36f, 0.43f, 0.72f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.95f, 0.97f, 1.0f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.65f, 0.71f, 1.0f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.085f, 0.15f, 0.20f, 1.0f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.10f, 0.25f, 0.31f, 1.0f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.08f, 0.34f, 0.40f, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.07f, 0.22f, 0.29f, 1.0f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.08f, 0.42f, 0.50f, 1.0f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.55f, 0.63f, 1.0f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.30f, 0.37f, 0.92f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.10f, 0.42f, 0.49f, 0.95f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.07f, 0.55f, 0.63f, 1.0f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.12f, 0.88f, 0.96f, 1.0f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.89f, 0.97f, 1.0f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.98f, 0.70f, 1.0f);
        style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.10f, 0.86f, 0.96f, 1.0f);
        bool earlyExit = false;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##PrismMediaConsole", nullptr, flags))
        {
            const ImVec2 windowPos = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            const float headerHeight = 106.0f;
            const float footerHeight = 72.0f;
            const float mainTop = headerHeight + 14.0f;
            const float mainHeight = windowSize.y - headerHeight - footerHeight - 28.0f;
            const float leftWidth = (std::clamp)(windowSize.x * 0.185f, 235.0f, 292.0f);
            const float rightWidth = (std::clamp)(windowSize.x * 0.235f, 300.0f, 385.0f);
            const float gap = 14.0f;
            const float side = 16.0f;
            const float centreWidth = windowSize.x - side * 2.0f - leftWidth - rightWidth - gap * 2.0f;
            ImDrawList* shell = ImGui::GetWindowDrawList();
            shell->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(5, 13, 22, 247), 22.0f);
            shell->AddRect(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(61, 81, 98, 210), 22.0f, 0, 1.5f);
            shell->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + headerHeight),
                IM_COL32(9, 20, 31, 250), 22.0f, ImDrawFlags_RoundCornersTop);
            shell->AddLine(ImVec2(windowPos.x, windowPos.y + headerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + headerHeight), IM_COL32(45, 69, 86, 190));
            shell->AddRectFilled(ImVec2(windowPos.x, windowPos.y + windowSize.y - footerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(8, 18, 27, 252), 22.0f, ImDrawFlags_RoundCornersBottom);
            shell->AddLine(ImVec2(windowPos.x, windowPos.y + windowSize.y - footerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y - footerHeight), IM_COL32(45, 69, 86, 190));
            const ImVec2 navMin(windowPos.x + side, windowPos.y + mainTop);
            const ImVec2 navMax(navMin.x + leftWidth, navMin.y + mainHeight);
            const ImVec2 contentMin(navMax.x + gap, navMin.y);
            const ImVec2 contentMax(contentMin.x + centreWidth, navMax.y);
            const ImVec2 inspectorMin(contentMax.x + gap, navMin.y);
            const ImVec2 inspectorMax(inspectorMin.x + rightWidth, navMax.y);
            draw_card_shadow(shell, navMin, navMax);
            draw_card_shadow(shell, contentMin, contentMax);
            draw_card_shadow(shell, inspectorMin, inspectorMax);

            const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f);
            ImGui::SetCursorPos(ImVec2(30.0f, 20.0f));
            ImGui::PushFont(ui_font(3)); ImGui::TextUnformatted("PRISM MEDIA"); ImGui::PopFont();
            ImGui::SameLine(0.0f, 10.0f); ImGui::PushFont(ui_font(3));
            ImGui::TextColored(ImVec4(0.12f, 0.86f, 0.96f, 1.0f), "4.0"); ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(33.0f, 61.0f));
            ImGui::TextColored(ImVec4(0.56f, 0.66f, 0.72f, 1.0f), "Console Focus  |  Changes save automatically");

            std::lock_guard<std::mutex> lock(g_screens_mutex);
            screen_t* statusScreen = nullptr;
            if (!g_screens.empty())
            {
                const int statusIndex = (std::clamp)(selectedScreen, 0, static_cast<int>(g_screens.size() - 1));
                statusScreen = &g_screens[static_cast<size_t>(statusIndex)];
            }
            const bool helperInstalled = sources::IsMediaClientInstalled();
            const bool sourceOnline = statusScreen && statusScreen->source;
            const bool youtubeActive = sourceOnline && statusScreen->contentMode == content_mode_t::INTEGRATED_MEDIA &&
                statusScreen->mediaService == media_service_t::YOUTUBE;
            const bool spotifyActive = sourceOnline && statusScreen->contentMode == content_mode_t::INTEGRATED_MEDIA &&
                statusScreen->mediaService == media_service_t::SPOTIFY;
            const float statusWidth = (std::clamp)((windowSize.x - 620.0f) / 3.0f, 150.0f, 188.0f);
            ImGui::SetCursorPos(ImVec2(windowSize.x - (statusWidth * 3.0f + 22.0f), 23.0f));
            status_card(0, "YouTube", youtubeActive ? "Active" : "Ready", youtubeActive ? kGreen : kCyan, statusWidth); ImGui::SameLine(0.0f, 8.0f);
            status_card(1, "Spotify Web", spotifyActive ? "Active" : "Ready", spotifyActive ? kGreen : kCyan, statusWidth); ImGui::SameLine(0.0f, 8.0f);
            status_card(2, "Client", sourceOnline ? "Online" : (helperInstalled ? "Standby" : "Missing"),
                sourceOnline ? kGreen : (helperInstalled ? IM_COL32(105, 205, 255, 255) : IM_COL32(255, 99, 92, 255)), statusWidth);

            if (g_screens.empty())
            {
                ImGui::SetCursorPos(ImVec2(windowSize.x * 0.5f - 230.0f, windowSize.y * 0.5f - 65.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 20.0f));
                ImGui::BeginChild("empty", ImVec2(460.0f, 160.0f), true);
                text_with_font(2, ImVec4(0.94f, 0.97f, 0.99f, 1.0f), "CREATE YOUR FIRST DISPLAY");
                ImGui::TextWrapped("Start with the recommended Smooth GPS profile and adaptive display settings.");
                if (ImGui::Button("Create recommended GPS screen", ImVec2(-1.0f, 42.0f))) add_screen(screen_type_t::GPS);
                ImGui::EndChild(); ImGui::PopStyleVar();
                earlyExit = true;
            }
            else
            {
                selectedScreen = (std::clamp)(selectedScreen, 0, static_cast<int>(g_screens.size() - 1));
                selectedPage = (std::clamp)(selectedPage, 0, 5);
                ImGui::SetCursorPos(ImVec2(side, mainTop));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 17.0f));
                ImGui::BeginChild("navigation", ImVec2(leftWidth, mainHeight), true);
                ImGui::PushFont(ui_font(1)); ImGui::TextDisabled("ACTIVE DISPLAY"); ImGui::PopFont();
                const char* activeKind = g_screens[static_cast<size_t>(selectedScreen)].type == screen_type_t::GPS
                    ? "GPS" : (g_screens[static_cast<size_t>(selectedScreen)].type == screen_type_t::DASHBOARD ? "Dashboard" : "Custom");
                const std::string activeLabel = std::string(activeKind) + "  " + std::to_string(selectedScreen + 1);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##active_display", activeLabel.c_str()))
                {
                for (int i = 0; i < static_cast<int>(g_screens.size()); ++i)
                {
                    const char* kind = g_screens[static_cast<size_t>(i)].type == screen_type_t::GPS ? "GPS" : (g_screens[static_cast<size_t>(i)].type == screen_type_t::DASHBOARD ? "Dashboard" : "Custom");
                    const std::string label = std::string(kind) + "  " + std::to_string(i + 1);
                    if (ImGui::Selectable(label.c_str(), selectedScreen == i)) selectedScreen = i;
                }
                    ImGui::EndCombo();
                }
                const float addButtonWidth = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
                if (ImGui::Button("+ GPS", ImVec2(addButtonWidth, 36.0f))) add_screen(screen_type_t::GPS);
                ImGui::SameLine(0.0f, 8.0f);
                if (ImGui::Button("+ Dash", ImVec2(addButtonWidth, 36.0f))) add_screen(screen_type_t::DASHBOARD);
                ImGui::SameLine(0.0f, 8.0f);
                if (ImGui::Button("+ Custom", ImVec2(-1.0f, 36.0f))) add_screen(screen_type_t::CUSTOM);
                ImGui::Dummy(ImVec2(0.0f, 5.0f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0.0f, 5.0f));
                const char* pages[] = { "Home", "Media", "Audio", "Controls", "Appearance", "System" };
                for (int i = 0; i < IM_ARRAYSIZE(pages); ++i)
                    if (nav_button(i, pages[i], selectedPage == i)) { selectedPage = i; panelAnimation = 0.0f; }
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 56.0f);
                ImGui::TextColored(ImVec4(0.30f, 0.92f, 0.98f, pulse), "AUTO-SAVE  LIVE");
                ImGui::TextDisabled("Ctrl+F8 closes the console");
                ImGui::EndChild(); ImGui::PopStyleVar();

                screen_t& screen = g_screens[static_cast<size_t>(selectedScreen)];
                panelAnimation = (std::min)(1.0f, panelAnimation + ImGui::GetIO().DeltaTime * 6.0f);
                const char* titles[] = { "HOME", "MEDIA", "AUDIO", "CONTROLS", "APPEARANCE", "SYSTEM" };
                const char* subtitles[] = {
                    "A live overview of the selected display and plugin health.",
                    "Choose what plays on the selected truck display.",
                    "Fine-tune how media audio blends with your environment.",
                    "Use the same interface comfortably with a mouse or gamepad.",
                    "Balance display readability with a natural in-cab look.",
                    "Performance profiles, live status and critical maintenance."
                };
                ImGui::SetCursorPos(ImVec2(side + leftWidth + gap, mainTop));
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f + 0.65f * (panelAnimation * panelAnimation * (3.0f - 2.0f * panelAnimation)));
                ImGui::BeginChild("content", ImVec2(centreWidth, mainHeight), false);
                const float contentBaseX = ImGui::GetCursorPosX();
                const ImVec2 pageIcon = ImGui::GetCursorScreenPos();
                ImDrawList* contentDraw = ImGui::GetWindowDrawList();
                draw_card_shadow(contentDraw, pageIcon, ImVec2(pageIcon.x + 48.0f, pageIcon.y + 48.0f), 11.0f);
                contentDraw->AddRectFilled(pageIcon, ImVec2(pageIcon.x + 48.0f, pageIcon.y + 48.0f),
                    IM_COL32(20, 105, 132, 245), 11.0f);
                contentDraw->AddRect(pageIcon, ImVec2(pageIcon.x + 48.0f, pageIcon.y + 48.0f),
                    IM_COL32(40, 225, 248, 155), 11.0f);
                draw_nav_icon(contentDraw, selectedPage, ImVec2(pageIcon.x + 24.0f, pageIcon.y + 24.0f), IM_COL32_WHITE);
                ImGui::SetCursorPosX(contentBaseX + 62.0f);
                text_with_font(3, ImVec4(0.94f, 0.97f, 0.99f, 1.0f), titles[selectedPage]);
                ImGui::SetCursorPosX(contentBaseX + 62.0f);
                ImGui::TextDisabled("%s", subtitles[selectedPage]);
                ImGui::SetCursorPosX(contentBaseX); ImGui::Dummy(ImVec2(0.0f, 3.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
                ImGui::BeginChild("page_card", ImVec2(0.0f, -1.0f), true);
                bool removeCurrent = false;
                switch (selectedPage) { case 0: draw_home(screen); break; case 1: draw_media(screen); break; case 2: draw_audio(screen); break; case 3: draw_controls(); break; case 4: draw_appearance(screen); break; case 5: draw_system(screen, removeCurrent); break; }
                ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::EndChild(); ImGui::PopStyleVar();

                ImGui::SetCursorPos(ImVec2(side + leftWidth + gap + centreWidth + gap, mainTop));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 20.0f));
                ImGui::BeginChild("inspector", ImVec2(rightWidth, mainHeight), true);
                draw_inspector(); ImGui::EndChild(); ImGui::PopStyleVar();
                if (removeCurrent) { release_screen(screen); g_screens.erase(g_screens.begin() + selectedScreen); selectedScreen = (std::max)(0, selectedScreen - 1); mark_changed(true); }
            }

            const float footerY = windowSize.y - footerHeight;
            ImGui::SetCursorPos(ImVec2(34.0f, footerY + 20.0f));
            ImGui::PushFont(ui_font(1));
            ImGui::TextColored(ImVec4(0.48f, 0.95f, 0.38f, 1.0f), "[A]"); ImGui::SameLine(); ImGui::TextUnformatted("Select");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(1.0f, 0.34f, 0.38f, 1.0f), "[B]"); ImGui::SameLine(); ImGui::TextUnformatted("Back");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(0.30f, 0.76f, 1.0f, 1.0f), "[LB/RB]"); ImGui::SameLine(); ImGui::TextUnformatted("Category");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.23f, 1.0f), "[Y]"); ImGui::SameLine(); ImGui::TextUnformatted("Explain");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextDisabled("Mouse click / scroll also supported");
            ImGui::PopFont();
        }
        ImGui::End(); ImGui::GetStyle() = oldStyle; (void)earlyExit;
    }

    void on_frame()
    {
        static bool initialized{};
        if (!initialized && ImGui::GetCurrentContext())
        {
            auto& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
            set_visible(false); initialized = true;
        }
        static bool held{};
        const bool toggle = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        process_media_hotkeys(menuVisible.load());
        if ((toggle && !held) || consume_gamepad_menu_toggle_request()) set_visible(!menuVisible.load());
        held = toggle;
        if (!menuVisible.load() && update_checker::should_show_toast())
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 18.0f,
                viewport->WorkPos.y + 18.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.92f);
            ImGui::Begin("##prism_update_toast", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
            ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.45f, 1.0f),
                "PrismTextureStreamer update available: %s", update_checker::latest_tag().c_str());
            ImGui::TextUnformatted("Open the plugin menu (Ctrl+F8) for the download page.");
            ImGui::End();
        }
        if (menuVisible.load()) draw_menu();
        if (saveNowRequest)
        {
            saveNowRequest = false;
            restoreStatus = settings::save() ? "Configuration saved; previous state preserved." : "Save failed; check the log.";
            savePending = false;
            lastChangeTick = 0;
        }
        if (restoreRequest >= 0)
        {
            const int request = restoreRequest;
            restoreRequest = -1;
            if (settings::restore_backup(static_cast<size_t>(request)))
            {
                restoreStatus = "Restored. Reload textures when ready.";
                criticalReloadPending = true;
                selectedScreen = 0;
            }
            else
                restoreStatus = "Restore failed; check the log.";
        }
        if (savePending)
        {
            const uint64_t now = GetTickCount64();
            if ((!ImGui::IsAnyItemActive() && now - lastChangeTick >= 350) || now - lastChangeTick >= 1500)
            { settings::save(); savePending = false; lastChangeTick = 0; }
        }
    }
}

namespace Gui
{
    bool is_visible() { return menuVisible.load(); }
    bool init() { dx11::present::on_frame(on_frame); return true; }
}
