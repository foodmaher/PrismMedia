#pragma once

#include "hotkeys.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

struct wind_audio_settings_t
{
	bool enabled = false;
	std::vector<std::string> stationaryFiles;
	std::vector<std::string> cityFiles;
	std::vector<std::string> highwayFiles;
	float masterVolume = 0.65f;
	float stationaryVolume = 0.45f;
	float cityVolume = 0.75f;
	float highwayVolume = 1.0f;
	float stationaryFadeKmh = 8.0f;
	float highwayStartKmh = 55.0f;
	float highwayFullKmh = 90.0f;
	float stereoSeparation = 0.85f;
	float mediaDucking = 1.0f;
	float windowTravelSeconds = 2.8f;
	float leftWindowOpen = 0.0f;
	float rightWindowOpen = 0.0f;
};

enum class window_binding_t : size_t
{
	LEFT_OPEN = 0,
	LEFT_CLOSE,
	RIGHT_OPEN,
	RIGHT_CLOSE
};

inline wind_audio_settings_t g_wind_audio_settings;
inline std::recursive_mutex g_wind_audio_settings_mutex;
inline std::array<hotkey_binding_t, 4> g_window_hotkeys{};
inline std::atomic<float> g_truck_speed_mps{};
inline std::atomic<float> g_wind_left_open{};
inline std::atomic<float> g_wind_right_open{};
inline std::atomic<float> g_wind_output_volume{};
inline std::atomic<float> g_wind_output_pan{};
inline std::atomic<float> g_wind_media_gain{ 1.0f };

namespace wind_audio
{
	const char* binding_name(window_binding_t binding);
	void sync_from_settings();
	void set_left_open(float amount);
	void set_right_open(float amount);
	void update(bool driving, bool interiorCamera);
	void silence();
	void shutdown();
}
