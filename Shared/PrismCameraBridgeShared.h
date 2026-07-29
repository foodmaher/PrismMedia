#pragma once

#include <Windows.h>
#include <cstdint>

namespace prism_camera_bridge
{
    inline constexpr wchar_t kMappingName[] =
        L"Local\\PrismTextureStreamerCameraBridgeV1";
    inline constexpr uint32_t kMagic = 0x50434231; // "PCB1"
    inline constexpr uint32_t kVersion = 1;

#pragma pack(push, 8)
    struct SharedState
    {
        uint32_t magic;
        uint32_t version;
        volatile LONG sequence;
        uint32_t valid;
        uint64_t updatedTick;
        float cameraX;
        float cameraY;
        float cameraZ;
        int32_t cameraType;
    };
#pragma pack(pop)
}
