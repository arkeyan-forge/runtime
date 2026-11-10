// Checks that 16-bit vertex component pairs come back in order. usage: ArkeyanVertexPairsTest
#include "d3d/vertex_pairs.h"
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

    uint16_t Half(const std::vector<uint8_t>& data, size_t at) { return uint16_t(data[at] | data[at + 1] << 8); }
}

int main()
{
    // Two 20-byte vertices: a SHORT4N position at 0 (x, y, z, w), a word at 8, a FLOAT16_2 at 12, a word at 16, as
    // the host has them after the 32-bit swap: each pair's halves exchanged.
    std::vector<uint8_t> data(40, 0);
    auto put = [&](size_t at, uint16_t value) { data[at] = uint8_t(value), data[at + 1] = uint8_t(value >> 8); };
    for (size_t v = 0; v < 2; v++)
    {
        size_t base = v * 20;
        put(base + 0, 0x2222), put(base + 2, 0x1111); // y, x
        put(base + 4, 0x7FFF), put(base + 6, 0x0000); // w, z
        put(base + 8, 0xAAAA), put(base + 10, 0xBBBB);
        put(base + 12, 0x4444), put(base + 14, 0x3333);
    }
    d3d::SwapHalfPairs(data.data(), data.size(), 20, 0, 8);
    for (size_t v = 0; v < 2; v++)
    {
        size_t base = v * 20;
        CHECK(Half(data, base) == 0x1111 && Half(data, base + 2) == 0x2222);
        CHECK(Half(data, base + 4) == 0x0000 && Half(data, base + 6) == 0x7FFF);
        CHECK(Half(data, base + 8) == 0xAAAA && Half(data, base + 12) == 0x4444); // outside the range: untouched
    }
    // A last vertex cut short by the stream's end is left alone; a zero stride does nothing.
    std::vector<uint8_t> shortData(6, 1);
    d3d::SwapHalfPairs(shortData.data(), shortData.size(), 20, 0, 8);
    d3d::SwapHalfPairs(shortData.data(), shortData.size(), 0, 0, 4);
    CHECK(shortData == std::vector<uint8_t>(6, 1));

    // SwapVertexWords in one pass equals the 32-bit swap then SwapHalfPairs, for each element the mask covers; a
    // last vertex cut short too (its whole words only).
    {
        std::vector<uint8_t> guest(2 * 20 + 10);
        for (size_t i = 0; i < guest.size(); i++)
            guest[i] = uint8_t(i * 7 + 3);
        std::vector<uint8_t> expected(guest.size());
        for (size_t i = 0; i + 4 <= guest.size(); i += 4)
            for (size_t b = 0; b < 4; b++)
                expected[i + b] = guest[i + 3 - b];
        expected[48] = guest[48], expected[49] = guest[49];
        d3d::SwapHalfPairs(expected.data(), expected.size(), 20, 0, 8);
        d3d::SwapHalfPairs(expected.data(), expected.size(), 20, 12, 4);
        // The cut vertex (bytes 40-49): its words 0 and 1 are whole, so SwapHalfPairs' all-or-nothing rule swaps them too.
        const uint64_t words = d3d::PairWords(0, 8) | d3d::PairWords(12, 4);
        CHECK(words == 0b1011);
        std::vector<uint8_t> converted(guest.size());
        d3d::SwapVertexWords(guest.data(), converted.data(), guest.size(), 20, words);
        CHECK(converted == expected);
        d3d::SwapVertexWords(guest.data(), converted.data(), guest.size(), 20, 0);
        CHECK(converted[0] == guest[3] && converted[2] == guest[1] && converted[48] == guest[48]);
    }
    CHECK(d3d::PairWords(2, 4) == 0 && d3d::PairWords(252, 8) == 0 && d3d::PairWords(248, 8) == 0xC0ull << 56);

    printf("vertex pairs test: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
