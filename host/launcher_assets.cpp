#include "launcher_assets.h"
#include "collection/datadir.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <span>
#include <stb_image_write.h>
// Use the miniz implementation already vendored with the renderer instead of adding a separate zlib
// dependency. Only this translation unit sees the header.
#include <miniz.h>

namespace LauncherAssets
{
    namespace
    {
        constexpr uint64_t CacheLimitBytes = 256ull << 20;
        constexpr uint32_t ApkfMagic = 0x41504B46; // "APKF"
        constexpr uint32_t TextureFormatDxt5 = 20; // k_DXT4_5

        struct RoleSource
        {
            Role role;
            const char* fileName;
            uint32_t width;
            uint32_t height;
        };

        // Frontend art that ships on the player's own disc. Dimensions are the texture sizes stored
        // with these APKF entries (the payload is exactly width * height bytes of BC3).
        constexpr RoleSource Sources[] = {
            { Role::Logo, "skylanderslogo.dds", 1024, 512 },
            { Role::HeroBackground, "godraysdark.dds", 512, 512 },
        };

        std::string RoleKey(Role role)
        {
            switch (role)
            {
            case Role::Logo: return "logo";
            case Role::HeroBackground: return "hero_bg";
            case Role::CharacterPortrait: return "portrait";
            case Role::ButtonTexture: return "button";
            case Role::Font: return "font";
            case Role::TitleIcon: return "title_icon";
            case Role::LoadingBackground: return "loading_bg";
            }
            return "unknown";
        }

        uint32_t ReadBE32(const uint8_t* p)
        {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        std::string BaseName(const std::string& path)
        {
            size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string FindPath(const uint8_t* header, size_t headerSize)
        {
            if (headerSize <= 0x4c)
                return {};
            const char* start = reinterpret_cast<const char*>(header + 0x4c);
            const void* end = memchr(start, 0, headerSize - 0x4c);
            if (end == nullptr)
                return {};
            return std::string(start, static_cast<const char*>(end) - start);
        }

        // STRM: magic, then a zlib stream at offset 0x14. The stream carries its own checksum.
        bool DecompressStrm(std::span<const uint8_t> file, std::vector<uint8_t>& out)
        {
            if (file.size() <= 0x14 || memcmp(file.data(), "strm", 4) != 0)
                return false;

            size_t size = 0;
            void* data = tinfl_decompress_mem_to_heap(file.data() + 0x14, file.size() - 0x14, &size,
                TINFL_FLAG_PARSE_ZLIB_HEADER);
            if (data == nullptr || size == 0)
                return false;
            out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
            mz_free(data);
            return true;
        }

        // Xbox 360 DXT textures are tiled in 32x32-block tiles. This is the same layout used by the
        // renderer (adapted from Xenia's texture_address::Tiled2D, BSD-3).
        uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlockLog2)
        {
            uint32_t outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
            uint32_t innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
            uint32_t bytes = (outerBlocks | innerBlocks) << bytesPerBlockLog2;
            uint32_t bank = (y >> 4) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15) |
                (((bytes >> 4) & 1) << 5) | (((bytes >> 5) & 7) << 8) | ((bytes >> 8) << 12);
        }

        uint8_t Expand5(uint32_t value)
        {
            return uint8_t((value << 3) | (value >> 2));
        }

        uint8_t Expand6(uint32_t value)
        {
            return uint8_t((value << 2) | (value >> 4));
        }

        // Decode one BC3 block to 16 RGBA texels.
        void DecodeBc3Block(const uint8_t* block, uint8_t* pixels)
        {
            uint8_t alpha0 = block[0];
            uint8_t alpha1 = block[1];
            uint64_t alphaBits = 0;
            for (int i = 0; i < 6; i++)
                alphaBits |= uint64_t(block[2 + i]) << (8 * i);

            uint8_t alphas[8] = { alpha0, alpha1 };
            if (alpha0 > alpha1)
            {
                for (int i = 0; i < 6; i++)
                    alphas[2 + i] = uint8_t(((7 - i) * alpha0 + (i + 1) * alpha1) / 7);
            }
            else
            {
                for (int i = 0; i < 4; i++)
                    alphas[2 + i] = uint8_t(((5 - i) * alpha0 + (i + 1) * alpha1) / 5);
                alphas[6] = 0;
                alphas[7] = 255;
            }

            uint16_t color0 = uint16_t(block[8] | block[9] << 8);
            uint16_t color1 = uint16_t(block[10] | block[11] << 8);
            uint32_t colorBits = uint32_t(block[12]) | uint32_t(block[13]) << 8 |
                uint32_t(block[14]) << 16 | uint32_t(block[15]) << 24;

            uint8_t colors[4][3] = {
                { Expand5((color0 >> 11) & 31), Expand6((color0 >> 5) & 63), Expand5(color0 & 31) },
                { Expand5((color1 >> 11) & 31), Expand6((color1 >> 5) & 63), Expand5(color1 & 31) },
                {},
                {},
            };
            if (color0 > color1)
            {
                for (int c = 0; c < 3; c++)
                {
                    colors[2][c] = uint8_t((2 * colors[0][c] + colors[1][c]) / 3);
                    colors[3][c] = uint8_t((colors[0][c] + 2 * colors[1][c]) / 3);
                }
            }
            else
            {
                for (int c = 0; c < 3; c++)
                {
                    colors[2][c] = uint8_t((colors[0][c] + colors[1][c]) / 2);
                    colors[3][c] = 0;
                }
            }

            for (int i = 0; i < 16; i++)
            {
                uint8_t* out = pixels + i * 4;
                uint32_t color = (colorBits >> (2 * i)) & 3;
                uint32_t alpha = uint32_t((alphaBits >> (3 * i)) & 7);
                out[0] = colors[color][0];
                out[1] = colors[color][1];
                out[2] = colors[color][2];
                out[3] = alphas[alpha];
            }
        }

