#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>

constexpr size_t kTrackedTruckWheelCount = 16;

struct environment_audio_settings_t
{
	bool enabled = true;
	float interiorEffect = 0.40f;
	float exteriorEffect = 0.75f;
};

inline environment_audio_settings_t g_environment_audio_settings;
inline std::mutex g_environment_audio_settings_mutex;
inline std::atomic<float> g_truck_speed_mps{};
inline std::array<std::atomic<bool>, kTrackedTruckWheelCount>
	g_environment_wheel_on_ground{};
inline std::array<std::atomic<bool>, kTrackedTruckWheelCount>
	g_environment_wheel_sample_seen{};
inline std::atomic<float> g_environment_grounded_ratio{ 1.0f };
inline std::atomic<float> g_environment_intensity{};
inline std::atomic<float> g_environment_media_gain{ 1.0f };
inline std::atomic<bool> g_environment_interior{ true };
inline std::atomic<double> g_environment_update_cpu_us{};

namespace environment_audio
{
	void update(bool driving, bool interiorCamera);
	void reset();
}
