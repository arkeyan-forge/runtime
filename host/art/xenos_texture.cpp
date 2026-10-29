// See xenos_texture.h. The block formats follow the public BC1-BC3 description; the tiling is Xenia's (BSD-3).
#include "xenos_texture.h"
#include <algorithm>
#include <cstring>

namespace art
{
    namespace
    {
        uint32_t Rgb565(uint16_t c)
        {
            uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return ((r << 3) | (r >> 2)) | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2)) << 16;
        }

        uint32_t Mix(uint32_t a, uint32_t b, int wa, int wb, int total)
        {
            uint32_t out = 0;
            for (int shift = 0; shift < 24; shift += 8)
                out |= ((((a >> shift) & 0xFF) * wa + ((b >> shift) & 0xFF) * wb) / total) << shift;
            return out;
        }

        struct Shape
        {
            uint32_t blockWidth;  // texels per block side (4, or 1 for uncompressed)
            uint32_t log2Bytes;   // bytes per block
        };

        Shape ShapeOf(TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::Dxt1: return { 4, 3 };
            case TextureFormat::Dxt3:
            case TextureFormat::Dxt5: return { 4, 4 };
            case TextureFormat::Argb8: return { 1, 2 };
            case TextureFormat::L8: return { 1, 0 };
            }
            return { 4, 4 };
        }
    }

    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t log2Bytes)
    {
        uint32_t outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
        uint32_t innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
        uint32_t bytes = (outerBlocks | innerBlocks) << log2Bytes;
        uint32_t bank = (y >> 4) & 1;
        uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15) | (((bytes >> 4) & 1) << 5) |
               (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
    }

    void DecodeBlock(const uint8_t* block, TextureFormat format, uint32_t texels[16])
    {
        bool alphaBlock = format != TextureFormat::Dxt1;
        const uint8_t* color = alphaBlock ? block + 8 : block;
        uint16_t c0 = uint16_t(color[0] | color[1] << 8), c1 = uint16_t(color[2] | color[3] << 8);
        uint32_t palette[4] = { Rgb565(c0), Rgb565(c1) };
        bool opaque = alphaBlock || c0 > c1;
        palette[2] = opaque ? Mix(palette[0], palette[1], 2, 1, 3) : Mix(palette[0], palette[1], 1, 1, 2);
        palette[3] = opaque ? Mix(palette[0], palette[1], 1, 2, 3) : 0;
        uint32_t indices = color[4] | color[5] << 8 | color[6] << 16 | uint32_t(color[7]) << 24;
        for (int i = 0; i < 16; i++)
        {
            uint32_t index = (indices >> (i * 2)) & 3;
            texels[i] = palette[index] | ((opaque || index != 3) ? 0xFF000000u : 0);
        }

        if (format == TextureFormat::Dxt3)
        {
            for (int i = 0; i < 16; i++)
                texels[i] = (texels[i] & 0x00FFFFFF) | uint32_t(((block[i / 2] >> (4 * (i & 1))) & 0xF) * 17) << 24;
        }
        else if (format == TextureFormat::Dxt5)
        {
            uint8_t a[8] = { block[0], block[1] };
            for (int i = 2; i < 8; i++)
            {
                if (a[0] > a[1])
                    a[i] = uint8_t((a[0] * (8 - i) + a[1] * (i - 1)) / 7);
                else
                    a[i] = i < 6 ? uint8_t((a[0] * (6 - i) + a[1] * (i - 1)) / 5) : (i == 6 ? 0 : 255);
            }
            uint64_t alphaIndices = 0;
            for (int i = 0; i < 6; i++)
                alphaIndices |= uint64_t(block[2 + i]) << (i * 8);
            for (int i = 0; i < 16; i++)
                texels[i] = (texels[i] & 0x00FFFFFF) | uint32_t(a[(alphaIndices >> (i * 3)) & 7]) << 24;
        }
    }

    bool DecodeTexture(const uint8_t* data, size_t size, const Texture& texture, Image& out)
    {
        out = {};
        if (texture.width == 0 || texture.height == 0 || texture.width > 8192 || texture.height > 8192)
            return false;
        Shape shape = ShapeOf(texture.format);
        uint32_t blockBytes = 1u << shape.log2Bytes;
        uint32_t blocksWide = (texture.width + shape.blockWidth - 1) / shape.blockWidth;
        uint32_t blocksHigh = (texture.height + shape.blockWidth - 1) / shape.blockWidth;
        uint32_t pitchBlocks = std::max(texture.pitch, texture.width) / shape.blockWidth;
        pitchBlocks = std::max(pitchBlocks, blocksWide);
        pitchBlocks =
            texture.tiled ? (pitchBlocks + 31) / 32 * 32 : (pitchBlocks * blockBytes + 255) / 256 * 256 / blockBytes;

        out.width = texture.width;
        out.height = texture.height;
        out.rgba.assign(size_t(texture.width) * texture.height, 0);
        uint8_t block[16];
        for (uint32_t by = 0; by < blocksHigh; by++)
        {
            for (uint32_t bx = 0; bx < blocksWide; bx++)
            {
                uint64_t address = texture.tiled ? TiledOffset(bx, by, pitchBlocks, shape.log2Bytes)
                                                 : (uint64_t(by) * pitchBlocks + bx) * blockBytes;
                if (address + blockBytes > size)
                {
                    out = {};
                    return false;
                }
                // Undo the console's byte order: swapped within 16- or 32-bit words.
                for (uint32_t i = 0; i < blockBytes; i++)
                {
                    uint32_t from = i;
                    if (texture.endian == TextureEndian::In16 && blockBytes >= 2)
                        from = i ^ 1;
                    else if (texture.endian == TextureEndian::In32 && blockBytes >= 4)
                        from = i ^ 3;
                    block[i] = data[address + from];
                }
                if (shape.blockWidth == 1)
                {
                    uint32_t texel;
                    if (texture.format == TextureFormat::L8)
                        texel = block[0] * 0x010101u | 0xFF000000u;
                    else
                    {
                        // 0xAARRGGBB in host order: bytes B, G, R, A.
                        texel = uint32_t(block[2]) | uint32_t(block[1]) << 8 | uint32_t(block[0]) << 16 |
                                uint32_t(block[3]) << 24;
                    }
                    out.rgba[size_t(by) * texture.width + bx] = texel;
                    continue;
                }
                uint32_t texels[16];
                DecodeBlock(block, texture.format, texels);
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < texture.width && y < texture.height)
                        out.rgba[size_t(y) * texture.width + x] = texels[i];
                }
            }
        }
        return true;
    }

    void FlipVertically(Image& image)
    {
        for (uint32_t y = 0; y < image.height / 2; y++)
            std::swap_ranges(image.rgba.begin() + size_t(y) * image.width,
                image.rgba.begin() + size_t(y + 1) * image.width,
                image.rgba.begin() + size_t(image.height - 1 - y) * image.width);
    }
}
