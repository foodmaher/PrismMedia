#define NOMINMAX
#include "wind_audio.h"

#include "sources/media_client.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
	bool binding_down(const hotkey_binding_t& binding)
	{
		if (g_is_binding_hotkey || binding.virtualKey == 0 ||
			(GetAsyncKeyState(static_cast<int>(binding.virtualKey)) &
				0x8000) == 0)
			return false;

		const bool control =
			(GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
		const bool shift =
			(GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		return binding.control == control && binding.alt == alt &&
			binding.shift == shift;
	}

	class wind_audio_worker_t
	{
	public:
		void submit(
			wind_sound_mode_t mode,
			const std::string& path,
			bool enabled,
			float volume,
			float pan,
			float speedBlend)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_thread.joinable())
			{
				m_started = true;
				m_thread = std::thread([this] { run(); });
			}
			m_mode = mode;
			m_path = path;
			m_enabled = enabled;
			m_volume = volume;
			m_pan = pan;
			m_speedBlend = speedBlend;
			++m_generation;
			m_wake.notify_one();
		}

		bool started() const
		{
			return m_started.load();
		}

		void shutdown()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_stop = true;
				m_wake.notify_one();
			}
			if (m_thread.joinable())
				m_thread.join();
			m_started = false;
		}

	private:
		void run()
		{
			wind_sound_mode_t configuredMode =
				wind_sound_mode_t::PROCEDURAL;
			std::string configuredPath;
			bool sourceConfigured{};
			uint64_t consumedGeneration{};
			uint64_t lastSourceAttempt{};

			for (;;)
			{
				wind_sound_mode_t mode{};
				std::string path;
				bool enabled{};
				float volume{};
				float pan{};
				float speedBlend{};
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_wake.wait(lock, [this, &consumedGeneration]
					{
						return m_stop ||
							m_generation != consumedGeneration;
					});
					if (m_stop)
						break;
					consumedGeneration = m_generation;
					mode = m_mode;
					path = m_path;
					enabled = m_enabled;
					volume = m_volume;
					pan = m_pan;
					speedBlend = m_speedBlend;
				}

				const bool sourceChanged = !sourceConfigured ||
					configuredMode != mode || configuredPath != path;
				const uint64_t now = GetTickCount64();
				if (sourceChanged &&
					(lastSourceAttempt == 0 || now < lastSourceAttempt ||
						now - lastSourceAttempt >= 2000))
				{
					lastSourceAttempt = now;
					sourceConfigured =
						sources::SetMediaClientWindSource(
							mode == wind_sound_mode_t::PROCEDURAL,
							path);
					if (sourceConfigured)
					{
						configuredMode = mode;
						configuredPath = path;
						lastSourceAttempt = 0;
					}
				}
				if (sourceConfigured)
				{
					if (!sources::SetMediaClientWindState(
						enabled, volume, pan, speedBlend))
						sourceConfigured = false;
				}
			}

			if (sourceConfigured)
				sources::SetMediaClientWindState(
					false, 0.0f, 0.0f, 0.0f);
		}

		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::thread m_thread;
		bool m_stop{};
		std::atomic<bool> m_started{};
		uint64_t m_generation{};
		wind_sound_mode_t m_mode{ wind_sound_mode_t::PROCEDURAL };
		std::string m_path;
		bool m_enabled{};
		float m_volume{};
		float m_pan{};
		float m_speedBlend{};
	};

	wind_audio_worker_t g_wind_worker;
}

namespace wind_audio
{
	const char* binding_name(window_binding_t binding)
	{
		switch (binding)
		{
		case window_binding_t::LEFT_OPEN: return "Left window: open";
		case window_binding_t::LEFT_CLOSE: return "Left window: close";
		case window_binding_t::RIGHT_OPEN: return "Right window: open";
		case window_binding_t::RIGHT_CLOSE: return "Right window: close";
		}
		return "Window control";
	}

	void sync_from_settings()
	{
		g_wind_left_open = (std::clamp)(
			g_wind_audio_settings.leftWindowOpen, 0.0f, 1.0f);
		g_wind_right_open = (std::clamp)(
			g_wind_audio_settings.rightWindowOpen, 0.0f, 1.0f);
	}

	void set_left_open(float amount)
	{
		amount = (std::clamp)(amount, 0.0f, 1.0f);
		g_wind_left_open = amount;
		g_wind_audio_settings.leftWindowOpen = amount;
	}