        bool DecodeDxt5(std::span<const uint8_t> data, uint32_t width, uint32_t height,
            std::vector<uint8_t>& rgba)
        {
            if ((width & 3) != 0 || (height & 3) != 0)
                return false;
            uint32_t blocksX = width / 4, blocksY = height / 4;
            uint32_t pitchBlocks = (blocksX + 31) / 32 * 32;
            if (uint64_t(pitchBlocks) * blocksY * 16 > data.size())
                return false;

            rgba.assign(size_t(width) * height * 4, 0);
            for (uint32_t by = 0; by < blocksY; by++)
            {
                for (uint32_t bx = 0; bx < blocksX; bx++)
                {
                    size_t offset = Tiled2D(bx, by, pitchBlocks, 4);
                    uint8_t block[64];
                    DecodeBc3Block(data.data() + offset, block);
                    for (uint32_t ty = 0; ty < 4; ty++)
                    {
                        size_t dest = (size_t(by * 4 + ty) * width + bx * 4) * 4;
                        memcpy(rgba.data() + dest, block + ty * 16, 16);
                    }
                }
            }
            return true;
        }

        bool WritePng(const std::string& path, uint32_t width, uint32_t height,
            const std::vector<uint8_t>& rgba)
        {
            return stbi_write_png(path.c_str(), int(width), int(height), 4, rgba.data(), int(width * 4)) != 0;
        }

        const uint8_t* FindBytes(const uint8_t* begin, const uint8_t* end, const char* needle)
        {
            const uint8_t* found = std::search(begin, end, needle, needle + 4);
            return found == end ? nullptr : found;
        }

        bool DecodeTextureFromStartMenu(std::span<const uint8_t> startMenu, const RoleSource& source,
            std::vector<uint8_t>& rgba, std::string* assetPathOut)
        {
            for (size_t position = 0; position + 0x100 <= startMenu.size();)
            {
                size_t magic = 0;
                while (magic + 4 <= startMenu.size() - position &&
                    ReadBE32(startMenu.data() + position + magic) != ApkfMagic)
                    magic++;
                if (magic + 4 > startMenu.size() - position)
                    break;
                size_t entry = position + magic;
                position = entry + 4;

                if (entry < 16 || entry + 0x100 > startMenu.size())
                    continue;
                uint32_t chunkSize = ReadBE32(startMenu.data() + entry - 4);
                if (chunkSize < 0x100 || entry + chunkSize > startMenu.size())
                    continue;

                std::string assetPath = FindPath(startMenu.data() + entry, std::min<size_t>(chunkSize, 0x100));
                if (BaseName(assetPath) != source.fileName)
                    continue;

                // The texture descriptor starts at +0x28 (big-endian) and the payload at +0x1000.
                uint32_t format = ReadBE32(startMenu.data() + entry + 0x28);
                const uint8_t* header = startMenu.data() + entry;
                const uint8_t* syhp = FindBytes(header, header + std::min<size_t>(chunkSize, 0x100), "SYHP");
                if (format != TextureFormatDxt5 || syhp == nullptr)
                    return false;
                size_t syhpOffset = size_t(syhp - header);
                if (syhpOffset + 20 > chunkSize)
                    return false;
                uint32_t dataSize = ReadBE32(header + syhpOffset + 16);
                if (dataSize == 0 || entry + 0x1000 + dataSize > startMenu.size())
                    return false;

                if (assetPathOut != nullptr)
                    *assetPathOut = assetPath;
                return DecodeDxt5(std::span<const uint8_t>(startMenu.data() + entry + 0x1000, dataSize),
                    source.width, source.height, rgba);
            }
            return false;
        }
    }

