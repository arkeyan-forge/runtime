#pragma once

// Xbox 360 (Xenos) texture pixels to RGBA: DXT1, DXT3, DXT5, A8R8G8B8 and 8-bit, linear or tiled, in the GPU's byte
// order. Used for the art read from the player's own disc (docs/runtime/host/game_art.md); nothing is shipped.
// Shared with Skylauncher (tools/sync_skylauncher.sh): no runtime dependency.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace art
{
    enum class TextureFormat
    {
        Dxt1,  // BC1, 8 bytes per 4x4 block
        Dxt3,  // BC2, 16 bytes per block
        Dxt5,  // BC3, 16 bytes per block
        Argb8, // D3DFMT_A8R8G8B8: one 32-bit word per texel, 0xAARRGGBB once in host order
        L8,    // one byte per texel, grey (alpha 255)
    };

    // How the console stores a texture's bytes: the GPU fetch constant's endian field.
    enum class TextureEndian
    {
        None = 0,
        In16 = 1, // 8-in-16: every 16-bit word byte-swapped (the DXT formats)
        In32 = 2, // 8-in-32: every 32-bit word byte-swapped (A8R8G8B8)
    };

    struct Texture
    {
        TextureFormat format = TextureFormat::Dxt5;
        TextureEndian endian = TextureEndian::In16;
        uint32_t width = 0, height = 0;
        bool tiled = true;
        // Row pitch in texels; 0 = the console's default (tiled: 32 blocks; linear: 256 bytes).
        uint32_t pitch = 0;
    };

    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> rgba; // R in the low byte, straight alpha
    };

    // The byte offset of block (x, y) in a 2D tiled surface whose pitch is `pitchBlocks` blocks (a multiple of 32),
    // with 2^log2Bytes bytes per block. Adapted from Xenia's texture_address::Tiled2D (BSD-3).
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t log2Bytes);

    // Decodes `texture` from `data` (`size` bytes, as on the disc). False, and `out` empty, if the data is too short.
    bool DecodeTexture(const uint8_t* data, size_t size, const Texture& texture, Image& out);

    // One BC1/BC2/BC3 block (little-endian words, after the byte swap) to 4x4 RGBA texels, row by row.
    void DecodeBlock(const uint8_t* block, TextureFormat format, uint32_t texels[16]);

    // Alchemy (Giants) stores its UI and portrait pictures upside down: rows reversed.
    void FlipVertically(Image& image);
}
