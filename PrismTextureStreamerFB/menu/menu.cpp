#include "menu.h"
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <iterator>
#include <map>
#include <vector>

#include "../version.h"

#include "../scs_logging.h"
using namespace scs_logging;

#include "../prism/prism.h"
#include "../dx11/internal_render_probe.h"
#include "../dx11/present.h"
#include "../dinput8/dinput8.h"

#include <ImGui/imgui.h>
#include "../misc/imgui_stdlib.h"
#include <ImGui/imgui_impl_dx11.h>
#include <ImGui/imgui_impl_win32.h>

#include "../screens.h"
#include "../hotkeys.h"
#include "../settings.h"
#include "../telemetry_state.h"
#include "../thread_scheduling.h"
#include "../environment_audio.h"
#include "../traffic_audio.h"
#include "../update_checker.h"
#include "../sources/media_client.h"
#include "../sources/native_media.h"
#include "../sources/reverse_camera.h"
#include "../sources/window.h"
#include "../sources/wgc_window.h"


static std::atomic<bool> menu_visible{};
static int hotkey_binding_index = -1;
static bool configuration_save_pending = false;
static uint64_t configuration_last_change_tick = 0;

static bool is_traffic_streaming_web_page(const std::string& value)
{
	std::string lower;
	lower.reserve(value.size());
	for (const unsigned char character : value)
		lower.push_back(static_cast<char>(std::tolower(character)));
	return lower.rfind("spotify:", 0) == 0 ||
		lower.find("spotify.com") != std::string::npos ||
		lower.find("youtube.com") != std::string::npos ||
		lower.find("youtu.be") != std::string::npos;
}

static float calculate_effective_brightness(const screen_t& screen)
{
	float effective = (std::clamp)(screen.brightness, 0.10f, 2.0f);
	if (screen.autoBrightnessEnabled && g_game_lighting_valid.load())
	{
		const float luminance = g_game_lighting_luminance.load();
		float scene = (std::clamp)(
			(luminance - 0.06f) / 0.54f, 0.0f, 1.0f);
		scene = scene * scene * (3.0f - 2.0f * scene);
		const float multiplier =
			screen.autoBrightnessDarkMultiplier +
			(screen.autoBrightnessBrightMultiplier -
				screen.autoBrightnessDarkMultiplier) * scene;
		effective *= multiplier;
	}

	return (std::clamp)(effective, 0.05f, 2.0f);
}

static void apply_screen_brightness(screen_t& screen)
{
	screen.effectiveBrightness =
		calculate_effective_brightness(screen);
	if (screen.source && screen.source->SupportsSourceBrightness())
		screen.source->SetSourceBrightness(screen.effectiveBrightness);
	// Reprocess the cached image for CPU-fallback sources and ensure the next
	// filtered WebView frame replaces the existing texture.
	screen.hasUploadedFrame = false;
}

static void set_menu_visibility(bool visible)
{
	menu_visible.store(visible);
	if (!visible)
	{
		hotkey_binding_index = -1;
		g_is_binding_hotkey = false;
		std::lock_guard<std::mutex> lock(g_screens_mutex);
		for (auto& screen : g_screens)
		{
			screen.reversePreview = false;
			if (screen.reverseSource)
				screen.reverseSource->SetPaused(
					!g_reverse_active.load());
		}
	}
	dinput8::set_mouse(visible);

	if (ImGui::GetCurrentContext())
	{
		// Use ImGui's software cursor while the plugin menu is open. The
		// cursor is rendered from the exact same io.MousePos coordinates
		// ImGui uses for hovering/clicking, so the visible pointer cannot
		// drift above/left of the control that is actually selected.
		ImGui::GetIO().MouseDrawCursor = visible;
	}
}

static bool rebuild_source(screen_t& screen)
{
	g_screen_source_creation_in_progress = true;
	screen.source.reset();
	screen.frameScratch.clear();
	screen.frameScratchWidth = 0;
	screen.frameScratchHeight = 0;
	screen.hasUploadedFrame = false;
	screen.uploadCpuMs = 0.0;
	screen.totalPluginCpuMs = 0.0;
	screen.estimatedFpsLoss = 0.0;
	screen.deliveredFps = 0.0;
	screen.uploadedFrames = 0;
	screen.lastUploadTick = 0;
	screen.sourceCreatedTick = 0;
	screen.lastSourceFrameTick = 0;
	screen.lastFrameInspectionTick = 0;
	screen.lastRenderDiagnosticTick = 0;
	screen.lastIssueDiagnosticTick = 0;
	screen.suspiciousMagentaFrame = false;
	screen.sourceFrameStale = false;
	screen.magentaSampleCount = 0;
	screen.diagnosticSampleCount = 0;
	screen.consecutiveMapFailures = 0;

	switch (screen.contentMode)
	{
	case content_mode_t::WINDOW_CAPTURE:
		if (!screen.source_application_name.empty())
		{
			if (screen.legacyCapture)
				screen.source = sources::CreateWindowSource(
					screen.source_application_name.c_str(),
					screen.source_application_display_name.empty()
						? nullptr : screen.source_application_display_name.c_str(),
					screen.framerate,
					screen.targetLiveTextureWidth,
					screen.targetLiveTextureHeight);
			else
				screen.source = sources::CreateWgcWindowSource(
					screen.source_application_name.c_str(),
					screen.source_application_display_name.empty()
						? nullptr : screen.source_application_display_name.c_str(),
					screen.framerate,
					screen.targetLiveTextureWidth,
					screen.targetLiveTextureHeight);
		}
		break;
	case content_mode_t::INTEGRATED_MEDIA:
		if (!screen.mediaUrl.empty())
			screen.source = sources::CreateMediaClientSource(
				screen.mediaUrl, screen.framerate,
				screen.targetLiveTextureWidth,
				screen.targetLiveTextureHeight);
		break;
	case content_mode_t::NATIVE_DIRECT_MEDIA:
		if (!screen.mediaUrl.empty())
			screen.source = sources::CreateNativeMediaSource(
				screen.mediaUrl, screen.framerate,
				screen.targetLiveTextureWidth,
				screen.targetLiveTextureHeight);
		break;
	}

	if (screen.source)
	{
		screen.sourceCreatedTick = GetTickCount64();
		screen.source->SetPaused(screen.paused);
		apply_screen_brightness(screen);
	}
	g_screen_source_creation_in_progress = false;
	return screen.source != nullptr;
}

static bool rebuild_reverse_source(screen_t& screen)
{
	g_screen_source_creation_in_progress = true;
	screen.reverseSource.reset();
	screen.reverseLastStartAttemptTick = 0;

	if (screen.reverseCameraEnabled &&
		screen.reverseCameraMethod ==
			reverse_camera_method_t::WINDOW_CROP &&
		(!screen.reverseZeroForwardImpact ||
			g_reverse_active.load() || screen.reversePreview))
	{
		screen.reverseLastStartAttemptTick = GetTickCount64();
		screen.reverseSource = sources::CreateReverseCameraSource(
			screen.reverseFramerate,
			screen.reverseCaptureWidth,
			screen.reverseCaptureHeight,
			screen.reverseCropLeft,
			screen.reverseCropTop,
			screen.reverseCropWidth,
			screen.reverseCropHeight);
	}

	if (screen.reverseSource)
		screen.reverseSource->SetPaused(
			!(g_reverse_active.load() || screen.reversePreview));
	g_screen_source_creation_in_progress = false;
	if (screen.reverseCameraEnabled &&
		screen.reverseCameraMethod ==
			reverse_camera_method_t::INTERNAL_PARK_PROBE)
	{
		dx11::internal_render_probe::
			set_park_activation_requested(true);
		dx11::internal_render_probe::
			set_park_render_requested(
				g_reverse_active.load() ||
				screen.reversePreview);
		return true;
	}
	return !screen.reverseCameraEnabled ||
		(screen.reverseZeroForwardImpact &&
			!g_reverse_active.load() && !screen.reversePreview) ||
		screen.reverseSource != nullptr;
}

static bool capture_hotkey(hotkey_binding_t& binding)
{
	if ((GetAsyncKeyState(VK_ESCAPE) & 1) != 0)
		return true;
	if ((GetAsyncKeyState(VK_BACK) & 1) != 0 ||
		(GetAsyncKeyState(VK_DELETE) & 1) != 0)
	{
		binding = {};
		return true;
	}

	for (UINT key = 7; key < 256; ++key)
	{
		if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
			key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
			key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT)
			continue;
		if ((GetAsyncKeyState(static_cast<int>(key)) & 1) == 0)
			continue;

		binding.virtualKey = key;
		binding.control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		binding.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
		binding.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		return true;
	}
	return false;
}

