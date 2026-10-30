// SSAArtTest: the disc picture readers (docs/runtime/host/game_art.md). Without arguments it needs no game data: every
// format is checked on synthetic fixtures built here. With SSA_ISO, GIANTS_ISO and/or SWAPFORCE_ISO set (read only) it
// also reads each game's pictures into a scratch data folder (SKYLANDERS_DATA, set it to a scratch folder) and checks
// what it got.
//   ./SSAArtTest [--keep]            --keep: leave the scratch pictures to look at
#include "host/art/alchemy_archive.h"
#include "host/art/disc_languages.h"
#include "host/art/game_art.h"
#include "host/art/ico.h"
#include "host/art/lux_images.h"
#include "host/art/xenos_texture.h"
#include "host/collection/catalogue.h"
#include "host/collection/datadir.h"
#include "host/collection/icons.h"
#include "host/collection/upgrades.h"
#include "vfs.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "../env.h"

namespace
{
    int g_checks = 0, g_failures = 0;

    // For getenv too: on Windows SDL_setenv_unsafe writes only the Win32 environment, which getenv does not read.
    void SetVariable(const char* name, const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            g_failures++;                                                                                              \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                     \
        }                                                                                                              \
    } while (0)

    void PutBe32(std::vector<uint8_t>& out, size_t at, uint32_t value)
    {
        if (out.size() < at + 4)
            out.resize(at + 4);
        out[at] = uint8_t(value >> 24), out[at + 1] = uint8_t(value >> 16), out[at + 2] = uint8_t(value >> 8),
        out[at + 3] = uint8_t(value);
    }

    void PutLe32(std::vector<uint8_t>& out, size_t at, uint32_t value)
    {
        if (out.size() < at + 4)
            out.resize(at + 4);
        out[at] = uint8_t(value), out[at + 1] = uint8_t(value >> 8), out[at + 2] = uint8_t(value >> 16),
        out[at + 3] = uint8_t(value >> 24);
    }

    uint16_t To565(uint32_t r, uint32_t g, uint32_t b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

    // A BC3 block of one colour and one alpha (indices 0), as the console stores it: 16-bit words big-endian.
    void Dxt5Block(uint8_t* at, uint16_t color, uint8_t alpha)
    {
        uint8_t block[16] = { alpha, alpha, 0, 0, 0, 0, 0, 0, uint8_t(color), uint8_t(color >> 8), uint8_t(color),
            uint8_t(color >> 8), 0, 0, 0, 0 };
        for (int i = 0; i < 16; i += 2)
            at[i] = block[i + 1], at[i + 1] = block[i];
    }

    uint32_t Expand565(uint16_t c)
    {
        uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return ((r << 3) | (r >> 2)) | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2)) << 16;
    }

    // --- Textures --------------------------------------------------------------------------------------------------

    void TestTiling()
    {
        // Within one 32x32-block tile, every block has its own place; from 4 bytes per block on, inside the tile (smaller
        // elements interleave with the next tiles).
        for (uint32_t log2 : { 0u, 2u, 3u, 4u })
        {
            std::set<uint32_t> seen;
            bool inside = true;
            for (uint32_t y = 0; y < 32; y++)
                for (uint32_t x = 0; x < 32; x++)
                {
                    uint32_t offset = art::TiledOffset(x, y, 32, log2);
                    seen.insert(offset >> log2);
                    inside = inside && (log2 < 2 || offset < (32u * 32u << log2));
                }
            CHECK(seen.size() == 32 * 32 && inside);
        }
        // The second tile of a row follows the first.
        CHECK(art::TiledOffset(32, 0, 64, 4) >= 32 * 32 * 16);
    }

    void TestDxt()
    {
        // DXT5 block: alpha 255/0 with index 0 then 1, colours pure red / pure blue with index 0 then 1.
        const uint8_t Block[16] = { 0xFF, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x1F, 0x00, 0x04, 0x00,
            0x00, 0x00 };
        uint32_t texels[16];
        art::DecodeBlock(Block, art::TextureFormat::Dxt5, texels);
        CHECK(texels[0] == 0xFF0000FF); // red, opaque
        CHECK(texels[1] == 0x00FF0000); // blue, transparent
        CHECK(texels[2] == 0xFF0000FF);
        // DXT1 with c0 <= c1: index 3 is transparent black.
        const uint8_t Dxt1[8] = { 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00 };
        art::DecodeBlock(Dxt1, art::TextureFormat::Dxt1, texels);
        CHECK(texels[0] == 0 && texels[1] == 0xFF000000 && texels[2] == 0xFF000000);
        // DXT3: 4-bit alpha, 0xF -> 255, 0x8 -> 136.
        uint8_t dxt3[16] = { 0x8F, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0 };
        art::DecodeBlock(dxt3, art::TextureFormat::Dxt3, texels);
        CHECK(texels[0] == 0xFF0000FF && texels[1] == 0x880000FF);

        // A tiled DXT5 texture of 24x20 texels (6x5 blocks, so a partial block row is cut): each block its own colour.
        art::Texture texture{ art::TextureFormat::Dxt5, art::TextureEndian::In16, 24, 18, true, 0 };
        std::vector<uint8_t> data(32 * 32 * 16, 0);
        auto colorOf = [](uint32_t bx, uint32_t by) { return To565(bx * 40, by * 50, 200); };
        for (uint32_t by = 0; by < 5; by++)
            for (uint32_t bx = 0; bx < 6; bx++)
                Dxt5Block(&data[art::TiledOffset(bx, by, 32, 4)], colorOf(bx, by), uint8_t(10 * (bx + 6 * by)));
        art::Image image;
        CHECK(art::DecodeTexture(data.data(), data.size(), texture, image));
        bool right = image.width == 24 && image.height == 18 && image.rgba.size() == 24 * 18;
        for (uint32_t y = 0; y < 18 && right; y++)
            for (uint32_t x = 0; x < 24; x++)
                right = right && image.rgba[y * 24 + x] ==
                                     (Expand565(colorOf(x / 4, y / 4)) | uint32_t(10 * (x / 4 + 6 * (y / 4))) << 24);
        CHECK(right);
        // Too little data: refused, nothing half-decoded.
        CHECK(!art::DecodeTexture(data.data(), 100, texture, image) && image.rgba.empty());

        // Linear DXT1, 8x4 (2 blocks): rows are 256 bytes apart, words swapped.
        art::Texture linear{ art::TextureFormat::Dxt1, art::TextureEndian::In16, 8, 4, false, 0 };
        std::vector<uint8_t> rows(256, 0);
        uint16_t green = To565(0, 255, 0), blue = To565(0, 0, 255);
        for (int i = 0; i < 2; i++)
        {
            uint16_t c = i == 0 ? green : blue;
            uint8_t block[8] = { uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0, 0, 0 };
            for (int b = 0; b < 8; b += 2)
                rows[i * 8 + b] = block[b + 1], rows[i * 8 + b + 1] = block[b];
        }
        CHECK(art::DecodeTexture(rows.data(), rows.size(), linear, image));
        CHECK(image.rgba[0] == 0xFF00FF00 && image.rgba[7] == 0xFFFF0000 && image.rgba[3 * 8 + 4] == 0xFFFF0000);
    }

    void TestArgbAndL8()
    {
        // A8R8G8B8, tiled, 8-in-32: each texel 0xAARRGGBB stored big-endian (A, R, G, B in memory).
        art::Texture texture{ art::TextureFormat::Argb8, art::TextureEndian::In32, 5, 3, true, 0 };
        std::vector<uint8_t> data(32 * 32 * 4, 0);
        auto argb = [](uint32_t x, uint32_t y) { return 0x80000000u | (x * 40) << 16 | (y * 70) << 8 | 0x33; };
        for (uint32_t y = 0; y < 3; y++)
            for (uint32_t x = 0; x < 5; x++)
            {
                uint32_t at = art::TiledOffset(x, y, 32, 2), v = argb(x, y);
                data[at] = uint8_t(v >> 24), data[at + 1] = uint8_t(v >> 16), data[at + 2] = uint8_t(v >> 8),
                data[at + 3] = uint8_t(v);
            }
        art::Image image;
        CHECK(art::DecodeTexture(data.data(), data.size(), texture, image));
        bool right = image.rgba.size() == 15;
        for (uint32_t y = 0; y < 3 && right; y++)
            for (uint32_t x = 0; x < 5; x++)
            {
                uint32_t v = argb(x, y),
                         rgba = (v >> 16 & 0xFF) | (v >> 8 & 0xFF) << 8 | (v & 0xFF) << 16 | (v >> 24) << 24;
                right = right && image.rgba[y * 5 + x] == rgba;
            }
        CHECK(right);

        art::Texture grey{ art::TextureFormat::L8, art::TextureEndian::None, 4, 2, false, 0 };
        std::vector<uint8_t> bytes(256 * 2, 0);
        bytes[1] = 0x40, bytes[256 + 3] = 0xFF;
        CHECK(art::DecodeTexture(bytes.data(), bytes.size(), grey, image));
        CHECK(image.rgba[1] == 0xFF404040 && image.rgba[4 + 3] == 0xFFFFFFFF && image.rgba[0] == 0xFF000000);

        art::Image flip{ 1, 3, { 1, 2, 3 } };
        art::FlipVertically(flip);
        CHECK((flip.rgba == std::vector<uint32_t>{ 3, 2, 1 }));
    }

    // --- Alchemy ---------------------------------------------------------------------------------------------------

    const char LzmaText[] = "Skylanders Giants: Tree Rex, Jet-Vac, Pop Fizz. ";
    // LzmaText x4, raw LZMA (lc=3 lp=0 pb=2, dictionary 0x8000), from Python's lzma module.
    const uint8_t LzmaStream[] = { 0x00, 0x29, 0x9A, 0xCB, 0x26, 0xDD, 0xDB, 0x00, 0x35, 0xD5, 0x0C, 0xDC, 0x43, 0x55,
        0xC8, 0x3C, 0x25, 0x6A, 0xF8, 0x23, 0x3A, 0x33, 0xE4, 0x3C, 0x10, 0x18, 0xBA, 0x22, 0x80, 0xB4, 0x29, 0x28,
        0xA0, 0x3E, 0x58, 0xDA, 0xE9, 0x3D, 0x98, 0x2E, 0xE1, 0x4D, 0xBC, 0x08, 0x75, 0x54, 0xC2, 0x93, 0x41, 0x43,
        0x58, 0x7A, 0x48, 0x43, 0xC1, 0xFF, 0xFF, 0xFB, 0x3F, 0x20, 0x00 };

    std::string Repeated() { return std::string(LzmaText) + LzmaText + LzmaText + LzmaText; }

    // An IGZ file with one igImage2: an 8x8 DXT5 picture, top half red and bottom half blue as stored (so the right way
    // up it is blue on top), named "levels/test/portraits/eruptor.png", format hash `format`; tiled, or `linear` (rows 32
    // blocks apart). v6: Giants' layout [V]; v7: Swap Force's [V] (sections at 0x14, tags stored byte-swapped, TMHN
    // handles with a kind, the name a string index, a dependency); 8-9: the later games' as the reader's table describes
    // them (tagged records with a 16-byte header, sections { id, offset, size, alignment }) [?].
    std::vector<uint8_t> MakeIgz(uint32_t format, uint32_t version = 6, bool linear = false)
    {
        const bool tagged = version >= 7, v7 = version == 7;
        std::vector<uint8_t> igz(0x800, 0);
        memcpy(igz.data(), "IGZ\x01", 4);
        PutBe32(igz, 4, version);
        // Section 0 (fixups) at 0x800, section 1 (the pool) after it.
        std::vector<uint8_t> fixups(tagged ? 0 : 0x20, 0);
        if (!tagged)
            PutBe32(fixups, 0x0C, 0x20); // the section's own header length
        auto record = [&](uint32_t type, const char* tag, uint32_t count, const std::vector<uint8_t>& payload) {
            std::vector<uint8_t> head(tagged ? 16 : 24, 0);
            if (tagged)
            {
                for (int i = 0; i < 4; i++)
                    head[i] = uint8_t(tag[v7 ? 3 - i : i]); // v7 stores the tag byte-swapped
                PutBe32(head, 4, count), PutBe32(head, 8, uint32_t(16 + payload.size())), PutBe32(head, 12, 16);
            }
            else
                PutBe32(head, 0, type), PutBe32(head, 12, count), PutBe32(head, 16, uint32_t(24 + payload.size())),
                    PutBe32(head, 20, 24);
            fixups.insert(fixups.end(), head.begin(), head.end());
            fixups.insert(fixups.end(), payload.begin(), payload.end());
        };
        std::string types("igObject\0igImage2\0", 18);
        std::vector<uint8_t> typeBytes(types.begin(), types.end());
        typeBytes.resize(20, 0);
        record(0, "TMET", 2, typeBytes);
        std::vector<uint8_t> ids;
        PutBe32(ids, 0, format), PutBe32(ids, 4, 0);
        record(2, "EXID", 1, ids);
        record(5, "RVTB", 1, { 0x03, 0x00, 0x00, 0x00 }); // one object, at pool 0 + 3 * 4 + 4 = 0x10
        const char* name = "levels/test/portraits/eruptor.png";
        const uint32_t pixels =
            linear ? 2 * 32 * 16 : 0x200; // two rows of 32 blocks, or a tiled 32x32-block tile's first part
        std::vector<uint8_t> handles;
        PutBe32(handles, 0, pixels | (v7 ? 0x50u << 24 : 0)),
            PutBe32(handles, 4, 0x100); // at pool 0 + 0x100; v7: kind 0x50
        record(0xA, v7 ? "TMHN" : "RHND", 1, handles);
        if (v7)
        {
            std::string strings = std::string(name) + '\0';
            record(0, "TSTR", 1, std::vector<uint8_t>(strings.begin(), strings.end()));
            const char dependencies[] = "JuiceStandard_Test_10\0textures:/JuiceStandard_Test_10.igz"; // { name, path }
            record(0, "TDEP", 1, std::vector<uint8_t>(dependencies, dependencies + sizeof(dependencies)));
        }

        std::vector<uint8_t> pool(0x100 + pixels, 0);
        PutBe32(pool, 0x10, 1); // class igImage2
        PutBe32(pool, 0x10 + 0x08, v7 ? 0 : 0x80); // the name: v7 string 0, else at pool 0 + 0x80
        PutBe32(pool, 0x10 + 0x30, 8 << 16 | 8); // 8 x 8
        PutBe32(pool, 0x10 + 0x3C, 0x80000000); // external id 0
        PutBe32(pool, 0x10 + 0x48, 0); // handle 0
        if (!v7)
            memcpy(&pool[0x80], name, strlen(name));
        for (uint32_t by = 0; by < 2; by++)
            for (uint32_t bx = 0; bx < 2; bx++)
                Dxt5Block(&pool[0x100 + (linear ? (by * 32 + bx) * 16 : art::TiledOffset(bx, by, 32, 4))],
                    by == 0 ? To565(255, 0, 0) : To565(0, 0, 255), 255);

        // Sections: tagged { id, offset, size, alignment } at 0x10, v7 { ?, offset, size, alignment } at 0x14; v6 { offset,
        // size, alignment, id } at 0x10.
        const size_t table = v7 ? 0x14 : 0x10, word = tagged ? 4 : 0;
        fixups.resize(std::max<size_t>(fixups.size(), 0x1C), 0);
        PutBe32(igz, table + word, 0x800), PutBe32(igz, table + 4 + word, uint32_t(fixups.size()));
        PutBe32(igz, table + 0x10 + word, uint32_t(0x800 + fixups.size())),
            PutBe32(igz, table + 0x14 + word, uint32_t(pool.size()));
        igz.insert(igz.end(), fixups.begin(), fixups.end());
        igz.insert(igz.end(), pool.begin(), pool.end());
        return igz;
    }

    // An IGA archive: entry 0 stored ("plain.txt"), entry 1 one LZMA chunk ("packed.txt"), entry 2 a stored IGZ. v8:
    // Giants' layout [V]; v10: Swap Force's [V] (big-endian, 16-byte entries, chunks 0x800-aligned); v11: as the reader's
    // table describes it [?] (name table at 0x28, 64-bit).
    std::vector<uint8_t> MakeIga(const std::vector<uint8_t>& igz, uint32_t version = 8)
    {
        const bool big = version == 10;
        const uint32_t count = 3, header = version == 8 ? 0x34 : 0x38, entrySize = big ? 16 : 12;
        const size_t align = big ? 0x800 : 0x100;
        auto Put32 = [big](std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
            big ? PutBe32(bytes, at, value) : PutLe32(bytes, at, value);
        };
        std::vector<uint8_t> archive(0x400, 0);
        memcpy(archive.data(),
            big ? "\x1a"
                  "AGI"
                : "IGA\x1a",
            4);
        Put32(archive, 4, version);
        Put32(archive, 0x0C, count);
        // Names: offsets, then the strings, at 0x100.
        std::vector<std::string> names = { "C:/tfb/plain.txt", "C:/tfb/packed.txt",
            "C:/tfb/levels/ui_levelfullscreen/MainMenu.png/0x1.png.igb.tex.igz" };
        std::vector<uint8_t> table(4 * count);
        for (uint32_t i = 0; i < count; i++)
        {
            Put32(table, 4 * i, uint32_t(table.size()));
            table.insert(table.end(), names[i].begin(), names[i].end());
            table.push_back(0);
        }
        if (version == 8)
            Put32(archive, 0x1C, 0x100), Put32(archive, 0x20, uint32_t(table.size()));
        else if (version == 10)
            Put32(archive, 0x2C, 0x100), Put32(archive, 0x30, uint32_t(table.size()));
        else
            Put32(archive, 0x28, 0x100), Put32(archive, 0x2C, 0), Put32(archive, 0x30, uint32_t(table.size()));
        memcpy(&archive[0x100], table.data(), table.size());

        std::string plain = "Stored as is";
        size_t at = 0x400;
        auto entry = [&](uint32_t i, uint32_t offset, uint32_t size, uint32_t flags) {
            size_t slot = header + 4 * count + entrySize * i + entrySize - 12;
            Put32(archive, slot, offset), Put32(archive, slot + 4, size), Put32(archive, slot + 8, flags);
        };
        entry(0, uint32_t(at), uint32_t(plain.size()), 0xFFFFFFFF);
        archive.insert(archive.end(), plain.begin(), plain.end());
        at = (archive.size() + align - 1) & ~(align - 1);
        archive.resize(at, 0);
        entry(1, uint32_t(at), uint32_t(Repeated().size()), 0x20000000);
        archive.push_back(uint8_t(sizeof(LzmaStream))), archive.push_back(0);
        for (uint8_t b : { 0x5D, 0x00, 0x80, 0x00, 0x00 })
            archive.push_back(b);
        archive.insert(archive.end(), LzmaStream, LzmaStream + sizeof(LzmaStream));
        at = (archive.size() + align - 1) & ~(align - 1);
        archive.resize(at, 0);
        entry(2, uint32_t(at), uint32_t(igz.size()), 0xFFFFFFFF);
        archive.insert(archive.end(), igz.begin(), igz.end());
        return archive;
    }

    void TestAlchemy()
    {
        std::string text = Repeated();
        std::vector<uint8_t> out(text.size());
        CHECK(art::alchemy::LzmaDecodeRaw(LzmaStream, sizeof(LzmaStream), out.data(), out.size()) == out.size());
        CHECK(std::string(out.begin(), out.end()) == text);

        std::vector<uint8_t> igz = MakeIgz(0xB9BCDC57), bytes = MakeIga(igz);
        auto reader = [&bytes](uint64_t offset, size_t size, uint8_t* to) {
            if (offset + size > bytes.size())
                return false;
            memcpy(to, bytes.data() + offset, size);
            return true;
        };
        auto archive = art::alchemy::IgaArchive::Open(reader, bytes.size());
        CHECK(archive && archive->Count() == 3);
        if (!archive)
            return;
        CHECK(archive->Name(1) == "C:/tfb/packed.txt");
        std::vector<uint8_t> plain = archive->Read(0), packed = archive->Read(1), first = archive->Read(1, 10);
        CHECK(std::string(plain.begin(), plain.end()) == "Stored as is");
        CHECK(std::string(packed.begin(), packed.end()) == text);
        CHECK(first.size() == 10 && std::string(first.begin(), first.end()) == text.substr(0, 10));
        CHECK(archive->Find("ui_levelfullscreen/mainmenu.png") == size_t(2) && !archive->Find("nothing"));
        CHECK(!art::alchemy::IgaArchive::Open(reader, 0x20));

        auto file = art::alchemy::IgzFile::Parse(archive->Read(2));
        CHECK(file.has_value());
        if (!file)
            return;
        std::vector<art::alchemy::IgzFile::Picture> pictures = file->Pictures();
        CHECK(pictures.size() == 1);
        if (pictures.size() != 1)
            return;
        CHECK(pictures[0].name == "levels/test/portraits/eruptor.png" && pictures[0].width == 8 &&
              pictures[0].height == 8);
        CHECK(pictures[0].format == 0xB9BCDC57);
        art::Image image;
        CHECK(art::alchemy::DecodePicture(*file, pictures[0], image));
        // Stored red on top: the right way up, blue is on top.
        CHECK(image.rgba.size() == 64 && image.rgba[0] == 0xFFFF0000 && image.rgba[63] == 0xFF0000FF);

        // An unknown format hash is refused.
        auto unknown = art::alchemy::IgzFile::Parse(MakeIgz(0x12345678));
        CHECK(unknown && unknown->Pictures().size() == 1 &&
              !art::alchemy::DecodePicture(*unknown, unknown->Pictures()[0], image));
        CHECK(!art::alchemy::IgzFile::Parse(std::vector<uint8_t>(0x900, 0)));
        CHECK(archive->Version() == 8 && archive->Verified() && file->Version() == 6 && file->Verified());

        // Every version of the reader's tables reads its own fixture: IGA v8 [V], v10 [V], v11; IGZ v6 [V], v7, v8, v9.
        // The [?] versions only check that the reader follows its table: the table itself is from public descriptions.
        auto versions = art::alchemy::IgaVersions();
        CHECK(versions.size() == 3 && versions[0].version == 8 && versions[0].verified && versions[1].version == 10 &&
              versions[1].verified && !versions[2].verified);
        for (const art::alchemy::FormatVersion& igaVersion : versions)
            for (const art::alchemy::FormatVersion& igzVersion : art::alchemy::IgzVersions())
            {
                std::vector<uint8_t> other = MakeIga(MakeIgz(0xB9BCDC57, igzVersion.version), igaVersion.version);
                auto read = [&other](uint64_t offset, size_t size, uint8_t* to) {
                    if (offset + size > other.size())
                        return false;
                    memcpy(to, other.data() + offset, size);
                    return true;
                };
                auto a = art::alchemy::IgaArchive::Open(read, other.size());
                CHECK(a && a->Version() == igaVersion.version && a->Verified() == igaVersion.verified);
                if (!a)
                    continue;
                std::vector<uint8_t> unpacked = a->Read(1);
                CHECK(a->Name(2).ends_with(".igz") && std::string(unpacked.begin(), unpacked.end()) == text);
                auto z = art::alchemy::IgzFile::Parse(a->Read(2));
                CHECK(z && z->Version() == igzVersion.version && z->Verified() == igzVersion.verified);
                art::Image decoded;
                CHECK(z && z->Pictures().size() == 1 && art::alchemy::DecodePicture(*z, z->Pictures()[0], decoded) &&
                      decoded.rgba[0] == 0xFFFF0000);
            }
        CHECK(art::alchemy::IgzVersions().size() == 4);
        std::vector<uint8_t> v12 = MakeIgz(0xB9BCDC57, 12);
        CHECK(!art::alchemy::IgzFile::Parse(v12)); // a version the table lacks is refused, not guessed

        // Swap Force's 32-bit pictures (250C4EF0: the title's logo): A8R8G8B8, tiled, 8-in-32.
        art::Texture argb;
        CHECK(art::alchemy::TextureFormatOf(0x250C4EF0, argb) && argb.format == art::TextureFormat::Argb8 &&
              argb.tiled && argb.endian == art::TextureEndian::In32);

        // IGZ v7 [V]: the picture's name from the strings, the material's dependency, and Swap Force's linear DXT5
        // (049D7FAA), its rows 32 blocks apart.
        auto v7 = art::alchemy::IgzFile::Parse(MakeIgz(0x049D7FAA, 7, true));
        CHECK(v7 && v7->Verified() && v7->Pictures().size() == 1);
        if (v7 && v7->Pictures().size() == 1)
        {
            art::Image decoded;
            CHECK(v7->Pictures()[0].name == "levels/test/portraits/eruptor.png" &&
                  v7->Pictures()[0].format == 0x049D7FAA);
            CHECK(art::alchemy::DecodePicture(*v7, v7->Pictures()[0], decoded) && decoded.rgba[0] == 0xFFFF0000 &&
                  decoded.rgba[63] == 0xFF0000FF);
            CHECK(v7->Dependencies() == std::vector<std::string>{ "textures:/JuiceStandard_Test_10.igz" });
        }

        // The games' manifests (games/<id>/art.json, built in): a game whose manifest exists is known.
        CHECK(art::Known("ssa") && art::Known("giants") && art::Known("swapforce") && !art::Known("trap_team"));
    }

    // --- Lux -------------------------------------------------------------------------------------------------------

    void TestLux()
    {
        // Fixed-Huffman zlib stream (Python zlib.compress, level 1) and a hand-made stored one.
        static const uint8_t Fixed[] = { 0x78, 0x01, 0x0B, 0xC9, 0xAF, 0x54, 0x70, 0xCA, 0xAF, 0xB0, 0x52, 0x70, 0xCF,
            0xCC, 0xC9, 0x51, 0x70, 0x2F, 0x2A, 0xCD, 0x2B, 0xD1, 0x51, 0x08, 0x2E, 0xA8, 0x2C, 0xCA, 0xD7, 0x51, 0x08,
            0x29, 0xCA, 0x4C, 0x4F, 0x4F, 0x2D, 0x52, 0xF0, 0x48, 0x2C, 0x28, 0xA8, 0xD4, 0x51, 0x70, 0x4A, 0x2C, 0xCE,
            0xD0, 0x51, 0x70, 0xCE, 0xC8, 0x2F, 0x00, 0x13, 0x7A, 0x23, 0x5D, 0x0B, 0x00, 0xEA, 0xC1, 0x68, 0xC2 };
        std::string expected = "Toy Box: ";
        for (int i = 0; i < 6; i++)
            expected += "Gill Grunt, Spyro, Trigger Happy, Bash, Chop Chop. ";
        std::vector<uint8_t> out;
        CHECK(art::lux::Inflate(Fixed, sizeof(Fixed), out) && std::string(out.begin(), out.end()) == expected);
        const uint8_t Stored[] = { 0x78, 0x01, 0x01, 0x03, 0x00, 0xFC, 0xFF, 'S', 'k', 'y' };
        out.clear();
        CHECK(art::lux::Inflate(Stored, sizeof(Stored), out) && std::string(out.begin(), out.end()) == "Sky");
        out.clear();
        CHECK(!art::lux::Inflate(Fixed, 20, out)); // cut short
        const uint8_t NotZlib[] = { 0x1F, 0x8B, 0x08, 0x00 };
        CHECK(!art::lux::Inflate(NotZlib, sizeof(NotZlib), out));

        // An unpacked UI image stream with one record: an APKF package holding an 8x4 DXT5 picture, tiled, its
        // fetch constant after the BaseFlush/MipFlush words, its pixels where the SYHP chunk says.
        std::vector<uint8_t> body(0x400, 0);
        memcpy(body.data(), "APKF", 4);
        memcpy(&body[0x20], "\0XET", 4);
        memcpy(&body[0x20 + 0x30], "toy_test_hud", 12);
        const uint8_t Flushes[8] = { 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
        memcpy(&body[0x80], Flushes, 8);
        PutBe32(body, 0x88, 0x80000002); // tiled, texture
        PutBe32(body, 0x8C, 20 | 1 << 6); // DXT4_5, 8-in-16
        PutBe32(body, 0x90, (8 - 1) | (4 - 1) << 13); // 8 x 4
        PutBe32(body, 0x9C, 1 << 9); // 2D
        memcpy(&body[0xC0], "SYHP", 4);
        PutBe32(body, 0xC8, 0x100), PutBe32(body, 0xD0, 0x200); // pixels at +0x100, 0x200 bytes
        Dxt5Block(&body[0x100 + art::TiledOffset(0, 0, 32, 4)], To565(255, 255, 0), 128);
        Dxt5Block(&body[0x100 + art::TiledOffset(1, 0, 32, 4)], To565(0, 255, 255), 255);
        std::vector<uint8_t> stream(8, 0);
        std::vector<uint8_t> head(16, 0);
        PutBe32(head, 12, uint32_t(body.size()));
        stream.insert(stream.end(), head.begin(), head.end());
        stream.insert(stream.end(), body.begin(), body.end());
        int found = 0;
        art::lux::DecodeImages(
            stream, [](const std::string& name) { return name == "toy_test_hud"; },
            [&](const std::string& name, const art::Image& image) {
                found++;
                CHECK(image.width == 8 && image.height == 4 && image.rgba[0] == 0x8000FFFFu);
                CHECK(image.rgba[4] == 0xFFFFFF00);
            });
        CHECK(found == 1);
        art::lux::DecodeImages(
            stream, [](const std::string&) { return false; }, [&](const std::string&, const art::Image&) { found++; });
        CHECK(found == 1);
        // A strm file around a stored zlib block of the same stream.
        std::vector<uint8_t> strm = { 's', 't', 'r', 'm' };
        strm.resize(0x14, 0);
        strm.push_back(0x78), strm.push_back(0x01), strm.push_back(0x01);
        uint16_t length = uint16_t(stream.size());
        strm.push_back(uint8_t(length)), strm.push_back(uint8_t(length >> 8)), strm.push_back(uint8_t(~length)),
            strm.push_back(uint8_t(~length >> 8));
        strm.insert(strm.end(), stream.begin(), stream.end());
        std::vector<uint8_t> unpacked;
        CHECK(art::lux::UnpackStream(strm, unpacked) && unpacked == stream);
    }

    // --- The rule: the viewed game's picture, else the other's ------------------------------------------------------

    void TestPortraitRule(const std::filesystem::path& scratch)
    {
        std::filesystem::path folder = scratch / "rule";
        std::filesystem::create_directories(folder);
        SetVariable("SKY_PORTRAITS", datadir::Text(folder));
        collection::Image square{ 4, 4, std::vector<uint32_t>(16, 0xFF336699) };
        for (const char* name : { "toy_eruptor_hud", "eruptor", "eruptor_lightcore", "eagle" })
            CHECK(collection::SavePng(square, datadir::Text(folder / (std::string(name) + ".png"))));
        const collection::Catalogue& catalogue = collection::Catalogue::Get();
        const collection::Toy* eruptor = catalogue.Find(9);
        const collection::Toy* jetVac = catalogue.Find(100);
        CHECK(eruptor != nullptr && jetVac != nullptr);
        if (eruptor == nullptr || jetVac == nullptr)
            return;
        auto file = [](const std::string& path) { return std::filesystem::path(path).filename().string(); };
        CHECK(file(collection::PortraitPath("giants", *eruptor)) == "eruptor.png");
        CHECK(file(collection::PortraitPath("ssa", *eruptor)) == "toy_eruptor_hud.png");
        CHECK(file(collection::PortraitPath("giants", *eruptor, 0x1206)) == "eruptor_lightcore.png");
        CHECK(
            file(collection::PortraitPath("ssa", *eruptor, 0x1206)) == "toy_eruptor_hud.png"); // the viewed game first
        CHECK(file(collection::PortraitPath("ssa", *jetVac)) == "eagle.png"); // else the other's
        bool fromDisc = false;
        collection::Image lightcore =
            collection::Portrait("giants", eruptor, collection::VariantKind::LightCore, 8, 0, &fromDisc);
        CHECK(fromDisc && lightcore.width == 8);
        // A toy no game has a picture of takes its family's picture (SSA's magic item icon), a toy's own first.
        const collection::Toy* anvil = catalogue.Find(200);
        const collection::Toy* peak = catalogue.Find(300);
        CHECK(anvil != nullptr && peak != nullptr);
        if (anvil != nullptr && peak != nullptr)
        {
            CHECK(collection::PortraitPath("ssa", *anvil).empty() && collection::PortraitPath("giants", *peak).empty());
            for (const char* name : { "elementiconmagicitem", "elementiconlocation" })
                CHECK(collection::SavePng(square, datadir::Text(folder / (std::string(name) + ".png"))));
            CHECK(file(collection::PortraitPath("ssa", *anvil)) == "elementiconmagicitem.png");
            CHECK(file(collection::PortraitPath("giants", *peak)) ==
                  "elementiconlocation.png"); // another game's family picture
            CHECK(collection::GameKindPicturePath("giants", collection::Category::Adventure).empty());
            CHECK(collection::SavePng(square, datadir::Text(folder / "magicitem_anvilrain.png")));
            CHECK(file(collection::PortraitPath("ssa", *anvil)) ==
                  "magicitem_anvilrain.png"); // the toy's own, from Giants
            collection::Portrait("ssa", peak, collection::VariantKind::Base, 8, 0, &fromDisc);
            CHECK(fromDisc);
            for (const char* name : { "elementiconmagicitem", "elementiconlocation", "magicitem_anvilrain" })
                std::filesystem::remove(folder / (std::string(name) + ".png"));
        }
        std::filesystem::remove(folder / "eagle.png");
        CHECK(collection::PortraitPath("ssa", *jetVac).empty());
        collection::Portrait("ssa", jetVac, collection::VariantKind::Base, 8, 0, &fromDisc);
        CHECK(!fromDisc); // drawn
        // Every portrait the catalogue gives Giants names a toy and variant it has.
        const collection::Game* giants = catalogue.FindGame("giants");
        CHECK(giants != nullptr && giants->portraits.size() >= 90);
        bool named = giants != nullptr;
        for (const auto& [key, name] : giants != nullptr ? giants->portraits : std::map<uint32_t, std::string>{})
        {
            const collection::Toy* toy = catalogue.Find(uint16_t(key >> 16));
            bool variant = (key & 0xFFFF) == 0;
            for (const collection::Variant& v : toy != nullptr ? toy->variants : std::vector<collection::Variant>{})
                variant = variant || v.id == (key & 0xFFFF);
            named = named && toy != nullptr && variant && !name.empty();
        }
        CHECK(named);
        SDL_unsetenv_unsafe("SKY_PORTRAITS");
    }

    // --- The discs (optional) --------------------------------------------------------------------------------------

    // Giants' logo is composed from the title's sprite sheet: the arch, "GIANTS", the 8 element medallions and the TM, on
    // the dark blue plate (games/giants/docs/art.md). Layout checked against the press logo, by proportions.
    void TestGiantsLogo(const std::string& path)
    {
        SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
        CHECK(loaded != nullptr);
        if (loaded == nullptr)
            return;
        SDL_Surface* logo = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(loaded);
        const int w = logo->w, h = logo->h;
        auto pixel = [&](int x, int y) {
            return reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(logo->pixels) + size_t(y) * logo->pitch)[x];
        };
        // Sizes: the press logo is 810x411 (1.97); the sheet's pieces give about 1020x540.
        CHECK(w >= 1000 && h >= 500);
        CHECK(double(w) / h > 1.85 && double(w) / h < 2.1);
        long opaque = 0, plate = 0, icy = 0, bandTotal = 0, bandPlate = 0, tail = 0;
        int icyLeft = w, icyRight = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                uint32_t p = pixel(x, y);
                int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF, a = p >> 24;
                bool dark = a > 200 && b > r * 2 + 30 && r + g + b < 330; // the plate's blue
                opaque += a > 16, plate += dark;
                if (y >= h * 0.78 && y < h * 0.96 && x > w * 0.05 && x < w * 0.92)
                {
                    bandTotal++, bandPlate += dark;
                    if (a > 200 && b > 170 && b > r + 30) // the medallions' icy blue
                        icy++, icyLeft = std::min(icyLeft, x), icyRight = std::max(icyRight, x);
                }
                tail += x >= w * 0.97 && y >= h * 0.7 && a > 200;
            }
        CHECK(double(opaque) / (w * h) > 0.75 && double(opaque) / (w * h) < 0.9); // press logo: 0.84
        CHECK(double(plate) / (w * h) > 0.15); // the plate, behind the pieces
        CHECK(double(bandPlate) / bandTotal > 0.1); // the plate between and around the medallions
        CHECK(double(icy) / bandTotal > 0.2); // the medallion row
        CHECK(icyLeft < w * 0.15 && icyRight > w * 0.85); // 8 medallions across ~80% of the width
        CHECK(tail > 20); // the TM, past the plate
        CHECK((pixel(0, 0) >> 24) == 0 && (pixel(w - 1, 0) >> 24) == 0 &&
              (pixel(0, h - 1) >> 24) == 0); // transparent corners
        // Under the medallions the plate reaches the bottom edge's margin: an opaque dark blue texel in the last 4% of rows.
        bool footer = false;
        for (int x = int(w * 0.3); x < w * 0.7 && !footer; x++)
            for (int y = int(h * 0.97); y < h; y++)
            {
                uint32_t p = pixel(x, y);
                footer |= (p >> 24) > 128 && ((p >> 16) & 0xFF) > (p & 0xFF);
            }
        CHECK(footer);
        printf("  giants logo: plate %.2f, medallions %.2f, TM %ld texels\n", double(plate) / (w * h),
            double(icy) / bandTotal, tail);
        SDL_DestroySurface(logo);
    }

    void TestDisc(const char* game, const char* iso, int minimumPortraits)
    {
        DiscFileSystem disc;
        CHECK(disc.Open(iso));
        uint64_t start = SDL_GetTicksNS();
        art::Extraction got = art::Extract(game, art::ReadFrom(disc), { 0x89, 'P', 'N', 'G' });
        double first = (SDL_GetTicksNS() - start) / 1e9;
        start = SDL_GetTicksNS();
        art::Extraction again = art::Extract(game, art::ReadFrom(disc));
        double cached = (SDL_GetTicksNS() - start) / 1e9;
        printf("%s: logo %d, background %d, title icon %d, %d portraits in %s (%.2f s; cached: %.2f s)\n", game,
            got.logo, got.background, got.titleIcon, got.portraits, datadir::Text(art::PortraitFolder(game)).c_str(),
            first, cached);
        CHECK(got.logo && got.background && got.titleIcon && got.portraits >= minimumPortraits);
        CHECK(again.portraits == got.portraits && again.logo && cached < first);
        auto size = [](const std::string& path, int& w, int& h) {
            SDL_Surface* surface = path.empty() ? nullptr : SDL_LoadPNG(path.c_str());
            w = surface != nullptr ? surface->w : 0, h = surface != nullptr ? surface->h : 0;
            SDL_DestroySurface(surface);
        };
        int w, h;
        size(art::CachedPath(game, art::Role::Logo), w, h);
        printf("  logo %dx%d", w, h);
        CHECK(w >= 512 && h >= 200 && w > h);
        // [SwapForce] No picture of the logo on the disc is larger than 872x400: the title screen's lossless 32-bit one
        // (juicestandard_title_00) is the sharpest, the DXT5 copy of it (title_10) the same size.
        CHECK(strcmp(game, "swapforce") != 0 || (w == 872 && h == 400));
        size(art::CachedPath(game, art::Role::Background), w, h);
        printf(", background %dx%d\n", w, h);
        CHECK(w >= 512 && h >= 512);
        if (strcmp(game, "giants") == 0)
            TestGiantsLogo(art::CachedPath(game, art::Role::Logo));
        // Every toy the game accepts that the catalogue gives a picture has one now.
        int missing = 0;
        const collection::Catalogue& catalogue = collection::Catalogue::Get();
        for (const collection::Toy* toy : catalogue.Accepted(game))
        {
            const collection::Game* entry = catalogue.FindGame(game);
            bool listed = entry->names.contains(toy->type) || entry->images.contains(toy->type) ||
                          entry->portraits.contains(uint32_t(toy->type) << 16);
            if (listed && collection::GamePortraitPath(game, *toy).empty())
                missing++, fprintf(stderr, "  no picture for %s\n", toy->name.c_str());
        }
        CHECK(missing == 0);
        // With this disc alone, every toy the game reads has a picture: its own, else its family's (SSA's magic items
        // and adventure packs it draws no picture of).
        int drawn = 0;
        for (const collection::Toy* toy : catalogue.Accepted(game))
        {
            bool own = !collection::GamePortraitPath(game, *toy).empty();
            if (!own && collection::GameKindPicturePath(game, toy->category).empty())
                drawn++, fprintf(stderr, "  only a drawn icon for %s\n", toy->name.c_str());
        }
        CHECK(drawn == 0);

        // Every picture of a variant the catalogue maps (portraits, artworks) is on the disc; the best picture of each
        // toy and variant is its own artwork when the game has one (Giants: 99, about 1000x960).
        const collection::Game* entry = catalogue.FindGame(game);
        int ownPortraits = 0, ownArtworks = 0, absent = 0;
        for (const auto* pictures : { &entry->portraits, &entry->artworks, &entry->halves })
            for (const auto& [key, name] : *pictures)
            {
                bool art = pictures != &entry->portraits; // the artworks folder holds the halves too
                std::filesystem::path path =
                    (art ? art::ArtworkFolder(game) : art::PortraitFolder(game)) / (name + ".png");
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error))
                    absent++, fprintf(stderr, "  missing %s\n", datadir::Text(path).c_str());
                (art ? ownArtworks : ownPortraits) += (key & 0xFFFF) != 0 && pictures != &entry->halves;
            }
        CHECK(absent == 0);
        printf("  %d artworks; variants with their own portrait %d, with their own artwork %d\n", got.artworks,
            ownPortraits, ownArtworks);
        if (strcmp(game, "swapforce") == 0)
        {
            // [SwapForce] 179 face-off portraits (640x320, one per Skylander and variant; 175 named by the toy data) and the
            // swap screen's pictures of the swappers' halves (50 named by the catalogue's "halves", and the belt picture that
            // the game names "altdecocowboytop", not mapped); Series 2 Zook's is its own.
            CHECK(got.artworks == 179 + 51 && again.artworks == 179 + 51);
            CHECK(entry->halves.size() == 50);
            // Every swapper half has a picture (the plain toy's, and each variant's own), cut out on transparency, to be stacked.
            int halves = 0;
            for (const collection::Toy* toy : catalogue.Accepted(game))
                if (toy->category == collection::Category::SwapTop || toy->category == collection::Category::SwapBottom)
                {
                    std::string path = collection::HalfPicturePath(*toy, 0);
                    size(path, w, h);
                    CHECK(!path.empty() && w >= 256 && h >= 256);
                    halves += !path.empty();
                }
            CHECK(halves == 32);
            // Boom Jet's two halves stack: top above bottom at the wider's width; Dark Blast Zone's own pictures.
            const collection::Toy* boomTop = catalogue.Find(2000);
            const collection::Toy* boomBottom = catalogue.Find(1000);
            collection::Image boom = boomTop != nullptr && boomBottom != nullptr
                                         ? collection::SwapperPicture(*boomTop, 0x2000, *boomBottom, 0x2000)
                                         : collection::Image{};
            CHECK(!boom.Empty() && boom.width > 200 && boom.height > boom.width / 2 && boom.height < boom.width * 2);
            // SWAPPER_PICTURES=<folder> writes every matched swapper's picture, the variants and a few mixed ones, to look at.
            if (const char* sheet = getenv("SWAPPER_PICTURES"); sheet != nullptr && *sheet)
            {
                std::filesystem::create_directories(sheet);
                auto write = [&](uint16_t top, uint16_t topVariant, uint16_t bottom, uint16_t bottomVariant,
                                 const std::string& name) {
                    collection::Image image = collection::SwapperPicture(
                        *catalogue.Find(top), topVariant, *catalogue.Find(bottom), bottomVariant);
                    CHECK(!image.Empty() && collection::SavePng(image, std::string(sheet) + "/" + name + ".png"));
                };
                for (uint16_t i = 0; i < 16; i++)
                    write(2000 + i, 0, 1000 + i, 0,
                        "matched-" + std::to_string(100 + i).substr(1) + "-" + catalogue.Find(2000 + i)->name);
                for (uint16_t i = 0; i < 16; i++)
                    for (const collection::Variant& variant : catalogue.Find(2000 + i)->variants)
                        if (variant.id != 0 && variant.id != 0x2000)
                            write(2000 + i, variant.id, 1000 + i, variant.id,
                                "variant-" + std::to_string(100 + i).substr(1) + "-" + std::to_string(variant.id));
                for (auto [top, bottom] : { std::pair{ 2012, 1010 }, std::pair{ 2000, 1015 }, std::pair{ 2007, 1002 },
                         std::pair{ 2004, 1001 }, std::pair{ 2010, 1006 } })
                    write(uint16_t(top), 0, uint16_t(bottom), 0,
                        "mixed-" + std::to_string(top) + "-" + std::to_string(bottom));
            }
            CHECK(collection::HalfPicturePath(*catalogue.Find(2004), 0x2402)
                    .ends_with("artworks/darkblasttop_wii_material.png"));
            CHECK(collection::HalfPicturePath(*catalogue.Find(1013), 0x2402)
                    .ends_with("artworks/altdecocowboylegs_wii_material.png"));
            CHECK(collection::HalfPicturePath(*catalogue.Find(2013), 0x2402)
                    .ends_with("artworks/altdecocowboybelt_wii_material.png"));
            const collection::Toy* zook = catalogue.Find(25);
            std::string best = zook != nullptr ? collection::BestPicturePath(game, *zook, 0x1801) : std::string();
            size(best, w, h);
            CHECK(best.ends_with("artworks/zooks2_fullscreen_materials.png") && w == 640 && h == 320);
            return; // no upgrades manifest for Swap Force yet
        }
        // The upgrades (game_text.cpp): every Skylander's list, read once; Spyro's as both games show it [V] (runs of
        // 2026-10-26: the figure with bits 2 and 6 and 9 shows Long Range Raze, the second path's first upgrade and the
        // Soul Gem's bought).
        CHECK(got.upgrades && again.upgrades);
        const collection::Toy* spyro = catalogue.Find(16);
        collection::UpgradeTable spyroUp = collection::Upgrades(*spyro, game, "en"),
                                 spyroFr = collection::Upgrades(*spyro, game, "fr");
        bool giants = strcmp(game, "giants") == 0;
        CHECK(spyroUp.fromDisc && spyroUp.upgrades.size() == (giants ? 12u : 11u));
        if (spyroUp.upgrades.size() >= 11)
        {
            const auto& u = spyroUp.upgrades;
            CHECK(u[0].bit == 2 && u[0].price == 500 && u[0].name == (giants ? "Long Range Raze" : "Longe Range Raze"));
            CHECK(u[7].bit == 6 && u[7].path == collection::UpgradePath::Second && u[7].name == "Stun Charge" &&
                  u[7].price == 1700);
            CHECK(u[10].bit == 9 && u[10].path == collection::UpgradePath::SoulGem &&
                  u[10].name == "Spyro's Earth Pound" && u[10].price == 4000);
            CHECK(spyroUp.pathNames[0] == "Sheep Burner Spyro" && spyroUp.pathNames[1] == "Blitz Spyro");
            CHECK(spyroFr.upgrades.size() == u.size() && spyroFr.upgrades[7].name == "Charge étourdissante");
        }
        int listed = 0;
        for (const collection::Toy* toy : catalogue.Accepted(game))
            listed += (toy->category == collection::Category::Core || toy->category == collection::Category::Giant) &&
                      collection::Upgrades(*toy, game).fromDisc;
        printf("  upgrades of %d Skylanders\n", listed);
        CHECK(listed >= (giants ? 53 : 37));
        if (giants)
        {
            CHECK(got.artworks == 99 && again.artworks == 99);
            const collection::Toy* eruptor = catalogue.Find(9);
            std::string best = eruptor != nullptr ? collection::BestPicturePath(game, *eruptor, 0x1801) : std::string();
            size(best, w, h);
            CHECK(best.ends_with("artworks/eruptorseries2.png") && h == 960 && w > 960);
            const collection::Toy* chill = catalogue.Find(106);
            CHECK(chill != nullptr &&
                  collection::BestPicturePath(game, *chill, 0x7777).ends_with("artworks/icevalkyrie.png"));
        }
    }
}

