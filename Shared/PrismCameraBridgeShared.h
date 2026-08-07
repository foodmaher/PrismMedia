#pragma once

#include <Windows.h>
#include <cstdint>

namespace prism_camera_bridge
{
    inline constexpr wchar_t kMappingName[] =
        L"Local\\PrismTextureStreamerCameraBridgeV4";
    inline constexpr uint32_t kMagic = 0x50434234; // "PCB4"
    inline constexpr uint32_t kVersion = 4;
    inline constexpr uint32_t kMaxTrafficVehicles = 32;

    enum SharedFlags : uint32_t
    {
        kLoaded = 1U << 0,
        kActivated = 1U << 1,
        kCameraValid = 1U << 2,
        kTelemetryRegistered = 1U << 3,
        kTrailerValid = 1U << 4,
        kTruckValid = 1U << 5,
        kTrafficValid = 1U << 6
    };

#pragma pack(push, 8)
    struct TrafficVehicle
    {
        int32_t id;
        uint32_t reserved;
        double x;
        double y;
        double z;
        float speed;
        float acceleration;
    };

    struct SharedState
    {
        uint32_t magic;
        uint32_t version;
        volatile LONG sequence;
        uint32_t flags;
        uint64_t updatedTick;
        float cameraX;
        float cameraY;
        float cameraZ;
        int32_t cameraType;
        uint32_t trailerCount;
        uint32_t reserved;
        double trailerX;
        double trailerY;
        double trailerZ;
        double trailerHeading;
        double trailerPitch;
        double trailerRoll;
        double truckX;
        double truckY;
        double truckZ;
        double truckHeading;
        double truckPitch;
        double truckRoll;
        uint32_t trafficCount;
        uint32_t trafficCapacity;
        TrafficVehicle traffic[kMaxTrafficVehicles];
    };
#pragma pack(pop)
}
