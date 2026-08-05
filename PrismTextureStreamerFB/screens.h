#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <string>

#include "sources/content_source.h"

struct ID3D11Texture2D;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

enum class screen_type_t : uint8_t {
	GPS = 1,
	DASHBOARD,
	CUSTOM
};

enum class scale_mode_t : uint8_t {
	STRETCH = 0,
	FIT,
	CROP
};

enum class performance_profile_t : uint8_t {
	CUSTOM = 0,
	ECONOMY,
	BALANCED,
	QUALITY,
	SMOOTH
};

enum class reverse_performance_profile_t : uint8_t {
	CUSTOM = 0,
	ECONOMY,
	BALANCED,
	QUALITY,
	ULTRA
};

enum class reverse_camera_method_t : uint8_t {
	WINDOW_CROP = 0,
	INTERNAL_PARK_PROBE
};

enum class content_mode_t : uint8_t {
	WINDOW_CAPTURE = 0,
	INTEGRATED_MEDIA,
	NATIVE_DIRECT_MEDIA
};

enum class media_service_t : uint8_t {
	YOUTUBE = 0,
	SPOTIFY
};

enum class spotify_playback_mode_t : uint8_t {
	EMBED = 0,
	FULL_WEB_PLAYER
};

struct screen_t
{
	screen_type_t type{};

	std::string original_texture; // /vehicle/truck/share/gps.tobj
	std::string override_texture; // /home/PrismTextureStreamer/gps.tobj

	uint32_t override_texture_size_w{}; // 64
	uint32_t override_texture_size_h{}; // 2048

	std::string source_application_name;
	std::string source_application_display_name;
	std::string mediaUrl;
	media_service_t mediaService = media_service_t::YOUTUBE;
	spotify_playback_mode_t spotifyPlaybackMode =
		spotify_playback_mode_t::EMBED;
	std::vector<std::string> youtubeUrls;
	std::vector<std::string> spotifyUrls;
	uint32_t selectedYoutubeUrl{};
	uint32_t selectedSpotifyUrl{};
	std::unique_ptr<IContentSource> source;
	std::vector<uint8_t> frameScratch;
	uint32_t frameScratchWidth{};
	uint32_t frameScratchHeight{};
	std::vector<uint8_t> internalParkScratch;
	uint32_t internalParkScratchWidth{};
	uint32_t internalParkScratchHeight{};
	uint64_t internalParkFrameSequence{};
	bool hasUploadedFrame{};
	std::vector<uint32_t> scaleX;
	uint32_t scaleXSourceOffset{};
	uint32_t scaleXSourceSpan{};
	uint32_t scaleXDestinationWidth{};
	bool legacyCapture = false;
	bool flipVertical = true;
	bool paused = false;
	bool hotkeyTarget = false;
	bool followTruckEngine = true;
	bool adaptiveAudioEnabled = false;
	float adaptiveAudioStrength = 0.85f;
	float adaptiveAudioSpeakerAzimuth = 0.0f;
	float adaptiveAudioFacingAwayVolume = 0.05f;
	float adaptiveAudioOutsideDistance = 0.85f;
	float adaptiveAudioOutsideVolume = 0.0f;
	float adaptiveAudioMenuVolume = 0.50f;
	bool adaptiveAudioExternalDistanceEnabled = true;
	float adaptiveAudioExternalNearVolume = 0.35f;
	float adaptiveAudioExternalNearCutoff = 1200.0f;
	float adaptiveAudioExternalFullVolumeDistance = 1.5f;
	float adaptiveAudioExternalMuteDistance = 20.0f;
	bool adaptiveAudioExternalLowPassEnabled = true;
	float adaptiveAudioExternalMinimumCutoff = 120.0f;
	bool reverseCameraEnabled = false;
	bool reversePreview = false;
	bool reverseZeroForwardImpact = true;
	reverse_camera_method_t reverseCameraMethod =
		reverse_camera_method_t::WINDOW_CROP;
	// 0 follows the last matching target. 1-4 pin a discovered target.
	uint8_t reverseInternalTargetVariant = 1;
	bool reverseCameraKitInstalled = false;
	bool reverseTrailerAwareMount = true;
	float reverseMountLateral = 0.0f;
	float reverseMountHeight = 2.6f;
	float reverseMountLongitudinal = -0.35f;
	float reverseMountYaw = 180.0f;
	float reverseMountPitch = -8.0f;
	uint64_t reverseLastStartAttemptTick{};
	reverse_performance_profile_t reversePerformanceProfile =
		reverse_performance_profile_t::BALANCED;
	uint32_t reverseCaptureWidth = 640;
	uint32_t reverseCaptureHeight = 360;
	uint8_t reverseFramerate = 15;
	float reverseCropLeft = 0.30f;
	float reverseCropTop = 0.02f;
	float reverseCropWidth = 0.40f;
	float reverseCropHeight = 0.22f;
	std::unique_ptr<IContentSource> reverseSource;
	scale_mode_t scaleMode = scale_mode_t::FIT;
	float brightness = 1.0f;
	float brightnessLutScale = -1.0f;
	std::array<uint8_t, 256> brightnessLut{};
	uint8_t edgeBleedGuard = 2;
	performance_profile_t performanceProfile = performance_profile_t::BALANCED;
	content_mode_t contentMode = content_mode_t::WINDOW_CAPTURE;

