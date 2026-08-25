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

enum class content_mode_t : uint8_t {
	WINDOW_CAPTURE = 0,
	INTEGRATED_MEDIA,
	NATIVE_DIRECT_MEDIA,
	// Route the selected truck material to Prism3D's built-in GPS texture.
	// This mode has no capture/helper process and therefore no IContentSource.
	GAME_GPS
};

enum class media_service_t : uint8_t {
	YOUTUBE = 0,
	SPOTIFY
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
	// Stable identity for this display's isolated browser/profile/audio tree.
	// It survives screen reordering so WebView logins remain attached to the
	// correct in-truck display.
	std::string mediaClientId;
	std::string mediaUrl;
	media_service_t mediaService = media_service_t::YOUTUBE;
	// Lightweight saved-link libraries. These store URLs only; playback still
	// uses one live source, so additional playlists have no idle CPU/GPU cost.
	std::vector<std::string> youtubeUrls;
	std::vector<std::string> spotifyUrls;
	uint32_t selectedYoutubeUrl{};
	uint32_t selectedSpotifyUrl{};
	std::unique_ptr<IContentSource> source;
	std::vector<uint8_t> frameScratch;
	uint32_t frameScratchWidth{};
	uint32_t frameScratchHeight{};
	std::vector<uint8_t> engineStandbyScratch;
	uint32_t engineStandbyScratchWidth{};
	uint32_t engineStandbyScratchHeight{};
	uint64_t engineStandbyIdentityRevision{};
	bool engineStandbyWasDisplayed{};
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
	// Brightness multiplier for the generated truck-name standby logo while
	// engine-follow pauses the media source.
	float engineOffBrightness = 0.35f;
	bool adaptiveAudioEnabled = true;
	float adaptiveAudioInteriorVolume = 1.0f;
	float adaptiveAudioStrength = 0.85f;
	float adaptiveAudioSpeakerAzimuth = 0.0f;
	float adaptiveAudioFacingAwayVolume = 0.0f;
	float adaptiveAudioOutsideDistance = 0.25f;
	float adaptiveAudioOutsideVolume = 0.0f;
	float adaptiveAudioMenuVolume = 0.15f;
	bool adaptiveAudioExternalDistanceEnabled = true;
	float adaptiveAudioExternalNearVolume = 1.0f;
	float adaptiveAudioExternalNearCutoff = 144.0f;
	float adaptiveAudioExternalFullVolumeDistance = 0.0f;
	float adaptiveAudioExternalMuteDistance = 20.0f;
	bool adaptiveAudioExternalLowPassEnabled = true;
	float adaptiveAudioExternalMinimumCutoff = 20.0f;
	scale_mode_t scaleMode = scale_mode_t::FIT;
	float brightness = 0.30f;
	bool autoBrightnessEnabled = true;
	float autoBrightnessDarkMultiplier = 0.65f;
	float autoBrightnessBrightMultiplier = 1.15f;
	// Runtime brightness after the optional game-lighting adjustment.
	// This is deliberately not persisted; it is rebuilt from saved controls.
	float effectiveBrightness = 1.0f;
	uint64_t brightnessLastAdjustmentTick{};
	float brightnessLutScale = -1.0f;
	std::array<uint8_t, 256> brightnessLut{};
	uint8_t edgeBleedGuard = 2;
	performance_profile_t performanceProfile = performance_profile_t::SMOOTH;
	content_mode_t contentMode = content_mode_t::INTEGRATED_MEDIA;

	// Performance measurements are smoothed on the game's render thread.
	double uploadCpuMs{};
	double totalPluginCpuMs{};
	double estimatedFpsLoss{};
	double deliveredFps{};
	uint64_t uploadedFrames{};
	uint64_t lastUploadTick{};

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
	bool suspiciousBlackFrame{};
	bool sourceFrameStale{};
	uint32_t magentaSampleCount{};
	uint32_t blackSampleCount{};
	uint32_t diagnosticSampleCount{};
	uint32_t consecutiveBlackFrameInspections{};
	uint32_t consecutiveMapFailures{};
	long lastMapResult{};

	ID3D11Texture2D* liveTexture{};
	ID3D11Texture2D* uploadTexture{};
	ID3D11RenderTargetView* liveTextureRenderTarget{};
	ID3D11DeviceContext* immediateContext{};

	uint8_t framerate = 60; // Framerate of source, can actually be updated live

	// Target live texture size
	uint32_t targetLiveTextureWidth = 1280;
	uint32_t targetLiveTextureHeight = 720;

	// Actual texture size of created live texture
	uint32_t liveTextureWidth{};
	uint32_t liveTextureHeight{};
};

inline std::atomic<bool> g_screen_source_creation_in_progress{}; // Mainly for WGC to prevent deadlock on create texture 2d
inline std::mutex g_screens_mutex;
inline std::vector<screen_t> g_screens;