	void set_right_open(float amount)
	{
		amount = (std::clamp)(amount, 0.0f, 1.0f);
		g_wind_right_open = amount;
		g_wind_audio_settings.rightWindowOpen = amount;
	}

	void update(bool driving, bool interiorCamera)
	{
		static uint64_t previousTick{};
		static uint64_t lastSendTick{};
		static float smoothedVolume{};
		static float smoothedPan{};

		const uint64_t now = GetTickCount64();
		const float deltaSeconds = previousTick == 0 || now < previousTick
			? 0.0f
			: (std::min)(0.10f, (now - previousTick) / 1000.0f);
		previousTick = now;

		const float travelSeconds = (std::clamp)(
			g_wind_audio_settings.windowTravelSeconds, 0.5f, 10.0f);
		const float travel = deltaSeconds / travelSeconds;
		float left = g_wind_left_open.load();
		float right = g_wind_right_open.load();

		const bool leftOpen = binding_down(
			g_window_hotkeys[static_cast<size_t>(
				window_binding_t::LEFT_OPEN)]);
		const bool leftClose = binding_down(
			g_window_hotkeys[static_cast<size_t>(
				window_binding_t::LEFT_CLOSE)]);
		const bool rightOpen = binding_down(
			g_window_hotkeys[static_cast<size_t>(
				window_binding_t::RIGHT_OPEN)]);
		const bool rightClose = binding_down(
			g_window_hotkeys[static_cast<size_t>(
				window_binding_t::RIGHT_CLOSE)]);

		if (leftOpen != leftClose)
			left = (std::clamp)(
				left + (leftOpen ? travel : -travel), 0.0f, 1.0f);
		if (rightOpen != rightClose)
			right = (std::clamp)(
				right + (rightOpen ? travel : -travel), 0.0f, 1.0f);
		set_left_open(left);
		set_right_open(right);

		const float speedKmh = std::fabs(g_truck_speed_mps.load()) * 3.6f;
		const float startSpeed = (std::clamp)(
			g_wind_audio_settings.startSpeedKmh, 0.0f, 100.0f);
		const float fullSpeed = (std::max)(
			startSpeed + 1.0f,
			g_wind_audio_settings.fullSpeedKmh);
		float speedBlend = (std::clamp)(
			(speedKmh - startSpeed) / (fullSpeed - startSpeed),
			0.0f, 1.0f);
		// Smoothstep approximates the gentle onset and stronger high-speed
		// pressure of real airflow without sudden volume steps.
		speedBlend = speedBlend * speedBlend * (3.0f - 2.0f * speedBlend);

		const float largerOpening = (std::max)(left, right);
		const float smallerOpening = (std::min)(left, right);
		const float opening = (std::clamp)(
			largerOpening + smallerOpening * 0.35f, 0.0f, 1.0f);
		const float openingResponse = std::pow(opening, 0.70f);
		const float masterVolume = (std::clamp)(
			g_wind_audio_settings.masterVolume, 0.0f, 1.0f);
		const bool shouldPlay = g_wind_audio_settings.enabled &&
			driving && interiorCamera && opening > 0.001f &&
			speedBlend > 0.001f;
		const float targetVolume = shouldPlay
			? masterVolume * openingResponse * speedBlend
			: 0.0f;
		const float targetPan = opening > 0.001f
			? (std::clamp)((right - left) / (left + right + 0.001f),
				-1.0f, 1.0f) * 0.70f
			: 0.0f;

		const float smoothing = 1.0f - std::exp(-deltaSeconds * 7.0f);
		smoothedVolume += (targetVolume - smoothedVolume) * smoothing;
		smoothedPan += (targetPan - smoothedPan) * smoothing;
		g_wind_output_volume = smoothedVolume;
		g_wind_output_pan = smoothedPan;

		if ((g_wind_audio_settings.enabled ||
			g_wind_worker.started()) &&
			now - lastSendTick >= 50)
		{
			g_wind_worker.submit(
				g_wind_audio_settings.soundMode,
				g_wind_audio_settings.customSoundPath,
				shouldPlay || smoothedVolume > 0.001f,
				smoothedVolume,
				smoothedPan,
				speedBlend);
			lastSendTick = now;
		}
	}

	void shutdown()
	{
		g_wind_worker.shutdown();
	}
}