	// Performance measurements are smoothed on the game's render thread.
	double uploadCpuMs{};
	double totalPluginCpuMs{};
	double estimatedFpsLoss{};
	double deliveredFps{};
	uint64_t uploadedFrames{};
	uint64_t lastUploadTick{};
	uint64_t internalParkLastBlitTick{};
	bool internalParkWasDisplayed{};

	// Low-frequency render diagnostics. These fields are updated only while
	// the screens mutex is held and never add a second capture/readback pass.
	uint64_t sourceCreatedTick{};
	uint64_t lastSourceFrameTick{};
	uint64_t lastFrameInspectionTick{};
	uint64_t lastRenderDiagnosticTick{};
	uint64_t lastIssueDiagnosticTick{};
	uint64_t lastTextureMatchTick{};
	uint64_t lastTextureRedirectTick{};
	bool suspiciousMagentaFrame{};
	bool sourceFrameStale{};
	uint32_t magentaSampleCount{};
	uint32_t diagnosticSampleCount{};
	uint32_t consecutiveMapFailures{};
	long lastMapResult{};

	ID3D11Texture2D* liveTexture{};
	ID3D11Texture2D* uploadTexture{};
	ID3D11RenderTargetView* liveTextureRenderTarget{};
	ID3D11DeviceContext* immediateContext{};

	uint8_t framerate = 30; // Framerate of source, can actually be updated live

	// Target live texture size
	uint32_t targetLiveTextureWidth = 1280;
	uint32_t targetLiveTextureHeight = 720;

	// Actual texture size of created live texture
	uint32_t liveTextureWidth{};
	uint32_t liveTextureHeight{};
};

inline void apply_reverse_performance_profile(screen_t& screen)
{
	switch (screen.reversePerformanceProfile)
	{
	case reverse_performance_profile_t::ECONOMY:
		screen.reverseCaptureWidth = 426;
		screen.reverseCaptureHeight = 240;
		screen.reverseFramerate = 10;
		break;
	case reverse_performance_profile_t::BALANCED:
		screen.reverseCaptureWidth = 640;
		screen.reverseCaptureHeight = 360;
		screen.reverseFramerate = 15;
		break;
	case reverse_performance_profile_t::QUALITY:
		screen.reverseCaptureWidth = 960;
		screen.reverseCaptureHeight = 540;
		screen.reverseFramerate = 20;
		break;
	case reverse_performance_profile_t::ULTRA:
		screen.reverseCaptureWidth = 1280;
		screen.reverseCaptureHeight = 720;
		screen.reverseFramerate = 30;
		break;
	case reverse_performance_profile_t::CUSTOM:
	default:
		break;
	}
}

inline std::atomic<bool> g_screen_source_creation_in_progress{}; // Mainly for WGC to prevent deadlock on create texture 2d
inline std::mutex g_screens_mutex;
inline std::vector<screen_t> g_screens;
