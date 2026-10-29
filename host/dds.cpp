#include "host/dds.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace dds
{
    namespace
    {
        constexpr uint32_t Magic = 0x20534444; // "DDS "
        constexpr uint32_t HeaderSize = 124;

        // DDS_HEADER flags and caps.
        constexpr uint32_t Caps = 0x1, Height = 0x2, Width = 0x4, Pitch = 0x8, PixelFormat = 0x1000,
                           MipMapCount = 0x20000, LinearSize = 0x80000;
        constexpr uint32_t CapsComplex = 0x8, CapsTexture = 0x1000, CapsMipMap = 0x400000;
        constexpr uint32_t Caps2Cube = 0x200, Caps2Volume = 0x200000;
        // DDS_PIXELFORMAT flags.
        constexpr uint32_t PixelAlpha = 0x1, PixelFourCC = 0x4, PixelRgb = 0x40;

        constexpr uint32_t FourCC(char a, char b, char c, char d)
        {
            return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
                   uint32_t(uint8_t(d)) << 24;
        }

        uint32_t Read32(const uint8_t* bytes, size_t offset)
        {
            uint32_t value;
            memcpy(&value, bytes + offset, 4);
            return value; // little-endian host (every target)
        }

        void Put32(std::vector<uint8_t>& out, size_t offset, uint32_t value) { memcpy(out.data() + offset, &value, 4); }

        // DXGI formats the reader knows: the format, whether red and blue are swapped.
        bool FromDxgi(uint32_t dxgi, Format& format, bool& swapRB)
        {
            swapRB = false;
            switch (dxgi)
            {
            case 71:
            case 72: format = Format::BC1; return true;
            case 74:
            case 75: format = Format::BC2; return true;
            case 77:
            case 78: format = Format::BC3; return true;
            case 80:
            case 81: format = Format::BC4; return true;
            case 83:
            case 84: format = Format::BC5; return true;
            case 28:
            case 29: format = Format::RGBA8; return true;
            case 87:
            case 91:
            case 88:
            case 93:
                format = Format::RGBA8;
                swapRB = true;
                return true;
            default: return false;
            }
        }
    }

    uint32_t BlockBytes(Format format)
    {
        switch (format)
        {
        case Format::BC1:
        case Format::BC4: return 8;
        case Format::RGBA8: return 4;
        default: return 16;
        }
    }

    bool IsBlock(Format format) { return format != Format::RGBA8; }

    LevelSize SizeOfLevel(Format format, uint32_t width, uint32_t height, uint32_t level)
    {
        uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
        if (!IsBlock(format))
            return { h, w * 4 };
        return { (h + 3) / 4, (w + 3) / 4 * BlockBytes(format) };
    }

    uint32_t FullChain(uint32_t width, uint32_t height)
    {
        return 32 - uint32_t(std::countl_zero(std::max({ width, height, 1u })));
    }

    uint64_t LevelOffset(const Info& info, uint32_t level)
    {
        uint64_t offset = info.dataOffset;
        for (uint32_t i = 0; i < level; i++)
            offset += SizeOfLevel(info.format, info.width, info.height, i).Bytes();
        return offset;
    }

    bool Parse(std::span<const uint8_t> bytes, uint64_t fileSize, Info& info, std::string& error)
    {
        if (bytes.size() < 4 + HeaderSize || Read32(bytes.data(), 0) != Magic || Read32(bytes.data(), 4) != HeaderSize)
        {
            error = "not a DDS file";
            return false;
        }
        const uint8_t* h = bytes.data() + 4;
        uint32_t flags = Read32(h, 4), height = Read32(h, 8), width = Read32(h, 12), mips = Read32(h, 24);
        uint32_t pixelFlags = Read32(h, 76), fourCC = Read32(h, 80), bits = Read32(h, 84);
        uint32_t maskR = Read32(h, 88), maskG = Read32(h, 92), maskB = Read32(h, 96), maskA = Read32(h, 100);
        uint32_t caps2 = Read32(h, 108);

        if (caps2 & (Caps2Cube | Caps2Volume))
        {
            error = "cube maps and volumes are not supported";
            return false;
        }
        if (width == 0 || height == 0 || width > 16384 || height > 16384)
        {
            error = "bad size " + std::to_string(width) + "x" + std::to_string(height);
            return false;
        }
        info = {};
        info.width = width;
        info.height = height;
        info.dataOffset = 4 + HeaderSize;
        info.levels = (flags & MipMapCount) && mips > 0 ? mips : 1;
        if (info.levels > FullChain(width, height))
        {
            error = "more mip levels than the size has";
            return false;
        }

        if (pixelFlags & PixelFourCC)
        {
            switch (fourCC)
            {
            case FourCC('D', 'X', 'T', '1'): info.format = Format::BC1; break;
            case FourCC('D', 'X', 'T', '2'):
            case FourCC('D', 'X', 'T', '3'): info.format = Format::BC2; break;
            case FourCC('D', 'X', 'T', '4'):
            case FourCC('D', 'X', 'T', '5'): info.format = Format::BC3; break;
            case FourCC('A', 'T', 'I', '1'):
            case FourCC('B', 'C', '4', 'U'): info.format = Format::BC4; break;
            case FourCC('A', 'T', 'I', '2'):
            case FourCC('B', 'C', '5', 'U'): info.format = Format::BC5; break;
            case FourCC('D', 'X', '1', '0'):
            {
                if (bytes.size() < 4 + HeaderSize + 20)
                {
                    error = "truncated DX10 header";
                    return false;
                }
                uint32_t dxgi = Read32(bytes.data(), 4 + HeaderSize),
                         dimension = Read32(bytes.data(), 4 + HeaderSize + 4);
                uint32_t miscFlag = Read32(bytes.data(), 4 + HeaderSize + 8),
                         arraySize = Read32(bytes.data(), 4 + HeaderSize + 12);
                if (!FromDxgi(dxgi, info.format, info.swapRB))
                {
                    error = "DXGI format " + std::to_string(dxgi) + " is not supported (BC1-BC5, RGBA8, BGRA8)";
                    return false;
                }
                if (dimension != 3 || (miscFlag & 4) || arraySize > 1)
                {
                    error = "only plain 2D textures are supported";
                    return false;
                }
                info.dataOffset += 20;
                break;
            }
            default: error = "unsupported pixel format (FourCC " + std::to_string(fourCC) + ")"; return false;
            }
        }
        else if ((pixelFlags & PixelRgb) && bits == 32)
        {
            info.format = Format::RGBA8;
            if (maskR == 0xFF && maskG == 0xFF00 && maskB == 0xFF0000)
                info.swapRB = false;
            else if (maskR == 0xFF0000 && maskG == 0xFF00 && maskB == 0xFF)
                info.swapRB = true;
            else
            {
                error = "unsupported 32-bit channel masks";
                return false;
            }
            if (!(pixelFlags & PixelAlpha) || maskA == 0)
                info.opaque = true;
            else if (maskA != 0xFF000000)
            {
                error = "unsupported 32-bit channel masks";
                return false;
            }
        }
        else
        {
            error = "unsupported pixel format (use BC1-BC5 or 32-bit RGBA)";
            return false;
        }

        uint64_t end = LevelOffset(info, info.levels);
        if (fileSize < end)
        {
            error = "truncated: " + std::to_string(fileSize) + " bytes, the header needs " + std::to_string(end);
            return false;
        }
        return true;
    }

    void ConvertTexels(const Info& info, uint8_t* texels, size_t size)
    {
        if (info.format != Format::RGBA8 || (!info.swapRB && !info.opaque))
            return;
        for (size_t i = 0; i + 4 <= size; i += 4)
        {
            if (info.swapRB)
                std::swap(texels[i], texels[i + 2]);
            if (info.opaque)
                texels[i + 3] = 0xFF;
        }
    }

    std::vector<uint8_t> Write(
        Format format, uint32_t width, uint32_t height, std::span<const std::span<const uint8_t>> levels)
    {
        std::vector<uint8_t> out(4 + HeaderSize, 0);
        Put32(out, 0, Magic);
        Put32(out, 4, HeaderSize);
        uint32_t flags = Caps | Height | Width | PixelFormat;
        LevelSize top = SizeOfLevel(format, width, height, 0);
        flags |= IsBlock(format) ? LinearSize : Pitch;
        if (levels.size() > 1)
            flags |= MipMapCount;
        Put32(out, 8, flags);
        Put32(out, 12, height);
        Put32(out, 16, width);
        Put32(out, 20, IsBlock(format) ? uint32_t(top.Bytes()) : top.rowBytes);
        Put32(out, 28, uint32_t(levels.size()));
        // DDS_PIXELFORMAT at header offset 72 (file offset 76).
        Put32(out, 76, 32);
        if (format == Format::RGBA8)
        {
            Put32(out, 80, PixelRgb | PixelAlpha);
            Put32(out, 88, 32);
            Put32(out, 92, 0xFF);
            Put32(out, 96, 0xFF00);
            Put32(out, 100, 0xFF0000);
            Put32(out, 104, 0xFF000000);
        }
        else
        {
            static const uint32_t FourCCs[] = { FourCC('D', 'X', 'T', '1'), FourCC('D', 'X', 'T', '3'),
                FourCC('D', 'X', 'T', '5'), FourCC('A', 'T', 'I', '1'), FourCC('A', 'T', 'I', '2') };
            Put32(out, 80, PixelFourCC);
            Put32(out, 84, FourCCs[uint32_t(format)]);
        }
        Put32(out, 108, CapsTexture | (levels.size() > 1 ? CapsComplex | CapsMipMap : 0));
        for (const auto& level : levels)
            out.insert(out.end(), level.begin(), level.end());
        return out;
    }
}
