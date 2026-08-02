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
			const wind_audio_settings_t& settings,
			bool enabled,
			float stationaryVolume,
			float cityVolume,
			float highwayVolume,
			float pan,
			float mediaGain)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_thread.joinable())
			{
				m_started = true;
				m_thread = std::thread([this] { run(); });
			}
			m_stationaryFiles = settings.stationaryFiles;
			m_cityFiles = settings.cityFiles;
			m_highwayFiles = settings.highwayFiles;
			m_enabled = enabled;
			m_stationaryVolume = stationaryVolume;
			m_cityVolume = cityVolume;
			m_highwayVolume = highwayVolume;
			m_pan = pan;
			m_mediaGain = mediaGain;
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
			std::vector<std::string> configuredStationaryFiles;
			std::vector<std::string> configuredCityFiles;
			std::vector<std::string> configuredHighwayFiles;
			bool sourceConfigured{};
			uint64_t consumedGeneration{};
			uint64_t lastSourceAttempt{};

			for (;;)
			{
				std::vector<std::string> stationaryFiles;
				std::vector<std::string> cityFiles;
				std::vector<std::string> highwayFiles;
				bool enabled{};
				float stationaryVolume{};
				float cityVolume{};
				float highwayVolume{};
				float pan{};
				float mediaGain{ 1.0f };
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
					stationaryFiles = m_stationaryFiles;
					cityFiles = m_cityFiles;
					highwayFiles = m_highwayFiles;
					enabled = m_enabled;
					stationaryVolume = m_stationaryVolume;
					cityVolume = m_cityVolume;
					highwayVolume = m_highwayVolume;
					pan = m_pan;
					mediaGain = m_mediaGain;
				}

				const bool sourceChanged = !sourceConfigured ||
					configuredStationaryFiles != stationaryFiles ||
					configuredCityFiles != cityFiles ||
					configuredHighwayFiles != highwayFiles;
				const uint64_t now = GetTickCount64();
				if (sourceChanged &&
					(lastSourceAttempt == 0 || now < lastSourceAttempt ||
						now - lastSourceAttempt >= 2000))
				{
					lastSourceAttempt = now;
					sourceConfigured =
						sources::SetMediaClientWindLibrary(
							stationaryFiles, cityFiles, highwayFiles);
					if (sourceConfigured)
					{
						configuredStationaryFiles = stationaryFiles;
						configuredCityFiles = cityFiles;
						configuredHighwayFiles = highwayFiles;
						lastSourceAttempt = 0;
					}
				}
				if (sourceConfigured)
				{
					if (!sources::SetMediaClientWindState(
						enabled, stationaryVolume, cityVolume,
						highwayVolume, pan, mediaGain))
						sourceConfigured = false;
				}
			}

			if (sourceConfigured)
				sources::SetMediaClientWindState(
					false, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
		}

		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::thread m_thread;
		bool m_stop{};
		std::atomic<bool> m_started{};
		uint64_t m_generation{};
		std::vector<std::string> m_stationaryFiles;
		std::vector<std::string> m_cityFiles;
		std::vector<std::string> m_highwayFiles;
		bool m_enabled{};
		float m_stationaryVolume{};
		float m_cityVolume{};
		float m_highwayVolume{};
		float m_pan{};
		float m_mediaGain{ 1.0f };
	};

	wind_audio_worker_t g_wind_worker;
	std::atomic<bool> g_reset_mix_on_next_update{};

	bool has_existing_file(const std::vector<std::string>& files)
	{
		for (const auto& file : files)
		{
			const DWORD attributes = GetFileAttributesA(file.c_str());
			if (attributes != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
				return true;
		}
		return false;
	}

	float smoothstep(float value)
	{
		value = (std::clamp)(value, 0.0f, 1.0f);
		return value * value * (3.0f - 2.0f * value);
	}
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
		std::lock_guard<std::recursive_mutex> lock(
			g_wind_audio_settings_mutex);
		g_wind_left_open = (std::clamp)(
			g_wind_audio_settings.leftWindowOpen, 0.0f, 1.0f);
		g_wind_right_open = (std::clamp)(
			g_wind_audio_settings.rightWindowOpen, 0.0f, 1.0f);
	}

	void set_left_open(float amount)
	{
		std::lock_guard<std::recursive_mutex> lock(
			g_wind_audio_settings_mutex);
		amount = (std::clamp)(amount, 0.0f, 1.0f);
		g_wind_left_open = amount;
		g_wind_audio_settings.leftWindowOpen = amount;
	}

	void set_right_open(float amount)
	{
		std::lock_guard<std::recursive_mutex> lock(
			g_wind_audio_settings_mutex);
		amount = (std::clamp)(amount, 0.0f, 1.0f);
		g_wind_right_open = amount;
		g_wind_audio_settings.rightWindowOpen = amount;
	}

	void update(bool driving, bool interiorCamera)
	{
		static uint64_t previousTick{};
		static uint64_t lastSendTick{};
		static float smoothedStationaryVolume{};
		static float smoothedCityVolume{};
		static float smoothedHighwayVolume{};
		static float smoothedPan{};
		static float smoothedMediaGain{ 1.0f };
		static std::vector<std::string> checkedStationaryFiles;
		static std::vector<std::string> checkedCityFiles;
		static std::vector<std::string> checkedHighwayFiles;
		static bool stationaryAvailable{};
		static bool cityAvailable{};
		static bool highwayAvailable{};
		static uint64_t lastAvailabilityCheck{};
		if (g_reset_mix_on_next_update.exchange(false))
		{
			smoothedStationaryVolume = 0.0f;
			smoothedCityVolume = 0.0f;
			smoothedHighwayVolume = 0.0f;
			smoothedPan = 0.0f;
			smoothedMediaGain = 1.0f;
		}
		wind_audio_settings_t settings;
		{
			std::lock_guard<std::recursive_mutex> lock(
				g_wind_audio_settings_mutex);
			settings = g_wind_audio_settings;
		}

		const uint64_t now = GetTickCount64();
		const float deltaSeconds = previousTick == 0 || now < previousTick
			? 0.0f
			: (std::min)(0.10f, (now - previousTick) / 1000.0f);
		previousTick = now;

		const float travelSeconds = (std::clamp)(
			settings.windowTravelSeconds, 0.5f, 10.0f);
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
		const float largerOpening = (std::max)(left, right);
		const float smallerOpening = (std::min)(left, right);
		const float opening = (std::clamp)(
			largerOpening + smallerOpening * 0.35f, 0.0f, 1.0f);
		const float openingResponse = std::pow(opening, 0.70f);
		const float masterVolume = (std::clamp)(
			settings.masterVolume, 0.0f, 1.0f);
		const bool libraryChanged =
			checkedStationaryFiles != settings.stationaryFiles ||
			checkedCityFiles != settings.cityFiles ||
			checkedHighwayFiles != settings.highwayFiles;
		if (libraryChanged || lastAvailabilityCheck == 0 ||
			now < lastAvailabilityCheck || now - lastAvailabilityCheck >= 2000)
		{
			checkedStationaryFiles = settings.stationaryFiles;
			checkedCityFiles = settings.cityFiles;
			checkedHighwayFiles = settings.highwayFiles;
			stationaryAvailable = has_existing_file(checkedStationaryFiles);
			cityAvailable = has_existing_file(checkedCityFiles);
			highwayAvailable = has_existing_file(checkedHighwayFiles);
			lastAvailabilityCheck = now;
		}
		const bool anyAvailable = stationaryAvailable || cityAvailable ||
			highwayAvailable;
		const bool audibleContext = settings.enabled &&
			driving && interiorCamera && opening > 0.001f;

		const float stationaryFade = (std::max)(1.0f,
			settings.stationaryFadeKmh);
		const float stationaryWeight = 1.0f - smoothstep(
			speedKmh / stationaryFade);
		const float highwayStart = (std::max)(0.0f,
			settings.highwayStartKmh);
		const float highwayFull = (std::max)(highwayStart + 1.0f,
			settings.highwayFullKmh);
		const float highwayWeight = smoothstep(
			(speedKmh - highwayStart) / (highwayFull - highwayStart));
		const float cityWeight = (1.0f - stationaryWeight) *
			(1.0f - highwayWeight);
		const float baseVolume = audibleContext
			? masterVolume * openingResponse
			: 0.0f;
		const float targetStationaryVolume = stationaryAvailable
			? baseVolume * stationaryWeight * (std::clamp)(
				settings.stationaryVolume, 0.0f, 1.0f)
			: 0.0f;
		const float targetCityVolume = cityAvailable
			? baseVolume * cityWeight * (std::clamp)(
				settings.cityVolume, 0.0f, 1.0f)
			: 0.0f;
		const float targetHighwayVolume = highwayAvailable
			? baseVolume * highwayWeight * (std::clamp)(
				settings.highwayVolume, 0.0f, 1.0f)
			: 0.0f;
		const float targetPan = opening > 0.001f
			? (std::clamp)((right - left) / (left + right + 0.001f),
				-1.0f, 1.0f) * (std::clamp)(
					settings.stereoSeparation, 0.0f, 1.0f)
			: 0.0f;
		const float windIntensity = (std::clamp)(
			masterVolume > 0.001f
				? (targetStationaryVolume + targetCityVolume +
					targetHighwayVolume) / masterVolume
				: 0.0f,
			0.0f, 1.0f);
		const float targetMediaGain = 1.0f - windIntensity *
			(std::clamp)(settings.mediaDucking, 0.0f, 1.0f);

		const float smoothing = 1.0f - std::exp(-deltaSeconds * 5.0f);
		smoothedStationaryVolume += (targetStationaryVolume -
			smoothedStationaryVolume) * smoothing;
		smoothedCityVolume += (targetCityVolume - smoothedCityVolume) *
			smoothing;
		smoothedHighwayVolume += (targetHighwayVolume -
			smoothedHighwayVolume) * smoothing;
		smoothedPan += (targetPan - smoothedPan) * smoothing;
		smoothedMediaGain += (targetMediaGain - smoothedMediaGain) *
			smoothing;
		g_wind_output_volume = (std::clamp)(
			smoothedStationaryVolume + smoothedCityVolume +
			smoothedHighwayVolume, 0.0f, 1.0f);
		g_wind_output_pan = smoothedPan;
		g_wind_media_gain = smoothedMediaGain;

		if (((settings.enabled && anyAvailable) ||
			g_wind_worker.started()) &&
			now - lastSendTick >= 50)
		{
			g_wind_worker.submit(
				settings,
				settings.enabled && anyAvailable,
				smoothedStationaryVolume,
				smoothedCityVolume,
				smoothedHighwayVolume,
				smoothedPan,
				smoothedMediaGain);
			lastSendTick = now;
		}
	}

	void silence()
	{
		if (!g_wind_worker.started())
			return;

		wind_audio_settings_t settings;
		{
			std::lock_guard<std::recursive_mutex> lock(
				g_wind_audio_settings_mutex);
			settings = g_wind_audio_settings;
		}
		const bool hasConfiguredFiles =
			!settings.stationaryFiles.empty() ||
			!settings.cityFiles.empty() ||
			!settings.highwayFiles.empty();
		g_wind_output_volume = 0.0f;
		g_wind_output_pan = 0.0f;
		g_wind_media_gain = 1.0f;
		g_reset_mix_on_next_update = true;
		g_wind_worker.submit(
			settings, settings.enabled && hasConfiguredFiles,
			0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
	}

	void shutdown()
	{
		g_wind_worker.shutdown();
	}
}
