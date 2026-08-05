#define NOMINMAX
#include "environment_audio.h"

#include "sources/media_client.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>

namespace
{
	uint64_t g_previous_tick{};
	float g_smoothed_intensity{};
	float g_smoothed_media_gain{ 1.0f };
	float g_last_sent_media_gain{ -1.0f };
	uint64_t g_last_send_attempt{};
	std::atomic<bool> g_reset_requested{};

	float smoothstep(float value)
	{
		value = (std::clamp)(value, 0.0f, 1.0f);
		return value * value * (3.0f - 2.0f * value);
	}

	float grounded_ratio()
	{
		size_t seen{};
		size_t grounded{};
		for (size_t index = 0; index < kTrackedTruckWheelCount; ++index)
		{
			if (!g_environment_wheel_sample_seen[index].load())
				continue;
			++seen;
			if (g_environment_wheel_on_ground[index].load())
				++grounded;
		}
		return seen == 0
			? 1.0f
			: static_cast<float>(grounded) / static_cast<float>(seen);
	}
}

namespace environment_audio
{
	void update(bool driving, bool interiorCamera)
	{
		const uint64_t now = GetTickCount64();
		const bool resetPending = g_reset_requested.load();
		if (!resetPending && g_previous_tick != 0 &&
			now >= g_previous_tick && now - g_previous_tick < 50)
		{
			return;
		}

		// Configuration changes are rare and originate from the render-thread
		// UI. Never let a background save/load operation stall the game thread.
		std::unique_lock<std::mutex> settingsLock(
			g_environment_audio_settings_mutex, std::try_to_lock);
		if (!settingsLock.owns_lock())
			return;

		const auto updateStarted = std::chrono::steady_clock::now();
		environment_audio_settings_t settings;
		settings = g_environment_audio_settings;
		settingsLock.unlock();

		const bool resetRequested = g_reset_requested.exchange(false);
		const bool firstUpdate = g_previous_tick == 0 ||
			now < g_previous_tick || resetRequested;
		const float deltaSeconds = firstUpdate
			? 0.0f
			: (std::min)(0.10f, (now - g_previous_tick) / 1000.0f);
		g_previous_tick = now;

		const float ground = grounded_ratio();
		const float speedKmh = std::fabs(g_truck_speed_mps.load()) * 3.6f;
		const float airflow = smoothstep((speedKmh - 5.0f) / 95.0f);
		const float road = smoothstep((speedKmh - 2.0f) / 78.0f) * ground;
		const float targetIntensity = settings.enabled && driving
			? (std::clamp)(airflow * 0.60f + road * 0.40f, 0.0f, 1.0f)
			: 0.0f;
		const float effect = interiorCamera
			? (std::clamp)(settings.interiorEffect, 0.0f, 1.0f)
			: (std::clamp)(settings.exteriorEffect, 0.0f, 1.0f);
		const float targetMediaGain = 1.0f - targetIntensity * effect;

		if (firstUpdate)
		{
			g_smoothed_intensity = targetIntensity;
			g_smoothed_media_gain = targetMediaGain;
		}
		else
		{
			const float smoothing =
				1.0f - std::exp(-deltaSeconds * 4.0f);
			g_smoothed_intensity +=
				(targetIntensity - g_smoothed_intensity) * smoothing;
			g_smoothed_media_gain +=
				(targetMediaGain - g_smoothed_media_gain) * smoothing;
		}

		g_environment_grounded_ratio = ground;
		g_environment_intensity = (std::clamp)(
			g_smoothed_intensity, 0.0f, 1.0f);
		g_environment_media_gain = (std::clamp)(
			g_smoothed_media_gain, 0.0f, 1.0f);
		g_environment_interior = interiorCamera;

		const float mediaGain = g_environment_media_gain.load();
		if ((g_last_sent_media_gain < 0.0f ||
			std::fabs(mediaGain - g_last_sent_media_gain) >= 0.005f) &&
			(g_last_send_attempt == 0 || now < g_last_send_attempt ||
				now - g_last_send_attempt >= 50))
		{
			g_last_send_attempt = now;
			if (sources::SetMediaClientDucking(mediaGain))
				g_last_sent_media_gain = mediaGain;
		}

		const double updateUs = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - updateStarted).count();
		const double previousUs = g_environment_update_cpu_us.load();
		g_environment_update_cpu_us = previousUs <= 0.0
			? updateUs
			: previousUs * 0.90 + updateUs * 0.10;
	}

	void reset()
	{
		g_environment_intensity = 0.0f;
		g_environment_media_gain = 1.0f;
		g_environment_update_cpu_us = 0.0;
		if (sources::SetMediaClientDucking(1.0f))
			g_last_sent_media_gain = 1.0f;
		g_reset_requested = true;
	}
}
