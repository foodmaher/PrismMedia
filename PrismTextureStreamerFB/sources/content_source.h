#pragma once
#include <cstdint>
#include <vector>
#include <Windows.h>

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
};
