// Test of the texture pack pieces (host/dds.h, host/texture_pack.h): the DDS writer and reader (BC1, BC3, RGBA8, mip
// chains, the DX10 header, bad files), the content hash's stability, and the dump and the replacement index on a scratch
// folder. No game data. Run with no arguments; exit code 0 = pass.
#include "host/dds.h"
#include "host/texture_pack.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace
{
    int g_checks;
    int g_failures;

    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; i++)
            data[i] = uint8_t(seed + i * 7);
        return data;
    }

    std::vector<uint8_t> WriteChain(dds::Format format, uint32_t width, uint32_t height, uint32_t levels,
        std::vector<std::vector<uint8_t>>& storage)
    {
        storage.clear();
        std::vector<std::span<const uint8_t>> spans;
        for (uint32_t level = 0; level < levels; level++)
            storage.push_back(
                Pattern(dds::SizeOfLevel(format, width, height, level).Bytes(), uint8_t(level * 31 + uint8_t(format))));
        for (const auto& level : storage)
            spans.push_back(level);
        return dds::Write(format, width, height, spans);
    }

    void RoundTrip(dds::Format format, uint32_t width, uint32_t height, uint32_t levels)
    {
        std::vector<std::vector<uint8_t>> storage;
        std::vector<uint8_t> file = WriteChain(format, width, height, levels, storage);
        dds::Info info;
        std::string error;
        CHECK(dds::Parse(file, file.size(), info, error));
        CHECK(info.format == format && info.width == width && info.height == height && info.levels == levels);
        CHECK(!info.swapRB && !info.opaque);
        for (uint32_t level = 0; level < levels; level++)
        {
            uint64_t offset = dds::LevelOffset(info, level);
            size_t size = storage[level].size();
            CHECK(offset + size <= file.size());
            CHECK(std::equal(storage[level].begin(), storage[level].end(), file.begin() + std::ptrdiff_t(offset)));
        }
        CHECK(dds::LevelOffset(info, levels) == file.size());
        // One byte short: truncated.
        CHECK(!dds::Parse(std::span<const uint8_t>(file).first(std::min(file.size(), dds::MaxHeaderBytes)),
            file.size() - 1, info, error));
    }

    void Put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) { memcpy(bytes.data() + offset, &value, 4); }

    void TestDds()
    {
        RoundTrip(dds::Format::BC1, 64, 64, 1);
        RoundTrip(dds::Format::BC1, 256, 128, 9);
        RoundTrip(dds::Format::BC2, 32, 32, 6);
        RoundTrip(dds::Format::BC3, 100, 60, 7); // sizes that are not multiples of 4
        RoundTrip(dds::Format::BC4, 16, 16, 5);
        RoundTrip(dds::Format::BC5, 64, 32, 7);
        RoundTrip(dds::Format::RGBA8, 37, 19, 6);
        RoundTrip(dds::Format::RGBA8, 1, 1, 1);

        // Level sizes: a 4x4 BC1 block is 8 bytes, the 1x1 level of a chain is one block.
        CHECK(dds::SizeOfLevel(dds::Format::BC1, 64, 64, 0).Bytes() == 16 * 16 * 8);
        CHECK(dds::SizeOfLevel(dds::Format::BC3, 64, 64, 6).Bytes() == 16);
        CHECK(dds::SizeOfLevel(dds::Format::RGBA8, 5, 3, 1).Bytes() == 2 * 1 * 4);
        CHECK(dds::FullChain(256, 128) == 9 && dds::FullChain(1, 1) == 1);

        std::vector<std::vector<uint8_t>> storage;
        dds::Info info;
        std::string error;

        // Too many levels, bad magic, a cube map, a short header.
        std::vector<uint8_t> file = WriteChain(dds::Format::RGBA8, 8, 8, 1, storage);
        Put32(file, 28, 9); // mip count (flag set below)
        Put32(file, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
        CHECK(!dds::Parse(file, file.size(), info, error));
        file = WriteChain(dds::Format::BC1, 8, 8, 1, storage);
        Put32(file, 0, 0);
        CHECK(!dds::Parse(file, file.size(), info, error) && !error.empty());
        file = WriteChain(dds::Format::BC1, 8, 8, 1, storage);
        Put32(file, 112, 0x200);
        CHECK(!dds::Parse(file, file.size(), info, error));
        file = WriteChain(dds::Format::BC1, 8, 8, 1, storage);
        CHECK(!dds::Parse(std::span<const uint8_t>(file).first(20), file.size(), info, error));
        // An unsupported FourCC.
        file = WriteChain(dds::Format::BC1, 8, 8, 1, storage);
        Put32(file, 84, 0x30315844 + 1);
        CHECK(!dds::Parse(file, file.size(), info, error));

        // A DX10 header (BC7 is refused, BC3 and BGRA8 are read).
        auto dx10 = [&](uint32_t dxgi, uint32_t bytes) {
            std::vector<uint8_t> out = WriteChain(dds::Format::BC1, 16, 16, 1, storage);
            out.resize(4 + 124 + 20 + bytes, 0x55);
            Put32(out, 80, 0x4);
            Put32(out, 84, 0x30315844); // "DX10"
            Put32(out, 128, dxgi);
            Put32(out, 132, 3); // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            Put32(out, 136, 0);
            Put32(out, 140, 1);
            Put32(out, 144, 0);
            return out;
        };
        file = dx10(77, 16 * 16);
        CHECK(dds::Parse(file, file.size(), info, error) && info.format == dds::Format::BC3 && info.dataOffset == 148);
        file = dx10(98, 16 * 16);
        CHECK(!dds::Parse(file, file.size(), info, error));
        file = dx10(87, 16 * 16 * 4);
        CHECK(dds::Parse(file, file.size(), info, error) && info.format == dds::Format::RGBA8 && info.swapRB);
        uint8_t texels[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        dds::ConvertTexels(info, texels, sizeof(texels));
        CHECK(texels[0] == 3 && texels[2] == 1 && texels[3] == 4 && texels[4] == 7 && texels[6] == 5);

        // 32-bit masks in BGRA order without an alpha mask: opaque.
        file = WriteChain(dds::Format::RGBA8, 4, 4, 1, storage);
        Put32(file, 80, 0x40);
        Put32(file, 92, 0xFF0000);
        Put32(file, 96, 0xFF00);
        Put32(file, 100, 0xFF);
        Put32(file, 104, 0);
        CHECK(dds::Parse(file, file.size(), info, error) && info.swapRB && info.opaque);
        uint8_t pixel[4] = { 1, 2, 3, 0 };
        dds::ConvertTexels(info, pixel, 4);
        CHECK(pixel[0] == 3 && pixel[1] == 2 && pixel[2] == 1 && pixel[3] == 255);
    }

    void TestHash()
    {
        std::vector<uint8_t> data = Pattern(4096, 3);
        uint64_t hash = texpack::ContentHash(18, 64, 64, data.data(), data.size());
        // Stable: the same content gives the same hash, and the value is fixed (a change would orphan every pack).
        CHECK(hash == texpack::ContentHash(18, 64, 64, data.data(), data.size()));
        CHECK(hash == 0x4e17940d54d51794ull); // pinned: a change would orphan every pack
        // Every ingredient counts.
        CHECK(hash != texpack::ContentHash(19, 64, 64, data.data(), data.size()));
        CHECK(hash != texpack::ContentHash(18, 32, 128, data.data(), data.size()));
        data[100] ^= 1;
        CHECK(hash != texpack::ContentHash(18, 64, 64, data.data(), data.size()));
        CHECK(texpack::HashName(0x1ull) == "0000000000000001");
        CHECK(texpack::HashName(0xABCDEF0123456789ull) == "abcdef0123456789");
    }

    void TestPackFolder(const std::filesystem::path& root)
    {
        std::filesystem::path dump = root / "textures-dump", packs = root / "textures";
        std::filesystem::create_directories(packs / "sub");

        // A replacement in a subfolder, a wrong name, a bad file, a file that is not a DDS.
        std::vector<std::vector<uint8_t>> storage;
        std::vector<uint8_t> good = WriteChain(dds::Format::BC3, 128, 64, 8, storage);
        auto save = [](const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
            std::ofstream(path, std::ios::binary)
                .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        };
        save(packs / "sub" / "00000000000000aa.dds", good);
        save(packs / "not-a-hash.dds", good);
        save(packs / "00000000000000bb.dds", std::vector<uint8_t>(good.begin(), good.begin() + 200));
        save(packs / "00000000000000cc.txt", good);

        // The generated pack (textures-hd): its own textures count, the player's same hash wins.
        std::filesystem::path generated = root / "textures-hd";
        std::filesystem::create_directories(generated);
        std::vector<std::vector<uint8_t>> small;
        save(generated / "00000000000000aa.dds", WriteChain(dds::Format::BC1, 64, 32, 7, small));
        save(generated / "00000000000000dd.dds", WriteChain(dds::Format::BC1, 64, 32, 7, small));

        // The HD textures alone (texture_packs off): the generated pack is read, the player's folder is not.
        texpack::Configure(dump, false, packs, false, generated);
        CHECK(texpack::Active() && !texpack::Dumping() && texpack::Files() == 2);
        CHECK(texpack::Find(0xaa) != nullptr && texpack::Find(0xaa)->info.width == 64);

        texpack::Configure(dump, true, packs, true, generated);
        CHECK(texpack::Active() && texpack::Dumping());
        CHECK(texpack::Find(0xdd) != nullptr && texpack::Find(0xdd)->info.width == 64);
        const texpack::Replacement* found = texpack::Find(0xaa);
        CHECK(found != nullptr && found->info.width == 128 && found->info.height == 64 && found->info.levels == 8);
        CHECK(texpack::Find(0xbb) == nullptr);
        CHECK(texpack::Find(0xcc) == nullptr);
        CHECK(texpack::Find(0x1234) == nullptr);
        for (uint32_t level = 0; found != nullptr && level < 8; level++)
        {
            std::vector<uint8_t> data;
            CHECK(texpack::ReadLevel(*found, level, data));
            CHECK(data == storage[level]);
        }

        // The dump: once per hash, the top level only, readable by the reader.
        std::vector<uint8_t> top = Pattern(dds::SizeOfLevel(dds::Format::BC1, 32, 32, 0).Bytes(), 9);
        CHECK(texpack::ShouldDump(0x77));
        CHECK(!texpack::ShouldDump(0x77));
        texpack::Dump(0x77, dds::Format::BC1, 32, 32, std::vector<uint8_t>(top));
        texpack::Flush();
        std::filesystem::path written = dump / "0000000000000077.dds";
        CHECK(std::filesystem::exists(written));
        std::ifstream in(written, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        dds::Info info;
        std::string error;
        CHECK(dds::Parse(file, file.size(), info, error) && info.format == dds::Format::BC1 && info.levels == 1);
        CHECK(file.size() == info.dataOffset + top.size() &&
              std::equal(top.begin(), top.end(), file.begin() + std::ptrdiff_t(info.dataOffset)));
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dump))
            files += entry.is_regular_file();
        CHECK(files == 1); // no .tmp left behind
    }
}

int main()
{
    TestDds();
    TestHash();
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("ssa-texpack-test-" + std::to_string(uintmax_t(time(nullptr))));
    TestPackFolder(root);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
