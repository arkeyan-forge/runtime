#pragma once

#include <cstdint>

namespace d3d
{
    // The colour targets a pixel shader writes (bit i: oC<i>), from its header's outputs dword (+0x1C: bits 0-3 oC0-oC3,
    // bit 4 the depth), which is what XenosRecomp declares as the translated shader's outputs.
    inline uint32_t PixelShaderColorOutputs(uint32_t outputs) { return outputs & 0xF; }

    // Render target `slot`'s write mask: its 4 bits of RB_COLOR_MASK, or none when the pixel shader does not write it
    // (`colorOutputs`: PixelShaderColorOutputs, 0 without a pixel shader). On Xenos such a target keeps its contents; the
    // host APIs leave it undefined: Apple and AMD keep it, NVIDIA writes 0 (Trap Team's title halo was black).
    inline uint8_t TargetWriteMask(uint32_t colorMask, uint32_t colorOutputs, uint32_t slot)
    {
        return ((colorOutputs >> slot) & 1) != 0 ? uint8_t((colorMask >> (slot * 4)) & 0xF) : 0;
    }
}
