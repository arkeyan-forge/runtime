#pragma once

// Xenos (Xbox 360 GPU) formats and render state, and their host equivalents. Register layouts and enum values follow
// Xenia's xenos.h/registers.h (BSD); the tiled-address function is adapted from Xenia's texture_address.h.
#include "gpu/gpu.h"
#include <cstdint>

namespace d3d::xenos
{
    // Texture fetch constant (6 dwords, host byte order).
    struct TextureFetch
    {
        uint32_t dword[6];

        uint32_t Type() const { return dword[0] & 3; } // 2 = texture
        uint32_t Pitch() const { return ((dword[0] >> 22) & 0x1FF) << 5; } // in pixels
        bool Tiled() const { return (dword[0] >> 31) != 0; }
        uint32_t Format() const { return dword[1] & 0x3F; }
        bool Stacked() const { return (dword[1] >> 10) & 1; } // 2D array
        uint32_t Endian() const { return (dword[1] >> 6) & 3; }
        uint32_t BaseAddress() const { return dword[1] & 0xFFFFF000; } // physical
        uint32_t Width() const { return (dword[2] & 0x1FFF) + 1; }
        uint32_t Height() const { return ((dword[2] >> 13) & 0x1FFF) + 1; }
        uint32_t Depth() const { return ((dword[2] >> 26) & 0x3F) + 1; } // stacked/3D depth
        uint32_t Dimension() const { return (dword[5] >> 9) & 3; }       // 0 1D, 1 2D, 2 3D, 3 cube
        uint32_t Swizzle() const { return (dword[3] >> 1) & 0xFFF; } // 3 bits per component: 0-3 XYZW, 4 = 0, 5 = 1
        uint32_t ClampX() const { return (dword[0] >> 10) & 7; }
        uint32_t ClampY() const { return (dword[0] >> 13) & 7; }
        uint32_t ClampZ() const { return (dword[0] >> 16) & 7; }
        uint32_t MagFilter() const { return (dword[3] >> 19) & 3; }
        uint32_t MinFilter() const { return (dword[3] >> 21) & 3; }
        uint32_t MipFilter() const { return (dword[3] >> 23) & 3; }
        uint32_t MipMinLevel() const { return (dword[4] >> 2) & 0xF; }
        uint32_t MipMaxLevel() const { return (dword[4] >> 6) & 0xF; }
        bool PackedMips() const { return (dword[5] >> 11) & 1; }
        uint32_t MipAddress() const { return dword[5] & 0xFFFFF000; } // physical; 0 = no mips
    };

    struct TextureFormatInfo
    {
        gpu::Format host; // UNKNOWN: not supported yet
        uint32_t blockBytes; // bytes per block (or texel)
        uint32_t blockWidth; // 4 for block-compressed formats, else 1
        bool convertToRgba8; // decoded on the CPU into R8G8B8A8 (formats with no host equivalent)
    };

    const TextureFormatInfo& TextureFormat(uint32_t format);

    // Where one level of a 2D, array or cube texture lives in guest memory. Levels 1 and up are stored from the mip
    // address, each padded to max(next_pow2(base size) >> level, 1) or a 32x32-block tile; with packed mips, the levels
    // from the one whose shorter side is 16 texels or less on share one tile (the packed tail), at fixed offsets. The
    // array slices (cube faces) of a level follow each other, 4 KB aligned.
    struct TextureLevel
    {
        uint32_t address; // physical, of slice 0
        uint32_t pitchBlocks; // row pitch in blocks (a multiple of 32 for tiled textures)
        uint32_t sliceBytes; // distance between array slices
        uint32_t x, y; // the level's offset in blocks inside the packed tail
        uint32_t width, height; // texels
    };

    // Array slices: 6 for a cube, the stack depth for a 2D array, else 1.
    uint32_t TextureArraySize(const TextureFetch& fetch);

    // How many levels the fetch constant describes (1: the base only), and where level `level` is.
    uint32_t TextureLevelCount(const TextureFetch& fetch);
    TextureLevel TextureLevelLayout(const TextureFetch& fetch, const TextureFormatInfo& info, uint32_t level);

    // Tiled 2D address (bytes) of block (x, y) in a texture whose pitch is `pitchBlocks` (a multiple of 32).
    uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlockLog2);

    // Byte swaps `size` bytes in place: endian 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
    void SwapEndian(void* data, uint64_t size, uint32_t endian);

    // EDRAM render targets.
    gpu::Format ColorTargetFormat(uint32_t colorFormat); // RB_COLOR_INFO.color_format
    constexpr gpu::Format DepthTargetFormat = gpu::Format::D32_FLOAT_S8_UINT;

    // A D3DDECLTYPE of the Xbox 360 (low 6 bits are the Xenos vertex format).
    struct VertexFormatInfo
    {
        gpu::Format host;
        uint32_t size; // bytes
        bool packedNormal; // DEC3N-style packed normal: given to shaders as a raw uint
    };
    VertexFormatInfo VertexFormat(uint32_t declType);

    plume::RenderBlend BlendFactor(uint32_t factor);
    plume::RenderBlendOperation BlendOp(uint32_t op);
    plume::RenderComparisonFunction Compare(uint32_t function);
    plume::RenderStencilOp StencilOp(uint32_t op);
    plume::RenderTextureAddressMode AddressMode(uint32_t clamp);
}
