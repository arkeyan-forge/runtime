#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace d3d
{
    constexpr uint32_t BinkBlack = 0xFF000000u;

    inline bool BinkDimensionsValid(uint32_t width, uint32_t height)
    {
        return width != 0 && height != 0 && width <= 4096 && height <= 4096;
    }

    struct BinkPlane
    {
        std::span<const uint8_t> bytes;
        uint32_t pitch = 0;

        bool Covers(uint32_t width, uint32_t height) const
        {
            return width != 0 && height != 0 && pitch >= width && uint64_t(height - 1) * pitch + width <= bytes.size();
        }
    };

    // A zero-filled allocation is not a decoded black frame: limited-range black has Y=16, Cr=Cb=128.
    // Ignore row padding when deciding whether a frame is empty.
    inline const char* ConvertBinkFrame(uint32_t width, uint32_t height, const BinkPlane& y, const BinkPlane& cr,
        const BinkPlane& cb, std::vector<uint32_t>& rgba)
    {
        if (!BinkDimensionsValid(width, height))
        {
            rgba.assign(1, BinkBlack);
            return "invalid dimensions";
        }
        rgba.resize(size_t(width) * height);
        const uint32_t chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
        if (!y.Covers(width, height) || !cr.Covers(chromaWidth, chromaHeight) || !cb.Covers(chromaWidth, chromaHeight))
        {
            std::fill(rgba.begin(), rgba.end(), BinkBlack);
            return "missing or truncated YUV plane";
        }

        bool populated = false;
        auto clamp = [](int value) { return uint32_t(std::clamp(value, 0, 255)); };
        for (uint32_t row = 0; row < height; row++)
        {
            for (uint32_t column = 0; column < width; column++)
            {
                const uint8_t yy = y.bytes[size_t(row) * y.pitch + column];
                const uint8_t vv = cr.bytes[size_t(row / 2) * cr.pitch + column / 2];
                const uint8_t uu = cb.bytes[size_t(row / 2) * cb.pitch + column / 2];
                populated |= (yy | vv | uu) != 0;
                const int luma = (int(yy) - 16) * 298, v = int(vv) - 128, u = int(uu) - 128;
                const uint32_t r = clamp((luma + 409 * v + 128) >> 8);
                const uint32_t g = clamp((luma - 100 * u - 208 * v + 128) >> 8);
                const uint32_t b = clamp((luma + 516 * u + 128) >> 8);
                rgba[size_t(row) * width + column] = r | (g << 8) | (b << 16) | BinkBlack;
            }
        }
        if (!populated)
        {
            std::fill(rgba.begin(), rgba.end(), BinkBlack);
            return "empty YUV frame";
        }
        return nullptr;
    }

    // Use the same terminal state as the movie skip, including a frame number for a zero-frame header.
    template <typename Write>
    void EndFailedBinkMovie(uint32_t bink, Write write)
    {
        write(bink + 8, 1u);
        write(bink + 12, 1u);
    }
}
