#include "menu.h"
#include <d3d11.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>

#include "../version.h"

#include "../scs_logging.h"
using namespace scs_logging;

#include "../prism/prism.h"
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
#include "../sources/media_client.h"
#include "../sources/native_media.h"
#include "../sources/reverse_camera.h"
#include "../sources/window.h"
#include "../sources/wgc_window.h"


static std::atomic<bool> menu_visible{};
static int hotkey_binding_index = -1;

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
		(!screen.reverseZeroForwardImpact ||
			g_reverse_active.load() || screen.reversePreview))
	{
		screen.reverseLastStartAttemptTick = GetTickCount64();
		screen.reverseSource = sources::CreateReverseCameraSource(
			screen.reverseFramerate,
			screen.targetLiveTextureWidth,
			screen.targetLiveTextureHeight,
			screen.reverseCropLeft,
			screen.reverseCropTop,
			screen.reverseCropWidth,
			screen.reverseCropHeight);
	}

	if (screen.reverseSource)
		screen.reverseSource->SetPaused(
			!(g_reverse_active.load() || screen.reversePreview));
	g_screen_source_creation_in_progress = false;
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
							screen.targetLiveTextureWidth,
							screen.targetLiveTextureHeight);
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
						"Integrated Media Client (recommended for YouTube)",
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
							"Recommended for YouTube and playlists. Uses the official "
							"YouTube embedded player in a clean hardware-accelerated "
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
						ImGui::SetNextItemWidth(-120.0f);
						if (ImGui::InputTextWithHint(
							"##media_url",
							screen.contentMode == content_mode_t::INTEGRATED_MEDIA
								? "YouTube / playlist / file / direct URL"
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
							screen.source->SendMediaCommand(
								media_command_t::PLAY_PAUSE);
						ImGui::SameLine();
						if (ImGui::Button("Previous"))
							screen.source->SendMediaCommand(
								media_command_t::PREVIOUS);
						ImGui::SameLine();
						if (ImGui::Button("Next"))
							screen.source->SendMediaCommand(
								media_command_t::NEXT);
						ImGui::SameLine();
						if (ImGui::Button("Mute"))
							screen.source->SendMediaCommand(
								media_command_t::MUTE);
						ImGui::SameLine();
						if (ImGui::Button("Vol -"))
							screen.source->SendMediaCommand(
								media_command_t::VOLUME_DOWN);
						ImGui::SameLine();
						if (ImGui::Button("Vol +"))
							screen.source->SendMediaCommand(
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
									"Volume when outside",
									&screen.adaptiveAudioOutsideVolume,
									0.0f, 1.0f, "%.2f",
									ImGuiSliderFlags_AlwaysClamp))
									saveConfiguration = true;
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
								const bool externalCamera =
									driving &&
									(!g_camera_interior_hint.load() ||
										!headTelemetryFresh);

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
										"Detected state: external camera (outside volume)");
									ImGui::Text(
										"Live head offset: unavailable outside the cab");
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
									"right. Set Outside volume to 0 for full silence "
									"when the camera leaves the cab. External-camera "
									"detection uses telemetry freshness plus the default "
									"1-9/0 camera keys.");
							}
							ImGui::EndDisabled();
							ImGui::TreePop();
						}

						if (ImGui::TreeNode("Automatic Reverse Mirror"))
						{
							ImGui::TextColored(
								ImVec4(0.35f, 0.85f, 0.40f, 1.0f),
								"Expected impact: none forward; low while reversing");
							ImGui::TextWrapped(
								"Reuses the game's fixed virtual-mirror picture, so the "
								"game handles every truck, trailer and articulation angle. "
								"Enable the virtual mirror with F2 first.");

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

								uint8_t reverseFpsMin = 5;
								uint8_t reverseFpsMax = 60;
								if (ImGui::SliderScalar(
									"Reverse view FPS",
									ImGuiDataType_U8,
									&screen.reverseFramerate,
									&reverseFpsMin,
									&reverseFpsMax))
								{
									if (screen.reverseSource)
										screen.reverseSource->SetFramerate(
											screen.reverseFramerate);
									saveConfiguration = true;
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
						if (ImGui::BeginPopupModal("Reverse Source Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
							ImGui::TextWrapped(
								"Failed to capture the game window for the reverse "
								"mirror. Use DX11 windowed/borderless mode and check "
								"the plugin log.");
							if (ImGui::Button("OK", ImVec2(120, 0)))
								ImGui::CloseCurrentPopup();
							ImGui::EndPopup();
						}
						ImGui::EndPopup();
					}


					if (ImGui::Button("Remove")) {

						if (screen.liveTexture) screen.liveTexture->Release();
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
		settings::save();
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