    bool DecodeFrontendTexture(std::span<const uint8_t> strmFile, std::string_view fileName, uint32_t width,
        uint32_t height, std::vector<uint8_t>& rgba)
    {
        std::vector<uint8_t> startMenu;
        if (!DecompressStrm(strmFile, startMenu))
            return false;
        std::string name(fileName);
        RoleSource source{ Role::Logo, name.c_str(), width, height };
        return DecodeTextureFromStartMenu(startMenu, source, rgba, nullptr);
    }

    void Provider::Init(const std::string& iso, uint32_t tid)
    {
        isoPath = iso;
        titleId = tid;
        enabled = tid != 0;
        if (!enabled)
        {
            cacheDir.clear();
            return;
        }
        std::filesystem::path game = datadir::Game("ssa");
        if (game.empty())
        {
            cacheDir.clear();
            enabled = false;
            return;
        }
        cacheDir = datadir::Text(game / "launcher-cache") + "/";
        datadir::TrimFolder(game / "launcher-cache", CacheLimitBytes);
        std::error_code error;
        std::filesystem::create_directories(cacheDir + fmt::format("{:08X}", titleId), error);
    }

    std::string Provider::CachePathFor(const std::string& roleKey) const
    {
        if (!enabled || cacheDir.empty())
            return {};
        return cacheDir + fmt::format("{:08X}", titleId) + "/" + roleKey + ".png";
    }

    bool Provider::LoadFromCache(Role role, Asset& out) const
    {
        out = {};
        std::string path = CachePathFor(RoleKey(role));
        if (path.empty())
            return false;
        std::ifstream file(path, std::ios::binary);
        if (!file.good())
            return false;
        out.path = path;
        out.valid = true;
        return true;
    }

    void Provider::ExtractFromDisc(DiscFileSystem& disc, const TitleMetadata& meta)
    {
        if (!enabled)
            return;

        // The title icon comes from the XDBF resource of the player's default.xex.
        if (!meta.iconPng.empty())
        {
            std::string path = CachePathFor(RoleKey(Role::TitleIcon));
            if (!path.empty())
            {
                std::ofstream out(path, std::ios::binary);
                if (out.good())
                    out.write(reinterpret_cast<const char*>(meta.iconPng.data()),
                        std::streamsize(meta.iconPng.size()));
            }
        }

        // The start menu STRM holds the frontend texture package. Decompress it, then find the
        // requested APKF entries by their original asset path and decode the BC3 payload.
        std::vector<uint8_t> startMenu;
        std::string startMenuPath;
        for (const char* path : { "ui/eng/startmenu.str", "ui/enu/startmenu.str" })
        {
            std::vector<uint8_t> strm = disc.ReadAll(path);
            if (!strm.empty() && DecompressStrm(strm, startMenu))
            {
                startMenuPath = path;
                break;
            }
        }

        std::string manifest = cacheDir + fmt::format("{:08X}", titleId) + "/manifest.ini";
        std::ofstream out(manifest);
        if (out.good())
        {
            out << "# Official frontend art is read from the player's own verified disc at runtime.\n";
            out << "# Decoded images are cached locally; nothing in this folder is shipped or committed.\n";
            out << "[title]\n";
            out << fmt::format("id={:08X}\n", titleId);
            out << "name=" << meta.name << "\n\n";
            out << "[sources]\n";
            out << "startmenu=" << (startMenuPath.empty() ? "not found" : startMenuPath) << " (STRM container, zlib at +0x14)\n";
            out << "title_icon=default.xex XDBF image resource\n";
            out << "\n[roles]\n";
        }

        if (!startMenu.empty())
        {
            for (const RoleSource& source : Sources)
            {
                std::vector<uint8_t> rgba;
                std::string assetPath;
                if (!DecodeTextureFromStartMenu(startMenu, source, rgba, &assetPath))
                    continue;
                std::string cachePath = CachePathFor(RoleKey(source.role));
                if (cachePath.empty() || !WritePng(cachePath, source.width, source.height, rgba))
                    continue;
                if (out.good())
                    out << fmt::format("{}={} ({}x{}, DXT5/BC3, tiled) -> {}.png\n",
                        RoleKey(source.role), assetPath, source.width, source.height, RoleKey(source.role));
            }
        }
        if (out.good())
            out << "title_icon=default.xex XDBF image resource -> title_icon.png\n";
    }

    Asset Provider::Get(Role role)
    {
        Asset asset;
        LoadFromCache(role, asset);
        return asset;
    }
} // namespace LauncherAssets
