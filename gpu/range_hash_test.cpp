// Checks the buffer cache's range hash: it follows every byte of the range across frames, ignores the bytes around it,
// hashes a chunk shared by overlapping ranges once a frame, and drops its table where ranges do not overlap.
// usage: ArkeyanRangeHashTest
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu/range_hash.h"
#include <cstdio>
#include <vector>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    constexpr uint32_t Base = 0x40000000, Chunk = gpu::RangeHasher::ChunkBytes;
}

int main()
{
    std::vector<uint8_t> memory(64 * Chunk);
    for (size_t i = 0; i < memory.size(); i++)
        memory[i] = uint8_t(i * 131 + 7);
    auto at = [&](uint32_t offset) { return memory.data() + offset; };

    // Ranges of every alignment and size, short (hashed whole) and long (chunks): each byte inside changes the hash
    // in the next frame, a byte outside does not.
    uint64_t frame = 1;
    for (uint32_t offset : { 0u, 1u, 100u, Chunk - 1, Chunk, 3 * Chunk + 17 })
        for (uint32_t bytes : { 4u, 300u, Chunk, 4 * Chunk - 1, 4 * Chunk + 1, 7 * Chunk + 513, 20 * Chunk })
        {
            gpu::RangeHasher hasher;
            const uint64_t before = hasher.Hash(at(offset), Base + offset, bytes, frame++);
            CHECK(hasher.Hash(at(offset), Base + offset, bytes, frame++) == before);
            for (uint32_t inside : { offset, offset + bytes / 2, offset + bytes - 1 })
            {
                memory[inside] ^= 0x5A;
                CHECK(hasher.Hash(at(offset), Base + offset, bytes, frame++) != before);
                memory[inside] ^= 0x5A;
            }
            for (uint32_t outside : { offset - 1, offset + bytes })
            {
                if (outside >= memory.size())
                    continue;
                memory[outside] ^= 0x5A;
                CHECK(hasher.Hash(at(offset), Base + offset, bytes, frame++) == before);
                memory[outside] ^= 0x5A;
            }
            CHECK(hasher.Hash(at(offset), Base + offset, bytes, frame++) == before);
        }

    // Overlapping ranges of one buffer in one frame read each chunk once; the next frame reads them again.
    {
        gpu::RangeHasher hasher;
        for (uint32_t start = 0; start < 8; start++)
            hasher.Hash(at(start * Chunk + 10), Base + start * Chunk + 10, 40 * Chunk, 100);
        CHECK(hasher.hashedBytes == 54 * Chunk); // 46 chunks and 8 edges, not 8 x 40
        hasher.hashedBytes = 0;
        hasher.Hash(at(10), Base + 10, 40 * Chunk, 101);
        CHECK(hasher.hashedBytes == 40 * Chunk);
    }

    // Ranges that do not overlap turn the table off for 120 frames, with the same hash values.
    {
        gpu::RangeHasher cached, plain;
        for (uint32_t range = 0; range < 4; range++)
            plain.Hash(at(range * 10 * Chunk), Base + range * 10 * Chunk, 8 * Chunk, 1);
        plain.Hash(at(0), Base, 8 * Chunk, 2);
        CHECK(!plain.UsesTable());
        CHECK(plain.Hash(at(5), Base + 5, 30 * Chunk, 3) == cached.Hash(at(5), Base + 5, 30 * Chunk, 3));
        CHECK(cached.UsesTable());
        for (uint64_t later = 4; later < 2 + gpu::RangeHasher::FramesWithoutTable; later++)
            plain.Hash(at(0), Base, 8 * Chunk, later);
        CHECK(!plain.UsesTable());
        plain.Hash(at(0), Base, 8 * Chunk, 2 + gpu::RangeHasher::FramesWithoutTable);
        CHECK(plain.UsesTable());
    }

    if (g_failures == 0)
        printf("range hash test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