static bool edit_gamepad_binding(
	const char* label,
	int id,
	gamepad_binding_t& binding)
{
	bool changed = false;
	ImGui::PushID(id);
	ImGui::TextUnformatted(label);
	ImGui::SameLine(150.0f);
	ImGui::SetNextItemWidth(90.0f);
	if (ImGui::BeginCombo(
		"##gamepad_modifier",
		gamepad_modifier_name(binding.modifier)))
	{
		for (int value = 0;
			value < static_cast<int>(gamepad_modifier_t::COUNT);
			++value)
		{
			const auto candidate =
				static_cast<gamepad_modifier_t>(value);
			if (ImGui::Selectable(
				gamepad_modifier_name(candidate),
				candidate == binding.modifier))
			{
				binding.modifier = candidate;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(190.0f);
	if (ImGui::BeginCombo(
		"##gamepad_input",
		gamepad_input_name(binding.input)))
	{
		for (int value = 0;
			value < static_cast<int>(gamepad_input_t::COUNT);
			++value)
		{
			const auto candidate =
				static_cast<gamepad_input_t>(value);
			if (ImGui::Selectable(
				gamepad_input_name(candidate),
				candidate == binding.input))
			{
				binding.input = candidate;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::PopID();
	return changed;
}

static void apply_performance_profile(screen_t& screen)
{
	switch (screen.performanceProfile)
	{
	case performance_profile_t::ECONOMY:
		screen.targetLiveTextureWidth = 854;
		screen.targetLiveTextureHeight = 480;
		screen.framerate = 20;
		break;
	case performance_profile_t::BALANCED:
		screen.targetLiveTextureWidth = 1280;
		screen.targetLiveTextureHeight = 720;
		screen.framerate = 30;
		break;
	case performance_profile_t::QUALITY:
		screen.targetLiveTextureWidth = 1920;
		screen.targetLiveTextureHeight = 1080;
		screen.framerate = 30;
		break;
	case performance_profile_t::SMOOTH:
		screen.targetLiveTextureWidth = 1280;
		screen.targetLiveTextureHeight = 720;
		screen.framerate = 60;
		break;
	default:
		break;
	}
}

void on_frame()
{
	bool saveConfiguration = false;
	static bool wasPressed = false;

	bool ctrlDown = GetAsyncKeyState(VK_CONTROL) & 0x8000;
	bool f8Down = GetAsyncKeyState(VK_F8) & 0x8000;
	bool isPressed = ctrlDown && f8Down;

	process_media_hotkeys(menu_visible.load());
	const bool gamepadMenuToggle =
		consume_gamepad_menu_toggle_request();
	if ((isPressed && !wasPressed) || gamepadMenuToggle)
		set_menu_visibility(!menu_visible.load());
	wasPressed = isPressed;

	if (!menu_visible.load() && update_checker::should_show_toast())
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(
			ImVec2(
				viewport->WorkPos.x + viewport->WorkSize.x - 18.0f,
				viewport->WorkPos.y + 18.0f),
			ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.92f);
		ImGui::Begin(
			"##prism_update_toast", nullptr,
			ImGuiWindowFlags_AlwaysAutoResize |
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoInputs |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoSavedSettings);
		ImGui::TextColored(
			ImVec4(0.35f, 0.95f, 0.45f, 1.0f),
			"PrismTextureStreamer update available: %s",
			update_checker::latest_tag().c_str());
		ImGui::TextUnformatted(
			"Open the plugin menu (Ctrl+F8) for the download page.");
		ImGui::End();
	}

	if (menu_visible.load()) {
		ImGui::SetNextWindowSizeConstraints(ImVec2(680, 350), ImVec2(1000, 900));
		ImGui::Begin(("Prism3D Texture Streamer v" + std::string(g_version)).c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

		if (update_checker::update_available() &&
			!update_checker::is_dismissed())
		{
			ImGui::TextColored(
				ImVec4(0.35f, 0.95f, 0.45f, 1.0f),
				"New version available: %s (installed: %s)",
				update_checker::latest_tag().c_str(), g_version);
			if (ImGui::Button("Open GitHub Releases"))
				update_checker::open_releases_page();
			ImGui::SameLine();
			if (ImGui::Button("Dismiss for this session"))
				update_checker::dismiss();
			ImGui::Separator();
		}

		static bool unsavedChanges = false;
		static bool hasGps = false;
		static bool hasDash = false;
		// static bool hasGps = false; // More than 1 custom IS possible..

		ImGui::BeginDisabled(hasGps);
		if (ImGui::Button("Add Screen GPS"))
		{
			unsavedChanges = true;
			saveConfiguration = true;

			screen_t screen;
			screen.type = screen_type_t::GPS;
			screen.original_texture = "/vehicle/truck/share/gps.tobj";
			screen.override_texture = "/home/PrismTextureStreamer/gps.tobj";
			screen.override_texture_size_h = 2048;
			screen.override_texture_size_w = 64;

			{
				std::lock_guard<std::mutex> lock(g_screens_mutex);
				g_screens.push_back(std::move(screen));
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(hasDash);
		if (ImGui::Button("Add Screen Dashboard"))
		{
			unsavedChanges = true;
			saveConfiguration = true;

			screen_t screen;
			screen.type = screen_type_t::DASHBOARD;
			screen.original_texture = "/vehicle/truck/share/dashboard.tobj";
			screen.override_texture = "/home/PrismTextureStreamer/dashboard.tobj";
			screen.override_texture_size_h = 64;
			screen.override_texture_size_w = 2048;

			{
				std::lock_guard<std::mutex> lock(g_screens_mutex);
				g_screens.push_back(std::move(screen));
			}
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Add Screen Custom"))
		{
			unsavedChanges = true;
			saveConfiguration = true;

			screen_t screen;
			screen.type = screen_type_t::CUSTOM;
			screen.original_texture = ".tobj";
			screen.override_texture = "/home/PrismTextureStreamer/.tobj";

			{
				std::lock_guard<std::mutex> lock(g_screens_mutex);
				g_screens.push_back(std::move(screen));
			}
		}
		ImGui::SameLine();

		if (unsavedChanges) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.10f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.35f, 0.15f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.18f, 0.08f, 1.0f));
		}

		ImGui::BeginDisabled(!unsavedChanges);
		bool justSaved = false;
		if (ImGui::Button("Apply Unsaved Changes"))
		{
			{
				std::lock_guard<std::mutex> lock(g_screens_mutex);
				for (auto& screen : g_screens)
				{
					screen.targetLiveTextureWidth =
						(std::clamp)(screen.targetLiveTextureWidth, 64U, 7680U);
					screen.targetLiveTextureHeight =
						(std::clamp)(screen.targetLiveTextureHeight, 64U, 4320U);
					screen.framerate =
						static_cast<uint8_t>((std::clamp)(static_cast<uint32_t>(screen.framerate), 1U, 120U));
					if (screen.source)
					{
						screen.source->SetFramerate(screen.framerate);
						screen.source->SetOutputSize(
							screen.targetLiveTextureWidth,
							screen.targetLiveTextureHeight);
					}
						if (screen.reverseSource)
						{
							screen.reverseSource->SetFramerate(
								screen.reverseFramerate);
							screen.reverseSource->SetOutputSize(
								screen.reverseCaptureWidth,
								screen.reverseCaptureHeight);
							screen.reverseSource->SetCaptureRegion(
								screen.reverseCropLeft,
							screen.reverseCropTop,
							screen.reverseCropWidth,
							screen.reverseCropHeight);
					}
				}
			}
			prism::string cmd("game");
			prism::execute_command::call(&cmd, -1);

			justSaved = true;
			unsavedChanges = false;
			saveConfiguration = true;
		}
		ImGui::EndDisabled();

		if (unsavedChanges || justSaved) ImGui::PopStyleColor(3);

		if (ImGui::CollapsingHeader("Configuration Backups"))
		{
			static int selectedBackup = 0;
			static std::string backupStatus;
			const auto backupHistory = settings::backup_history();
			const char* backupLabels[] = {
				backupHistory[0].description.c_str(),
				backupHistory[1].description.c_str(),
				backupHistory[2].description.c_str()
			};

			ImGui::TextWrapped(
				"The previous three distinct saved configurations are kept "
				"outside config.ini. Deleting the active configuration does "
				"not delete these restore points. A new save replaces the "
				"oldest slot.");
			ImGui::SetNextItemWidth(330.0f);
			ImGui::Combo(
				"Restore point", &selectedBackup,
				backupLabels, IM_ARRAYSIZE(backupLabels));

			if (ImGui::Button("Save configuration now"))
			{
				if (settings::save())
				{
					configuration_save_pending = false;
					configuration_last_change_tick = 0;
					unsavedChanges = false;
					backupStatus =
						"Configuration saved. Previous state preserved.";
				}
				else
					backupStatus = "Configuration save failed; check the log.";
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(
				!backupHistory[static_cast<size_t>(selectedBackup)].available);
			if (ImGui::Button("Restore selected backup"))
			{
				if (settings::restore_backup(
					static_cast<size_t>(selectedBackup)))
				{
					configuration_save_pending = false;
					configuration_last_change_tick = 0;
					unsavedChanges = false;
					prism::string command("game");
					prism::execute_command::call(&command, -1);
					backupStatus =
						"Backup restored and game textures reloaded.";
				}
				else
					backupStatus = "Backup restore failed; check the log.";
			}
			ImGui::EndDisabled();
			if (!backupStatus.empty())
				ImGui::TextDisabled("%s", backupStatus.c_str());
		}

		if (ImGui::CollapsingHeader("Media Hotkeys"))
		{
			ImGui::TextWrapped(
				"These keys control the screen marked as the hotkey target. "
				"Click a binding, then press a key or key combination. "
				"Backspace/Delete clears it; Escape cancels.");

			for (int commandIndex = 0;
				commandIndex < static_cast<int>(g_media_hotkeys.size());
				++commandIndex)
			{
				const auto command = static_cast<media_command_t>(commandIndex);
				ImGui::PushID(10000 + commandIndex);
				ImGui::TextUnformatted(media_command_name(command));
				ImGui::SameLine(150.0f);

				const std::string label = hotkey_binding_index == commandIndex
					? "Press a key...##binding"
					: hotkey_name(g_media_hotkeys[commandIndex]) + "##binding";
				if (ImGui::Button(label.c_str(), ImVec2(220.0f, 0.0f)))
				{
					hotkey_binding_index = commandIndex;
					g_is_binding_hotkey = true;
				}

				if (hotkey_binding_index == commandIndex &&
					capture_hotkey(g_media_hotkeys[commandIndex]))
				{
					hotkey_binding_index = -1;
					g_is_binding_hotkey = false;
					saveConfiguration = true;
				}
				ImGui::PopID();
			}

			ImGui::Separator();
			ImGui::TextUnformatted("Gamepad combinations");
			if (ImGui::Checkbox(
				"Enable gamepad controls",
				&g_gamepad_hotkeys_enabled))
				saveConfiguration = true;
			ImGui::TextWrapped(
				"XInput is preferred. PlayStation and generic controllers may "
				"fall back to the Windows joystick API. If the log reports no "
				"controller, disable Steam Input for ETS2/ATS so Windows can "
				"expose the physical controller directly. The plugin menu "
				"combination works even while the menu is closed.");

			const char* controllerChoices[] = {
				"Automatic (first connected)",
				"Controller 1", "Controller 2",
				"Controller 3", "Controller 4"
			};
			int controllerChoice = g_gamepad_controller_index + 1;
			if (ImGui::Combo(
				"Controller", &controllerChoice,
				controllerChoices, IM_ARRAYSIZE(controllerChoices)))
			{
				g_gamepad_controller_index = controllerChoice - 1;
				saveConfiguration = true;
			}
			if (ImGui::SliderFloat(
				"Stick / trigger threshold",
				&g_gamepad_axis_threshold,
				0.20f, 0.95f, "%.2f"))
				saveConfiguration = true;

			if (edit_gamepad_binding(
				"Plugin menu", 10999, g_gamepad_menu_hotkey))
				saveConfiguration = true;
			if (g_gamepad_menu_hotkey.input == gamepad_input_t::START)
			{
				ImGui::TextColored(
					ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
					"Start/Menu is also consumed by ETS2/ATS and can show "
					"the game cursor. Right Stick Click is recommended.");
			}
			ImGui::Separator();

			for (int commandIndex = 0;
				commandIndex < static_cast<int>(
					g_media_gamepad_hotkeys.size());
				++commandIndex)
			{
				const auto command =
					static_cast<media_command_t>(commandIndex);
				auto& binding =
					g_media_gamepad_hotkeys[commandIndex];
				if (edit_gamepad_binding(
					media_command_name(command),
					11000 + commandIndex,
					binding))
					saveConfiguration = true;
			}
			ImGui::TextDisabled(
				"Defaults: LB + Right Stick Click toggles this menu; "
				"RB + A plays/pauses; "
				"RB + right-stick directions control tracks and volume. "
				"ETS2/ATS still receives the same gamepad input.");
		}

			if (ImGui::CollapsingHeader(
				"Environment-Aware Media Volume"))
			{
				std::lock_guard<std::mutex> environmentSettingsLock(
					g_environment_audio_settings_mutex);
				ImGui::TextWrapped(
					"Estimates the game's environment intensity from live truck "
					"speed and wheel-ground contact, then reduces integrated media "
					"so ETS2/ATS sounds remain clear. It plays no external audio "
					"files and adds no capture or decoding work.");

				if (ImGui::Checkbox(
					"Enable environment-aware media reduction",
					&g_environment_audio_settings.enabled))
					saveConfiguration = true;

				float interiorEffectPercent =
					g_environment_audio_settings.interiorEffect * 100.0f;
				if (ImGui::SliderFloat(
					"Interior media reduction", &interiorEffectPercent,
					0.0f, 100.0f, "%.0f%%"))
				{
					g_environment_audio_settings.interiorEffect =
						interiorEffectPercent / 100.0f;
					saveConfiguration = true;
				}
				float exteriorEffectPercent =
					g_environment_audio_settings.exteriorEffect * 100.0f;
				if (ImGui::SliderFloat(
					"Exterior media reduction", &exteriorEffectPercent,
					0.0f, 100.0f, "%.0f%%"))
				{
					g_environment_audio_settings.exteriorEffect =
						exteriorEffectPercent / 100.0f;
					saveConfiguration = true;
				}
				ImGui::TextDisabled(
					"At 100%%, the strongest estimate can fully mute media in "
					"that camera mode.");

				ImGui::Text(
					"Live: %.1f km/h | road contact %.0f%% | environment %.0f%%",
					std::fabs(g_truck_speed_mps.load()) * 3.6f,
					g_environment_grounded_ratio.load() * 100.0f,
					g_environment_intensity.load() * 100.0f);
					ImGui::Text(
						"Mode: %s | resulting media volume %.0f%%",
					!g_telemetry_driving.load()
						? "menus / before driving"
						: (g_environment_interior.load()
							? "interior" : "exterior"),
						g_environment_media_gain.load() * 100.0f);
					ImGui::TextDisabled(
						"Estimator cost: %.1f us/update | capped at 20 Hz",
						g_environment_update_cpu_us.load());
					ImGui::TextDisabled(
					"This is a live telemetry estimate, not access to the game's "
					"private audio mixer.");
			}




		std::map<std::string, std::string> applications; // window title, application name
		EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
			auto* applications = reinterpret_cast<std::map<std::string, std::string>*>(lParam);

			if (!IsWindowVisible(hwnd))
				return TRUE;

			LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
			if (exStyle & WS_EX_TOOLWINDOW)
				return TRUE;

			std::string windowTitle;
			bool hasTitle{};

			int titleLen = GetWindowTextLengthA(hwnd);
			if (titleLen != 0)
			{
				hasTitle = true;

				windowTitle = std::string(titleLen + 1, '\0');
				GetWindowTextA(hwnd, windowTitle.data(), titleLen + 1);
				windowTitle.resize(titleLen);
			}



			std::string applicationName;

			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);

			HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (!hProc) return TRUE; // Continue, dont add if no process name

			char path[MAX_PATH]{};
			DWORD size = MAX_PATH;
			QueryFullProcessImageNameA(hProc, 0, path, &size);
			CloseHandle(hProc);

			applicationName = std::string(path);

			auto pos = applicationName.rfind('\\');
			applicationName = pos != std::string::npos ? applicationName.substr(pos + 1) : applicationName;

			if (!hasTitle || windowTitle.empty() || windowTitle == "")
				windowTitle = applicationName;

			(*applications)[windowTitle] = applicationName;

			return TRUE;
		}, reinterpret_cast<LPARAM>(&applications));


		{
			std::lock_guard<std::mutex> lock(g_screens_mutex);
			std::vector<int> to_remove{}; // indexes to remove

			int i = 0;
			hasGps = false;
			hasDash = false;
			for (screen_t& screen : g_screens) {
				bool isGps		= screen.type == screen_type_t::GPS;
				bool isDash		= screen.type == screen_type_t::DASHBOARD;
				bool isCustom	= screen.type == screen_type_t::CUSTOM;

				if (isGps)
					hasGps = true;
				else if (isDash)
					hasDash = true;


				std::string name = isGps ? "GPS##" : (isDash ? "Dashboard##" : "Custom");
				if (ImGui::CollapsingHeader((name + std::to_string(i)).c_str()))
				{
					ImGui::PushID(i);

					if (isCustom) {
						if (ImGui::BeginTable("screen_table", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
						{
							ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 170.0f);
							ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "Game Screen Texture");
							ImGui::TableSetColumnIndex(1);
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::InputText("##original_texture", &screen.original_texture)) unsavedChanges = true;


							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "Unqiue Override Texture");
							ImGui::TableSetColumnIndex(1);
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::InputText("##override_texture", &screen.override_texture)) unsavedChanges = true;


							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "Override Texture Width");
							ImGui::TableSetColumnIndex(1);
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::InputScalar("##override_texture_size_w", ImGuiDataType_U32, &screen.override_texture_size_w)) unsavedChanges = true;


							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "Override Texture Height");
							ImGui::TableSetColumnIndex(1);
							ImGui::SetNextItemWidth(-FLT_MIN);
							if (ImGui::InputScalar("##override_texture_size_h", ImGuiDataType_U32, &screen.override_texture_size_h)) unsavedChanges = true;

							ImGui::EndTable();
						}
					}

					const char* contentModes[] = {
						"Window Capture (most compatible)",
						"Integrated Media Client (YouTube / Spotify)",
						"Native Direct Media (lowest overhead)"
					};
					int selectedContentMode = static_cast<int>(screen.contentMode);
					if (ImGui::Combo(
						"Playback Method", &selectedContentMode,
						contentModes, IM_ARRAYSIZE(contentModes)))
					{
						screen.contentMode =
							static_cast<content_mode_t>(selectedContentMode);
						screen.source.reset();
						screen.hasUploadedFrame = false;
						unsavedChanges = true;
					}

					switch (screen.contentMode)
					{
					case content_mode_t::WINDOW_CAPTURE:
						ImGui::TextColored(
							ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
							"Expected impact: Medium / High");
						ImGui::TextWrapped(
							"Captures any visible application. Best compatibility, "
							"but the source app, desktop capture, GPU readback and "
							"game upload all use resources.");
						break;
					case content_mode_t::INTEGRATED_MEDIA:
						if (screen.mediaService == media_service_t::SPOTIFY)
						{
							ImGui::TextColored(
								ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
								"Expected impact: Medium / High");
							ImGui::TextWrapped(
								"Spotify always uses its Full Web Player for persistent "
								"login and complete controls. The old Embed path is removed.");
						}
						else
						{
							ImGui::TextColored(
								ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
								"Expected impact: Low / Medium");
							ImGui::TextWrapped(
								"YouTube uses its official player in a clean, "
								"hardware-accelerated helper without browser tabs or "
								"extensions. One optimized Windows capture transfer is "
								"still required.");
						}
						if (!sources::IsMediaClientInstalled())
							ImGui::TextColored(
								ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
								"PrismMediaClient.exe is not beside the plugin DLL.");
						break;
					case content_mode_t::NATIVE_DIRECT_MEDIA:
						ImGui::TextColored(
							ImVec4(0.30f, 0.90f, 0.45f, 1.0f),
							"Expected impact: Lowest");
						ImGui::TextWrapped(
							"Bypasses window capture and uses Windows Media Foundation "
							"hardware decoding. Supports local files and direct stream "
							"URLs. YouTube page URLs are not direct media and must use "
							"the Integrated Media Client. Next/Previous seek 30 seconds.");
						break;
					}

					const char* performanceProfiles[] = {
						"Custom", "Economy (854x480 @ 20)", "Balanced (1280x720 @ 30)",
						"Quality (1920x1080 @ 30)", "Smooth (1280x720 @ 60)"
					};
					int selectedProfile = static_cast<int>(screen.performanceProfile);
					if (ImGui::Combo("Performance Profile", &selectedProfile, performanceProfiles, IM_ARRAYSIZE(performanceProfiles)))
					{
						screen.performanceProfile = static_cast<performance_profile_t>(selectedProfile);
						apply_performance_profile(screen);
						if (screen.source) {
							screen.source->SetFramerate(screen.framerate);
							screen.source->SetOutputSize(
								screen.targetLiveTextureWidth,
								screen.targetLiveTextureHeight);
						}
						unsavedChanges = true;
					}

					ImGui::Text("Resolution");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(80.0f);
					if (ImGui::InputScalar("##res_w", ImGuiDataType_U32, &screen.targetLiveTextureWidth)) {
						screen.performanceProfile = performance_profile_t::CUSTOM;
						if (screen.source)
							screen.source->SetOutputSize(
								screen.targetLiveTextureWidth,
								screen.targetLiveTextureHeight);
						unsavedChanges = true;
					}
					ImGui::SameLine();
					ImGui::Text("x");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(80.0f);
					if (ImGui::InputScalar("##res_h", ImGuiDataType_U32, &screen.targetLiveTextureHeight)) {
						screen.performanceProfile = performance_profile_t::CUSTOM;
						if (screen.source)
							screen.source->SetOutputSize(
								screen.targetLiveTextureWidth,
								screen.targetLiveTextureHeight);
						unsavedChanges = true;
					}

					ImGui::Text("Framerate");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(160.0f);
					uint8_t fps_min = 1, fps_max = 120;
					if (ImGui::SliderScalar("##target_fps", ImGuiDataType_U8, &screen.framerate, &fps_min, &fps_max)) {
						screen.performanceProfile = performance_profile_t::CUSTOM;
						unsavedChanges = true;
						if (screen.source.get())
							screen.source->SetFramerate(screen.framerate);
					}

					const char* scalingModes[] = { "Stretch", "Fit (keep aspect ratio)", "Crop (fill screen)" };
					int selectedScalingMode = static_cast<int>(screen.scaleMode);
					if (ImGui::Combo("Scaling Mode", &selectedScalingMode, scalingModes, IM_ARRAYSIZE(scalingModes)))
					{
						screen.scaleMode = static_cast<scale_mode_t>(selectedScalingMode);
					saveConfiguration = true;
					}

					float brightnessPercent = screen.brightness * 100.0f;
					if (ImGui::SliderFloat(
						"Screen Brightness",
						&brightnessPercent,
						10.0f, 200.0f, "%.0f%%",
						ImGuiSliderFlags_AlwaysClamp))
					{
						screen.brightness = brightnessPercent / 100.0f;
						apply_screen_brightness(screen);
						saveConfiguration = true;
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::TextWrapped(
							screen.source &&
								screen.source->SupportsSourceBrightness()
							? "Integrated Media uses a GPU black-overlay filter "
							  "below 100%%, with no per-pixel game-thread cost. "
							  "Values above 100%% use the media client's GPU "
							  "brightness filter."
							: "100%% preserves the source. Window Capture and "
							  "Native Direct Media use a compatible CPU colour "
							  "lookup at other values.");
						ImGui::EndTooltip();
					}

					if (ImGui::Checkbox(
						"Automatic brightness from game lighting",
						&screen.autoBrightnessEnabled))
					{
						apply_screen_brightness(screen);
						saveConfiguration = true;
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::TextWrapped(
							"Samples a 4 x 4 grid from the game before the plugin "
							"UI is drawn, at most four times per second. GPU work "
							"is read asynchronously and remains off when this "
							"option is disabled on every screen.");
						ImGui::EndTooltip();
					}
					if (screen.autoBrightnessEnabled)
					{
						float darkPercent =
							screen.autoBrightnessDarkMultiplier * 100.0f;
						if (ImGui::SliderFloat(
							"Dark-scene brightness multiplier",
							&darkPercent, 25.0f, 125.0f, "%.0f%%",
							ImGuiSliderFlags_AlwaysClamp))
						{
							screen.autoBrightnessDarkMultiplier =
								darkPercent / 100.0f;
							apply_screen_brightness(screen);
							saveConfiguration = true;
						}
						float brightPercent =
							screen.autoBrightnessBrightMultiplier * 100.0f;
						if (ImGui::SliderFloat(
							"Bright-scene brightness multiplier",
							&brightPercent, 50.0f, 200.0f, "%.0f%%",
							ImGuiSliderFlags_AlwaysClamp))
						{
							screen.autoBrightnessBrightMultiplier =
								brightPercent / 100.0f;
							apply_screen_brightness(screen);
							saveConfiguration = true;
						}
						if (g_game_lighting_valid.load())
						{
							ImGui::TextDisabled(
								"Game lighting: %.0f%% | effective screen: %.0f%%",
								g_game_lighting_luminance.load() * 100.0f,
								screen.effectiveBrightness * 100.0f);
						}
						else
						{
							ImGui::TextDisabled(
								"Waiting for the first game-lighting sample...");
						}
					}

					uint8_t guardMinimum = 0;
					uint8_t guardMaximum = 16;
					if (ImGui::SliderScalar(
						"Edge Colour-Bleed Guard",
						ImGuiDataType_U8,
						&screen.edgeBleedGuard,
						&guardMinimum,
						&guardMaximum,
						"%u px"))
					{
						screen.hasUploadedFrame = false;
						saveConfiguration = true;
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::TextWrapped(
							"Prevents the outer video colour from leaking into the "
							"truck GPS bezel. 2 px is recommended; 0 disables it.");
						ImGui::EndTooltip();
					}

					if (ImGui::Checkbox("Pause / Freeze", &screen.paused))
					{
						if (screen.source)
							screen.source->SetPaused(screen.paused);
						saveConfiguration = true;
					}
					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::Text("Keeps the last image and stops plugin frame processing");
						ImGui::EndTooltip();
					}

					bool sourceChanged = false;
					if (screen.contentMode == content_mode_t::WINDOW_CAPTURE)
					{
						const bool captureModeChanged =
							ImGui::Checkbox("Legacy Capture", &screen.legacyCapture);
						if (ImGui::IsItemHovered())
						{
							ImGui::BeginTooltip();
							ImGui::Text("Not recommended; only use if modern capture fails");
							ImGui::EndTooltip();
						}

						const char* preview =
							screen.source_application_display_name.empty()
							? (screen.source_application_name.empty()
								? "Select Source..."
								: screen.source_application_name.c_str())
							: screen.source_application_display_name.c_str();

						sourceChanged =
							captureModeChanged &&
							!screen.source_application_name.empty();
						if (ImGui::BeginCombo("Window Source", preview))
						{
							int applicationIndex = 0;
							for (const auto& [title, application] : applications)
							{
								const bool selected =
									title == screen.source_application_display_name ||
									application == screen.source_application_name;
								if (ImGui::Selectable(
									("[" + application + "] " + title + "##" +
										std::to_string(applicationIndex)).c_str(),
									selected))
								{
									screen.source_application_name = application;
									screen.source_application_display_name = title;
									sourceChanged = true;
								}
								if (selected)
									ImGui::SetItemDefaultFocus();
								++applicationIndex;
							}
							ImGui::EndCombo();
						}
					}
					else
					{
						if (screen.contentMode ==
							content_mode_t::INTEGRATED_MEDIA)
						{
							const char* mediaServices[] = {
								"YouTube", "Spotify"
							};
							int selectedService =
								static_cast<int>(screen.mediaService);
							if (ImGui::Combo(
								"Media Service", &selectedService,
								mediaServices,
								IM_ARRAYSIZE(mediaServices)))
							{
								screen.mediaService =
									static_cast<media_service_t>(
										selectedService);
								auto& selectedUrls =
									screen.mediaService ==
										media_service_t::YOUTUBE
									? screen.youtubeUrls
									: screen.spotifyUrls;
								uint32_t& selectedIndex =
									screen.mediaService ==
										media_service_t::YOUTUBE
									? screen.selectedYoutubeUrl
									: screen.selectedSpotifyUrl;
								if (!selectedUrls.empty())
								{
									selectedIndex = (std::min)(
										selectedIndex,
										static_cast<uint32_t>(
											selectedUrls.size() - 1));
									screen.mediaUrl =
										selectedUrls[selectedIndex];
								}
								else
								{
									screen.mediaUrl.clear();
								}
								if (screen.source &&
									!screen.mediaUrl.empty())
								{
									screen.source->LoadMedia(
										screen.mediaUrl);
								}
								saveConfiguration = true;
							}

							if (screen.mediaService ==
								media_service_t::SPOTIFY)
							{
								ImGui::TextColored(
									ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
									"Spotify uses Full Web Player");
								ImGui::TextWrapped(
									"The normal Spotify website provides persistent "
									"login and native track controls. All Spotify "
									"playback now follows this single implementation.");
								if (screen.source && ImGui::Button(
									"Open Spotify login / controls"))
									screen.source->ShowInteractivePlayer(true);
								ImGui::SameLine();
								if (screen.source && ImGui::Button(
									"Return helper to silent mode"))
									screen.source->ShowInteractivePlayer(false);
								if (screen.source && ImGui::Button(
									"Clear Spotify login/session"))
									screen.source->ClearBrowserSession();
							}

							auto& mediaUrls =
								screen.mediaService ==
									media_service_t::YOUTUBE
								? screen.youtubeUrls
								: screen.spotifyUrls;
							uint32_t& selectedMediaUrl =
								screen.mediaService ==
									media_service_t::YOUTUBE
								? screen.selectedYoutubeUrl
								: screen.selectedSpotifyUrl;
							if (!mediaUrls.empty())
							{
								selectedMediaUrl = (std::min)(
									selectedMediaUrl,
									static_cast<uint32_t>(
										mediaUrls.size() - 1));
							}

							const char* selectedUrlPreview =
								mediaUrls.empty()
								? "No saved links"
								: mediaUrls[selectedMediaUrl].c_str();
							if (ImGui::BeginCombo(
								"Saved Links", selectedUrlPreview))
							{
								for (size_t urlIndex = 0;
									urlIndex < mediaUrls.size();
									++urlIndex)
								{
									const bool selected =
										urlIndex == selectedMediaUrl;
									const std::string label =
										std::to_string(urlIndex + 1) +
										". " + mediaUrls[urlIndex] +
										"##media_link_" +
										std::to_string(urlIndex);
									if (ImGui::Selectable(
										label.c_str(), selected))
									{
										selectedMediaUrl =
											static_cast<uint32_t>(
												urlIndex);
										screen.mediaUrl =
											mediaUrls[urlIndex];
										if (screen.source)
											screen.source->LoadMedia(
												screen.mediaUrl);
										saveConfiguration = true;
									}
									if (selected)
										ImGui::SetItemDefaultFocus();
								}
								ImGui::EndCombo();
							}
						}

						ImGui::SetNextItemWidth(-120.0f);
						if (ImGui::InputTextWithHint(
							"##media_url",
							screen.contentMode == content_mode_t::INTEGRATED_MEDIA
								? "YouTube / Spotify / playlist / file / direct URL"
								: "Local file path or direct media URL",
							&screen.mediaUrl))
							unsavedChanges = true;
						ImGui::SameLine();
						if (ImGui::Button("Start / Reload"))
						{
							if (screen.contentMode ==
								content_mode_t::INTEGRATED_MEDIA &&
								screen.source &&
								screen.source->LoadMedia(screen.mediaUrl))
							{
								sourceChanged = false;
							}
							else
							{
								sourceChanged = true;
							}
							saveConfiguration = true;
						}

						if (screen.contentMode ==
							content_mode_t::INTEGRATED_MEDIA)
						{
							auto& mediaUrls =
								screen.mediaService ==
									media_service_t::YOUTUBE
								? screen.youtubeUrls
								: screen.spotifyUrls;
							uint32_t& selectedMediaUrl =
								screen.mediaService ==
									media_service_t::YOUTUBE
								? screen.selectedYoutubeUrl
								: screen.selectedSpotifyUrl;

							if (ImGui::Button("Add Link") &&
								!screen.mediaUrl.empty())
							{
								const auto existing = std::find(
									mediaUrls.begin(), mediaUrls.end(),
									screen.mediaUrl);
								if (existing == mediaUrls.end())
								{
									mediaUrls.push_back(screen.mediaUrl);
									selectedMediaUrl =
										static_cast<uint32_t>(
											mediaUrls.size() - 1);
								}
								else
								{
									selectedMediaUrl =
										static_cast<uint32_t>(
											std::distance(
												mediaUrls.begin(),
												existing));
								}
								saveConfiguration = true;
							}
							ImGui::SameLine();
							if (ImGui::Button("Update Selected") &&
								!screen.mediaUrl.empty() &&
								!mediaUrls.empty())
							{
								selectedMediaUrl = (std::min)(
									selectedMediaUrl,
									static_cast<uint32_t>(
										mediaUrls.size() - 1));
								mediaUrls[selectedMediaUrl] =
									screen.mediaUrl;
								saveConfiguration = true;
							}
							ImGui::SameLine();
							if (ImGui::Button("Remove Selected") &&
								!mediaUrls.empty())
							{
								selectedMediaUrl = (std::min)(
									selectedMediaUrl,
									static_cast<uint32_t>(
										mediaUrls.size() - 1));
								mediaUrls.erase(
									mediaUrls.begin() +
									selectedMediaUrl);
								if (mediaUrls.empty())
								{
									selectedMediaUrl = 0;
									screen.mediaUrl.clear();
								}
								else
								{
									selectedMediaUrl = (std::min)(
										selectedMediaUrl,
										static_cast<uint32_t>(
											mediaUrls.size() - 1));
									screen.mediaUrl =
										mediaUrls[selectedMediaUrl];
								}
								saveConfiguration = true;
							}
							ImGui::TextDisabled(
								"%s links: %zu",
								screen.mediaService ==
									media_service_t::YOUTUBE
									? "YouTube" : "Spotify",
								mediaUrls.size());
						}
					}

					if (sourceChanged)
					{
						if (!rebuild_source(screen))
							ImGui::OpenPopup("Source Error");
						saveConfiguration = true;
					}

						if (screen.source && screen.source->SupportsMediaControls())
					{
							ImGui::Text("Status: %s",
								screen.source->GetStatusText().c_str());
							if (ImGui::Button("Play / Pause"))
								dispatch_media_command(
									screen,
									media_command_t::PLAY_PAUSE);
							ImGui::SameLine();
							if (ImGui::Button("Previous"))
							{
								dispatch_media_command(
									screen,
									media_command_t::PREVIOUS);
								if (screen.mediaService ==
									media_service_t::SPOTIFY)
									saveConfiguration = true;
							}
							ImGui::SameLine();
							if (ImGui::Button("Next"))
							{
								dispatch_media_command(
									screen,
									media_command_t::NEXT);
								if (screen.mediaService ==
									media_service_t::SPOTIFY)
									saveConfiguration = true;
							}
							ImGui::SameLine();
							if (ImGui::Button("Mute"))
								dispatch_media_command(
									screen,
									media_command_t::MUTE);
							ImGui::SameLine();
							if (ImGui::Button("Vol -"))
								dispatch_media_command(
									screen,
									media_command_t::VOLUME_DOWN);
							ImGui::SameLine();
							if (ImGui::Button("Vol +"))
								dispatch_media_command(
									screen,
									media_command_t::VOLUME_UP);
						bool isHotkeyTarget = screen.hotkeyTarget;
						if (ImGui::Checkbox(
							"Use this screen for media hotkeys",
							&isHotkeyTarget))
						{
							if (isHotkeyTarget)
							{
								for (auto& other : g_screens)
									other.hotkeyTarget = false;
							}
							screen.hotkeyTarget = isHotkeyTarget;
							saveConfiguration = true;
							}
						}

						const bool supportsVehiclePower =
							screen.source &&
							screen.source->SupportsVehiclePowerControl();
						ImGui::BeginDisabled(!supportsVehiclePower);
						if (ImGui::Checkbox(
							"Play media only while truck engine is running",
							&screen.followTruckEngine))
						{
							if (screen.source)
							{
								const bool powered =
									!screen.followTruckEngine ||
									!g_telemetry_driving.load() ||
									g_engine_enabled.load();
								screen.source->SetVehiclePowered(powered);
							}
							apply_screen_brightness(screen);
							saveConfiguration = true;
						}
						if (screen.followTruckEngine)
						{
							float engineOffPercent =
								screen.engineOffBrightness * 100.0f;
							if (ImGui::SliderFloat(
								"Engine-off logo brightness",
								&engineOffPercent,
								5.0f, 100.0f, "%.0f%%",
								ImGuiSliderFlags_AlwaysClamp))
							{
								screen.engineOffBrightness =
									engineOffPercent / 100.0f;
								apply_screen_brightness(screen);
								saveConfiguration = true;
							}
							if (ImGui::IsItemHovered())
							{
								ImGui::BeginTooltip();
								ImGui::TextWrapped(
									"Replaces media with the current truck brand and "
									"model logo while playback is paused. This controls "
									"the standby logo brightness.");
								ImGui::EndTooltip();
							}
						}
						ImGui::EndDisabled();
						if (!supportsVehiclePower)
						{
							ImGui::TextDisabled(
								"Engine-follow playback is available for integrated "
								"and native media.");
						}
						else if (!g_telemetry_driving.load())
						{
							ImGui::TextDisabled(
								"Engine control inactive in menus / before driving.");
						}
						else
						{
							ImGui::TextColored(
								g_engine_enabled.load()
									? ImVec4(0.35f, 0.85f, 0.40f, 1.0f)
									: ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
								"Truck engine: %s | Media power: %s",
								g_engine_enabled.load() ? "running" : "off",
								(!screen.followTruckEngine ||
									g_engine_enabled.load())
									? "on" : "paused");
						}

						if (ImGui::TreeNode("Adaptive Cabin Audio"))
						{
							const bool supported =
								screen.source &&
								screen.source->SupportsSpatialAudio();
							if (!supported)
							{
								ImGui::TextColored(
									ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
									"Available with Integrated Media Client");
								ImGui::TextWrapped(
									"Spatializing Native Direct Media inside the plugin "
									"would also change the game's engine/audio session, so "
									"that unsafe path is intentionally disabled.");
							}

							ImGui::BeginDisabled(!supported);
							if (ImGui::Checkbox(
								"Enable head-position adaptive sound",
								&screen.adaptiveAudioEnabled))
							{
								if (!screen.adaptiveAudioEnabled &&
									screen.source)
									screen.source->SetSpatialAudio(
										1.0f, 0.0f, false);
								saveConfiguration = true;
							}

							if (screen.adaptiveAudioEnabled)
							{
								float interiorVolumePercent =
									screen.adaptiveAudioInteriorVolume * 100.0f;
								if (ImGui::SliderFloat(
									"Interior-cab master volume",
									&interiorVolumePercent,
									0.0f, 100.0f, "%.0f%%",
									ImGuiSliderFlags_AlwaysClamp))
								{
									screen.adaptiveAudioInteriorVolume =
										interiorVolumePercent / 100.0f;
									saveConfiguration = true;
								}
								if (ImGui::IsItemHovered())
								{
									ImGui::BeginTooltip();
									ImGui::TextWrapped(
										"Reduces media only in the interior camera. "
										"Outside-cab near and far volumes remain unchanged.");
									ImGui::EndTooltip();
								}
								if (ImGui::SliderFloat(
									"Spatial strength",
									&screen.adaptiveAudioStrength,
									0.0f, 1.0f, "%.2f",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Speaker direction",
									&screen.adaptiveAudioSpeakerAzimuth,
									-90.0f, 90.0f, "%.0f deg",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Volume facing away",
									&screen.adaptiveAudioFacingAwayVolume,
									0.0f, 1.0f, "%.2f",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Outside-cab distance",
									&screen.adaptiveAudioOutsideDistance,
									0.25f, 2.5f, "%.2f m",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Minimum volume when far away",
									&screen.adaptiveAudioOutsideVolume,
									0.0f, 1.0f, "%.2f",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Outside-cab volume at 0 m",
									&screen.adaptiveAudioExternalNearVolume,
									0.0f, 1.0f, "%.2f",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::IsItemHovered())
								{
									ImGui::BeginTooltip();
									ImGui::TextWrapped(
										"Maximum media volume heard from an external "
										"camera beside the closed truck.");
									ImGui::EndTooltip();
								}
								if (ImGui::SliderFloat(
									"Outside-cab cutoff at 0 m",
									&screen.adaptiveAudioExternalNearCutoff,
									20.0f, 20000.0f, "%.0f Hz",
									ImGuiSliderFlags_Logarithmic |
										ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::IsItemHovered())
								{
									ImGui::BeginTooltip();
									ImGui::TextWrapped(
										"Closed-cab muffling applied even when the "
										"external camera is touching the truck. "
										"The user-selected near and far cutoffs "
										"are scaled dynamically with distance.");
									ImGui::EndTooltip();
								}
								if (ImGui::Checkbox(
									"Use exact external-camera distance (SPF)",
									&screen.adaptiveAudioExternalDistanceEnabled))
									saveConfiguration = true;
								if (screen.adaptiveAudioExternalDistanceEnabled)
								{
									if (ImGui::SliderFloat(
										"Near-value distance",
										&screen.adaptiveAudioExternalFullVolumeDistance,
										0.0f, 10.0f, "%.1f m",
										ImGuiSliderFlags_AlwaysClamp))
										saveConfiguration = true;
									if (ImGui::SliderFloat(
										"Far-value distance",
										&screen.adaptiveAudioExternalMuteDistance,
										2.0f, 50.0f, "%.1f m",
										ImGuiSliderFlags_AlwaysClamp))
										saveConfiguration = true;
									if (ImGui::Checkbox(
										"Distance low-pass (muffle far audio)",
										&screen.adaptiveAudioExternalLowPassEnabled))
										saveConfiguration = true;
									if (screen.adaptiveAudioExternalLowPassEnabled &&
										ImGui::SliderFloat(
											"Far-distance cutoff",
											&screen.adaptiveAudioExternalMinimumCutoff,
											20.0f,
											(std::max)(
												20.0f,
												screen.
													adaptiveAudioExternalNearCutoff),
											"%.0f Hz",
											ImGuiSliderFlags_Logarithmic |
												ImGuiSliderFlags_AlwaysClamp))
										saveConfiguration = true;
								}
								float menuVolumePercent =
									screen.adaptiveAudioMenuVolume * 100.0f;
								if (ImGui::SliderFloat(
									"Volume in menus / before driving",
									&menuVolumePercent,
									0.0f, 100.0f, "%.0f%%",
									ImGuiSliderFlags_AlwaysClamp))
								{
									screen.adaptiveAudioMenuVolume =
										menuVolumePercent / 100.0f;
									saveConfiguration = true;
								}

								const bool driving =
									g_telemetry_driving.load();
								const uint64_t now = GetTickCount64();
								const uint64_t lastHeadUpdate =
									g_last_head_update_tick.load();
								const bool headTelemetryFresh =
									driving && lastHeadUpdate != 0 &&
									now >= lastHeadUpdate &&
									now - lastHeadUpdate <= 500;
								const bool exactCamera =
									g_camera_bridge_connected.load();
								const bool externalCamera =
									driving &&
									(exactCamera
										? g_camera_type.load() != 2
										: (!g_camera_interior_hint.load() ||
											!headTelemetryFresh));

								if (!driving)
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
										"Detected state: menu / paused (%.0f%% volume)",
										menuVolumePercent);
								}
								else if (externalCamera)
								{
									ImGui::TextColored(
										ImVec4(0.35f, 0.75f, 1.0f, 1.0f),
										"Detected state: external camera");
									if (exactCamera &&
										g_head_anchor_calibrated.load())
									{
										ImGui::Text(
											"Camera distance from driver: %.1f m",
											g_external_camera_distance.load());
										ImGui::Text(
											"Distance gain: %.0f%% | low-pass: %.0f Hz",
											g_adaptive_audio_distance_gain.load() *
												100.0f,
											g_adaptive_audio_lowpass_hz.load());
									}
									else if (exactCamera)
									{
										ImGui::TextColored(
											ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
											"Head anchor not calibrated: switch to "
											"camera 1 once.");
									}
									else
									{
										if (!g_camera_bridge_mapping_present.load())
										{
											ImGui::TextColored(
												ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
												"SPF bridge is not loaded; using fixed "
												"outside volume.");
										}
										else if (!g_camera_bridge_activated.load())
										{
											ImGui::TextColored(
												ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
												"SPF found the bridge, but it is disabled. "
												"Enable it in Delete > Plugins.");
										}
										else
										{
											ImGui::TextColored(
												ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
												"SPF camera data is not ready; using fixed "
												"outside volume.");
										}
									}
								}
								else
								{
									const float headDistance = std::sqrt(
										g_head_offset_x.load() *
											g_head_offset_x.load() +
										g_head_offset_y.load() *
											g_head_offset_y.load() +
										g_head_offset_z.load() *
											g_head_offset_z.load());
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"Detected state: interior camera");
									ImGui::Text(
										"Live head offset: %.2f m | heading: %.0f deg",
										headDistance,
										g_head_heading.load() * 360.0f);
								}
								ImGui::TextWrapped(
									"Speaker direction: negative is left, positive is "
									"right. The SPF companion measures the true camera "
									"distance and follows the moving truck. Without SPF, "
									"camera keys and telemetry freshness provide a safe "
									"fixed-volume fallback.");
							}
							ImGui::EndDisabled();
							ImGui::TreePop();
						}

						if (ImGui::TreeNode("AI Traffic Radios (SPF)"))
						{
							ImGui::TextWrapped(
								"Uses independent local audio files or direct audio URLs. "
								"It never opens Spotify or YouTube, so the dashboard "
								"player keeps its playback session. One nearest audible "
								"AI vehicle is decoded at a time.");

							if (!screen.trafficRadioSources.empty())
							{
								screen.selectedTrafficRadioSource = (std::min)(
									screen.selectedTrafficRadioSource,
									static_cast<uint32_t>(
										screen.trafficRadioSources.size() - 1));
								const std::string& preview = screen.trafficRadioSources[
									screen.selectedTrafficRadioSource];
								if (ImGui::BeginCombo(
									"Traffic audio sources", preview.c_str()))
								{
									for (uint32_t index = 0;
										index < screen.trafficRadioSources.size(); ++index)
									{
										const bool selected =
											index == screen.selectedTrafficRadioSource;
										if (ImGui::Selectable(
											screen.trafficRadioSources[index].c_str(), selected))
										{
											screen.selectedTrafficRadioSource = index;
											screen.trafficRadioSourceDraft =
												screen.trafficRadioSources[index];
											saveConfiguration = true;
										}
										if (selected)
											ImGui::SetItemDefaultFocus();
									}
									ImGui::EndCombo();
								}
							}
							ImGui::InputTextWithHint(
								"##traffic_audio_source",
								"C:\\Music\\song.mp3 or https://host/audio.mp3",
								&screen.trafficRadioSourceDraft);
							const bool invalidPage = is_traffic_streaming_web_page(
								screen.trafficRadioSourceDraft);
							if (invalidPage)
							{
								ImGui::TextColored(
									ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
									"Spotify/YouTube page links cannot be traffic sources. "
									"Use a local audio file or a direct media/stream URL.");
							}
							const bool sourceReady =
								!screen.trafficRadioSourceDraft.empty() && !invalidPage;
							ImGui::BeginDisabled(!sourceReady);
							if (ImGui::Button("Add traffic source"))
							{
								const auto existing = std::find(
									screen.trafficRadioSources.begin(),
									screen.trafficRadioSources.end(),
									screen.trafficRadioSourceDraft);
								if (existing == screen.trafficRadioSources.end())
									screen.trafficRadioSources.push_back(
										screen.trafficRadioSourceDraft);
								screen.selectedTrafficRadioSource =
									static_cast<uint32_t>(
										std::distance(
											screen.trafficRadioSources.begin(),
											std::find(
												screen.trafficRadioSources.begin(),
												screen.trafficRadioSources.end(),
												screen.trafficRadioSourceDraft)));
								saveConfiguration = true;
							}
							ImGui::SameLine();
							ImGui::BeginDisabled(screen.trafficRadioSources.empty());
							if (ImGui::Button("Update selected"))
							{
								screen.trafficRadioSources[
									screen.selectedTrafficRadioSource] =
										screen.trafficRadioSourceDraft;
								saveConfiguration = true;
							}
							ImGui::EndDisabled();
							ImGui::EndDisabled();
							ImGui::SameLine();
							ImGui::BeginDisabled(screen.trafficRadioSources.empty());
							if (ImGui::Button("Remove selected"))
							{
								screen.trafficRadioSources.erase(
									screen.trafficRadioSources.begin() +
										screen.selectedTrafficRadioSource);
								if (screen.trafficRadioSources.empty())
								{
									screen.selectedTrafficRadioSource = 0;
									screen.trafficRadioSourceDraft.clear();
									screen.trafficRadioEnabled = false;
								}
								else
								{
									screen.selectedTrafficRadioSource = (std::min)(
										screen.selectedTrafficRadioSource,
										static_cast<uint32_t>(
											screen.trafficRadioSources.size() - 1));
									screen.trafficRadioSourceDraft =
										screen.trafficRadioSources[
											screen.selectedTrafficRadioSource];
								}
								saveConfiguration = true;
							}
							ImGui::EndDisabled();

							const bool supported =
								!screen.trafficRadioSources.empty() &&
								sources::IsMediaClientInstalled();
							if (!sources::IsMediaClientInstalled())
								ImGui::TextColored(
									ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
									"PrismMediaClient.exe is required.");

							ImGui::BeginDisabled(!supported);
							if (ImGui::Checkbox(
								"Enable positional AI traffic music",
								&screen.trafficRadioEnabled))
								saveConfiguration = true;
							if (screen.trafficRadioEnabled)
							{
								float densityPercent =
									screen.trafficRadioVehicleDensity * 100.0f;
								if (ImGui::SliderFloat(
									"AI vehicles with music",
									&densityPercent, 0.0f, 100.0f, "%.0f%%",
									ImGuiSliderFlags_AlwaysClamp))
								{
									screen.trafficRadioVehicleDensity =
										densityPercent / 100.0f;
									saveConfiguration = true;
								}
								float volumePercent =
									screen.trafficRadioMaximumVolume * 100.0f;
								if (ImGui::SliderFloat(
									"Maximum traffic-radio volume",
									&volumePercent, 0.0f, 100.0f, "%.0f%%",
									ImGuiSliderFlags_AlwaysClamp))
								{
									screen.trafficRadioMaximumVolume =
										volumePercent / 100.0f;
									saveConfiguration = true;
								}
								if (ImGui::SliderFloat(
									"Traffic-radio full-volume distance",
									&screen.trafficRadioFullVolumeDistance,
									0.0f, 10.0f, "%.1f m",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Traffic-radio mute distance",
									&screen.trafficRadioMuteDistance,
									5.0f, 60.0f, "%.1f m",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Traffic-radio near cutoff",
									&screen.trafficRadioNearCutoff,
									100.0f, 8000.0f, "%.0f Hz",
									ImGuiSliderFlags_Logarithmic |
										ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
								if (ImGui::SliderFloat(
									"Traffic-radio far cutoff",
									&screen.trafficRadioFarCutoff,
									20.0f,
									(std::max)(
										20.0f, screen.trafficRadioNearCutoff),
									"%.0f Hz",
									ImGuiSliderFlags_Logarithmic |
										ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;

								const auto radioStatus = traffic_audio::status();
								if (!radioStatus.bridgeAvailable)
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
										"Waiting for SPF AI traffic data.");
								}
								else if (radioStatus.active)
								{
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"AI %d at %.1f m | gain %.0f%% | pan %.2f | %.0f Hz",
										radioStatus.emitterId,
										radioStatus.distance,
										radioStatus.gain * 100.0f,
										radioStatus.pan,
										radioStatus.cutoffHz);
									ImGui::TextWrapped(
										"Source: %s",
										radioStatus.currentSource.c_str());
								}
								else
								{
									ImGui::TextDisabled(
										"Observed %u AI; %u selected; none in range.",
										radioStatus.observedVehicles,
										radioStatus.eligibleVehicles);
								}
							}
							ImGui::EndDisabled();
							ImGui::TreePop();
						}

						if (ImGui::TreeNode("Automatic Reverse Camera"))
						{
							ImGui::TextColored(
								ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
								"Expected impact: none forward; low while reversing");
								ImGui::TextWrapped(
									"The current legacy mode captures a calibrated part of the "
									"game window. The experimental internal mode activates "
									"ETS2's dormant park-camera slot and reads its 256x256 "
									"result through a non-blocking three-buffer staging ring. "
									"The GPS remains the stable CPU-writable texture proven by "
									"the v2.8.1 loading-crash hotfix.");

							if (ImGui::TreeNode(
								"Internal render-to-texture probe"))
							{
								const auto probeStatus =
									dx11::internal_render_probe::status();
								if (probeStatus.supportedBuild)
								{
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"Exact ETS2 1.60.1.7 build recognized.");
								}
								else
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.30f, 0.30f, 1.0f),
										"This ETS2 executable is not supported by "
										"the internal probe.");
									ImGui::Text(
										"Timestamp 0x%08X | image 0x%08X | "
										"signature matches %u",
										probeStatus.timeDateStamp,
										probeStatus.imageSize,
										probeStatus.signatureMatches);
								}

								ImGui::Text(
									"Internal camera scheduler: %s",
									probeStatus.mirrorHookInstalled
										? (probeStatus.mirrorScheduleSeen
											? "detected and active"
											: "hooked; waiting for gameplay")
										: "not hooked");
								ImGui::Text(
									"D3D11 render-target observer: %s",
									probeStatus.contextHookInstalled
										? "ready" : "not available");
								ImGui::Text(
									"Park-camera hooks: init %s | scheduler %s",
									probeStatus.resourceInitHookInstalled
										? "ready" : "not available",
									probeStatus.activeMaskHookInstalled
										? "ready" : "not available");
								ImGui::Text(
									"Park activation: requested %s | "
									"camera %s | resource %s",
									probeStatus.parkActivationRequested
										? "yes" : "no",
									probeStatus.parkCameraInstalled
										? "installed" : "not installed",
									probeStatus.parkResourcePresent
										? "created" : "not created");
								ImGui::Text(
									"Park render request: %s | "
									"render schedules: %llu",
									probeStatus.parkRenderRequested
										? "active" : "inactive",
									static_cast<unsigned long long>(
										probeStatus.parkScheduleCount));
								ImGui::Text(
									"Scheduler calls seen: %llu",
									static_cast<unsigned long long>(
										probeStatus.mirrorScheduleCount));

								if (probeStatus.mirrorScheduleSeen)
								{
									ImGui::Text(
										"Internal camera slots present "
										"(mask 0x%03X):",
										probeStatus.mirrorSlotMask);
									bool anySlot{};
									for (uint32_t slot = 0;
										slot < 9; ++slot)
									{
										if ((probeStatus.mirrorSlotMask &
											(1U << slot)) == 0)
										{
											continue;
										}
										ImGui::SameLine();
										ImGui::Text(
											"%s%s(%ux%u)",
											anySlot ? "," : "",
											dx11::internal_render_probe::
												slot_name(slot),
											probeStatus.slotWidth[slot],
											probeStatus.slotHeight[slot]);
										anySlot = true;
									}
									if (!anySlot)
									{
										ImGui::SameLine();
										ImGui::TextUnformatted("none yet");
									}
								}

								ImGui::Separator();
								ImGui::TextWrapped(
									"The internal camera is displayed automatically in reverse "
									"or Preview after its live colour target is observed. The "
									"trace remains optional diagnostics.");
								ImGui::TextColored(
									ImVec4(0.55f, 0.75f, 0.95f, 1.0f),
									"Normal forward driving has no additional park-camera "
									"render cost.");

								const bool canTrace =
									probeStatus.supportedBuild &&
									probeStatus.mirrorHookInstalled &&
									probeStatus.activeMaskHookInstalled &&
									probeStatus.contextHookInstalled &&
									(!probeStatus.parkActivationRequested ||
										(probeStatus.parkCameraInstalled &&
											probeStatus.parkResourcePresent &&
											probeStatus.parkRenderRequested));
								ImGui::BeginDisabled(
									!canTrace && !probeStatus.tracing);
								if (!probeStatus.tracing)
								{
									if (ImGui::Button(
										"Start 10-second RTT trace"))
									{
										dx11::internal_render_probe::
											begin_trace(10);
									}
								}
								else
								{
									const uint64_t now = GetTickCount64();
									const uint64_t millisecondsLeft =
										probeStatus.traceEndTick > now
											? probeStatus.traceEndTick - now
											: 0;
									ImGui::TextColored(
										ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
										"Tracing: %.1f seconds left | "
										"%zu targets observed",
										static_cast<double>(
											millisecondsLeft) / 1000.0,
										probeStatus.candidateCount);
									ImGui::SameLine();
									if (ImGui::Button("Stop RTT trace"))
									{
										dx11::internal_render_probe::
											end_trace();
									}
								}
								ImGui::EndDisabled();

								if (!probeStatus.tracing &&
									probeStatus.candidateCount > 0)
								{
									const auto candidates =
										dx11::internal_render_probe::
											candidates();
									ImGui::Separator();
									ImGui::Text(
										"Best render-target candidates "
										"(%zu total):",
										candidates.size());
									const size_t shown = (std::min)(
										candidates.size(),
										static_cast<size_t>(12));
									for (size_t index = 0;
										index < shown; ++index)
									{
										const auto& candidate =
											candidates[index];
										ImGui::Text(
											"#%u  %ux%u  %s  "
											"slot 0x%03X  binds %llu  "
											"inside/near %llu/%llu",
											candidate.id,
											candidate.width,
											candidate.height,
											dx11::internal_render_probe::
												format_name(
													candidate.format),
											candidate.
												matchingCameraSlotMask,
											static_cast<
												unsigned long long>(
													candidate.bindCount),
											static_cast<
												unsigned long long>(
													candidate.
														duringMirrorScheduleBindCount),
											static_cast<
												unsigned long long>(
													candidate.
														nearMirrorBindCount));
									}
									ImGui::TextWrapped(
										"Full results were written to game.log.txt. "
										"These details can be used to validate a future "
										"ETS2 executable update.");
								}
								ImGui::TreePop();
							}

							if (ImGui::Checkbox(
								"Show reverse view on this screen",
								&screen.reverseCameraEnabled))
							{
								if (!rebuild_reverse_source(screen))
									ImGui::OpenPopup("Reverse Source Error");
								saveConfiguration = true;
							}

							if (screen.reverseCameraEnabled)
							{
								ImGui::Text(
									"Reverse detected: %s",
									g_reverse_active.load() ? "Yes" : "No");
								static const char* reverseMethodNames[] = {
									"Window crop (stable)",
									"Internal park camera (safe staged test)"
								};
								int reverseMethod = static_cast<int>(
									screen.reverseCameraMethod);
								if (ImGui::Combo(
									"Reverse camera method",
									&reverseMethod,
									reverseMethodNames,
									IM_ARRAYSIZE(reverseMethodNames)))
								{
									screen.reverseCameraMethod =
										static_cast<reverse_camera_method_t>(
											reverseMethod);
									if (!rebuild_reverse_source(screen))
										ImGui::OpenPopup(
											"Reverse Source Error");
									saveConfiguration = true;
								}

								const bool internalParkMethod =
									screen.reverseCameraMethod ==
										reverse_camera_method_t::
											INTERNAL_PARK_PROBE;
								if (ImGui::Checkbox(
									"Preview / calibrate now",
									&screen.reversePreview))
								{
									if (screen.reversePreview &&
										!screen.reverseSource)
									{
										if (!rebuild_reverse_source(screen))
											ImGui::OpenPopup(
												"Reverse Source Error");
									}
									if (screen.reverseSource)
										screen.reverseSource->SetPaused(
											!(g_reverse_active.load() ||
												screen.reversePreview));
								}

								if (internalParkMethod)
								{
									ImGui::TextColored(
										ImVec4(0.55f, 0.75f, 0.95f, 1.0f),
										"Safe output: engine target -> non-blocking staging "
										"ring -> stable dynamic GPS texture.");
									const auto parkStatus =
										dx11::internal_render_probe::status();
									ImGui::TextColored(
										parkStatus.parkCameraInstalled &&
											parkStatus.parkResourcePresent &&
											parkStatus.parkReadbackReady
											? ImVec4(
												0.35f, 0.85f, 0.40f, 1.0f)
											: ImVec4(
												1.0f, 0.72f, 0.25f, 1.0f),
										"Internal slot 7: %s | resource: %s | "
										"readback: %s",
										parkStatus.parkCameraInstalled
											? "installed" : "waiting",
										parkStatus.parkResourcePresent
											? "ready" : "waiting",
										parkStatus.parkReadbackReady
											? "ready" : "waiting");
									if (!parkStatus.parkCameraInstalled)
									{
										ImGui::TextWrapped(
											"Save this selection, return to the "
											"profile/truck menu, and load the truck "
											"again. ETS2 creates mirror resources "
											"only while the truck interior loads.");
									}
									else if (!parkStatus.parkColorTargetReady)
									{
										ImGui::TextWrapped(
											"Put the truck in reverse or enable Preview. "
											"Normal media remains visible until the live "
											"park target is observed.");
									}
									else
									{
										ImGui::TextColored(
											ImVec4(
												0.35f, 0.85f, 0.40f, 1.0f),
											"GPU target: %ux%u %s | staged pixels: %s",
											parkStatus.parkTargetWidth,
											parkStatus.parkTargetHeight,
											dx11::internal_render_probe::
												format_name(
													parkStatus.
														parkTargetFormat),
											parkStatus.parkReadbackReady
												? "ready" : "waiting");
										ImGui::Text(
											"Target FPS: %u | decoded frames: %llu | "
											"busy skips: %llu",
											parkStatus.parkTargetFramerate,
											static_cast<unsigned long long>(
												parkStatus.parkOutputFrames),
											static_cast<unsigned long long>(
												parkStatus.
													parkReadbackBusySkips));
									}

									static const char*
										internalTargetNames[] = {
											"Auto (last matching buffer)",
											"Candidate A (recommended)",
											"Candidate B",
											"Candidate C",
											"Candidate D"
										};
									int internalTarget =
										static_cast<int>(
											screen.
												reverseInternalTargetVariant);
									if (ImGui::Combo(
										"Internal colour target",
										&internalTarget,
										internalTargetNames,
										IM_ARRAYSIZE(
											internalTargetNames)))
									{
										screen.reverseInternalTargetVariant =
											static_cast<uint8_t>(
												internalTarget);
										dx11::internal_render_probe::
											set_park_target_variant(
												static_cast<uint32_t>(
													internalTarget));
										saveConfiguration = true;
									}
									if (parkStatus.
										parkSelectedCandidate != 0)
									{
										ImGui::Text(
											"Matching buffers found: %u | "
											"active: Candidate %c",
											parkStatus.
												parkTargetCandidateCount,
											static_cast<char>(
												'A' +
												parkStatus.
													parkSelectedCandidate -
												1));
									}
									else
									{
										ImGui::Text(
											"Matching buffers found: %u | "
											"active: waiting",
											parkStatus.
												parkTargetCandidateCount);
									}
									ImGui::TextWrapped(
										"Candidate A is the first park-sized HDR "
										"buffer and is recommended for this ETS2 "
										"build. If the picture is flat or grey, try "
										"B, then C/D. Selection is saved and does "
										"not change the readback performance cost.");

									static const char*
										internalReverseProfileNames[] = {
											"Custom FPS",
											"Economy (10 FPS)",
											"Balanced (15 FPS)",
											"Quality (20 FPS)",
											"Ultra (30 FPS)"
										};
									int internalReverseProfile =
										static_cast<int>(
											screen.reversePerformanceProfile);
									if (ImGui::Combo(
										"Internal camera performance",
										&internalReverseProfile,
										internalReverseProfileNames,
										IM_ARRAYSIZE(
											internalReverseProfileNames)))
									{
										screen.reversePerformanceProfile =
											static_cast<
												reverse_performance_profile_t>(
													internalReverseProfile);
										apply_reverse_performance_profile(
											screen);
										saveConfiguration = true;
									}

									switch (
										screen.reversePerformanceProfile)
									{
									case reverse_performance_profile_t::ECONOMY:
										ImGui::TextColored(
											ImVec4(
												0.35f, 0.85f, 0.40f, 1.0f),
											"Very low impact: renders only while "
											"reversing at 10 FPS.");
										break;
									case reverse_performance_profile_t::BALANCED:
										ImGui::TextColored(
											ImVec4(
												0.35f, 0.85f, 0.40f, 1.0f),
											"Low impact: recommended 15 FPS.");
										break;
									case reverse_performance_profile_t::QUALITY:
										ImGui::TextColored(
											ImVec4(
												0.95f, 0.78f, 0.25f, 1.0f),
											"Medium impact: smoother 20 FPS.");
										break;
									case reverse_performance_profile_t::ULTRA:
										ImGui::TextColored(
											ImVec4(
												1.0f, 0.48f, 0.25f, 1.0f),
											"Higher impact: 30 FPS internal render.");
										break;
									case reverse_performance_profile_t::CUSTOM:
									default:
										{
											int internalFps =
												static_cast<int>(
													screen.reverseFramerate);
											if (ImGui::SliderInt(
												"Internal camera FPS",
												&internalFps,
												5,
												60))
											{
												screen.reverseFramerate =
													static_cast<uint8_t>(
														internalFps);
												saveConfiguration = true;
											}
										}
										break;
									}
									ImGui::TextWrapped(
										"Park-camera render cost is zero forward: slot 7 "
										"is scheduled only in reverse or Preview. Normal "
										"media returns to the stable direct dynamic-texture "
										"upload path. Readback maps never wait for the GPU; "
										"a busy frame is dropped instead.");
								}
								else
								{
									if (ImGui::Checkbox(
									"Zero forward impact",
									&screen.reverseZeroForwardImpact))
								{
									if (!rebuild_reverse_source(screen))
										ImGui::OpenPopup(
											"Reverse Source Error");
									saveConfiguration = true;
								}
								if (ImGui::IsItemHovered())
								{
									ImGui::BeginTooltip();
									ImGui::Text(
										"Closes capture while driving forward; "
										"reverse may take a moment to appear");
									ImGui::EndTooltip();
								}

								static const char* reverseProfileNames[] = {
									"Custom",
									"Economy (426x240 @ 10)",
									"Balanced (640x360 @ 15)",
									"Quality (960x540 @ 20)",
									"Ultra (1280x720 @ 30)"
								};
								int reverseProfile = static_cast<int>(
									screen.reversePerformanceProfile);
								if (ImGui::Combo(
									"Reverse performance profile",
									&reverseProfile,
									reverseProfileNames,
									IM_ARRAYSIZE(reverseProfileNames)))
								{
									screen.reversePerformanceProfile =
										static_cast<reverse_performance_profile_t>(
											reverseProfile);
									apply_reverse_performance_profile(screen);
									if (!rebuild_reverse_source(screen))
										ImGui::OpenPopup(
											"Reverse Source Error");
									saveConfiguration = true;
								}

								switch (screen.reversePerformanceProfile)
								{
								case reverse_performance_profile_t::ECONOMY:
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"Impact: Very low. Best for weak GPUs or 30 FPS gameplay.");
									break;
								case reverse_performance_profile_t::BALANCED:
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"Impact: Low. Recommended for normal reversing.");
									break;
								case reverse_performance_profile_t::QUALITY:
									ImGui::TextColored(
										ImVec4(0.95f, 0.78f, 0.25f, 1.0f),
										"Impact: Medium. Clearer image and smoother motion.");
									break;
								case reverse_performance_profile_t::ULTRA:
									ImGui::TextColored(
										ImVec4(1.0f, 0.48f, 0.25f, 1.0f),
										"Impact: High. Use only when substantial GPU headroom exists.");
									break;
								case reverse_performance_profile_t::CUSTOM:
								default:
									ImGui::TextColored(
										ImVec4(0.55f, 0.75f, 0.95f, 1.0f),
										"Impact depends on custom resolution and FPS.");
									break;
								}
								ImGui::Text(
									"Capture: %ux%u at %u FPS",
									screen.reverseCaptureWidth,
									screen.reverseCaptureHeight,
									screen.reverseFramerate);

								if (screen.reversePerformanceProfile ==
									reverse_performance_profile_t::CUSTOM)
								{
									int reverseWidth = static_cast<int>(
										screen.reverseCaptureWidth);
									int reverseHeight = static_cast<int>(
										screen.reverseCaptureHeight);
									int reverseFps = static_cast<int>(
										screen.reverseFramerate);
									bool customCaptureChanged = false;
									bool customCaptureFinished = false;
									customCaptureChanged |= ImGui::SliderInt(
										"Reverse capture width",
										&reverseWidth, 256, 1920);
									customCaptureFinished |=
										ImGui::IsItemDeactivatedAfterEdit();
									customCaptureChanged |= ImGui::SliderInt(
										"Reverse capture height",
										&reverseHeight, 144, 1080);
									customCaptureFinished |=
										ImGui::IsItemDeactivatedAfterEdit();
									customCaptureChanged |= ImGui::SliderInt(
										"Reverse view FPS",
										&reverseFps, 5, 60);
									customCaptureFinished |=
										ImGui::IsItemDeactivatedAfterEdit();
									if (customCaptureChanged)
									{
										screen.reverseCaptureWidth =
											static_cast<uint32_t>(reverseWidth);
										screen.reverseCaptureHeight =
											static_cast<uint32_t>(reverseHeight);
										screen.reverseFramerate =
											static_cast<uint8_t>(reverseFps);
										saveConfiguration = true;
									}
									if (customCaptureFinished)
									{
										if (!rebuild_reverse_source(screen))
											ImGui::OpenPopup(
												"Reverse Source Error");
									}
								}

								bool cropChanged = false;
								cropChanged |= ImGui::SliderFloat(
									"Mirror left",
									&screen.reverseCropLeft,
									0.0f, 0.98f, "%.3f",
									ImGuiSliderFlags_AlwaysClamp);
								cropChanged |= ImGui::SliderFloat(
									"Mirror top",
									&screen.reverseCropTop,
									0.0f, 0.98f, "%.3f",
									ImGuiSliderFlags_AlwaysClamp);
								cropChanged |= ImGui::SliderFloat(
									"Mirror width",
									&screen.reverseCropWidth,
									0.02f, 1.0f, "%.3f",
									ImGuiSliderFlags_AlwaysClamp);
								cropChanged |= ImGui::SliderFloat(
									"Mirror height",
									&screen.reverseCropHeight,
									0.02f, 1.0f, "%.3f",
									ImGuiSliderFlags_AlwaysClamp);
								if (cropChanged)
								{
									if (screen.reverseSource)
										screen.reverseSource->SetCaptureRegion(
											screen.reverseCropLeft,
											screen.reverseCropTop,
											screen.reverseCropWidth,
											screen.reverseCropHeight);
									saveConfiguration = true;
								}

								const bool reverseDormant =
									screen.reverseZeroForwardImpact &&
									!g_reverse_active.load() &&
									!screen.reversePreview;
								if (reverseDormant)
								{
									ImGui::TextColored(
										ImVec4(0.55f, 0.75f, 0.95f, 1.0f),
										"Capture is closed until reverse is selected.");
								}
								else if (!screen.reverseSource)
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
										"Reverse source is not running.");
									if (ImGui::Button("Start Reverse Source"))
									{
										if (!rebuild_reverse_source(screen))
											ImGui::OpenPopup(
												"Reverse Source Error");
									}
								}
								}

								ImGui::Separator();
								ImGui::TextWrapped(
									"SPF trailer-aware camera tracking");
								if (!g_camera_bridge_mapping_present.load())
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
										"SPF bridge is not loaded.");
								}
								else if (!g_camera_bridge_activated.load())
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
										"SPF found the bridge, but it is not activated.");
								}
								else if (!g_camera_bridge_telemetry_registered.load())
								{
									ImGui::TextColored(
										ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
										"SPF trailer telemetry registration failed.");
								}
								else if (g_camera_bridge_trailer_valid.load())
								{
									ImGui::TextColored(
										ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
										"Tail trailer anchor ready (%u trailer%s).",
										g_camera_bridge_trailer_count.load(),
										g_camera_bridge_trailer_count.load() == 1
											? "" : "s");
								}
								else
								{
									ImGui::TextColored(
										ImVec4(0.55f, 0.75f, 0.95f, 1.0f),
										"No connected trailer; truck anchor would be used.");
								}
								ImGui::TextWrapped(
									"SPF supplies the live tail position. ETS2 itself owns "
									"the independent park/park_360 render path; the probe "
									"above identifies its GPU texture before the plugin "
									"creates a trailer-aware camera. Until that validation "
									"is complete, the legacy calibrated window capture "
									"remains available.");
							}
							ImGui::TreePop();
						}

					if (ImGui::TreeNode("Live Performance Monitor"))
					{
							IContentSource* monitoredSource =
								screen.reverseCameraEnabled &&
								(g_reverse_active.load() ||
									screen.reversePreview) &&
								screen.reverseSource
								? screen.reverseSource.get()
								: screen.source.get();
							const auto sourceStats = monitoredSource
								? monitoredSource->GetPerformanceStats()
							: source_performance_stats_t{};
						const double gameFps = ImGui::GetIO().Framerate;
						const double observedFrameMs =
							gameFps > 0.1 ? 1000.0 / gameFps : 0.0;
						if (observedFrameMs > 0.0)
						{
							const double estimatedWithoutPluginMs =
								(std::max)(0.1, observedFrameMs - screen.uploadCpuMs);
							const double instantaneousLoss =
								(std::max)(0.0,
									1000.0 / estimatedWithoutPluginMs - gameFps);
							const double smoothing =
								1.0 - std::exp(-observedFrameMs / 2500.0);
							screen.estimatedFpsLoss =
								screen.estimatedFpsLoss == 0.0
								? instantaneousLoss
								: screen.estimatedFpsLoss * (1.0 - smoothing) +
									instantaneousLoss * smoothing;
						}

						ImGui::Text("Current game FPS: %.1f", gameFps);
						const ImVec4 lossColor =
							screen.estimatedFpsLoss < 1.0
							? ImVec4(0.35f, 0.90f, 0.40f, 1.0f)
							: (screen.estimatedFpsLoss < 3.0
								? ImVec4(1.0f, 0.72f, 0.25f, 1.0f)
								: ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
						ImGui::TextColored(
							lossColor, "Smoothed estimated FPS loss: %.2f",
							screen.estimatedFpsLoss);
						ImGui::Text(
							"Game-thread texture upload: %.3f ms",
							screen.uploadCpuMs);
						ImGui::Text(
							"Source worker CPU: %.3f ms/frame",
							sourceStats.workerCpuMs);
						ImGui::Text(
							"Total measured plugin CPU work: %.3f ms/frame",
							screen.totalPluginCpuMs);
						ImGui::Text(
							"GPU readback portion: %.3f ms/frame",
							sourceStats.readbackMs);
						ImGui::Text(
							"Delivered source rate: %.1f FPS",
							sourceStats.deliveredFps > 0.0
								? sourceStats.deliveredFps : screen.deliveredFps);
						ImGui::Text(
							"Dropped/overloaded frames: %llu",
							static_cast<unsigned long long>(
								sourceStats.droppedFrames));
						ImGui::Text(
							"Frames uploaded to game: %llu",
							static_cast<unsigned long long>(
								screen.uploadedFrames));
						ImGui::Text(
							"Hardware decode requested: %s",
							sourceStats.hardwareDecoded ? "Yes" : "No / external");
						ImGui::Text(
							"Window capture bypassed: %s",
							sourceStats.directMedia ? "Yes" : "No");
						if (screen.suspiciousMagentaFrame)
						{
							ImGui::TextColored(
								ImVec4(1.0f, 0.30f, 0.65f, 1.0f),
								"Render diagnostic: suspicious magenta/pink "
								"source frame (%u/%u samples)",
								screen.magentaSampleCount,
								screen.diagnosticSampleCount);
						}
						else if (screen.sourceFrameStale)
						{
							ImGui::TextColored(
								ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
								"Render diagnostic: source frames are stale");
						}
						else
						{
							ImGui::TextColored(
								ImVec4(0.35f, 0.90f, 0.40f, 1.0f),
								"Render diagnostic: healthy");
						}
						const DWORD backgroundCpu0 =
							thread_scheduling::preferred_processor(0);
						DWORD backgroundCpu1 =
							thread_scheduling::preferred_processor(1);
						DWORD backgroundCpu2 =
							thread_scheduling::preferred_processor(2);
						if (backgroundCpu0 !=
							thread_scheduling::kUnassignedProcessor)
						{
							if (backgroundCpu1 ==
								thread_scheduling::kUnassignedProcessor)
								backgroundCpu1 = backgroundCpu0;
							if (backgroundCpu2 ==
								thread_scheduling::kUnassignedProcessor)
								backgroundCpu2 = backgroundCpu1;
							ImGui::Text(
								"Background CPU hints: LP %lu, %lu, %lu",
								static_cast<unsigned long>(backgroundCpu0),
								static_cast<unsigned long>(backgroundCpu1),
								static_cast<unsigned long>(backgroundCpu2));
						}
						else
						{
							ImGui::TextDisabled(
								"Background CPU hints: learning render-thread use...");
						}
						ImGui::TextDisabled(
							"Soft scheduler hints only; game affinity is unchanged. "
							"Media helper priority: Below Normal.");
						if (ImGui::TreeNode(
							"CPU logical-processor activity (sampled)"))
						{
							const ImVec4 gameColor(0.92f, 0.20f, 0.20f, 1.0f);
							const ImVec4 pluginColor(0.20f, 0.85f, 0.30f, 1.0f);
							const ImVec4 sharedColor(1.0f, 0.78f, 0.12f, 1.0f);
							const ImVec4 idleColor(0.30f, 0.30f, 0.30f, 1.0f);
							const ImGuiColorEditFlags legendFlags =
								ImGuiColorEditFlags_NoTooltip |
								ImGuiColorEditFlags_NoDragDrop;
							ImGui::ColorButton(
								"##game_cpu", gameColor, legendFlags,
								ImVec2(12.0f, 12.0f));
							ImGui::SameLine();
							ImGui::TextUnformatted("Game render-thread observed");
							ImGui::SameLine();
							ImGui::ColorButton(
								"##plugin_cpu", pluginColor, legendFlags,
								ImVec2(12.0f, 12.0f));
							ImGui::SameLine();
							ImGui::TextUnformatted("Plugin worker observed");
							ImGui::SameLine();
							ImGui::ColorButton(
								"##shared_cpu", sharedColor, legendFlags,
								ImVec2(12.0f, 12.0f));
							ImGui::SameLine();
							ImGui::TextUnformatted("Both");

							const DWORD processorCount =
								thread_scheduling::tracked_processor_count();
							for (DWORD processor = 0;
								processor < processorCount; ++processor)
							{
								const uint32_t gameHits =
									thread_scheduling::sampled_game_hits(
										processor);
								const uint32_t pluginHits =
									thread_scheduling::sampled_plugin_hits(
										processor);
								const ImVec4 color = gameHits && pluginHits
									? sharedColor
									: (gameHits ? gameColor
										: (pluginHits ? pluginColor : idleColor));
								ImGui::PushID(
									12000 + static_cast<int>(processor));
								ImGui::ColorButton(
									"##processor", color, legendFlags,
									ImVec2(14.0f, 14.0f));
								ImGui::SameLine();
								ImGui::Text(
									"LP %lu   game samples %u   plugin samples %u",
									static_cast<unsigned long>(processor),
									gameHits, pluginHits);
								ImGui::PopID();
							}
							ImGui::TextWrapped(
								"This low-overhead two-second sample observes the "
								"game render thread and in-process plugin workers. "
								"It is not a complete profiler of every ETS2/ATS "
								"engine thread or the separate WebView2 processes.");
							ImGui::TreePop();
						}
						ImGui::TextWrapped(
							"Diagnostics: PrismTextureStreamerFB.log and "
							"PrismMediaClient.log beside the game executable.");
						ImGui::Separator();
						ImGui::TextWrapped(
							"The FPS-loss value measures render-thread time removed "
							"by the plugin. Decoder/GPU contention is shown separately "
							"and cannot be converted to an exact FPS number on every PC.");
						ImGui::TreePop();
					}

						if (ImGui::BeginPopupModal("Source Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
							ImGui::TextWrapped("Failed to use source, check the console for more details");
							if (ImGui::Button("OK", ImVec2(120, 0))) {
								ImGui::CloseCurrentPopup();
							}
							ImGui::EndPopup();
						}

						if (ImGui::BeginPopupModal(
							"Reverse Source Error", nullptr,
							ImGuiWindowFlags_AlwaysAutoResize))
						{
							ImGui::TextWrapped(
								"Failed to capture the game window for the reverse "
								"mirror. Use DX11 windowed/borderless mode and check "
								"the plugin log.");
							if (ImGui::Button("OK", ImVec2(120, 0)))
								ImGui::CloseCurrentPopup();
							ImGui::EndPopup();
						}


					if (ImGui::Button("Remove")) {

						if (screen.liveTexture) screen.liveTexture->Release();
						if (screen.uploadTexture)
							screen.uploadTexture->Release();
						if (screen.liveTextureRenderTarget)
							screen.liveTextureRenderTarget->Release();
						if (screen.immediateContext) screen.immediateContext->Release();

						to_remove.push_back(i);
						saveConfiguration = true;
					}
					ImGui::SameLine();
					if (ImGui::Button("Flip Screen")) {
						screen.flipVertical = !screen.flipVertical;
						saveConfiguration = true;
					}

					ImGui::PopID();
				}

				i++;
			}

			for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it) {
				g_screens.erase(g_screens.begin() + *it);
			}
		}




		ImGui::End();
	}

	if (saveConfiguration)
	{
		configuration_save_pending = true;
		configuration_last_change_tick = GetTickCount64();
	}

	if (configuration_save_pending)
	{
		const uint64_t now = GetTickCount64();
		const bool timedOut =
			configuration_last_change_tick != 0 &&
			now >= configuration_last_change_tick &&
			now - configuration_last_change_tick >= 1000;
		if (!ImGui::IsAnyItemActive() || timedOut)
		{
			settings::save();
			configuration_save_pending = false;
			configuration_last_change_tick = 0;
		}
	}
}

namespace Gui {
	bool is_visible()
	{
		return menu_visible.load();
	}

	bool init()
	{
		dx11::present::on_frame(on_frame);
		return true;
	}
}
