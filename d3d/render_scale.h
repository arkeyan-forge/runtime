#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace d3d
{
    // Colour tables keep their guest size: duplicating entries makes bilinear lookups form a staircase.
    // A scaled picture has both sides above 32, so this also works on host sizes.
    constexpr bool IsTableSize(uint32_t width, uint32_t height) { return std::min(width, height) <= 32; }

    constexpr int32_t TargetScale(uint32_t width, uint32_t height, int32_t resolutionScale)
    {
        return IsTableSize(width, height) ? 1 : resolutionScale;
    }

    struct HostKernel
    {
        std::array<float, 8> constants;
        uint32_t scaledTextures;
    };

    // c0-c1 and fetch offsets must both step in host pixels for the edge filter.
    constexpr HostKernel ScaleHostKernel(HostKernel kernel, int32_t resolutionScale)
    {
        if (resolutionScale > 1)
        {
            for (float& value : kernel.constants)
                value /= float(resolutionScale);
            kernel.scaledTextures = 0;
        }
        return kernel;
    }
}
