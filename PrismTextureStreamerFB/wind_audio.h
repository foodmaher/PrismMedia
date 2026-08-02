#pragma once

#include "hotkeys.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <string>

enum class wind_sound_mode_t : unsigned char
{
	PROCEDURAL = 0,
	CUSTOM_FILE
};

struct wind_audio_settings_t
{
	bool enabled = false;
	wind_sound_mode_t soundMode = wind_sound_mode_t::PROCEDURAL;
	std::string customSoundPath;
	float masterVolume = 0.65f;
	float startSpeedKmh = 5.0f;
	float fullSpeedKmh = 100.0f;
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
inline std::array<hotkey_binding_t, 4> g_window_hotkeys{};
inline std::atomic<float> g_truck_speed_mps{};
inline std::atomic<float> g_wind_left_open{};
inline std::atomic<float> g_wind_right_open{};
inline std::atomic<float> g_wind_output_volume{};
inline std::atomic<float> g_wind_output_pan{};

namespace wind_audio
{
	const char* binding_name(window_binding_t binding);
	void sync_from_settings();
	void set_left_open(float amount);
	void set_right_open(float amount);
	void update(bool driving, bool interiorCamera);
	void shutdown();
}
