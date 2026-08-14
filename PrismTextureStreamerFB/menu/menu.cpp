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
    int selectedPage{};
    float panelAnimation = 1.0f;
    int restoreRequest = -1;
    std::string restoreStatus;

    struct inspector_t
    {
        const char* title = "Explore any option";
        const char* effect = "Hover a control to see what changes in-game.";
        const char* impact = "No performance impact";
        const char* apply = "Auto-saved";
    } inspector;

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
        if (ImGui::IsItemHovered())
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
        const ImVec2 size(ImGui::GetContentRegionAvail().x, 118.0f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(7, 16, 29, 225), 10.0f);
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(36, 104, 141, 150), 10.0f);
        const float time = static_cast<float>(ImGui::GetTime());
        const float gain = screen.adaptiveAudioEnabled ? (std::clamp)(screen.adaptiveAudioInteriorVolume, 0.0f, 1.0f) : 1.0f;
        ImVec2 a[72]{}, b[72]{};
        for (int i = 0; i < 72; ++i)
        {
            const float x = static_cast<float>(i) / 71.0f;
            const float envelope = 0.22f + 0.78f * std::sin(x * 3.14159265f);
            a[i] = ImVec2(origin.x + 12.0f + x * (size.x - 24.0f), origin.y + 58.0f + std::sin(x * 24.0f + time * 2.2f) * 24.0f * envelope * gain);
            b[i] = ImVec2(origin.x + 12.0f + x * (size.x - 24.0f), origin.y + 58.0f + std::sin(x * 18.0f - time * 1.45f) * 13.0f * envelope);
        }
        draw->AddPolyline(b, 72, IM_COL32(141, 82, 255, 150), false, 2.0f);
        draw->AddPolyline(a, 72, IM_COL32(33, 220, 235, 235), false, 2.5f);
        draw->AddText(ImVec2(origin.x + 14.0f, origin.y + 10.0f), IM_COL32(206, 240, 248, 255), "LIVE AUDIO TRANSITION");
        draw->AddText(ImVec2(origin.x + 14.0f, origin.y + 91.0f), IM_COL32(124, 155, 171, 255), "650 ms cabin / exterior crossfade");
        ImGui::Dummy(size);
    }

    void draw_media(screen_t& screen)
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "MEDIA SOURCE");
        ImGui::TextDisabled("One live source. YouTube playlist URLs still work; no saved playlist library.");
        const char* modes[] = { "Window capture", "Integrated YouTube / Spotify", "Native direct media" };
        int mode = static_cast<int>(screen.contentMode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Playback method", &mode, modes, IM_ARRAYSIZE(modes)))
        { screen.contentMode = static_cast<content_mode_t>(mode); rebuild_source(screen); mark_changed(); }
        explain_last_item("Playback method", "Chooses how frames reach the truck display. Integrated media is recommended for web services.", "Integrated: low/medium; Native: lowest; Window: medium/high");

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
            if (ImGui::Checkbox("Compatibility capture", &screen.legacyCapture))
            { rebuild_source(screen); mark_changed(); }
            explain_last_item("Compatibility capture", "Uses the older capture path only when Windows Graphics Capture fails.", "Higher CPU/GPU use");
        }
        else
        {
            if (screen.contentMode == content_mode_t::INTEGRATED_MEDIA)
            {
                int service = static_cast<int>(screen.mediaService);
                const char* services[] = { "YouTube", "Spotify Web" };
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::Combo("Service", &service, services, IM_ARRAYSIZE(services)))
                { screen.mediaService = static_cast<media_service_t>(service); rebuild_source(screen); mark_changed(); }
                explain_last_item("Media service", "YouTube uses the focused player; Spotify always uses the official full web player.", "Low/medium");
                if (!sources::IsMediaClientInstalled())
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "PrismMediaClient.exe is missing.");
            }
            ImGui::SetNextItemWidth(-88.0f);
            const bool enter = ImGui::InputText("##media_url", &screen.mediaUrl, ImGuiInputTextFlags_EnterReturnsTrue);
            explain_last_item("Media URL", "Paste one video, playlist, Spotify page, local file, or direct stream. Press Enter to load live.", "No game reload", "Loads live, then auto-saves");
            ImGui::SameLine();
            const bool load = ImGui::Button("Load now");
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
                if (ImGui::Button("Play / Pause", ImVec2(124.0f, 0.0f))) screen.source->SendMediaCommand(media_command_t::PLAY_PAUSE);
                explain_last_item("Play / Pause", "Uses a state-neutral transport control, so a stale icon cannot claim the wrong state.");
                ImGui::SameLine(); if (ImGui::Button("Previous")) screen.source->SendMediaCommand(media_command_t::PREVIOUS);
                ImGui::SameLine(); if (ImGui::Button("Next")) screen.source->SendMediaCommand(media_command_t::NEXT);
            }
            if (screen.mediaService == media_service_t::SPOTIFY && screen.contentMode == content_mode_t::INTEGRATED_MEDIA && screen.source)
            {
                if (ImGui::Button("Open Spotify sign-in")) screen.source->ShowInteractivePlayer(true);
                explain_last_item("Spotify sign-in", "Temporarily shows the official Spotify Web page for authentication.", "No game reload");
                ImGui::SameLine(); if (ImGui::Button("Hide browser")) screen.source->ShowInteractivePlayer(false);
            }
        }
        if (ImGui::Checkbox("Follow truck engine", &screen.followTruckEngine)) mark_changed();
        explain_last_item("Follow truck engine", "Fades media and shows the standby truck graphic while the engine is off.");
        if (ImGui::SliderFloat("Engine-off display", &screen.engineOffBrightness, 0.05f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::BeginPopupModal("Source unavailable", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        { ImGui::TextWrapped("The source could not start. Check the URL, runtime files, and plugin log."); if (ImGui::Button("OK", ImVec2(110, 0))) ImGui::CloseCurrentPopup(); ImGui::EndPopup(); }
    }

    void draw_audio(screen_t& screen)
    {
        draw_wave_preview(screen);
        if (ImGui::Checkbox("Adaptive cabin audio", &screen.adaptiveAudioEnabled)) mark_changed();
        explain_last_item("Adaptive cabin audio", "Blends gain, stereo position and muffling as the camera moves inside or outside.", "Very low CPU", "Applies live with a 650 ms fade");
        ImGui::BeginDisabled(!screen.adaptiveAudioEnabled);
        if (ImGui::SliderFloat("Cabin volume", &screen.adaptiveAudioInteriorVolume, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        explain_last_item("Cabin volume", "Full-volume anchor when the camera is inside the truck.");
        if (ImGui::SliderFloat("Spatial strength", &screen.adaptiveAudioStrength, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::SliderFloat("Speaker direction", &screen.adaptiveAudioSpeakerAzimuth, -180.0f, 180.0f, "%.0f deg")) mark_changed();
        if (ImGui::SliderFloat("Facing-away floor", &screen.adaptiveAudioFacingAwayVolume, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::SliderFloat("Exterior volume", &screen.adaptiveAudioOutsideVolume, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        explain_last_item("Exterior volume", "Target volume outside before distance attenuation.", "Very low CPU", "Smooth 650 ms crossfade");
        if (ImGui::SliderFloat("Menu volume", &screen.adaptiveAudioMenuVolume, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::Checkbox("Exterior distance filter", &screen.adaptiveAudioExternalDistanceEnabled)) mark_changed();
        ImGui::BeginDisabled(!screen.adaptiveAudioExternalDistanceEnabled);
        if (ImGui::SliderFloat("Near exterior volume", &screen.adaptiveAudioExternalNearVolume, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::SliderFloat("Near cutoff", &screen.adaptiveAudioExternalNearCutoff, 20.0f, 4000.0f, "%.0f Hz")) mark_changed();
        if (ImGui::SliderFloat("Full-volume distance", &screen.adaptiveAudioExternalFullVolumeDistance, 0.0f, 25.0f, "%.1f m")) mark_changed();
        if (ImGui::SliderFloat("Mute distance", &screen.adaptiveAudioExternalMuteDistance, 1.0f, 100.0f, "%.1f m")) mark_changed();
        if (ImGui::Checkbox("Smooth low-pass", &screen.adaptiveAudioExternalLowPassEnabled)) mark_changed();
        if (ImGui::SliderFloat("Minimum cutoff", &screen.adaptiveAudioExternalMinimumCutoff, 20.0f, 2000.0f, "%.0f Hz")) mark_changed();
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::Separator();
        std::lock_guard<std::mutex> lock(g_environment_audio_settings_mutex);
        if (ImGui::Checkbox("Protect game sounds", &g_environment_audio_settings.enabled)) mark_changed();
        explain_last_item("Protect game sounds", "Gently ducks media when road and engine activity increase; it adds no external audio.", "Very low CPU");
        if (ImGui::SliderFloat("Interior reduction", &g_environment_audio_settings.interiorEffect, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        if (ImGui::SliderFloat("Exterior reduction", &g_environment_audio_settings.exteriorEffect, 0.0f, 1.0f, "%.0f%%")) mark_changed();
        ImGui::TextDisabled("Live output %.0f%% | environment %.0f%%", g_environment_media_gain.load() * 100.0f, g_environment_intensity.load() * 100.0f);
    }

    void draw_controls()
    {
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "MOUSE + GAMEPAD");
        ImGui::TextWrapped("Every card is keyboard/gamepad navigable. D-pad or stick moves; A activates.");
        if (ImGui::Checkbox("Enable gamepad controls", &g_gamepad_hotkeys_enabled)) mark_changed();
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
        if (ImGui::SliderFloat("Brightness", &screen.brightness, 0.10f, 2.0f, "%.2fx")) { apply_brightness(screen); mark_changed(); }
        explain_last_item("Brightness", "Changes the streamed image without rebuilding the source.", "Negligible", "Instant + auto-save");
        if (ImGui::Checkbox("Automatic game-light adaptation", &screen.autoBrightnessEnabled)) { apply_brightness(screen); mark_changed(); }
        explain_last_item("Automatic brightness", "Uses a phone-like sensor curve: sunlight brightening settles in about 2.5 seconds and dimming in about 4 seconds, avoiding rapid jumps.", "Negligible", "Smooth live adaptation + auto-save");
        if (ImGui::SliderFloat("Dark multiplier", &screen.autoBrightnessDarkMultiplier, 0.25f, 1.25f, "%.2fx")) { apply_brightness(screen); mark_changed(); }
        if (ImGui::SliderFloat("Bright multiplier", &screen.autoBrightnessBrightMultiplier, 0.50f, 2.0f, "%.2fx")) { apply_brightness(screen); mark_changed(); }
        const char* scales[] = { "Stretch", "Fit", "Crop" }; int scale = static_cast<int>(screen.scaleMode);
        if (ImGui::Combo("Scaling", &scale, scales, IM_ARRAYSIZE(scales))) { screen.scaleMode = static_cast<scale_mode_t>(scale); screen.hasUploadedFrame = false; mark_changed(); }
        int guard = screen.edgeBleedGuard;
        if (ImGui::SliderInt("Edge guard", &guard, 0, 16, "%d px")) { screen.edgeBleedGuard = static_cast<uint8_t>(guard); screen.hasUploadedFrame = false; mark_changed(); }
        if (ImGui::Checkbox("Flip vertically", &screen.flipVertical)) { screen.hasUploadedFrame = false; mark_changed(); }
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
        if (screen.performanceProfile == performance_profile_t::CUSTOM)
        {
            int width = static_cast<int>(screen.targetLiveTextureWidth), height = static_cast<int>(screen.targetLiveTextureHeight), fps = screen.framerate;
            bool changed = ImGui::SliderInt("Width", &width, 64, 3840) | ImGui::SliderInt("Height", &height, 64, 2160) | ImGui::SliderInt("FPS", &fps, 1, 120);
            if (changed) { screen.targetLiveTextureWidth = static_cast<uint32_t>(width); screen.targetLiveTextureHeight = static_cast<uint32_t>(height); screen.framerate = static_cast<uint8_t>(fps); apply_profile(screen); mark_changed(); }
        }
        const auto stats = screen.source ? screen.source->GetPerformanceStats() : source_performance_stats_t{};
        const std::string status = screen.source ? screen.source->GetStatusText() : "Waiting for source";
        ImGui::Text("Source: %s", status.c_str());
        ImGui::TextDisabled("Upload %.3f ms | worker %.3f ms | delivered %.1f FPS", screen.uploadCpuMs, stats.workerCpuMs, stats.deliveredFps > 0.0 ? stats.deliveredFps : screen.deliveredFps);
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
        ImGui::TextColored(ImVec4(0.64f, 0.42f, 1.0f, 1.0f), "OPTION EFFECT");
        ImGui::Spacing(); ImGui::TextWrapped("%s", inspector.title); ImGui::Separator();
        ImGui::TextWrapped("%s", inspector.effect); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.20f, 0.88f, 0.94f, 1.0f), "Device impact"); ImGui::TextWrapped("%s", inspector.impact);
        ImGui::Spacing(); ImGui::TextColored(ImVec4(0.48f, 0.93f, 0.55f, 1.0f), "How it applies"); ImGui::TextWrapped("%s", inspector.apply);
    }

    void draw_menu()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 wanted((std::min)(1120.0f, viewport->WorkSize.x - 24.0f), (std::min)(720.0f, viewport->WorkSize.y - 24.0f));
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(wanted, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f); ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f); ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.055f, 0.09f, 0.98f)); ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.085f, 0.125f, 0.82f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.07f, 0.22f, 0.29f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.42f, 0.50f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.12f, 0.34f, 0.43f, 0.90f)); ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.20f, 0.92f, 0.95f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.58f, 0.36f, 1.0f, 1.0f));
        bool earlyExit = false;
        if (ImGui::Begin("Prism Texture Streamer 4.0", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
        {
            const float pulse = 0.78f + 0.22f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.0f);
            ImGui::TextColored(ImVec4(0.23f, 0.93f, 0.97f, 1.0f), "PRISM"); ImGui::SameLine(); ImGui::TextDisabled("Texture Streamer v%s", g_version);
            ImGui::SameLine(ImGui::GetWindowWidth() - 230.0f); ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.60f, pulse), "AUTO-SAVE  LIVE"); ImGui::Separator();
            std::lock_guard<std::mutex> lock(g_screens_mutex);
            if (g_screens.empty())
            {
                ImGui::TextWrapped("Start with the recommended GPS screen: Smooth preset, adaptive display, and no personal URL.");
                if (ImGui::Button("Create recommended GPS screen", ImVec2(310.0f, 42.0f))) add_screen(screen_type_t::GPS);
                earlyExit = true;
            }
            else
            {
                selectedScreen = (std::clamp)(selectedScreen, 0, static_cast<int>(g_screens.size() - 1));
                ImGui::BeginChild("navigation", ImVec2(182.0f, -1.0f), true); ImGui::TextDisabled("SCREENS");
                for (int i = 0; i < static_cast<int>(g_screens.size()); ++i)
                {
                    const char* kind = g_screens[static_cast<size_t>(i)].type == screen_type_t::GPS ? "GPS" : (g_screens[static_cast<size_t>(i)].type == screen_type_t::DASHBOARD ? "Dashboard" : "Custom");
                    const std::string label = std::string(kind) + "  " + std::to_string(i + 1);
                    if (ImGui::Selectable(label.c_str(), selectedScreen == i)) selectedScreen = i;
                }
                if (ImGui::SmallButton("+ GPS")) add_screen(screen_type_t::GPS); ImGui::SameLine(); if (ImGui::SmallButton("+ Custom")) add_screen(screen_type_t::CUSTOM);
                ImGui::Separator(); const char* pages[] = { "Media", "Audio", "Controls", "Appearance", "System" };
                for (int i = 0; i < IM_ARRAYSIZE(pages); ++i)
                    if (ImGui::Selectable(pages[i], selectedPage == i, 0, ImVec2(0.0f, 36.0f))) { selectedPage = i; panelAnimation = 0.0f; }
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 54.0f); ImGui::TextDisabled("D-pad / Stick: move"); ImGui::TextDisabled("A: select  Ctrl+F8: close"); ImGui::EndChild(); ImGui::SameLine();
                screen_t& screen = g_screens[static_cast<size_t>(selectedScreen)];
                panelAnimation = (std::min)(1.0f, panelAnimation + ImGui::GetIO().DeltaTime * 6.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.35f + 0.65f * (panelAnimation * panelAnimation * (3.0f - 2.0f * panelAnimation)));
                ImGui::BeginChild("content", ImVec2(-258.0f, -1.0f), true); bool removeCurrent = false;
                switch (selectedPage) { case 0: draw_media(screen); break; case 1: draw_audio(screen); break; case 2: draw_controls(); break; case 3: draw_appearance(screen); break; case 4: draw_system(screen, removeCurrent); break; }
                ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::SameLine(); ImGui::BeginChild("inspector", ImVec2(248.0f, -1.0f), true); draw_inspector(); ImGui::EndChild();
                if (removeCurrent) { release_screen(screen); g_screens.erase(g_screens.begin() + selectedScreen); selectedScreen = (std::max)(0, selectedScreen - 1); mark_changed(true); }
            }
        }
        ImGui::End(); ImGui::PopStyleColor(7); ImGui::PopStyleVar(3); (void)earlyExit;
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
        if (menuVisible.load()) draw_menu();
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
