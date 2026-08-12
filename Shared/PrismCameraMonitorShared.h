#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace prism_camera_monitor
{
    inline constexpr wchar_t kMappingName[] =
        L"Local\\PrismTextureStreamerIndependentCameraV1";
    inline constexpr uint32_t kMagic = 0x50434D31; // "PCM1"
    inline constexpr uint32_t kVersion = 1;
    inline constexpr uint32_t kMaximumWidth = 512;
    inline constexpr uint32_t kMaximumHeight = 512;
    inline constexpr uint32_t kMaximumPixelBytes =
        kMaximumWidth * kMaximumHeight * 4;

    enum class Stage : uint32_t
    {
        Offline = 0,
        PluginReady = 1,
        DiagnosticStarted = 2,
        Slot7Blocked = 3,
        CameraStateDiscovery = 4,
        CameraStateReady = 5,
        OwnedTargetReady = 6,
        JobSubmitted = 7,
        RendererEntered = 8,
        UniqueTargetTagged = 9,
        ReadbackPending = 10,
        FrameReady = 11,
        Blocked = 12,
        Failed = 13
    };

    enum Flags : uint32_t
    {
        kPluginConnected = 1U << 0,
        kSlot7Disabled = 1U << 1,
        kCameraStateVerified = 1U << 2,
        kOwnedTargetVerified = 1U << 3,
        kFrameAvailable = 1U << 4,
        kDiagnosticRunning = 1U << 5
    };

#pragma pack(push, 1)
    struct SharedState
    {
        uint32_t magic;
        uint32_t version;
        volatile LONG sequence;
        uint32_t stage;
        uint32_t flags;
        int32_t errorCode;
        uint64_t updatedTick;
        uint64_t runId;
        uint64_t frameSequence;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t pixelBytes;
        volatile LONG requestSequence;
        uint32_t reserved;
        uint64_t observedRenderJobs;
        uint64_t rejectedSlot7Jobs;
        uint64_t taggedTargets;
        uint64_t readbackFrames;
        char stageText[128];
        char detailText[512];
        uint8_t pixels[kMaximumPixelBytes];
    };
#pragma pack(pop)

    static_assert(offsetof(SharedState, requestSequence) == 64);
    static_assert(offsetof(SharedState, pixels) == 744);
}
