#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <mutex>
#include <string>

#include "sources/content_source.h"

struct ID3D11Texture2D;
struct ID3D11DeviceContext;

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
	NATIVE_DIRECT_MEDIA
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
	std::unique_ptr<IContentSource> source;
	std::vector<uint8_t> frameScratch;
	uint32_t frameScratchWidth{};
	uint32_t frameScratchHeight{};
	bool hasUploadedFrame{};
	std::vector<uint32_t> scaleX;
	uint32_t scaleXSourceOffset{};
	uint32_t scaleXSourceSpan{};
	uint32_t scaleXDestinationWidth{};
	bool legacyCapture = false;
	bool flipVertical = true;
	bool paused = false;
	bool hotkeyTarget = false;
	scale_mode_t scaleMode = scale_mode_t::FIT;
	performance_profile_t performanceProfile = performance_profile_t::BALANCED;
	content_mode_t contentMode = content_mode_t::WINDOW_CAPTURE;

	// Performance measurements are smoothed on the game's render thread.
	double uploadCpuMs{};
	double totalPluginCpuMs{};
	double estimatedFpsLoss{};
	double deliveredFps{};
	uint64_t uploadedFrames{};
	uint64_t lastUploadTick{};

	ID3D11Texture2D* liveTexture{};
	ID3D11DeviceContext* immediateContext{};

	uint8_t framerate = 30; // Framerate of source, can actually be updated live

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
