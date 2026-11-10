// The disc reader (vfs.cpp) on damaged and crafted images: a name past its directory, a directory that contains
// itself, a directory past the end of a truncated file, a directory size of 4 GB. Each opens or fails without reading
// out of bounds or recursing forever. Synthetic images in a scratch folder only: ./ArkeyanVfsTest <scratch dir>
#include "vfs.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0, g_failures = 0;
#define EXPECT(cond)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            g_failures++;                                                                                              \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                                                             \
        }                                                                                                              \
    } while (0)

    constexpr uint32_t Sector = 2048;

    void Put32(std::vector<uint8_t>& image, size_t at, uint32_t value) { memcpy(&image[at], &value, 4); }
    void Put16(std::vector<uint8_t>& image, size_t at, uint16_t value) { memcpy(&image[at], &value, 2); }

    // An image whose volume descriptor (sector 32) names a root directory of `rootSize` bytes at sector 34.
    std::vector<uint8_t> Image(uint32_t rootSize, size_t sectors = 40)
    {
        std::vector<uint8_t> image(sectors * Sector, 0);
        memcpy(&image[32 * Sector], "MICROSOFT*XBOX*MEDIA", 20);
        Put32(image, 32 * Sector + 20, 34);
        Put32(image, 32 * Sector + 24, rootSize);
        return image;
    }

    // One entry at `at` of the root: no subtrees, then sector, size, attributes and the name length.
    void Entry(
        std::vector<uint8_t>& image, size_t at, uint32_t sector, uint32_t size, uint8_t attributes, uint8_t nameLength)
    {
        const size_t base = 34 * Sector + at;
        Put16(image, base, 0);
        Put16(image, base + 2, 0);
        Put32(image, base + 4, sector);
        Put32(image, base + 8, size);
        image[base + 12] = attributes;
        image[base + 13] = nameLength;
        for (size_t i = 0; i < nameLength && base + 14 + i < image.size(); i++)
            image[base + 14 + i] = 'a';
    }

    bool Write(const std::filesystem::path& file, const std::vector<uint8_t>& image, size_t length)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image.data()), std::streamsize(length));
        return bool(out);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: ArkeyanVfsTest <scratch dir>\n");
        return 2;
    }
    const std::filesystem::path root = std::filesystem::path(argv[1]) / "vfs";
    std::filesystem::create_directories(root);

    // A name that runs past the end of its directory: the entry is skipped.
    {
        std::vector<uint8_t> image = Image(Sector);
        Entry(image, 0, 0, 0, 0, 1);
        Put16(image, 34 * Sector + 2, uint16_t((Sector - 16) / 4)); // its right subtree: the last 16 bytes
        Entry(image, Sector - 16, 0, 0, 0, 255);
        EXPECT(Write(root / "name.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(disc.Open((root / "name.iso").string().c_str()));
        EXPECT(disc.Find("a") != nullptr && disc.Files().size() == 1);
    }
    // A directory that contains itself: refused, not a stack overflow.
    {
        std::vector<uint8_t> image = Image(Sector);
        Entry(image, 0, 34, Sector, 0x10, 1);
        EXPECT(Write(root / "loop.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(!disc.Open((root / "loop.iso").string().c_str()));
        EXPECT(disc.Files().empty() && disc.Find("a") == nullptr);
    }
    // Many directory records that each point back at the root (an 80 KB image that once ran the launcher out of memory):
    // refused at once, the walk is bounded by the extents it has seen, not by the branching to the power of the depth.
    {
        std::vector<uint8_t> image = Image(Sector, 40);
        for (size_t i = 0; i < 40; i++)
        {
            Entry(image, i * 48, 34, Sector, 0x10, 40);
            Put16(image, 34 * Sector + i * 48 + 2, i + 1 < 40 ? uint16_t((i + 1) * 12) : 0); // the next record
        }
        EXPECT(Write(root / "fan.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(!disc.Open((root / "fan.iso").string().c_str()));
        EXPECT(disc.Files().empty());
    }
    // A directory chain deeper than any disc's: refused.
    {
        std::vector<uint8_t> image = Image(Sector, 140);
        Entry(image, 0, 35, Sector, 0x10, 1);
        for (size_t d = 0; d < 100; d++) // directory at sector 35 + d holds one directory, at the next sector
        {
            const size_t base = (35 + d) * Sector;
            Put32(image, base + 4, uint32_t(36 + d));
            Put32(image, base + 8, Sector);
            image[base + 12] = 0x10;
            image[base + 13] = 1;
            image[base + 14] = 'b';
        }
        EXPECT(Write(root / "deep.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(!disc.Open((root / "deep.iso").string().c_str()));
        EXPECT(disc.Files().empty());
    }
    // A directory bigger than the image holding it is skipped, whatever its size field says; the rest still opens.
    {
        std::vector<uint8_t> image = Image(Sector);
        Entry(image, 0, 36, 8u << 20, 0x10, 1);
        EXPECT(Write(root / "long.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(disc.Open((root / "long.iso").string().c_str()));
        EXPECT(disc.Find("a") != nullptr);
    }
    // The root past the end of a truncated file: nothing read from beyond it.
    {
        std::vector<uint8_t> image = Image(Sector);
        Entry(image, 0, 0, 0, 0, 1);
        EXPECT(Write(root / "short.iso", image, 33 * Sector + 100));
        DiscFileSystem disc;
        EXPECT(disc.Open((root / "short.iso").string().c_str()));
        EXPECT(disc.Find("a") == nullptr);
    }
    // A root of 4 GB: refused before any allocation.
    {
        std::vector<uint8_t> image = Image(0xFFFFFFFFu);
        EXPECT(Write(root / "huge.iso", image, image.size()));
        DiscFileSystem disc;
        EXPECT(disc.Open((root / "huge.iso").string().c_str()));
        EXPECT(disc.Files().empty());
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    printf("vfs test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
