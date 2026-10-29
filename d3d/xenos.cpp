#include "d3d/xenos.h"
#include <algorithm>
#include <bit>
#include <byteswap.h>
#include <cstring>

using plume::RenderBlend;
using plume::RenderBlendOperation;
using plume::RenderComparisonFunction;
using plume::RenderFormat;
using plume::RenderStencilOp;
using plume::RenderTextureAddressMode;

namespace d3d::xenos
{
    const TextureFormatInfo& TextureFormat(uint32_t format)
    {
        static const TextureFormatInfo Unsupported{ RenderFormat::UNKNOWN, 0, 1, false };
        static const TextureFormatInfo Table[64] = {
            /* 0 k_1_REVERSE */ Unsupported,
            /* 1 k_1 */ Unsupported,
            /* 2 k_8 */ { RenderFormat::R8_UNORM, 1, 1, false },
            /* 3 k_1_5_5_5 */ { RenderFormat::R8G8B8A8_UNORM, 2, 1, true },
            /* 4 k_5_6_5 */ { RenderFormat::R8G8B8A8_UNORM, 2, 1, true },
            /* 5 k_6_5_5 */ Unsupported,
            /* 6 k_8_8_8_8 */ { RenderFormat::R8G8B8A8_UNORM, 4, 1, false },
            /* 7 k_2_10_10_10 */ Unsupported, // needs a CPU conversion (plume has no 10:10:10:2 format)
            /* 8 k_8_A */ { RenderFormat::R8_UNORM, 1, 1, false },
            /* 9 k_8_B */ Unsupported,
            /* 10 k_8_8 */ { RenderFormat::R8G8_UNORM, 2, 1, false },
            /* 11 */ Unsupported,
            /* 12 */ Unsupported,
            /* 13 */ Unsupported,
            /* 14 k_8_8_8_8_A */ { RenderFormat::R8G8B8A8_UNORM, 4, 1, false },
            /* 15 k_4_4_4_4 */ { RenderFormat::R8G8B8A8_UNORM, 2, 1, true },
            /* 16 k_10_11_11 */ Unsupported,
            /* 17 k_11_11_10 */ Unsupported,
            /* 18 k_DXT1 */ { RenderFormat::BC1_UNORM, 8, 4, false },
            /* 19 k_DXT2_3 */ { RenderFormat::BC2_UNORM, 16, 4, false },
            /* 20 k_DXT4_5 */ { RenderFormat::BC3_UNORM, 16, 4, false },
            /* 21 */ Unsupported,
            /* 22 k_24_8 */ Unsupported,
            /* 23 k_24_8_FLOAT */ Unsupported,
            /* 24 k_16 */ { RenderFormat::R16_UNORM, 2, 1, false },
            /* 25 k_16_16 */ { RenderFormat::R16G16_UNORM, 4, 1, false },
            /* 26 k_16_16_16_16 */ { RenderFormat::R16G16B16A16_UNORM, 8, 1, false },
            /* 27 */ Unsupported,
            /* 28 */ Unsupported,
            /* 29 */ Unsupported,
            /* 30 k_16_FLOAT */ { RenderFormat::R16_FLOAT, 2, 1, false },
            /* 31 k_16_16_FLOAT */ { RenderFormat::R16G16_FLOAT, 4, 1, false },
            /* 32 k_16_16_16_16_FLOAT */ { RenderFormat::R16G16B16A16_FLOAT, 8, 1, false },
            /* 33 k_32 */ Unsupported,
            /* 34 k_32_32 */ Unsupported,
            /* 35 k_32_32_32_32 */ Unsupported,
            /* 36 k_32_FLOAT */ { RenderFormat::R32_FLOAT, 4, 1, false },
            /* 37 k_32_32_FLOAT */ { RenderFormat::R32G32_FLOAT, 8, 1, false },
            /* 38 k_32_32_32_32_FLOAT */ { RenderFormat::R32G32B32A32_FLOAT, 16, 1, false },
            /* 39-48 */ Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            /* 49 k_DXN */ { RenderFormat::BC5_UNORM, 16, 4, false },
            /* 50 k_8_8_8_8_AS_16_16_16_16 */ { RenderFormat::R8G8B8A8_UNORM, 4, 1, false },
            /* 51 k_DXT1_AS_16_16_16_16 */ { RenderFormat::BC1_UNORM, 8, 4, false },
            /* 52 k_DXT2_3_AS_16_16_16_16 */ { RenderFormat::BC2_UNORM, 16, 4, false },
            /* 53 k_DXT4_5_AS_16_16_16_16 */ { RenderFormat::BC3_UNORM, 16, 4, false },
            /* 54 k_2_10_10_10_AS_16_16_16_16 */
            { RenderFormat::R16G16B16A16_UNORM, 4, 1, false }, // resolve targets only
            /* 55-58 */ Unsupported,
            Unsupported,
            Unsupported,
            Unsupported,
            /* 59 k_DXT5A */ { RenderFormat::BC4_UNORM, 8, 4, false },
            /* 60 k_CTX1 */ Unsupported,
            /* 61-63 */ Unsupported,
            Unsupported,
            Unsupported,
        };
        return Table[format & 63];
    }