void TestIco()
{
    // A 64x64 PNG header (signature, IHDR chunk) is all MakeIco reads.
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0,
        64, 0, 0, 0, 64, 8, 6, 0, 0, 0 };
    png.resize(40, 0);
    std::vector<uint8_t> ico = MakeIco(png);
    CHECK(ico.size() == 22 + png.size());
    CHECK(ico[2] == 1 && ico[4] == 1); // an icon, one image
    CHECK(ico[6] == 64 && ico[7] == 64); // its size
    CHECK(ico[14] == png.size() && ico[18] == 22); // its length and offset
    CHECK(std::equal(png.begin(), png.end(), ico.begin() + 22));
    png[19] = 0; // 256 is stored as 0
    png[18] = 1;
    png[23] = 0;
    png[22] = 1;
    CHECK(MakeIco(png)[6] == 0);
    png[22] = 2; // not square
    CHECK(MakeIco(png).empty());
    CHECK(MakeIco({ 1, 2, 3 }).empty());
}

// Which languages a disc holds, from file names typed by hand (no game data).
void TestDiscLanguages()
{
    using V = std::vector<std::string>;
    // Spyro's Adventure: text folders and voice banks, per language; other folders (Japanese) and stray files are ignored.
    V lux = { "/audio/eng/voice.bnk", "/audio/fre/voice.bnk", "/audio/spa/voice.bnk", "/audio/levels/a.bnk",
        "/text/eng/global.lxb", "/text/fre/global.lxb", "/Text/GER/global.lxb", "/text/jap/global.lxb", "/text/dan",
        "/movies/eng/legal.bik" };
    art::DiscLanguages ssa = art::LanguagesOnDisc("ssa", lux);
    CHECK((ssa.text == V{ "en", "fr", "de" }));
    CHECK((ssa.voice == V{ "en", "fr", "es" })); // text and voices are decided apart
    CHECK((art::LanguagesOnDisc("ssa", { "\\audio\\ita\\voice.bnk" }).voice == V{ "it" }));
    CHECK(art::LanguagesOnDisc("ssa", { "/default.xex" }).text.empty()); // no marker: unknown

    // Swap Force: archives/<name>_<code>.pak.
    V us = { "/archives/boss_kaos_bank_lv_en.pak", "/archives/boss_kaos_bank_lv_fr.pak", "/archives/ui_en.pak",
        "/archives/shared.pak", "/archives/hat_staging.pak", "/default.xex" };
    CHECK((art::LanguagesOnDisc("swapforce", us).text == V{ "en", "fr" }));
    CHECK((art::LanguagesOnDisc("swapforce", us).voice == V{ "en", "fr" }));
    V europe = { "/archives/a_it.pak", "/archives/a_es.pak", "/archives/b_es.pak", "/other/c_en.pak" };
    CHECK((art::LanguagesOnDisc("swapforce", europe).text == V{ "es", "it" }));
    CHECK(art::LanguagesOnDisc("swapforce", { "/archives/a_xx.pak", "/archives/a.pak" }).voice.empty());

    // Giants: not in file names; all twelve whatever the list.
    art::DiscLanguages giants = art::LanguagesOnDisc("giants", {});
    CHECK(giants.text.size() == 12 && giants.text == giants.voice && giants.text[4] == "es-419");
    CHECK(art::LanguagesOnDisc("trapteam", us).text.empty());

    CHECK(art::JoinLanguages({ "en", "fr" }) == "en,fr" && art::JoinLanguages({}).empty());
    CHECK((art::SplitLanguages("en,,fr,") == V{ "en", "fr" }) && art::SplitLanguages("").empty());
}

