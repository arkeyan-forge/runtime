#pragma once

// DDS files for the texture packs (docs/runtime/gpu/texture_packs.md). Plain C++ only, no game or GPU code, so a test can use it.
//
// Written: the legacy header (BC1-3 as DXT1/3/5, BC4/BC5 as ATI1/ATI2, RGBA8 as 32-bit masks), one level or a full chain.
// Read, as a modder's tool writes them: the legacy header or the DX10 one (DXGI BC1-BC5, R8G8B8A8, B8G8R8A8, UNORM or
// not), 32-bit masks in RGBA or BGRA order (no alpha mask: opaque). Not read: cube maps, volumes, arrays, sRGB-only
// intent is ignored (the data is used as is), anything else (BC6/BC7, 16-bit...) is reported.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dds
{
    enum class Format : uint8_t
    {
        BC1,
        BC2,
        BC3,
        BC4,
        BC5,
        RGBA8
    };

    // 8 bytes per 4x4 block for BC1 and BC4, 16 for BC2, BC3 and BC5, 4 per texel for RGBA8.
    uint32_t BlockBytes(Format format);
    bool IsBlock(Format format);

    // One level's size: rows of blocks (texels for RGBA8) and the bytes per row.
    struct LevelSize
    {
        uint32_t rows;
        uint32_t rowBytes;
        uint64_t Bytes() const { return uint64_t(rows) * rowBytes; }
    };
    LevelSize SizeOfLevel(Format format, uint32_t width, uint32_t height, uint32_t level);

    // The levels a chain of `width` x `height` has down to 1x1.
    uint32_t FullChain(uint32_t width, uint32_t height);

    struct Info
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t levels = 1;
        Format format = Format::RGBA8;
        uint64_t dataOffset = 0; // of level 0 in the file; the levels follow each other, largest first
        bool swapRB = false;     // RGBA8 stored as BGRA: swap red and blue after reading
        bool opaque = false;     // RGBA8 with no alpha channel: alpha is set to 255 after reading
    };

    // The most a header takes (magic, 124-byte header, DX10 extension).
    constexpr size_t MaxHeaderBytes = 4 + 124 + 20;

    // Parses the start of a file (`bytes` at least min(file size, MaxHeaderBytes)). `fileSize` is checked against the
    // size the header promises. Returns false and says why in `error`.
    bool Parse(std::span<const uint8_t> bytes, uint64_t fileSize, Info& info, std::string& error);

    // Where level `level` starts, from the start of the file.
    uint64_t LevelOffset(const Info& info, uint32_t level);

    // Fixes RGBA8 texels read as stored (swaps red and blue, forces alpha).
    void ConvertTexels(const Info& info, uint8_t* texels, size_t size);

    // The file: header, then each level's rows packed (`levels[0]` is the top level, `rowBytes` per row as SizeOfLevel).
    std::vector<uint8_t> Write(
        Format format, uint32_t width, uint32_t height, std::span<const std::span<const uint8_t>> levels);
}