    // Adapted from Xenia's texture_address::Tiled2D/TiledCombine (BSD-3).
    namespace
    {
        uint32_t Log2Ceil(uint32_t value) { return value <= 1 ? 0 : 32 - std::countl_zero(value - 1); }
        uint32_t Log2Floor(uint32_t value) { return 31 - std::countl_zero(value); }
        uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

        // The level holding the packed mip tail. From Xenia's texture_util (BSD).
        uint32_t PackedMipLevel(uint32_t width, uint32_t height)
        {
            uint32_t log2Size = Log2Ceil(std::min(width, height));
            return log2Size > 4 ? log2Size - 4 : 0;
        }

        // Offset in texels of level `mip` inside the packed tail of a width x height texture. From Xenia's
        // GetPackedMipOffset (BSD): the 16x16, 8x8 and 4x4 levels are stacked across the short side of the tile, smaller
        // ones along the long side.
        void PackedMipOffset(uint32_t width, uint32_t height, uint32_t mip, uint32_t& x, uint32_t& y)
        {
            uint32_t log2Width = Log2Ceil(width), log2Height = Log2Ceil(height);
            uint32_t log2Size = std::min(log2Width, log2Height);
            x = y = 0;
            if (log2Size > 4 + mip)
                return;
            uint32_t packedBase = log2Size > 4 ? log2Size - 4 : 0;
            uint32_t packedMip = mip - packedBase;
            if (packedMip < 3)
                (log2Width > log2Height ? y : x) = 16 >> packedMip;
            else if (log2Width > log2Height)
                x = (1u << (log2Width - packedBase)) >> (packedMip - 2);
            else
                y = (1u << (log2Height - packedBase)) >> (packedMip - 2);
        }

        // Row pitch in blocks and array slice size in bytes of mip level `level` (1 and up, or 0 for a level-0 packed
        // tail).
        void MipStorage(const TextureFetch& fetch, const TextureFormatInfo& info, uint32_t level, uint32_t& pitchBlocks,
            uint32_t& sliceBytes)
        {
            auto blocks = [&](uint32_t texels) { return (texels + info.blockWidth - 1) / info.blockWidth; };
            uint32_t rowAlignment = fetch.Tiled() ? 32 : std::max(256 / info.blockBytes, 32u);
            pitchBlocks = AlignUp(blocks(std::max((1u << Log2Ceil(fetch.Width())) >> level, 1u)), rowAlignment);
            uint32_t rows = AlignUp(blocks(std::max((1u << Log2Ceil(fetch.Height())) >> level, 1u)), 32);
            sliceBytes = AlignUp(pitchBlocks * info.blockBytes * rows, 4096);
        }
    }

    uint32_t TextureArraySize(const TextureFetch& fetch)
    {
        if (fetch.Dimension() == 3)
            return 6;
        return fetch.Dimension() == 1 && fetch.Stacked() ? fetch.Depth() : 1;
    }

    uint32_t TextureLevelCount(const TextureFetch& fetch)
    {
        uint32_t width = fetch.Width(), height = fetch.Height();
        uint32_t mipAddress = fetch.MipAddress();
        // When the base already is the packed tail, Direct3D leaves the mip address at 0: the tail is at the base.
        if (mipAddress == 0 && fetch.PackedMips() && PackedMipLevel(width, height) == 0)
            mipAddress = fetch.BaseAddress();
        if (mipAddress == 0 || fetch.BaseAddress() == 0)
            return 1;
        uint32_t sizeMaxLevel = Log2Floor(std::max(width, height));
        uint32_t minLevel = std::min(fetch.MipMinLevel(), sizeMaxLevel);
        return std::max(std::min(fetch.MipMaxLevel(), sizeMaxLevel), minLevel) + 1;
    }