int main(int argc, char** argv)
{
    bool keep = argc > 1 && strcmp(argv[1], "--keep") == 0;
    TestTiling();
    TestDxt();
    TestArgbAndL8();
    TestAlchemy();
    TestLux();
    TestIco();
    TestDiscLanguages();

    // Scratch data folder: SKYLANDERS_DATA if set (the harness sets one), else a temporary one.
    std::filesystem::path scratch;
    bool temporary = false;
    if (const char* data = getenv("SKYLANDERS_DATA"))
        scratch = datadir::FromText(data);
    else
    {
        temporary = true;
        scratch = std::filesystem::temp_directory_path() / "SSAArtTest";
        SetVariable("SKYLANDERS_DATA", datadir::Text(scratch));
    }
    std::filesystem::create_directories(scratch);
    TestPortraitRule(scratch);
    if (const char* iso = getenv("SSA_ISO"); iso != nullptr && *iso)
        TestDisc("ssa", iso, 70);
    if (const char* iso = getenv("GIANTS_ISO"); iso != nullptr && *iso)
        TestDisc("giants", iso, 123);
    if (const char* iso = getenv("SWAPFORCE_ISO"); iso != nullptr && *iso)
        TestDisc("swapforce", iso, 208);
    if (keep)
        printf("kept in %s\n", datadir::Text(scratch).c_str());
    else if (temporary)
        std::filesystem::remove_all(scratch);
    printf("art test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
