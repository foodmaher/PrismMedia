#define NOMINMAX
#include "menu.h"

#include <Windows.h>
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <ImGui/imgui.h>
#include <ImGui/imgui_impl_dx11.h>
#include <ImGui/imgui_impl_win32.h>

#include "../dinput8/dinput8.h"
#include "../custom_render_probe.h"
#include "../diagnostic_log.h"
#include "../dx11/present.h"
#include "../environment_audio.h"
#include "../hotkeys.h"
#include "../misc/imgui_stdlib.h"
#include "../override_assets.h"
#include "../prism/execute_command.h"
#include "../prism/memserver_texture_queue.h"
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
    int selectedAudioPanel{ 1 };
    float panelAnimation = 1.0f;
    uint64_t menuOpenedTick{};
    bool menuNeedsFocus{};
    int restoreRequest = -1;
    bool saveNowRequest{};
    std::string restoreStatus;

    constexpr float kWindowEdgeMargin = 12.0f;

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
        ImGui::PushID(label);
        ImGui::TextDisabled("%s", label);
        float percent = normalized * 100.0f;
        ImGui::SetNextItemWidth(-1.0f);
        const bool changed = ImGui::SliderFloat("##percent", &percent,
            minimumPercent, maximumPercent, "%.0f%%",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        if (!changed)
            return false;
        normalized = percent / 100.0f;
        return true;
    }

    bool slider_value(const char* label, float& value, float minimum,
        float maximum, const char* format,
        ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp)
    {
        ImGui::PushID(label);
        ImGui::TextDisabled("%s", label);
        ImGui::SetNextItemWidth(-1.0f);
        const bool changed = ImGui::SliderFloat(
            "##value", &value, minimum, maximum, format, flags);
        ImGui::PopID();
        return changed;
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
        // soft card elevation from the approved mock-up. Keep the layer count
        // deliberately small because this is rendered inside the game at the
        // swap-chain resolution.
        for (int layer = 3; layer >= 1; --layer)
        {
            const float spread = static_cast<float>(layer) * 2.0f;
            const int alpha = 8 + (4 - layer) * 5;
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

    bool nav_button(int icon, const char* label, bool selected,
        float height = 62.0f)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
        const bool activated = ImGui::InvisibleButton(label, size);
        if (selected)
            ImGui::SetItemDefaultFocus();
        const bool pressed = activated;
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
        const float centreY = start.y + height * 0.5f;
        const ImVec2 badgeMin(start.x + 18.0f, centreY - 18.0f);
        const ImVec2 badgeMax(start.x + 54.0f, centreY + 18.0f);
        draw->AddRectFilled(badgeMin, badgeMax,
            selected ? IM_COL32(18, 176, 209, 230) : IM_COL32(39, 52, 66, 230), 8.0f);
        draw_nav_icon(draw, icon, ImVec2(start.x + 36.0f, centreY),
            selected ? IM_COL32_WHITE : kMuted);
        draw->AddText(ui_font(1), 19.0f,
            ImVec2(start.x + 70.0f, centreY - 10.0f),
            selected ? kCyan : IM_COL32(230, 237, 243, 255), label);
        return pressed;
    }

    bool transport_button(const char* id, const char* label, int icon,
        float width)
    {
        ImGui::PushID(id);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, 48.0f);
        const bool clicked = ImGui::InvisibleButton("##transport", size);
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (hovered)
            draw_card_shadow(draw, start,
                ImVec2(start.x + size.x, start.y + size.y), 10.0f);
        draw->AddRectFilled(start,
            ImVec2(start.x + size.x, start.y + size.y),
            hovered ? IM_COL32(18, 80, 101, 250) : IM_COL32(20, 40, 55, 245),
            10.0f);
        draw->AddRect(start,
            ImVec2(start.x + size.x, start.y + size.y),
            hovered ? kCyan : kBorder, 10.0f, 0, hovered ? 2.0f : 1.0f);
        const ImVec2 c(start.x + 25.0f, start.y + 24.0f);
        const ImU32 colour = hovered ? kCyan : IM_COL32(232, 240, 246, 255);
        if (icon == 0)
        {
            draw->AddTriangleFilled(ImVec2(c.x - 6.0f, c.y - 8.0f),
                ImVec2(c.x + 6.0f, c.y), ImVec2(c.x - 6.0f, c.y + 8.0f), colour);
            draw->AddRectFilled(ImVec2(c.x + 9.0f, c.y - 8.0f),
                ImVec2(c.x + 12.0f, c.y + 8.0f), colour, 1.0f);
        }
        else if (icon == 1 || icon == 2)
        {
            const float direction = icon == 1 ? -1.0f : 1.0f;
            draw->AddTriangleFilled(
                ImVec2(c.x + direction * 7.0f, c.y - 8.0f),
                ImVec2(c.x - direction * 6.0f, c.y),
                ImVec2(c.x + direction * 7.0f, c.y + 8.0f), colour);
            draw->AddLine(
                ImVec2(c.x + direction * 10.0f, c.y - 8.0f),
                ImVec2(c.x + direction * 10.0f, c.y + 8.0f), colour, 2.5f);
        }
        else
        {
            const ImVec2 speaker[] = {
                ImVec2(c.x - 10.0f, c.y - 5.0f), ImVec2(c.x - 5.0f, c.y - 5.0f),
                ImVec2(c.x + 1.0f, c.y - 10.0f), ImVec2(c.x + 1.0f, c.y + 10.0f),
                ImVec2(c.x - 5.0f, c.y + 5.0f), ImVec2(c.x - 10.0f, c.y + 5.0f)
            };
            draw->AddConvexPolyFilled(speaker, 6, colour);
            if (icon == 3)
            {
                draw->AddLine(ImVec2(c.x + 6.0f, c.y - 6.0f),
                    ImVec2(c.x + 14.0f, c.y + 6.0f), colour, 2.0f);
                draw->AddLine(ImVec2(c.x + 14.0f, c.y - 6.0f),
                    ImVec2(c.x + 6.0f, c.y + 6.0f), colour, 2.0f);
            }
            else
            {
                draw->AddLine(ImVec2(c.x + 6.0f, c.y),
                    ImVec2(c.x + 14.0f, c.y), colour, 2.0f);
                if (icon == 5)
                    draw->AddLine(ImVec2(c.x + 10.0f, c.y - 4.0f),
                        ImVec2(c.x + 10.0f, c.y + 4.0f), colour, 2.0f);
            }
        }
        draw->AddText(ui_font(1), 16.0f,
            ImVec2(start.x + 48.0f, start.y + 15.0f), colour, label);
        ImGui::PopID();
        return clicked;
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

    const screen_t* texture_owner_other_than(
        const screen_t& screen, const std::string& originalTexture)
    {
        if (originalTexture.empty())
            return nullptr;
        for (const auto& other : g_screens)
        {
            if (&other != &screen && other.enabled &&
                other.original_texture == originalTexture)
                return &other;
        }
        return nullptr;
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
        screen.suspiciousMagentaFrame = false;
        screen.suspiciousBlackFrame = false;
        screen.sourceFrameStale = false;
        screen.consecutiveBlackFrameInspections = 0;
        screen.blackSampleCount = 0;
        screen.consecutiveMapFailures = 0;
    }

    bool rebuild_source(screen_t& screen)
    {
        if (!screen.enabled)
        {
            screen.source.reset();
            screen.textureRouteArmed = false;
            screen.textureRouteArmedTick = 0;
            reset_source_stats(screen);
            diagnostic_log::writef(
                "route",
                "Display %s remains disabled; media source and texture "
                "routing were not started.",
                screen.mediaClientId.c_str());
            return false;
        }
        if (texture_owner_other_than(screen, screen.original_texture))
        {
            screen.source.reset();
            reset_source_stats(screen);
            diagnostic_log::writef(
                "error",
                "Media source %s was not started because game texture %s "
                "is already assigned to another display.",
                screen.mediaClientId.c_str(),
                screen.original_texture.c_str());
            return false;
        }
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
                screen.mediaClientId,
                screen.override_texture,
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
            diagnostic_log::writef(
                "route",
                "Media source ready for display %s; waiting for exact TOBJ "
                "redirect %s -> %s before any GPU texture can be claimed.",
                screen.mediaClientId.c_str(),
                screen.original_texture.c_str(),
                screen.override_texture.c_str());
        }
        else
        {
            diagnostic_log::writef(
                "route",
                "No media source was created for enabled display %s; its "
                "native TOBJ will pass through until a valid source is "
                "configured.",
                screen.mediaClientId.c_str());
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
        const bool wasVisible = menuVisible.exchange(visible);
        if (visible && !wasVisible)
        {
            menuOpenedTick = GetTickCount64();
            menuNeedsFocus = true;
        }
        if (!visible) { hotkeyBindingIndex = -1; g_is_binding_hotkey = false; }
        dinput8::set_mouse(visible);
        if (!ImGui::GetCurrentContext()) return;
        auto& io = ImGui::GetIO();
        if (visible)
        {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
            io.MouseDrawCursor = true;
            io.ConfigNavCursorVisibleAlways = true;
        }
        else
        {
            io.MouseDrawCursor = false;
            io.ConfigNavCursorVisibleAlways = false;
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
        const float centreX = origin.x + size.x * 0.5f;
        // Keep the cabin/outside transition readable without the old X shape.
        draw->AddLine(ImVec2(bridgeLeft, midY + 7.0f),
            ImVec2(centreX - 8.0f, midY + 7.0f), kBlue, 3.0f);
        draw->AddLine(ImVec2(centreX + 8.0f, midY + 7.0f),
            ImVec2(bridgeRight, midY + 7.0f),
            IM_COL32(42, 216, 172, 255), 3.0f);
        draw->AddCircleFilled(ImVec2(centreX, midY + 7.0f), 4.0f, kCyan);
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

    bool home_card(const char* id, int icon, const char* title,
        const char* headline, const char* detail, const char* footer,
        ImU32 accent, float height)
    {
        ImGui::PushID(id);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
        const bool clicked = ImGui::InvisibleButton("##home_card", size);
        const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw_card_shadow(draw, start,
            ImVec2(start.x + size.x, start.y + size.y), 14.0f);
        draw->AddRectFilled(start,
            ImVec2(start.x + size.x, start.y + size.y),
            hovered ? IM_COL32(21, 40, 55, 252) : IM_COL32(15, 29, 42, 250),
            14.0f);
        draw->AddRectFilled(start,
            ImVec2(start.x + size.x, start.y + 5.0f), accent,
            14.0f, ImDrawFlags_RoundCornersTop);
        draw->AddRect(start,
            ImVec2(start.x + size.x, start.y + size.y),
            hovered ? accent : kBorder, 14.0f, 0, hovered ? 2.0f : 1.0f);

        const ImVec2 badgeMin(start.x + 22.0f, start.y + 24.0f);
        const ImVec2 badgeMax(badgeMin.x + 48.0f, badgeMin.y + 48.0f);
        draw->AddRectFilled(badgeMin, badgeMax,
            hovered ? IM_COL32(24, 108, 135, 245) : IM_COL32(30, 50, 67, 245),
            12.0f);
        draw_nav_icon(draw, icon,
            ImVec2(badgeMin.x + 24.0f, badgeMin.y + 24.0f),
            hovered ? IM_COL32_WHITE : accent);
        draw->AddText(ui_font(1), 17.0f,
            ImVec2(start.x + 84.0f, start.y + 26.0f), accent, title);
        draw->AddText(ui_font(2), 25.0f,
            ImVec2(start.x + 84.0f, start.y + 50.0f),
            IM_COL32(240, 246, 250, 255), headline);
        draw->AddText(ui_font(0), 16.0f,
            ImVec2(start.x + 22.0f, start.y + 91.0f),
            IM_COL32(213, 224, 232, 255), detail, nullptr,
            size.x - 44.0f);
        draw->AddText(ui_font(0), 15.0f,
            ImVec2(start.x + 22.0f, start.y + size.y - 31.0f),
            kMuted, footer);
        const char* open = "OPEN";
        const ImVec2 openSize = ui_font(1)->CalcTextSizeA(
            15.0f, FLT_MAX, 0.0f, open);
        draw->AddText(ui_font(1), 15.0f,
            ImVec2(start.x + size.x - openSize.x - 22.0f,
                start.y + size.y - 31.0f),
            hovered ? accent : kMuted, open);
        ImGui::PopID();
        return clicked;
    }

    const char* screen_type_label(screen_type_t type)
    {
        return type == screen_type_t::GPS ? "GPS" :
            (type == screen_type_t::DASHBOARD ? "Dashboard" : "Custom");
    }

    int screen_type_ordinal(size_t screenIndex)
    {
        if (screenIndex >= g_screens.size())
            return 1;
        const screen_type_t type = g_screens[screenIndex].type;
        int ordinal{};
        for (size_t index = 0; index <= screenIndex; ++index)
            ordinal += g_screens[index].type == type ? 1 : 0;
        return (std::max)(1, ordinal);
    }

    void draw_home(screen_t& screen)
    {
        const auto stats = screen.source ? screen.source->GetPerformanceStats() : source_performance_stats_t{};
        const char* screenKind = screen_type_label(screen.type);
        char displayHeadline[64]{};
        char displayDetail[192]{};
        char performanceHeadline[64]{};
        char performanceDetail[192]{};
        char audioDetail[192]{};
        char brightnessHeadline[64]{};
        char brightnessDetail[192]{};
        std::snprintf(displayHeadline, sizeof(displayHeadline),
            "%s %d", screenKind,
            screen_type_ordinal(static_cast<size_t>(selectedScreen)));
        std::snprintf(displayDetail, sizeof(displayDetail),
            "%s\n%ux%u at %u FPS",
            screen.source ? screen.source->GetStatusText().c_str() : "Waiting for source",
            screen.targetLiveTextureWidth, screen.targetLiveTextureHeight,
            static_cast<unsigned>(screen.framerate));
        std::snprintf(performanceHeadline, sizeof(performanceHeadline),
            "%.1f FPS", ImGui::GetIO().Framerate);
        std::snprintf(performanceDetail, sizeof(performanceDetail),
            "Upload %.3f ms | worker %.3f ms\nDelivered %.1f FPS | dropped %llu",
            screen.uploadCpuMs, stats.workerCpuMs,
            stats.deliveredFps > 0.0 ? stats.deliveredFps : screen.deliveredFps,
            static_cast<unsigned long long>(stats.droppedFrames));
        std::snprintf(audioDetail, sizeof(audioDetail),
            "Cabin %.0f%% | outside %.0f%%\nEnvironment output %.0f%%",
            screen.adaptiveAudioInteriorVolume * 100.0f,
            screen.adaptiveAudioOutsideVolume * 100.0f,
            g_environment_media_gain.load() * 100.0f);
        std::snprintf(brightnessHeadline, sizeof(brightnessHeadline),
            "%.0f%% brightness", screen.effectiveBrightness * 100.0f);
        std::snprintf(brightnessDetail, sizeof(brightnessDetail),
            "Automatic adjustment: %s\nEngine: %s",
            screen.autoBrightnessEnabled ? "On" : "Off",
            g_engine_enabled.load() ? "running" : "off");

        const float availableHeight = ImGui::GetContentRegionAvail().y;
        const float cardHeight = (std::max)(
            170.0f, (availableHeight - 18.0f) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f, 7.0f));
        if (ImGui::BeginTable("home_cards", 2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX))
        {
            ImGui::TableNextRow(0.0f, cardHeight);
            ImGui::TableNextColumn();
            if (home_card("home_source", 1, "ACTIVE DISPLAY",
                displayHeadline, displayDetail, "Source and playlist",
                kCyan, cardHeight - 14.0f))
            { selectedPage = 1; panelAnimation = 0.0f; }

            ImGui::TableNextColumn();
            if (home_card("home_performance", 5, "LIVE PERFORMANCE",
                performanceHeadline, performanceDetail, "Detailed metrics",
                kGreen, cardHeight - 14.0f))
            { selectedPage = 5; panelAnimation = 0.0f; }

            ImGui::TableNextRow(0.0f, cardHeight);
            ImGui::TableNextColumn();
            if (home_card("home_audio", 2, "ADAPTIVE AUDIO",
                screen.adaptiveAudioEnabled ? "Enabled" : "Disabled",
                audioDetail, "Cabin and exterior tuning",
                kBlue, cardHeight - 14.0f))
            { selectedPage = 2; panelAnimation = 0.0f; }

            ImGui::TableNextColumn();
            if (home_card("home_display", 4, "DISPLAY",
                brightnessHeadline, brightnessDetail,
                "Brightness and scaling", IM_COL32(183, 112, 255, 255),
                cardHeight - 14.0f))
            { selectedPage = 4; panelAnimation = 0.0f; }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();
    }

    bool audio_feature_card(const char* id, const char* title, int icon,
        int panelIndex, bool selected, float width)
    {
        ImGui::PushID(id);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, 126.0f);
        const bool clicked = ImGui::InvisibleButton("##audio_feature", size);
        if (selected)
            ImGui::SetItemDefaultFocus();
        const bool selectedByNavigation = ImGui::IsItemFocused() &&
            selectedAudioPanel != panelIndex;
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
        return clicked || selectedByNavigation;
    }

    void select_audio_panel(int panel)
    {
        selectedAudioPanel = (panel + 3) % 3;
        panelAnimation = 0.0f;
        switch (selectedAudioPanel)
        {
        case 0:
            inspector = { "Volume & balance",
                "Controls cabin, menu and exterior media levels.",
                "Very low CPU", "Applies live" };
            break;
        case 1:
            inspector = { "Cabin / outside",
                "Smoothly blends volume and tone between camera environments.",
                "Very low CPU", "Applies live" };
            break;
        default:
            inspector = { "Filters",
                "Controls distance-based low-pass muffling outside the truck.",
                "Very low CPU", "Applies live" };
            break;
        }
    }

    void draw_audio_feature_cards()
    {
        const float gap = 12.0f;
        const float width = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
        if (audio_feature_card("volume", "VOLUME & BALANCE", 0, 0,
            selectedAudioPanel == 0, width))
            select_audio_panel(0);
        ImGui::SameLine(0.0f, gap);
        if (audio_feature_card("transition", "CABIN / OUTSIDE", 1, 1,
            selectedAudioPanel == 1, width))
            select_audio_panel(1);
        ImGui::SameLine(0.0f, gap);
        if (audio_feature_card("filters", "FILTERS", 2, 2,
            selectedAudioPanel == 2, width))
            select_audio_panel(2);
    }

    void draw_media(screen_t& screen)
    {
        text_with_font(2, ImVec4(0.93f, 0.97f, 0.99f, 1.0f), "MEDIA SOURCE");
        ImGui::TextDisabled("Each display has an isolated player, browser profile and audio session.");
        ImGui::Dummy(ImVec2(0.0f, 4.0f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0.0f, 4.0f));

        if (g_screens.size() > 1)
        {
            int targetIndex = 0;
            for (size_t index = 0; index < g_screens.size(); ++index)
            {
                if (g_screens[index].hotkeyTarget)
                {
                    targetIndex = static_cast<int>(index);
                    break;
                }
            }
            std::vector<std::string> targetLabels;
            std::vector<const char*> targetLabelPointers;
            targetLabels.reserve(g_screens.size());
            targetLabelPointers.reserve(g_screens.size());
            for (size_t index = 0; index < g_screens.size(); ++index)
            {
                const char* kind = screen_type_label(g_screens[index].type);
                targetLabels.push_back(
                    std::string(kind) + " " +
                    std::to_string(screen_type_ordinal(index)));
            }
            for (const auto& label : targetLabels)
                targetLabelPointers.push_back(label.c_str());

            ImGui::TextDisabled("MEDIA KEY TARGET");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo(
                    "##media_key_target", &targetIndex,
                    targetLabelPointers.data(),
                    static_cast<int>(targetLabelPointers.size())))
            {
                for (auto& candidate : g_screens)
                    candidate.hotkeyTarget = false;
                g_screens[static_cast<size_t>(targetIndex)].hotkeyTarget = true;
                mark_changed();
            }
            explain_last_item(
                "Media key target",
                "Routes keyboard, Steam Input and gamepad media commands to one display without interrupting the others.",
                "No playback restart", "Applies instantly");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }

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
        explain_last_item("Pause / Freeze", "Keeps the last image, pauses media audio/video, and stops plugin frame processing. Unfreezing restores the prior play/pause intent.", "Reduces resource use", "Applies instantly");

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
                {
                    screen.mediaService = static_cast<media_service_t>(service);
                    auto& urls = screen.mediaService == media_service_t::YOUTUBE
                        ? screen.youtubeUrls : screen.spotifyUrls;
                    uint32_t& selected = screen.mediaService == media_service_t::YOUTUBE
                        ? screen.selectedYoutubeUrl : screen.selectedSpotifyUrl;
                    if (!urls.empty())
                    {
                        selected = (std::min)(selected,
                            static_cast<uint32_t>(urls.size() - 1));
                        screen.mediaUrl = urls[selected];
                    }
                    else
                        screen.mediaUrl.clear();
                    rebuild_source(screen);
                    mark_changed();
                }
                explain_last_item("Media service", "YouTube uses the focused player; Spotify always uses the official full web player.", "Low/medium");
                if (!sources::IsMediaClientInstalled())
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "PrismMediaClient.exe is missing.");

                auto& mediaUrls = screen.mediaService == media_service_t::YOUTUBE
                    ? screen.youtubeUrls : screen.spotifyUrls;
                uint32_t& selectedMediaUrl = screen.mediaService == media_service_t::YOUTUBE
                    ? screen.selectedYoutubeUrl : screen.selectedSpotifyUrl;
                if (!mediaUrls.empty())
                    selectedMediaUrl = (std::min)(selectedMediaUrl,
                        static_cast<uint32_t>(mediaUrls.size() - 1));

                ImGui::Dummy(ImVec2(0.0f, 3.0f));
                ImGui::TextDisabled("SAVED PLAYLISTS / LINKS");
                const char* savedPreview = mediaUrls.empty()
                    ? "No saved links yet"
                    : mediaUrls[selectedMediaUrl].c_str();
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##saved_media", savedPreview))
                {
                    for (size_t index = 0; index < mediaUrls.size(); ++index)
                    {
                        const std::string label = std::to_string(index + 1) +
                            ".  " + mediaUrls[index] + "##saved_" +
                            std::to_string(index);
                        const bool selected = index == selectedMediaUrl;
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            selectedMediaUrl = static_cast<uint32_t>(index);
                            screen.mediaUrl = mediaUrls[index];
                            if (!screen.source ||
                                !screen.source->LoadMedia(screen.mediaUrl))
                                rebuild_source(screen);
                            mark_changed();
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
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
            if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA)
            {
                auto& mediaUrls = screen.mediaService == media_service_t::YOUTUBE
                    ? screen.youtubeUrls : screen.spotifyUrls;
                uint32_t& selectedMediaUrl = screen.mediaService == media_service_t::YOUTUBE
                    ? screen.selectedYoutubeUrl : screen.selectedSpotifyUrl;
                const float libraryButtonWidth =
                    (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
                if (ImGui::Button("+ Add link", ImVec2(libraryButtonWidth, 0.0f)) &&
                    !screen.mediaUrl.empty())
                {
                    const auto existing = std::find(
                        mediaUrls.begin(), mediaUrls.end(), screen.mediaUrl);
                    if (existing == mediaUrls.end())
                    {
                        mediaUrls.push_back(screen.mediaUrl);
                        selectedMediaUrl = static_cast<uint32_t>(mediaUrls.size() - 1);
                    }
                    else
                        selectedMediaUrl = static_cast<uint32_t>(
                            std::distance(mediaUrls.begin(), existing));
                    mark_changed();
                }
                explain_last_item("Add saved link", "Stores this URL for quick switching. It does not reload the game or start another background player.");
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::BeginDisabled(mediaUrls.empty() || screen.mediaUrl.empty());
                if (ImGui::Button("Update selected", ImVec2(libraryButtonWidth, 0.0f)))
                {
                    selectedMediaUrl = (std::min)(selectedMediaUrl,
                        static_cast<uint32_t>(mediaUrls.size() - 1));
                    mediaUrls[selectedMediaUrl] = screen.mediaUrl;
                    mark_changed();
                }
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::BeginDisabled(mediaUrls.empty());
                if (ImGui::Button("Remove selected", ImVec2(-1.0f, 0.0f)))
                {
                    selectedMediaUrl = (std::min)(selectedMediaUrl,
                        static_cast<uint32_t>(mediaUrls.size() - 1));
                    mediaUrls.erase(mediaUrls.begin() + selectedMediaUrl);
                    if (mediaUrls.empty())
                    {
                        selectedMediaUrl = 0;
                        screen.mediaUrl.clear();
                    }
                    else
                    {
                        selectedMediaUrl = (std::min)(selectedMediaUrl,
                            static_cast<uint32_t>(mediaUrls.size() - 1));
                        screen.mediaUrl = mediaUrls[selectedMediaUrl];
                    }
                    mark_changed();
                }
                ImGui::EndDisabled();
                ImGui::TextDisabled("%zu saved %s link%s", mediaUrls.size(),
                    screen.mediaService == media_service_t::YOUTUBE
                        ? "YouTube" : "Spotify",
                    mediaUrls.size() == 1 ? "" : "s");
            }
            if (screen.source && screen.source->SupportsMediaControls())
            {
                ImGui::Text("Status: %s", screen.source->GetStatusText().c_str());
                const float transportGap = 8.0f;
                const float transportWidth =
                    (ImGui::GetContentRegionAvail().x - transportGap * 5.0f) / 6.0f;
                if (transport_button("play_pause", "Play / Pause", 0, transportWidth))
                    dispatch_media_command(screen, media_command_t::PLAY_PAUSE);
                explain_last_item("Play / Pause", "Uses a state-neutral transport control, so a stale icon cannot claim the wrong state.");
                ImGui::SameLine(0.0f, transportGap);
                if (transport_button("previous", "Previous", 1, transportWidth))
                    dispatch_media_command(screen, media_command_t::PREVIOUS);
                ImGui::SameLine(0.0f, transportGap);
                if (transport_button("next", "Next", 2, transportWidth))
                    dispatch_media_command(screen, media_command_t::NEXT);
                ImGui::SameLine(0.0f, transportGap);
                if (transport_button("mute", "Mute", 3, transportWidth))
                    dispatch_media_command(screen, media_command_t::MUTE);
                ImGui::SameLine(0.0f, transportGap);
                if (transport_button("volume_down", "Volume -", 4, transportWidth))
                    dispatch_media_command(screen, media_command_t::VOLUME_DOWN);
                ImGui::SameLine(0.0f, transportGap);
                if (transport_button("volume_up", "Volume +", 5, transportWidth))
                    dispatch_media_command(screen, media_command_t::VOLUME_UP);

                bool hotkeyTarget = screen.hotkeyTarget;
                if (toggle_switch("Use this display for media keys", hotkeyTarget))
                {
                    if (hotkeyTarget)
                        for (auto& other : g_screens) other.hotkeyTarget = false;
                    screen.hotkeyTarget = hotkeyTarget;
                    mark_changed();
                }
            }
            if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA)
            {
                if (ImGui::Button("Open client"))
                {
                    if ((!screen.source && rebuild_source(screen)) ||
                        screen.source)
                        screen.source->ShowInteractivePlayer(true);
                }
                explain_last_item(
                    "Open client",
                    "Starts this display's client if it was fully closed, then shows its interactive media window.",
                    "No game reload");
                ImGui::SameLine();
                ImGui::BeginDisabled(!screen.source);
                if (ImGui::Button("Hide client"))
                    screen.source->ShowInteractivePlayer(false);
                explain_last_item(
                    "Hide client",
                    "Closes the visible helper window without stopping its in-game video or audio.",
                    "Playback continues");
                ImGui::SameLine();
                const bool closeClient = ImGui::Button("Close client");
                explain_last_item(
                    "Close client",
                    "Terminates this display's helper, capture and audio session and releases their resources. Open client starts it again.",
                    "Playback stops", "Session-only; no game reload");
                if (screen.mediaService == media_service_t::SPOTIFY &&
                    screen.source)
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Spotify session"))
                        screen.source->ClearBrowserSession();
                }
                ImGui::EndDisabled();
                if (closeClient && screen.source)
                {
                    const std::string closedClientId = screen.mediaClientId;
                    screen.source.reset();
                    reset_source_stats(screen);
                    diagnostic_log::writef(
                        "media",
                        "Media client %s was completely closed by the user; "
                        "capture and audio resources were released.",
                        closedClientId.c_str());
                }
                if (!screen.source)
                    ImGui::TextDisabled(
                        screen.mediaUrl.empty()
                            ? "Add a media URL before opening the client."
                            : "Client closed - Open client restarts this display.");
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
        if (selectedAudioPanel == 1)
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

        if (selectedAudioPanel == 0)
        {
            ImGui::TextDisabled("LEVELS AND POSITION");
            if (slider_percent("Cabin volume", screen.adaptiveAudioInteriorVolume)) mark_changed();
            explain_last_item("Cabin volume", "Full-volume anchor when the camera is inside the truck.");
            if (slider_percent("Menu volume", screen.adaptiveAudioMenuVolume)) mark_changed();
            explain_last_item("Menu volume", "Media level while the game is in menus or before driving.");
            if (slider_percent("Spatial strength", screen.adaptiveAudioStrength)) mark_changed();
            explain_last_item("Spatial strength", "Controls how strongly left/right camera position moves the media sound.");
            if (slider_value("Speaker direction",
                screen.adaptiveAudioSpeakerAzimuth,
                -90.0f, 90.0f, "%.0f deg")) mark_changed();
            if (slider_percent("Facing-away floor", screen.adaptiveAudioFacingAwayVolume)) mark_changed();
        }
        else if (selectedAudioPanel == 1)
        {
            ImGui::TextDisabled("CABIN / OUTSIDE TRANSITION");
            if (slider_value("Outside-cab distance",
                screen.adaptiveAudioOutsideDistance,
                0.25f, 2.5f, "%.2f m")) mark_changed();
            if (slider_percent("Minimum volume when far away", screen.adaptiveAudioOutsideVolume)) mark_changed();
            explain_last_item("Exterior volume", "Target volume outside before distance attenuation.", "Very low CPU", "Smooth 650 ms crossfade");
            if (toggle_switch("Exterior distance filter", screen.adaptiveAudioExternalDistanceEnabled)) mark_changed();
            ImGui::BeginDisabled(!screen.adaptiveAudioExternalDistanceEnabled);
            if (slider_percent("Near exterior volume", screen.adaptiveAudioExternalNearVolume)) mark_changed();
            if (slider_value("Full-volume distance",
                screen.adaptiveAudioExternalFullVolumeDistance,
                0.0f, 10.0f, "%.1f m")) mark_changed();
            if (slider_value("Mute distance",
                screen.adaptiveAudioExternalMuteDistance,
                2.0f, 50.0f, "%.1f m")) mark_changed();
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::TextDisabled("EXTERIOR TONE FILTER");
            if (toggle_switch("Exterior distance filter", screen.adaptiveAudioExternalDistanceEnabled)) mark_changed();
            ImGui::BeginDisabled(!screen.adaptiveAudioExternalDistanceEnabled);
            if (slider_value("Near cutoff",
                screen.adaptiveAudioExternalNearCutoff,
                20.0f, 20000.0f, "%.0f Hz",
                ImGuiSliderFlags_Logarithmic |
                ImGuiSliderFlags_AlwaysClamp)) mark_changed();
            if (toggle_switch("Smooth low-pass", screen.adaptiveAudioExternalLowPassEnabled)) mark_changed();
            ImGui::BeginDisabled(!screen.adaptiveAudioExternalLowPassEnabled);
            if (slider_value("Minimum cutoff",
                screen.adaptiveAudioExternalMinimumCutoff, 20.0f,
                (std::max)(20.0f,
                    screen.adaptiveAudioExternalNearCutoff), "%.0f Hz",
                ImGuiSliderFlags_Logarithmic |
                ImGuiSliderFlags_AlwaysClamp)) mark_changed();
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        ImGui::EndDisabled();
        ImGui::EndDisabled();

        if (selectedAudioPanel == 1 && spatialSupported && screen.adaptiveAudioEnabled)
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

        if (selectedAudioPanel == 2)
        {
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
    }

    void draw_controls()
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "MOUSE + GAMEPAD");
        ImGui::TextWrapped("D-pad or left stick moves, A activates, B goes back, LB/RB changes pages, and LT/RT switches the Audio tabs.");
        if (toggle_switch("Enable gamepad controls", g_gamepad_hotkeys_enabled)) mark_changed();
        const char* controllers[] = { "Automatic", "Controller 1", "Controller 2", "Controller 3", "Controller 4" };
        int controller = g_gamepad_controller_index + 1;
        if (ImGui::Combo("Controller", &controller, controllers, IM_ARRAYSIZE(controllers))) { g_gamepad_controller_index = controller - 1; mark_changed(); }
        if (slider_value("Media-binding stick threshold",
            g_gamepad_axis_threshold, 0.20f, 0.95f, "%.2f"))
            mark_changed();
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

    void begin_settings_card(const char* id, const char* title,
        const char* subtitle, float height, ImU32 accent)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const ImVec2 end(start.x + ImGui::GetContentRegionAvail().x,
            start.y + height);
        draw_card_shadow(ImGui::GetWindowDrawList(), start, end, 13.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 16.0f));
        ImGui::BeginChild(id, ImVec2(0.0f, height), ImGuiChildFlags_Borders,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushFont(ui_font(1));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "%s", title);
        ImGui::PopFont();
        ImGui::TextDisabled("%s", subtitle);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    void end_settings_card()
    {
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void draw_appearance(screen_t& screen)
    {
        const float gap = 14.0f;
        const float columnWidth = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (ImGui::BeginTable("appearance_cards", 2,
            ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("lighting", ImGuiTableColumnFlags_WidthFixed,
                columnWidth);
            ImGui::TableSetupColumn("geometry", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            begin_settings_card("appearance_lighting", "ADAPTIVE LIGHTING",
                "Natural phone-style response without abrupt steps", 360.0f,
                kCyan);
            if (slider_percent("Screen brightness", screen.brightness,
                10.0f, 200.0f))
            {
                apply_brightness(screen);
                mark_changed();
            }
            explain_last_item("Brightness", "Sets the base display level and applies immediately when adjusted manually.", "Negligible", "Instant + auto-save");
            if (toggle_switch("Automatic game-light adaptation",
                screen.autoBrightnessEnabled))
            {
                if (screen.autoBrightnessEnabled)
                {
                    screen.effectiveBrightness = screen.brightness;
                    screen.brightnessLastAdjustmentTick = GetTickCount64();
                    if (screen.source && screen.source->SupportsSourceBrightness())
                        screen.source->SetSourceBrightness(screen.effectiveBrightness);
                }
                else
                    apply_brightness(screen);
                mark_changed();
            }
            explain_last_item("Automatic brightness", "Samples game lighting at half the GPS frame rate, then adapts progressively like a phone: about 2.5 seconds to brighten and 4 seconds to dim.", "Negligible", "Smooth live adaptation + auto-save");
            ImGui::BeginDisabled(!screen.autoBrightnessEnabled);
            if (slider_percent("Dark-scene multiplier",
                screen.autoBrightnessDarkMultiplier, 25.0f, 125.0f))
                mark_changed();
            if (slider_percent("Bright-scene multiplier",
                screen.autoBrightnessBrightMultiplier, 50.0f, 200.0f))
                mark_changed();
            ImGui::EndDisabled();
            end_settings_card();

            ImGui::TableNextColumn();
            begin_settings_card("appearance_geometry", "IMAGE FIT",
                "Alignment and texture-edge correction", 360.0f,
                IM_COL32(183, 112, 255, 255));
            const char* scales[] = { "Stretch", "Fit", "Crop" };
            int scale = static_cast<int>(screen.scaleMode);
            ImGui::TextDisabled("SCALING MODE");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##appearance_scaling", &scale, scales,
                IM_ARRAYSIZE(scales)))
            {
                screen.scaleMode = static_cast<scale_mode_t>(scale);
                screen.hasUploadedFrame = false;
                mark_changed();
            }
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            int guard = screen.edgeBleedGuard;
            ImGui::TextDisabled("EDGE GUARD");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::SliderInt("##appearance_guard", &guard, 0, 16,
                "%d px"))
            {
                screen.edgeBleedGuard = static_cast<uint8_t>(guard);
                screen.hasUploadedFrame = false;
                mark_changed();
            }
            ImGui::Dummy(ImVec2(0.0f, 9.0f));
            if (toggle_switch("Flip vertically", screen.flipVertical))
            {
                screen.hasUploadedFrame = false;
                mark_changed();
            }
            ImGui::Separator();
            ImGui::TextDisabled("OUTPUT");
            ImGui::Text("%u x %u at %u FPS", screen.targetLiveTextureWidth,
                screen.targetLiveTextureHeight,
                static_cast<unsigned>(screen.framerate));
            ImGui::TextDisabled("Changes apply to the selected display only.");
            end_settings_card();
            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        begin_settings_card("appearance_sensor", "LIVE LIGHT SENSOR",
            "Half-rate sampling with continuous visual adaptation", 112.0f,
            kGreen);
        if (g_game_lighting_valid.load())
        {
            ImGui::Text("Scene %.0f%%   |   Effective display %.0f%%   |   Sensor %u Hz",
                g_game_lighting_luminance.load() * 100.0f,
                screen.effectiveBrightness * 100.0f,
                g_auto_brightness_sample_hz.load());
            ImGui::ProgressBar((std::clamp)(screen.effectiveBrightness / 2.0f,
                0.0f, 1.0f), ImVec2(-1.0f, 16.0f), "");
        }
        else
            ImGui::TextDisabled("Waiting for the first game-lighting sample...");
        end_settings_card();
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
        screen.textureRouteArmed = false;
        screen.textureRouteArmedTick = 0;
    }

    std::string screen_owner_name(const screen_t& screen)
    {
        size_t ordinal{};
        for (const auto& candidate : g_screens)
        {
            if (candidate.type == screen.type)
                ++ordinal;
            if (&candidate == &screen)
                break;
        }
        return std::string(screen_type_label(screen.type)) + " " +
            std::to_string((std::max)(size_t{ 1 }, ordinal));
    }

    void log_route_snapshot(const screen_t& screen, const char* reason)
    {
        diagnostic_log::writef(
            "route",
            "Snapshot (%s): display=%s id=%s enabled=%d source=%d "
            "original='%s' override='%s' identity=%ux%u armed=%d "
            "route=%llu matched=%llu live=%p redirect_tick=%llu "
            "match_tick=%llu.",
            reason ? reason : "manual",
            screen_owner_name(screen).c_str(),
            screen.mediaClientId.c_str(),
            screen.enabled ? 1 : 0,
            screen.source ? 1 : 0,
            screen.original_texture.c_str(),
            screen.override_texture.c_str(),
            screen.override_texture_size_w,
            screen.override_texture_size_h,
            screen.textureRouteArmed ? 1 : 0,
            static_cast<unsigned long long>(screen.textureRouteSequence),
            static_cast<unsigned long long>(
                screen.textureRouteMatchedSequence),
            static_cast<void*>(screen.liveTexture),
            static_cast<unsigned long long>(screen.lastTextureRedirectTick),
            static_cast<unsigned long long>(screen.lastTextureMatchTick));
    }

    bool run_early_custom_probe_reload(
        const char* snapshotReason,
        bool requireNewDiagnostic)
    {
        diagnostic_log::write(
            "route",
            requireNewDiagnostic
                ? "Preparing the combined one-cycle targeted test before "
                  "executing the truck-texture reload command."
                : "Preparing a normal truck-texture reload without arming "
                  "the one-shot diagnostic.");
        for (const auto& candidate : g_screens)
            log_route_snapshot(candidate, snapshotReason);

        bool diagnosticPrepared = false;
        if (requireNewDiagnostic)
        {
            for (const auto& candidate : g_screens)
            {
                if (candidate.enabled &&
                    candidate.type == screen_type_t::CUSTOM &&
                    !candidate.original_texture.empty() &&
                    candidate.liveTexture)
                {
                    diagnosticPrepared =
                        custom_render_probe::prepare_capture(
                            candidate.mediaClientId.c_str(),
                            candidate.original_texture.c_str(),
                            candidate.liveTexture);
                    if (diagnosticPrepared)
                        break;
                }
            }
        }
        if (requireNewDiagnostic && !diagnosticPrepared)
        {
            diagnostic_log::write(
                "probe",
                "Early capture was not armed (already completed/active, or "
                "no enabled custom display has an active live texture)." );
            return false;
        }

        prism::string command("game");
        if (!prism::execute_command::call(&command, -1))
        {
            diagnostic_log::write(
                "error",
                diagnosticPrepared
                    ? "Game rejected the truck-texture reload command after "
                      "the list-entry probe was armed. It remains bounded "
                      "and will restore the branch automatically."
                    : "Game rejected the normal truck-texture reload "
                      "command.");
            return false;
        }

        diagnostic_log::write(
            "route",
            diagnosticPrepared
                ? "Game accepted the truck-texture reload command; the "
                  "combined list/cleanup/Release probe is active before "
                  "cleanup."
                : "Game accepted the normal truck-texture reload command.");
        return true;
    }

    bool prepare_override_assets(screen_t& screen, std::string& status)
    {
        std::string originalLower;
        originalLower.reserve(screen.original_texture.size());
        for (const unsigned char character : screen.original_texture)
            originalLower.push_back(
                static_cast<char>(std::tolower(character)));
        if (originalLower.rfind("/home/prismtexturestreamer/", 0) == 0)
        {
            status = "Select the truck/accessory's game TOBJ, not a generated "
                "PrismMedia override file.";
            diagnostic_log::writef(
                "error",
                "Rejected generated override %s as an input for display %s.",
                screen.original_texture.c_str(),
                screen.mediaClientId.c_str());
            return false;
        }
        if (const screen_t* owner = texture_owner_other_than(
                screen, screen.original_texture))
        {
            status = "Cannot prepare an independent display: " +
                screen_owner_name(*owner) + " already owns " +
                screen.original_texture +
                ". Select the tablet/accessory's unique TOBJ first.";
            return false;
        }
        std::vector<std::pair<uint32_t, uint32_t>> usedDimensions;
        std::vector<std::string> usedOverridePaths;
        bool identityConflict{};
        usedDimensions.reserve(g_screens.size());
        usedOverridePaths.reserve(g_screens.size());
        for (const auto& other : g_screens)
        {
            if (&other == &screen)
                continue;
            usedDimensions.emplace_back(
                other.override_texture_size_w,
                other.override_texture_size_h);
            usedOverridePaths.push_back(other.override_texture);
            identityConflict = identityConflict ||
                other.override_texture == screen.override_texture ||
                (other.override_texture_size_w ==
                    screen.override_texture_size_w &&
                 other.override_texture_size_h ==
                    screen.override_texture_size_h);
        }
        return override_assets::ensure(
            screen, usedDimensions, usedOverridePaths,
            identityConflict, status);
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
        struct performance_snapshot_t
        {
            const screen_t* owner{};
            source_performance_stats_t stats{};
            std::string status{ "Waiting for source" };
            double gameFps{};
            double uploadCpuMs{};
            double totalPluginCpuMs{};
            double deliveredFps{};
            uint64_t uploadedFrames{};
            double environmentCpuUs{};
            DWORD cpu0{ thread_scheduling::kUnassignedProcessor };
            DWORD cpu1{ thread_scheduling::kUnassignedProcessor };
            DWORD cpu2{ thread_scheduling::kUnassignedProcessor };
            DWORD processorCount{};
            std::array<uint32_t,
                thread_scheduling::kTrackedProcessors> gameHits{};
            std::array<uint32_t,
                thread_scheduling::kTrackedProcessors> pluginHits{};
            uint64_t lastRefreshTick{};
            uint64_t lastProcessorRefreshTick{};
        };
        static performance_snapshot_t snapshot{};
        const uint64_t monitorNow = GetTickCount64();
        if (snapshot.owner != &screen)
        {
            snapshot = performance_snapshot_t{};
            snapshot.owner = &screen;
            snapshot.status = "Waiting for source";
        }
        // Query live source data at 4 Hz. Rendering the monitor must not turn
        // source atomics/status calls into game-frame work at 60-144 Hz.
        if (snapshot.lastRefreshTick == 0 ||
            monitorNow - snapshot.lastRefreshTick >= 250)
        {
            const uint64_t elapsed = snapshot.lastRefreshTick == 0
                ? 250 : monitorNow - snapshot.lastRefreshTick;
            snapshot.lastRefreshTick = monitorNow;
            snapshot.stats = screen.source
                ? screen.source->GetPerformanceStats()
                : source_performance_stats_t{};
            snapshot.status = screen.source
                ? screen.source->GetStatusText() : "Waiting for source";
            snapshot.gameFps = ImGui::GetIO().Framerate;
            snapshot.uploadCpuMs = screen.uploadCpuMs;
            snapshot.totalPluginCpuMs = screen.totalPluginCpuMs;
            snapshot.deliveredFps = snapshot.stats.deliveredFps > 0.0
                ? snapshot.stats.deliveredFps : screen.deliveredFps;
            snapshot.uploadedFrames = screen.uploadedFrames;
            snapshot.environmentCpuUs =
                g_environment_update_cpu_us.load();

            const double observedFrameMs = snapshot.gameFps > 0.1
                ? 1000.0 / snapshot.gameFps : 0.0;
            if (observedFrameMs > 0.0)
            {
                // Only measured render-thread upload work is allowed into
                // this estimate. The monitor's own UI draw is not attributed
                // to texture streaming.
                const double withoutUploadMs = (std::max)(
                    0.1, observedFrameMs - snapshot.uploadCpuMs);
                const double instantaneousLoss = (std::max)(0.0,
                    1000.0 / withoutUploadMs - snapshot.gameFps);
                const double smoothing = 1.0 - std::exp(
                    -static_cast<double>(elapsed) / 2500.0);
                screen.estimatedFpsLoss = screen.estimatedFpsLoss == 0.0
                    ? instantaneousLoss
                    : screen.estimatedFpsLoss * (1.0 - smoothing) +
                        instantaneousLoss * smoothing;
            }
        }
        if (snapshot.lastProcessorRefreshTick == 0 ||
            monitorNow - snapshot.lastProcessorRefreshTick >= 500)
        {
            snapshot.lastProcessorRefreshTick = monitorNow;
            snapshot.cpu0 = thread_scheduling::preferred_processor(0);
            snapshot.cpu1 = thread_scheduling::preferred_processor(1);
            snapshot.cpu2 = thread_scheduling::preferred_processor(2);
            snapshot.processorCount =
                thread_scheduling::tracked_processor_count();
            for (DWORD processor = 0;
                processor < snapshot.processorCount; ++processor)
            {
                snapshot.gameHits[processor] =
                    thread_scheduling::sampled_game_hits(processor);
                snapshot.pluginHits[processor] =
                    thread_scheduling::sampled_plugin_hits(processor);
            }
        }

        ImGui::Text("Source: %s", snapshot.status.c_str());
        if (screen.suspiciousBlackFrame)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.34f, 0.24f, 1.0f),
                "Capture fault: arriving frames are persistently black.");
            ImGui::TextWrapped(
                "Restart the selected media source; do not repair the "
                "display TOBJ because the game texture target is not the "
                "cause of this fault.");
        }
        else if (screen.suspiciousMagentaFrame)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.34f, 0.64f, 1.0f),
                "Capture fault: arriving frames are predominantly magenta.");
        }
        else if (screen.sourceFrameStale)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Capture warning: the source is not delivering new frames.");
        }
        if (ImGui::CollapsingHeader("Live performance", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Current game FPS: %.1f", snapshot.gameFps);
            const ImVec4 lossColour = screen.estimatedFpsLoss < 1.0 ? ImVec4(0.35f, 0.90f, 0.40f, 1.0f) :
                (screen.estimatedFpsLoss < 3.0 ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
            ImGui::TextColored(lossColour,
                "Estimated game-thread FPS cost: %.2f",
                screen.estimatedFpsLoss);
            ImGui::Text("Game-thread texture upload: %.3f ms", snapshot.uploadCpuMs);
            ImGui::Text("Source worker CPU: %.3f ms/frame", snapshot.stats.workerCpuMs);
            ImGui::Text("Total measured plugin CPU: %.3f ms/frame", snapshot.totalPluginCpuMs);
            ImGui::Text("GPU readback portion: %.3f ms/frame", snapshot.stats.readbackMs);
            ImGui::Text("Delivered source rate: %.1f FPS", snapshot.deliveredFps);
            ImGui::Text("Dropped/overloaded frames: %llu",
                static_cast<unsigned long long>(snapshot.stats.droppedFrames));
            ImGui::Text("Frames uploaded to game: %llu",
                static_cast<unsigned long long>(snapshot.uploadedFrames));
            ImGui::Text("Hardware decode requested: %s", snapshot.stats.hardwareDecoded ? "Yes" : "No / external");
            ImGui::Text("Window capture bypassed: %s", snapshot.stats.directMedia ? "Yes" : "No");
            ImGui::Text("Monitor refresh: 4 Hz | environment %.1f us/update",
                snapshot.environmentCpuUs);
            if (snapshot.cpu0 != thread_scheduling::kUnassignedProcessor)
            {
                DWORD cpu1 = snapshot.cpu1 ==
                    thread_scheduling::kUnassignedProcessor
                    ? snapshot.cpu0 : snapshot.cpu1;
                DWORD cpu2 = snapshot.cpu2 ==
                    thread_scheduling::kUnassignedProcessor
                    ? cpu1 : snapshot.cpu2;
                ImGui::Text("Background CPU hints: LP %lu, %lu, %lu",
                    static_cast<unsigned long>(snapshot.cpu0),
                    static_cast<unsigned long>(cpu1),
                    static_cast<unsigned long>(cpu2));
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
                for (DWORD processor = 0;
                    processor < snapshot.processorCount; ++processor)
                {
                    const uint32_t gameHits = snapshot.gameHits[processor];
                    const uint32_t pluginHits = snapshot.pluginHits[processor];
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

        if (ImGui::CollapsingHeader(
                "Display discovery", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static bool showAllLoadedTextures{};
            static bool discoveryStarted{};
            static int selectedDiscoveredTexture = -1;
            static std::string discoveryScreenId;
            static std::string overrideAssetStatus;
            static uint64_t lastDiscoveryRefreshTick{};
            static std::vector<
                prism::memserver_texture_queue::discovered_texture_t>
                discoveredTextures;

            if (discoveryScreenId != screen.mediaClientId)
            {
                discoveryScreenId = screen.mediaClientId;
                selectedDiscoveredTexture = -1;
            }

            const uint64_t discoveryNow = GetTickCount64();
            if (lastDiscoveryRefreshTick == 0 ||
                discoveryNow - lastDiscoveryRefreshTick >= 250)
            {
                lastDiscoveryRefreshTick = discoveryNow;
                discoveredTextures = prism::memserver_texture_queue::
                    discovered_textures();
                if (selectedDiscoveredTexture >=
                    static_cast<int>(discoveredTextures.size()))
                    selectedDiscoveredTexture = -1;
                if (selectedDiscoveredTexture < 0)
                {
                    for (size_t index = 0;
                        index < discoveredTextures.size(); ++index)
                    {
                        if (discoveredTextures[index].path ==
                            screen.original_texture)
                        {
                            selectedDiscoveredTexture =
                                static_cast<int>(index);
                            break;
                        }
                    }
                }
            }

            ImGui::TextWrapped(
                "Lists TOBJ textures observed from the truck and accessories. "
                "Observation is passive and never reloads or changes an "
                "unconfirmed configurator order.");
            if (ImGui::Button(
                    "Observe display textures for 30 seconds",
                    ImVec2(-1.0f, 36.0f)))
            {
                prism::memserver_texture_queue::begin_display_discovery();
                selectedDiscoveredTexture = -1;
                discoveryStarted = true;
            }
            explain_last_item(
                "Observe current truck displays",
                "Keeps the existing passive history and marks textures used while you switch the accessory or infotainment display.",
                "Negligible for 30 seconds", "No game reload");

            if (prism::memserver_texture_queue::display_discovery_active())
                ImGui::TextColored(
                    ImVec4(0.20f, 0.88f, 0.94f, 1.0f),
                    "Observing... switch the installed accessory/display mode");

            size_t likelyCount{};
            size_t currentCount{};
            for (const auto& candidate : discoveredTextures)
            {
                likelyCount += candidate.likely_display ? 1U : 0U;
                currentCount += candidate.seen_during_current_scan ? 1U : 0U;
            }
            ImGui::TextDisabled(
                "%zu observed in scan | %zu likely display%s | %zu total TOBJ%s",
                currentCount,
                likelyCount, likelyCount == 1 ? "" : "s",
                discoveredTextures.size(),
                discoveredTextures.size() == 1 ? "" : "s");
            ImGui::Checkbox(
                "Show all loaded TOBJ files", &showAllLoadedTextures);

            if (ImGui::BeginListBox(
                    "##discovered_tobjs", ImVec2(-1.0f, 174.0f)))
            {
                bool anyVisible{};
                for (size_t index = 0;
                    index < discoveredTextures.size(); ++index)
                {
                    const auto& candidate = discoveredTextures[index];
                    if (!showAllLoadedTextures &&
                        ((!candidate.likely_display &&
                          !candidate.seen_during_current_scan) ||
                         candidate.unsafe_candidate))
                        continue;
                    anyVisible = true;
                    ImGui::PushID(static_cast<int>(index));
                    const std::string label =
                        std::string(candidate.seen_during_current_scan
                            ? "[SCAN] " : "[HISTORY] ") +
                        (candidate.unsafe_candidate
                            ? "[UNSAFE]  "
                            : candidate.likely_display
                                ? "[DISPLAY]  " : "[OTHER]  ") +
                        candidate.path;
                    if (ImGui::Selectable(
                            label.c_str(),
                            selectedDiscoveredTexture ==
                                static_cast<int>(index)))
                        selectedDiscoveredTexture = static_cast<int>(index);
                    ImGui::PopID();
                }
                if (!anyVisible)
                {
                    ImGui::TextDisabled(
                        discoveryStarted
                            ? "No likely display paths yet. Switch the tablet screen, then enable 'Show all' only for expert inspection."
                            : "Start observation, then switch the installed accessory or its display mode.");
                }
                ImGui::EndListBox();
            }

            const bool validSelection = selectedDiscoveredTexture >= 0 &&
                selectedDiscoveredTexture <
                    static_cast<int>(discoveredTextures.size());
            const auto* selectedCandidate = validSelection
                ? &discoveredTextures[
                    static_cast<size_t>(selectedDiscoveredTexture)]
                : nullptr;
            const screen_t* prospectiveOwner = selectedCandidate
                ? texture_owner_other_than(screen, selectedCandidate->path)
                : nullptr;
            const bool assignmentSafe = selectedCandidate &&
                !selectedCandidate->unsafe_candidate && !prospectiveOwner;
            ImGui::BeginDisabled(!assignmentSafe);
            if (ImGui::Button(
                    "Use selected display texture",
                    ImVec2(-1.0f, 34.0f)) && assignmentSafe)
            {
                const std::string& selectedPath =
                    selectedCandidate->path;
                if (screen.original_texture != selectedPath)
                    screen.original_texture = selectedPath;
                if (prepare_override_assets(screen, overrideAssetStatus) &&
                    !screen.source && !screen.mediaUrl.empty())
                    rebuild_source(screen);
                mark_changed(true);
            }
            ImGui::EndDisabled();
            if (selectedCandidate && selectedCandidate->unsafe_candidate)
            {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.48f, 0.30f, 1.0f),
                    "Blocked: this is a world/traffic texture, not a safe "
                    "automatic display target.");
            }
            else if (prospectiveOwner)
            {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.48f, 0.30f, 1.0f),
                    "Shared texture: already owned by %s. Two surfaces that "
                    "use the same TOBJ can only show the same media.",
                    screen_owner_name(*prospectiveOwner).c_str());
            }
            ImGui::TextDisabled(
                "PrismMedia verifies or generates the matching TOBJ/DDS pair "
                "and preserves unique identity dimensions.");
            if (!overrideAssetStatus.empty())
                ImGui::TextWrapped("%s", overrideAssetStatus.c_str());
            if (ImGui::Button(
                    "Repair active display assets",
                    ImVec2(-1.0f, 36.0f)))
            {
                if (prepare_override_assets(screen, overrideAssetStatus))
                {
                    mark_changed(true);
                }
            }
            explain_last_item(
                "Repair display assets",
                "Regenerates the active GPS, dashboard or custom display's TOBJ/DDS override pair without touching the current truck state.",
                "Only while writing assets", "Reload remains a separate critical action");
        }

        if (const screen_t* owner = texture_owner_other_than(
                screen, screen.original_texture))
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.32f, 0.24f, 1.0f),
                "DISPLAY COLLISION: %s also owns %s",
                screen_owner_name(*owner).c_str(),
                screen.original_texture.c_str());
            ImGui::TextWrapped(
                "This display is isolated for media and audio, but Prism3D "
                "cannot route two different videos through one shared TOBJ. "
                "Choose a unique tablet/material TOBJ before reloading.");
        }

        if (ImGui::CollapsingHeader("Critical texture identity"))
        {
            if (ImGui::InputText("Game texture", &screen.original_texture))
            {
                if (texture_owner_other_than(
                        screen, screen.original_texture))
                {
                    screen.source.reset();
                    reset_source_stats(screen);
                }
                mark_changed(true);
            }
            explain_last_item("Game texture", "Changes which Prism3D texture is intercepted.", "Critical", "Requires texture reload");
            if (ImGui::InputText("Override texture", &screen.override_texture)) mark_changed(true);
            if (ImGui::InputScalar("Override width", ImGuiDataType_U32, &screen.override_texture_size_w)) mark_changed(true);
            if (ImGui::InputScalar("Override height", ImGuiDataType_U32, &screen.override_texture_size_h)) mark_changed(true);
        }
        if (criticalReloadPending)
        {
            static bool accessoryOrderConfirmed{};
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.28f, 1.0f), "Texture identity changed. Reload only when ready.");
            if (!g_telemetry_driving.load())
            {
                ImGui::Checkbox(
                    "The accessory order is confirmed/installed",
                    &accessoryOrderConfirmed);
                ImGui::TextDisabled(
                    "Reloading from the configurator cannot apply a pending "
                    "basket; confirm the order first.");
            }
            const bool reloadAllowed = g_telemetry_driving.load() ||
                accessoryOrderConfirmed;
            ImGui::BeginDisabled(!reloadAllowed);
            if (ImGui::Button(
                    "Reload installed truck textures", ImVec2(-1.0f, 34.0f)))
            {
                diagnostic_log::write(
                    "route", "User requested a truck-texture reload.");
                if (run_early_custom_probe_reload(
                        "before texture reload", false))
                {
                    criticalReloadPending = false;
                    accessoryOrderConfirmed = false;
                }
            }
            ImGui::EndDisabled();
            explain_last_item("Reload game textures", "Runs the game reload only after the accessory is installed and the texture identity is saved.", "Brief loading interruption", "Manual critical action");
        }

        ImGui::SeparatorText("Combined per-instance diagnostic");
        ImGui::TextWrapped(
            "Runs the list-entry, both native cleanup-call, exact COM "
            "Release and Direct3D draw checks in one reload. Only an exact "
            "match may be changed; the working fallback returns "
            "automatically.");
        ImGui::BeginDisabled(
            !g_telemetry_driving.load(std::memory_order_acquire));
        if (ImGui::Button(
                "Run combined one-cycle test", ImVec2(-1.0f, 36.0f)))
        {
            run_early_custom_probe_reload(
                "before diagnostic reload", true);
        }
        ImGui::EndDisabled();
        explain_last_item(
            "Combined custom-render test",
            "One reload arms the old texture, bounded list-entry probe, "
            "both cleanup calls, exact COM Release and Direct3D draw "
            "correlation.",
            "Temporary loading interruption", "One diagnostic per session");
        if (!g_telemetry_driving.load(std::memory_order_acquire))
            ImGui::TextDisabled("Enter the truck before running the diagnostic.");

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
        if (ImGui::Button(
                "Write display routing snapshot to log",
                ImVec2(-1.0f, 34.0f)))
        {
            log_route_snapshot(screen, "manual UI request");
        }
        explain_last_item(
            "Routing snapshot",
            "Writes this display's selected TOBJ, generated identity, "
            "pending route token and GPU match state to PrismMedia.log.",
            "Negligible", "Diagnostic only");
        ImGui::Separator();
        if (ImGui::Button("Remove this screen", ImVec2(-1.0f, 0.0f))) removeCurrent = true;
        explain_last_item("Remove screen", "Stops its source immediately and removes its saved configuration.", "Reduces resource use");
    }

    void add_screen(screen_type_t type)
    {
        screen_t screen; screen.type = type;
        screen.mediaClientId = sources::MakeMediaClientInstanceId();
        screen.hotkeyTarget = g_screens.empty();
        if (type == screen_type_t::GPS)
        { screen.original_texture = "/vehicle/truck/share/gps.tobj"; screen.override_texture = "/home/PrismTextureStreamer/gps.tobj"; screen.override_texture_size_w = 64; screen.override_texture_size_h = 2048; }
        else if (type == screen_type_t::DASHBOARD)
        { screen.original_texture = "/vehicle/truck/share/dashboard.tobj"; screen.override_texture = "/home/PrismTextureStreamer/dashboard.tobj"; screen.override_texture_size_w = 2048; screen.override_texture_size_h = 64; }
        else { screen.original_texture = ".tobj"; screen.override_texture = "/home/PrismTextureStreamer/.tobj"; }

        // A second generic GPS/dashboard path addresses the same Prism3D
        // resource, not a second physical surface. Start unassigned instead
        // of silently creating a collision that can blank the primary GPS.
        for (const auto& existing : g_screens)
        {
            if (existing.original_texture == screen.original_texture)
            {
                screen.original_texture.clear();
                break;
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> usedDimensions;
        std::vector<std::string> usedOverridePaths;
        bool identityConflict{};
        usedDimensions.reserve(g_screens.size());
        usedOverridePaths.reserve(g_screens.size());
        for (const auto& existing : g_screens)
        {
            usedDimensions.emplace_back(
                existing.override_texture_size_w,
                existing.override_texture_size_h);
            usedOverridePaths.push_back(existing.override_texture);
            identityConflict = identityConflict ||
                existing.override_texture == screen.override_texture ||
                (existing.override_texture_size_w ==
                    screen.override_texture_size_w &&
                 existing.override_texture_size_h ==
                    screen.override_texture_size_h);
        }
        std::string assetStatus;
        override_assets::ensure(
            screen, usedDimensions, usedOverridePaths,
            identityConflict, assetStatus);
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
        const ImVec2 availableArea(
            (std::max)(480.0f,
                viewport->WorkSize.x - kWindowEdgeMargin * 2.0f),
            (std::max)(420.0f,
                viewport->WorkSize.y - kWindowEdgeMargin * 2.0f));
        // Behave like a normal window. Choose a modest initial size and centre
        // only once; after that ImGui owns movement and resizing with no
        // per-frame reposition/resize manager.
        const ImVec2 wanted((std::min)(1360.0f, availableArea.x),
            (std::min)(780.0f, availableArea.y));
        ImGui::SetNextWindowPos(
            ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(wanted, ImGuiCond_FirstUseEver);
        const ImVec2 minimumSize(
            (std::min)(1040.0f, availableArea.x),
            (std::min)(680.0f, availableArea.y));
        const ImVec2 maximumSize(
            (std::max)(minimumSize.x, availableArea.x),
            (std::max)(minimumSize.y, availableArea.y));
        ImGui::SetNextWindowSizeConstraints(minimumSize, maximumSize);
        if (menuNeedsFocus)
        {
            ImGui::SetNextWindowFocus();
            menuNeedsFocus = false;
        }
        ImGuiStyle oldStyle = ImGui::GetStyle();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.WindowRounding = 22.0f;
        style.WindowBorderSize = 0.0f;
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
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBackground;
        if (ImGui::Begin("##PrismMediaConsole", nullptr, flags))
        {
            if (GetTickCount64() >= menuOpenedTick + 180)
            {
                if (ImGui::IsKeyPressed(
                    ImGuiKey_GamepadFaceRight, false))
                {
                    if (selectedPage == 0)
                        set_visible(false);
                    else
                    {
                        selectedPage = 0;
                        panelAnimation = 0.0f;
                    }
                }
                if (selectedPage == 2 && ImGui::IsKeyPressed(
                    ImGuiKey_GamepadL2, false))
                    select_audio_panel(selectedAudioPanel - 1);
                if (selectedPage == 2 && ImGui::IsKeyPressed(
                    ImGuiKey_GamepadR2, false))
                    select_audio_panel(selectedAudioPanel + 1);
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                {
                    selectedPage = (selectedPage + 5) % 6;
                    panelAnimation = 0.0f;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                {
                    selectedPage = (selectedPage + 1) % 6;
                    panelAnimation = 0.0f;
                }
            }
            const ImVec2 windowPos = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            const float headerHeight = 106.0f;
            const float footerHeight = 72.0f;
            const float mainTop = headerHeight + 14.0f;
            const float mainHeight = windowSize.y - headerHeight - footerHeight - 28.0f;
            const float leftWidth = (std::clamp)(windowSize.x * 0.185f, 235.0f, 292.0f);
            const float rightWidth = (std::clamp)(windowSize.x * 0.235f, 300.0f, 385.0f);
            const float gap = 14.0f;
            const float side = 20.0f;
            const float centreWidth = windowSize.x - side * 2.0f - leftWidth - rightWidth - gap * 2.0f;
            ImDrawList* shell = ImGui::GetWindowDrawList();
            shell->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(5, 13, 22, 255), 22.0f);
            shell->AddRect(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(61, 81, 98, 210), 22.0f, 0, 1.5f);
            shell->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + headerHeight),
                IM_COL32(9, 20, 31, 255), 22.0f, ImDrawFlags_RoundCornersTop);
            shell->AddLine(ImVec2(windowPos.x, windowPos.y + headerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + headerHeight), IM_COL32(45, 69, 86, 190));
            shell->AddRectFilled(ImVec2(windowPos.x, windowPos.y + windowSize.y - footerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                IM_COL32(8, 18, 27, 255), 22.0f, ImDrawFlags_RoundCornersBottom);
            shell->AddLine(ImVec2(windowPos.x, windowPos.y + windowSize.y - footerHeight),
                ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y - footerHeight), IM_COL32(45, 69, 86, 190));
            const ImVec2 grip(windowPos.x + windowSize.x - 15.0f,
                windowPos.y + windowSize.y - 15.0f);
            shell->AddLine(ImVec2(grip.x - 12.0f, grip.y + 8.0f),
                ImVec2(grip.x + 8.0f, grip.y - 12.0f), kCyan, 2.0f);
            shell->AddLine(ImVec2(grip.x - 5.0f, grip.y + 8.0f),
                ImVec2(grip.x + 8.0f, grip.y - 5.0f), kCyan, 2.0f);
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
            ImGui::TextColored(ImVec4(0.56f, 0.66f, 0.72f, 1.0f),
                "Drag to move  |  Resize from a corner  |  Changes save automatically");

            std::lock_guard<std::mutex> lock(g_screens_mutex);
            screen_t* statusScreen = nullptr;
            if (!g_screens.empty())
            {
                const int statusIndex = (std::clamp)(selectedScreen, 0, static_cast<int>(g_screens.size() - 1));
                statusScreen = &g_screens[static_cast<size_t>(statusIndex)];
            }
            static bool helperInstalled{};
            static uint64_t helperInstalledCheckedTick{};
            const uint64_t helperCheckNow = GetTickCount64();
            if (helperInstalledCheckedTick == 0 ||
                helperCheckNow - helperInstalledCheckedTick >= 2000)
            {
                helperInstalled = sources::IsMediaClientInstalled();
                helperInstalledCheckedTick = helperCheckNow;
            }
            const bool sourceOnline = statusScreen && statusScreen->source;
            const bool youtubeActive = sourceOnline && statusScreen->contentMode == content_mode_t::INTEGRATED_MEDIA &&
                statusScreen->mediaService == media_service_t::YOUTUBE;
            const bool spotifyActive = sourceOnline && statusScreen->contentMode == content_mode_t::INTEGRATED_MEDIA &&
                statusScreen->mediaService == media_service_t::SPOTIFY;
            const float statusWidth = (std::clamp)((windowSize.x - 620.0f) / 3.0f, 150.0f, 188.0f);
            constexpr float statusGap = 8.0f;
            constexpr float headerRightPadding = 30.0f;
            ImGui::SetCursorPos(ImVec2(windowSize.x -
                (statusWidth * 3.0f + statusGap * 2.0f +
                    headerRightPadding), 23.0f));
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
                ImGui::BeginChild("navigation", ImVec2(leftWidth, mainHeight),
                    ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PushFont(ui_font(1)); ImGui::TextDisabled("ACTIVE DISPLAY"); ImGui::PopFont();
                const char* activeKind = screen_type_label(
                    g_screens[static_cast<size_t>(selectedScreen)].type);
                const std::string activeLabel = std::string(activeKind) +
                    "  " + std::to_string(screen_type_ordinal(
                        static_cast<size_t>(selectedScreen))) +
                    (g_screens[static_cast<size_t>(selectedScreen)].enabled
                        ? "" : "  (off)");
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##active_display", activeLabel.c_str()))
                {
                for (int i = 0; i < static_cast<int>(g_screens.size()); ++i)
                {
                    const char* kind = screen_type_label(
                        g_screens[static_cast<size_t>(i)].type);
                    const std::string label = std::string(kind) + "  " +
                        std::to_string(screen_type_ordinal(
                            static_cast<size_t>(i))) +
                        (g_screens[static_cast<size_t>(i)].enabled
                            ? "" : "  (off)");
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
                const float navButtonHeight = (std::clamp)(
                    (mainHeight - 210.0f) / 6.0f, 42.0f, 62.0f);
                for (int i = 0; i < IM_ARRAYSIZE(pages); ++i)
                    if (nav_button(i, pages[i], selectedPage == i,
                        navButtonHeight))
                    { selectedPage = i; panelAnimation = 0.0f; }
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 68.0f);
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
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
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
                const ImGuiWindowFlags pageFlags = selectedPage == 0
                    ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                    : ImGuiWindowFlags_None;
                ImGui::BeginChild("page_card", ImVec2(0.0f, -1.0f),
                    ImGuiChildFlags_Borders, pageFlags);
                bool removeCurrent = false;
                bool displayEnabled = screen.enabled;
                if (toggle_switch(
                        "Enable media replacement on this display",
                        displayEnabled))
                {
                    screen.enabled = displayEnabled;
                    screen.textureRouteArmed = false;
                    screen.textureRouteArmedTick = 0;
                    if (!screen.enabled)
                    {
                        diagnostic_log::writef(
                            "route",
                            "Disabled %s (%s). Future %s requests pass "
                            "through unchanged; reload textures to restore "
                            "the native display.",
                            screen_owner_name(screen).c_str(),
                            screen.mediaClientId.c_str(),
                            screen.original_texture.c_str());
                        release_screen(screen);
                    }
                    else
                    {
                        std::string enableStatus;
                        const bool assetsReady =
                            !screen.original_texture.empty() &&
                            prepare_override_assets(screen, enableStatus);
                        if (assetsReady)
                            rebuild_source(screen);
                        diagnostic_log::writef(
                            "route",
                            "Enabled %s (%s): %s. Reload textures to bind "
                            "the exact display route.",
                            screen_owner_name(screen).c_str(),
                            screen.mediaClientId.c_str(),
                            assetsReady ? enableStatus.c_str()
                                : "waiting for a valid unique game TOBJ");
                    }
                    log_route_snapshot(screen, screen.enabled
                        ? "display enabled" : "display disabled");
                    mark_changed(true);
                }
                explain_last_item(
                    "Per-display media replacement",
                    "Enabled displays redirect only their selected game "
                    "TOBJ. Disabled displays stop their player and leave "
                    "future texture requests native.",
                    screen.enabled ? "Active source only" : "No media cost",
                    "Requires one texture reload after changing");
                if (!screen.enabled)
                {
                    ImGui::TextDisabled(
                        "OFF - no texture redirect, media client or audio "
                        "processing for this display.");
                }
                ImGui::Separator();
                switch (selectedPage) { case 0: draw_home(screen); break; case 1: draw_media(screen); break; case 2: draw_audio(screen); break; case 3: draw_controls(); break; case 4: draw_appearance(screen); break; case 5: draw_system(screen, removeCurrent); break; }
                ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::EndChild(); ImGui::PopStyleVar(2);

                ImGui::SetCursorPos(ImVec2(side + leftWidth + gap + centreWidth + gap, mainTop));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 20.0f));
                ImGui::BeginChild("inspector", ImVec2(rightWidth, mainHeight),
                    ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                draw_inspector(); ImGui::EndChild(); ImGui::PopStyleVar();
                if (removeCurrent) { release_screen(screen); g_screens.erase(g_screens.begin() + selectedScreen); selectedScreen = (std::max)(0, selectedScreen - 1); mark_changed(true); }
            }

            const float footerY = windowSize.y - footerHeight;
            ImGui::SetCursorPos(ImVec2(34.0f, footerY + 20.0f));
            ImGui::PushFont(ui_font(1));
            ImGui::TextColored(ImVec4(0.48f, 0.95f, 0.38f, 1.0f), "[A]"); ImGui::SameLine(); ImGui::TextUnformatted("Select");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(1.0f, 0.34f, 0.38f, 1.0f), "[B]"); ImGui::SameLine(); ImGui::TextUnformatted("Back");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(0.30f, 0.76f, 1.0f, 1.0f), "[LB/RB]"); ImGui::SameLine(); ImGui::TextUnformatted("Category");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.23f, 1.0f), "[LT/RT]"); ImGui::SameLine(); ImGui::TextUnformatted("Audio tabs");
            ImGui::SameLine(0.0f, 34.0f); ImGui::TextDisabled("D-pad / left stick moves  |  Mouse supported");
            ImGui::PopFont();
        }
        ImGui::End(); ImGui::GetStyle() = oldStyle; (void)earlyExit;
    }

    void on_frame()
    {
        static bool initialized{};
        if (!initialized && ImGui::GetCurrentContext())
        {
            auto& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.ConfigWindowsMoveFromTitleBarOnly = false;
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
                "PrismMedia update available: %s", update_checker::latest_tag().c_str());
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