    TextureLevel TextureLevelLayout(const TextureFetch& fetch, const TextureFormatInfo& info, uint32_t level)
    {
        uint32_t width = fetch.Width(), height = fetch.Height();
        uint32_t packedLevel = fetch.PackedMips() ? PackedMipLevel(width, height) : UINT32_MAX;
        TextureLevel out{};
        out.width = std::max(width >> level, 1u);
        out.height = std::max(height >> level, 1u);
        if (level >= packedLevel)
        {
            PackedMipOffset(width, height, level, out.x, out.y);
            out.x /= info.blockWidth;
            out.y /= info.blockWidth;
        }

        if (level == 0)
        {
            // The base takes its pitch from the fetch constant (linear rows are 256-byte aligned).
            out.address = fetch.BaseAddress();
            uint32_t pitchBlocks = std::max(fetch.Pitch(), width) / info.blockWidth;
            out.pitchBlocks = fetch.Tiled() ? AlignUp(pitchBlocks, 32)
                                            : AlignUp(pitchBlocks * info.blockBytes, 256) / info.blockBytes;
            uint32_t rows = (packedLevel == 0 ? 1u << Log2Ceil(height) : height) + info.blockWidth - 1;
            out.sliceBytes = AlignUp(out.pitchBlocks * info.blockBytes * AlignUp(rows / info.blockWidth, 32), 4096);
            return out;
        }

        // Levels 1 up to the packed tail follow each other from the mip address, all slices of one level together
        // (2D arrays are padded to 4 slices); the tail holds all the levels after it.
        uint32_t arraySize = TextureArraySize(fetch);
        uint32_t mipArraySize = fetch.Dimension() == 1 && arraySize > 1 ? AlignUp(arraySize, 4) : arraySize;
        out.address = fetch.MipAddress() != 0 ? fetch.MipAddress() : fetch.BaseAddress();
        uint32_t stored = std::min(level, packedLevel);
        for (uint32_t previous = 1; previous < stored; previous++)
        {
            MipStorage(fetch, info, previous, out.pitchBlocks, out.sliceBytes);
            out.address += out.sliceBytes * mipArraySize;
        }
        MipStorage(fetch, info, stored, out.pitchBlocks, out.sliceBytes);
        return out;
    }

    uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlockLog2)
    {
        uint32_t outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
        uint32_t innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
        uint32_t bytes = (outerBlocks | innerBlocks) << bytesPerBlockLog2;
        uint32_t bank = (y >> 4) & 1;
        uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15) | (((bytes >> 4) & 1) << 5) |
               (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
    }

    void SwapEndian(void* data, uint64_t size, uint32_t endian)
    {
        auto bytes = static_cast<uint8_t*>(data);
        switch (endian)
        {
        case 1: // 8-in-16
            for (uint64_t i = 0; i + 2 <= size; i += 2)
            {
                uint16_t value;
                memcpy(&value, bytes + i, 2);
                value = ByteSwap(value);
                memcpy(bytes + i, &value, 2);
            }
            break;
        case 2: // 8-in-32
            for (uint64_t i = 0; i + 4 <= size; i += 4)
            {
                uint32_t value;
                memcpy(&value, bytes + i, 4);
                value = ByteSwap(value);
                memcpy(bytes + i, &value, 4);
            }
            break;
        case 3: // 16-in-32
            for (uint64_t i = 0; i + 4 <= size; i += 4)
            {
                uint32_t value;
                memcpy(&value, bytes + i, 4);
                value = (value >> 16) | (value << 16);
                memcpy(bytes + i, &value, 4);
            }
            break;
        }
    }

    gpu::Format ColorTargetFormat(uint32_t colorFormat)
    {
        switch (colorFormat)
        {
        case 0: // k_8_8_8_8
        case 1: // k_8_8_8_8_GAMMA
            return RenderFormat::R8G8B8A8_UNORM;
        case 2:  // k_2_10_10_10 (plume has no 10:10:10:2 format: 16 bits per channel instead)
        case 10: // k_2_10_10_10_AS_10_10_10_10
            return RenderFormat::R16G16B16A16_UNORM;
        case 6: // k_16_16_FLOAT
            return RenderFormat::R16G16_FLOAT;
        case 14: // k_32_FLOAT
            return RenderFormat::R32_FLOAT;
        case 15: // k_32_32_FLOAT
            return RenderFormat::R32G32_FLOAT;
        default: // 2_10_10_10_FLOAT, 16_16, 16_16_16_16 (fixed-point -32..32) and their variants: wide float targets
            return RenderFormat::R16G16B16A16_FLOAT;
        }
    }

    VertexFormatInfo VertexFormat(uint32_t declType)
    {
        switch (declType)
        {
        case 0x2C83A4: return { RenderFormat::R32_FLOAT, 4, false };          // FLOAT1
        case 0x2C23A5: return { RenderFormat::R32G32_FLOAT, 8, false };       // FLOAT2
        case 0x2A23B9: return { RenderFormat::R32G32B32_FLOAT, 12, false };   // FLOAT3
        case 0x1A23A6: return { RenderFormat::R32G32B32A32_FLOAT, 16, false }; // FLOAT4
        case 0x182886: return { RenderFormat::B8G8R8A8_UNORM, 4, false }; // D3DCOLOR (stored ARGB, swizzled to RGBA)
        case 0x1A2286: return { RenderFormat::R8G8B8A8_UINT, 4, false }; // UBYTE4
        case 0x1A2086: return { RenderFormat::R8G8B8A8_UNORM, 4, false }; // UBYTE4N
        case 0x2C2359: return { RenderFormat::R16G16_SINT, 4, false }; // SHORT2
        case 0x1A235A: return { RenderFormat::R16G16B16A16_SINT, 8, false }; // SHORT4
        case 0x2C2159: return { RenderFormat::R16G16_SNORM, 4, false }; // SHORT2N
        case 0x1A215A: return { RenderFormat::R16G16B16A16_SNORM, 8, false }; // SHORT4N
        case 0x2C2059: return { RenderFormat::R16G16_UNORM, 4, false }; // USHORT2N
        case 0x1A205A: return { RenderFormat::R16G16B16A16_UNORM, 8, false }; // USHORT4N
        case 0x2C235F: return { RenderFormat::R16G16_FLOAT, 4, false }; // FLOAT16_2
        case 0x1A2360: return { RenderFormat::R16G16B16A16_FLOAT, 8, false }; // FLOAT16_4
        case 0x2A2187: return { RenderFormat::R32_UINT, 4, true }; // DEC3N
        case 0x2A2287: return { RenderFormat::R32_UINT, 4, true }; // UDEC3
        default: return { RenderFormat::UNKNOWN, 0, false };
        }
    }

    RenderBlend BlendFactor(uint32_t factor)
    {
        switch (factor)
        {
        case 0: return RenderBlend::ZERO;
        case 1: return RenderBlend::ONE;
        case 4: return RenderBlend::SRC_COLOR;
        case 5: return RenderBlend::INV_SRC_COLOR;
        case 6: return RenderBlend::SRC_ALPHA;
        case 7: return RenderBlend::INV_SRC_ALPHA;
        case 8: return RenderBlend::DEST_COLOR;
        case 9: return RenderBlend::INV_DEST_COLOR;
        case 10: return RenderBlend::DEST_ALPHA;
        case 11: return RenderBlend::INV_DEST_ALPHA;
        case 12:
        case 14: return RenderBlend::BLEND_FACTOR;
        case 13:
        case 15: return RenderBlend::INV_BLEND_FACTOR;
        case 16: return RenderBlend::SRC_ALPHA_SAT;
        default: return RenderBlend::ONE;
        }
    }

    RenderBlendOperation BlendOp(uint32_t op)
    {
        switch (op)
        {
        case 1: return RenderBlendOperation::SUBTRACT;
        case 2: return RenderBlendOperation::MIN;
        case 3: return RenderBlendOperation::MAX;
        case 4: return RenderBlendOperation::REV_SUBTRACT;
        default: return RenderBlendOperation::ADD;
        }
    }

    RenderComparisonFunction Compare(uint32_t function)
    {
        static const RenderComparisonFunction Table[8] = { RenderComparisonFunction::NEVER,
            RenderComparisonFunction::LESS, RenderComparisonFunction::EQUAL, RenderComparisonFunction::LESS_EQUAL,
            RenderComparisonFunction::GREATER, RenderComparisonFunction::NOT_EQUAL,
            RenderComparisonFunction::GREATER_EQUAL, RenderComparisonFunction::ALWAYS };
        return Table[function & 7];
    }

    RenderStencilOp StencilOp(uint32_t op)
    {
        static const RenderStencilOp Table[8] = { RenderStencilOp::KEEP, RenderStencilOp::ZERO,
            RenderStencilOp::REPLACE, RenderStencilOp::INCREMENT_AND_CLAMP, RenderStencilOp::DECREMENT_AND_CLAMP,
            RenderStencilOp::INVERT, RenderStencilOp::INCREMENT_AND_WRAP, RenderStencilOp::DECREMENT_AND_WRAP };
        return Table[op & 7];
    }

    RenderTextureAddressMode AddressMode(uint32_t clamp)
    {
        switch (clamp)
        {
        case 0: return RenderTextureAddressMode::WRAP;
        case 1: return RenderTextureAddressMode::MIRROR;
        case 3:
        case 5:
        case 7: return RenderTextureAddressMode::MIRROR_ONCE;
        case 6: return RenderTextureAddressMode::BORDER;
        default: return RenderTextureAddressMode::CLAMP;
        }
    }
}
