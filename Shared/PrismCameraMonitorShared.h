#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace prism_camera_monitor
{
    inline constexpr wchar_t kMappingName[] =
        L"Local\\PrismTextureStreamerIndependentCameraV2";
    inline constexpr uint32_t kMagic = 0x50434D32; // "PCM2"
    inline constexpr uint32_t kVersion = 2;
    inline constexpr uint32_t kMaximumWidth = 512;
    inline constexpr uint32_t kMaximumHeight = 512;
    inline constexpr uint32_t kMaximumPixelBytes =
        kMaximumWidth * kMaximumHeight * 4;
    inline constexpr uint32_t kMaximumCorrelationCandidates = 16;

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
        Failed = 13,
        GuidedCorrelation = 14,
        CorrelationReady = 15
    };

    enum Flags : uint32_t
    {
        kPluginConnected = 1U << 0,
        kSlot7Disabled = 1U << 1,
        kCameraStateVerified = 1U << 2,
        kOwnedTargetVerified = 1U << 3,
        kFrameAvailable = 1U << 4,
        kDiagnosticRunning = 1U << 5,
        kCorrelationActive = 1U << 6,
        kCandidatesAvailable = 1U << 7
    };

    enum class CorrelationPhase : uint32_t
    {
        Idle = 0,
        BaselineCabin = 1,
        LookLeft = 2,
        LookRight = 3,
        Exterior = 4,
        LeftMirror = 5,
        RightMirror = 6,
        ReturnCabin = 7,
        AnalysisComplete = 8
    };

    enum class CorrelationSource : uint32_t
    {
        Unknown = 0,
        CameraInput = 1,
        CameraPointer = 2,
        RenderRequest = 3,
        RequestPointer = 4,
        RenderCommand = 5,
        CommandPointer = 6
    };

#pragma pack(push, 1)
    struct CorrelationCandidate
    {
        uint64_t address;
        uint32_t offset;
        uint32_t sourceKind;
        float score;
        uint32_t changedPhaseMask;
        uint32_t capturedPhaseMask;
        uint32_t sampleCount;
        float values[4];
        char label[16];
    };

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
        volatile LONG phaseRequestSequence;
        uint64_t observedRenderJobs;
        uint64_t rejectedSlot7Jobs;
        uint64_t taggedTargets;
        uint64_t readbackFrames;
        uint64_t correlationSamples;
        uint32_t currentPhase;
        uint32_t requestedPhase;
        uint32_t completedPhaseMask;
        uint32_t candidateCount;
        char stageText[128];
        char detailText[512];
        char instructionText[256];
        CorrelationCandidate candidates[kMaximumCorrelationCandidates];
        uint8_t pixels[kMaximumPixelBytes];
    };
#pragma pack(pop)

    static_assert(offsetof(SharedState, requestSequence) == 64);
    static_assert(sizeof(CorrelationCandidate) == 64);
    static_assert(offsetof(SharedState, pixels) == 2048);
}
