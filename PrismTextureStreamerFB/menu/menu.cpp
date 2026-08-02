#include "menu.h"
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include <commdlg.h>

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
#include "../wind_audio.h"
#include "../sources/media_client.h"
#include "../sources/native_media.h"
#include "../sources/reverse_camera.h"
#include "../sources/window.h"
#include "../sources/wgc_window.h"


static std::atomic<bool> menu_visible{};
static int hotkey_binding_index = -1;
static bool configuration_save_pending = false;
static uint64_t configuration_last_change_tick = 0;

static bool append_audio_files(
	std::vector<std::string>& files,
	std::unique_lock<std::recursive_mutex>& settingsLock)
{
	std::vector<char> selectedPaths(32768, '\0');
	OPENFILENAMEA dialog{};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = GetActiveWindow();
	dialog.lpstrFile = selectedPaths.data();
	dialog.nMaxFile = static_cast<DWORD>(selectedPaths.size());
	dialog.lpstrFilter =
		"Audio files (*.wav;*.mp3;*.wma;*.m4a;*.aac)\0"
		"*.wav;*.mp3;*.wma;*.m4a;*.aac\0"
		"All files (*.*)\0*.*\0\0";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
		OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
	settingsLock.unlock();
	const BOOL selected = GetOpenFileNameA(&dialog);
	settingsLock.lock();
	if (!selected)
		return false;

	const std::string first(selectedPaths.data());
	const char* next = selectedPaths.data() + first.size() + 1;
	std::vector<std::string> additions;
	if (*next == '\0')
	{
		additions.push_back(first);
	}
	else
	{
		while (*next != '\0')
		{
			additions.push_back(first + "\\" + next);
			next += std::strlen(next) + 1;
		}
	}

	bool changed{};
	for (const auto& path : additions)
	{
		if (std::find(files.begin(), files.end(), path) == files.end())
		{
			files.push_back(path);
			changed = true;
		}
	}
	return changed;
}

