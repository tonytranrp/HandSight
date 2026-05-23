#pragma once

#include <cstdint>

namespace handsight
{
    static constexpr std::uint32_t kStreamMagic = 0x48534632; // "HSF2"
    static constexpr std::uint32_t kStreamVersion = 2;
    static constexpr std::uint32_t kPixelFormatJpeg = 1;

#pragma pack(push, 1)
    struct StreamHeader
    {
        std::uint32_t magic;
        std::uint32_t version;
        std::uint32_t width;
        std::uint32_t height;
        std::uint32_t pixelFormat;
        std::uint32_t rotationDegrees;
    };

    struct FrameHeader
    {
        std::uint32_t payloadSize;
        std::uint64_t timestampUs;
    };
#pragma pack(pop)
}
