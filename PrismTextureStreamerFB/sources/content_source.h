#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <Windows.h>

enum class media_command_t : uint8_t
{
    PLAY_PAUSE = 0,
    NEXT,
    PREVIOUS,
    MUTE,
    VOLUME_UP,
    VOLUME_DOWN
};

struct source_performance_stats_t
{
    double workerCpuMs{};
    double readbackMs{};
    double deliveredFps{};
    uint64_t droppedFrames{};
    bool hardwareDecoded{};
    bool directMedia{};
};

// A content source produces BGRA8 frames on its own worker thread.
class IContentSource
{
public:
    virtual ~IContentSource() = default;

    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;

    virtual void SetFramerate(uint8_t framerate) = 0;
    virtual void SetPaused(bool paused) = 0;
    virtual void SetOutputSize(uint32_t width, uint32_t height) = 0;
    // Returns true only once for each newly captured frame.
    virtual bool CopyLatestFrame(std::vector<uint8_t>& dst, uint32_t& width, uint32_t& height) = 0;

    // Optional media-player capabilities. Window capture sources keep the
    // defaults, while the integrated and native players override them.
    virtual bool SupportsMediaControls() const { return false; }
    virtual bool LoadMedia(const std::string&) { return false; }
    virtual bool SendMediaCommand(media_command_t) { return false; }
    virtual source_performance_stats_t GetPerformanceStats() const { return {}; }
    virtual std::string GetStatusText() const { return "Capturing"; }
};