static bool draw_audio_file_list(
	const char* id,
	const char* heading,
	const char* description,
	std::vector<std::string>& files,
	std::unique_lock<std::recursive_mutex>& settingsLock)
{
	bool changed{};
	ImGui::PushID(id);
	ImGui::TextUnformatted(heading);
	ImGui::TextDisabled("%s", description);
	for (size_t index = 0; index < files.size(); ++index)
	{
		ImGui::PushID(static_cast<int>(index));
		ImGui::SetNextItemWidth(500.0f);
		if (ImGui::InputText("##audio-file", &files[index]))
			changed = true;
		ImGui::SameLine();
		if (ImGui::Button("Remove"))
		{
			files.erase(files.begin() + index);
			changed = true;
			ImGui::PopID();
			break;
		}
		if (!files[index].empty() &&
			GetFileAttributesA(files[index].c_str()) ==
				INVALID_FILE_ATTRIBUTES)
		{
			ImGui::TextColored(
				ImVec4(1.0f, 0.55f, 0.20f, 1.0f),
				"File not found; this entry will be ignored.");
		}
		ImGui::PopID();
	}
	if (files.empty())
		ImGui::TextDisabled("No files: this layer stays silent.");
	if (ImGui::Button("Add field"))
	{
		files.emplace_back();
		changed = true;
	}
	ImGui::SameLine();
	if (ImGui::Button("Browse files..."))
		changed = append_audio_files(files, settingsLock) || changed;
	ImGui::PopID();
	return changed;
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
		screen.source->SetPaused(screen.paused);
		screen.source->SetSourceBrightness(screen.brightness);
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

	if (isPressed && !wasPressed) {
		menu_visible = !menu_visible;
		if (!menu_visible)
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
		dinput8::set_mouse(menu_visible);

		if (ImGui::GetCurrentContext()) {
			ImGuiIO* io = &ImGui::GetIO();
			if (io) io->MouseDrawCursor = menu_visible;
		}
	}
	wasPressed = isPressed;

	process_media_hotkeys(menu_visible);

	if (menu_visible) {
		ImGui::SetNextWindowSizeConstraints(ImVec2(680, 350), ImVec2(1000, 900));
		ImGui::Begin(("Prism3D Texture Streamer v" + std::string(g_version)).c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);

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
		}

		if (ImGui::CollapsingHeader("Open-Window Wind Audio"))
		{
			std::unique_lock<std::recursive_mutex> windSettingsLock(
				g_wind_audio_settings_mutex);
			ImGui::TextWrapped(
				"Adds airflow only in an interior driving camera. Volume rises "
				"smoothly with truck speed and how far each window is open; "
				"stereo balance follows the open side.");

			if (ImGui::Checkbox(
				"Enable realistic open-window wind",
				&g_wind_audio_settings.enabled))
				saveConfiguration = true;
			if (g_wind_audio_settings.enabled &&
				!sources::IsMediaClientInstalled())
			{
				ImGui::TextColored(
					ImVec4(1.0f, 0.55f, 0.20f, 1.0f),
					"Media helper not found. Copy the complete "
					"PrismTextureStreamerFB folder from the release package.");
			}

			ImGui::TextWrapped(
				"Every valid file loops continuously. Multiple files rotate "
				"when a track ends, while speed crossfades between these layers. "
				"Missing or empty layers are ignored; no generated noise is used.");
			if (draw_audio_file_list(
				"stationary", "Stationary sounds",
				"For stopped and very-low-speed ambience, such as birds.",
				g_wind_audio_settings.stationaryFiles,
				windSettingsLock))
				saveConfiguration = true;
			ImGui::Separator();
			if (draw_audio_file_list(
				"city", "City sounds",
				"For normal low and medium road speeds.",
				g_wind_audio_settings.cityFiles,
				windSettingsLock))
				saveConfiguration = true;
			ImGui::Separator();
			if (draw_audio_file_list(
				"highway", "Highway sounds",
				"For sustained high-speed airflow.",
				g_wind_audio_settings.highwayFiles,
				windSettingsLock))
				saveConfiguration = true;
			ImGui::Separator();

			float maximumVolumePercent =
				g_wind_audio_settings.masterVolume * 100.0f;
			if (ImGui::SliderFloat(
				"Maximum wind volume",
				&maximumVolumePercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.masterVolume =
					maximumVolumePercent / 100.0f;
				saveConfiguration = true;
			}
			float stationaryVolumePercent =
				g_wind_audio_settings.stationaryVolume * 100.0f;
			if (ImGui::SliderFloat(
				"Stationary layer volume", &stationaryVolumePercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.stationaryVolume =
					stationaryVolumePercent / 100.0f;
				saveConfiguration = true;
			}
			float cityVolumePercent =
				g_wind_audio_settings.cityVolume * 100.0f;
			if (ImGui::SliderFloat(
				"City layer volume", &cityVolumePercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.cityVolume =
					cityVolumePercent / 100.0f;
				saveConfiguration = true;
			}
			float highwayVolumePercent =
				g_wind_audio_settings.highwayVolume * 100.0f;
			if (ImGui::SliderFloat(
				"Highway layer volume", &highwayVolumePercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.highwayVolume =
					highwayVolumePercent / 100.0f;
				saveConfiguration = true;
			}
			if (ImGui::SliderFloat(
				"Stationary fades by",
				&g_wind_audio_settings.stationaryFadeKmh,
				1.0f, 30.0f, "%.0f km/h"))
				saveConfiguration = true;
			if (ImGui::SliderFloat(
				"Highway transition starts",
				&g_wind_audio_settings.highwayStartKmh,
				20.0f, 120.0f, "%.0f km/h"))
				saveConfiguration = true;
			if (ImGui::SliderFloat(
				"Highway fully active by",
				&g_wind_audio_settings.highwayFullKmh,
				40.0f, 160.0f, "%.0f km/h"))
				saveConfiguration = true;
			if (g_wind_audio_settings.highwayFullKmh <=
				g_wind_audio_settings.highwayStartKmh)
			{
				g_wind_audio_settings.highwayFullKmh =
					g_wind_audio_settings.highwayStartKmh + 1.0f;
			}
			float stereoPercent =
				g_wind_audio_settings.stereoSeparation * 100.0f;
			if (ImGui::SliderFloat(
				"Open-side stereo separation", &stereoPercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.stereoSeparation =
					stereoPercent / 100.0f;
				saveConfiguration = true;
			}
			float duckingPercent =
				g_wind_audio_settings.mediaDucking * 100.0f;
			if (ImGui::SliderFloat(
				"Media reduction at full wind", &duckingPercent,
				0.0f, 100.0f, "%.0f%%"))
			{
				g_wind_audio_settings.mediaDucking =
					duckingPercent / 100.0f;
				saveConfiguration = true;
			}
			ImGui::TextDisabled(
				"100%% makes integrated YouTube/Spotify audio silent at full wind.");
			if (ImGui::SliderFloat(
				"Window full-travel time",
				&g_wind_audio_settings.windowTravelSeconds,
				0.5f, 10.0f, "%.1f s"))
				saveConfiguration = true;

			ImGui::Separator();
			ImGui::TextWrapped(
				"Bind these to exactly the same four keys configured in "
				"ETS2/ATS. Hold a key while the game moves its window; the "
				"plugin will track the same movement. Backspace/Delete clears "
				"a binding and Escape cancels capture.");
			for (int index = 0;
				index < static_cast<int>(g_window_hotkeys.size()); ++index)
			{
				constexpr int kWindowBindingBase = 100;
				const int bindingId = kWindowBindingBase + index;
				ImGui::PushID(12000 + index);
				ImGui::TextUnformatted(wind_audio::binding_name(
					static_cast<window_binding_t>(index)));
				ImGui::SameLine(180.0f);
				const std::string label = hotkey_binding_index == bindingId
					? "Press a key...##window-binding"
					: hotkey_name(g_window_hotkeys[index]) +
						"##window-binding";
				if (ImGui::Button(label.c_str(), ImVec2(220.0f, 0.0f)))
				{
					hotkey_binding_index = bindingId;
					g_is_binding_hotkey = true;
				}
				if (hotkey_binding_index == bindingId &&
					capture_hotkey(g_window_hotkeys[index]))
				{
					hotkey_binding_index = -1;
					g_is_binding_hotkey = false;
					saveConfiguration = true;
				}
				ImGui::PopID();
			}

			ImGui::Separator();
			ImGui::TextUnformatted(
				"Manual synchronization (use if a key was missed):");
			float leftOpening = g_wind_left_open.load() * 100.0f;
			float rightOpening = g_wind_right_open.load() * 100.0f;
			if (ImGui::SliderFloat(
				"Left window opening", &leftOpening,
				0.0f, 100.0f, "%.0f%%"))
			{
				wind_audio::set_left_open(leftOpening / 100.0f);
				saveConfiguration = true;
			}
			if (ImGui::SliderFloat(
				"Right window opening", &rightOpening,
				0.0f, 100.0f, "%.0f%%"))
			{
				wind_audio::set_right_open(rightOpening / 100.0f);
				saveConfiguration = true;
			}

			ImGui::Text(
				"Live: %.1f km/h | windows L %.0f%% / R %.0f%% | "
				"environment %.0f%% | pan %+.2f | media %.0f%%",
				std::fabs(g_truck_speed_mps.load()) * 3.6f,
				g_wind_left_open.load() * 100.0f,
				g_wind_right_open.load() * 100.0f,
				g_wind_output_volume.load() * 100.0f,
				g_wind_output_pan.load(),
				g_wind_media_gain.load() * 100.0f);
			ImGui::TextColored(
				ImVec4(0.35f, 0.90f, 0.45f, 1.0f),
				"Recommended: seamless local loops, 60-70%% maximum, "
				"80-90%% stereo separation, and the truck's real window "
				"travel time (usually about 3 seconds).");
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
						ImGui::TextColored(
							ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
							"Expected impact: Low / Medium");
						ImGui::TextWrapped(
							"Recommended for YouTube and Spotify playlists. Uses the "
							"official embedded players in a clean hardware-accelerated "
							"helper, avoiding a full browser, tabs and extensions. "
							"One optimized Windows capture transfer is still required.");
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
						if (screen.source &&
							screen.source->SupportsSourceBrightness())
						{
							screen.source->SetSourceBrightness(
								screen.brightness);
							screen.hasUploadedFrame = false;
						}
						else
						{
							// Compatible fallback sources reprocess the cached
							// image, including while capture is paused.
							screen.hasUploadedFrame = false;
						}
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
							if (screen.contentMode ==
								content_mode_t::INTEGRATED_MEDIA &&
								screen.mediaService ==
									media_service_t::SPOTIFY)
							{
								ImGui::TextDisabled(
									"Spotify Next/Previous cycles saved Spotify "
									"links; Spotify Embed cannot skip tracks.");
							}

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
							saveConfiguration = true;
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
							screen.estimatedFpsLoss =
								(std::max)(0.0,
									1000.0 / estimatedWithoutPluginMs - gameFps);
						}

						ImGui::Text("Current game FPS: %.1f", gameFps);
						const ImVec4 lossColor =
							screen.estimatedFpsLoss < 1.0
							? ImVec4(0.35f, 0.90f, 0.40f, 1.0f)
							: (screen.estimatedFpsLoss < 3.0
								? ImVec4(1.0f, 0.72f, 0.25f, 1.0f)
								: ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
						ImGui::TextColored(
							lossColor, "Estimated FPS loss: %.2f",
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
