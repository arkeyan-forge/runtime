// Synthetic YUV buffers only; no movie, window or guest executable is needed.
#include "d3d/bink_frame.h"
#include <array>
#include <cstdio>
#include <limits>

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

    bool Black(const std::vector<uint32_t>& pixels)
    {
        return !pixels.empty() &&
               std::all_of(pixels.begin(), pixels.end(), [](uint32_t pixel) { return pixel == d3d::BinkBlack; });
    }
}

int main()
{
    std::array<uint8_t, 4> y{};
    std::array<uint8_t, 1> cr{}, cb{};
    std::vector<uint32_t> pixels(4, 0xFFFFFFFFu);
    auto convert = [&] { return d3d::ConvertBinkFrame(2, 2, { y, 2 }, { cr, 1 }, { cb, 1 }, pixels); };

    // The regression: an untouched allocation used to produce RGBA (0, 135, 0, 255).
    CHECK(convert() != nullptr);
    CHECK(Black(pixels));
    std::array<uint32_t, 4> header{ 2, 2, 100, 0 };
    if (convert() != nullptr)
        d3d::EndFailedBinkMovie(0, [&](uint32_t address, uint32_t value) { header.at(address / 4) = value; });
    CHECK(header[0] == 2 && header[1] == 2);
    CHECK(header[2] == 1 && header[3] == 1);
    // Retrying a failed or initially zero-frame movie is still terminal, without unsigned wraparound.
    header[2] = header[3] = std::numeric_limits<uint32_t>::max();
    d3d::EndFailedBinkMovie(0, [&](uint32_t address, uint32_t value) { header.at(address / 4) = value; });
    CHECK(header[2] == 1 && header[3] == 1);
    header[2] = header[3] = 0;
    d3d::EndFailedBinkMovie(0, [&](uint32_t address, uint32_t value) { header.at(address / 4) = value; });
    CHECK(header[2] == 1 && header[3] == 1);

    // A decoded black frame must continue playing.
    y.fill(16);
    cr.fill(128);
    cb.fill(128);
    CHECK(convert() == nullptr);
    CHECK(Black(pixels));
    y.fill(235);
    CHECK(convert() == nullptr);
    CHECK(std::all_of(pixels.begin(), pixels.end(), [](uint32_t pixel) { return pixel == 0xFFFFFFFFu; }));

    // BT.601 chroma order: Cr raises red; Cb raises blue, on every backend.
    y.fill(81);
    cr.fill(240);
    cb.fill(90);
    CHECK(convert() == nullptr);
    CHECK((pixels[0] & 0xFF) >= 250 && ((pixels[0] >> 8) & 0xFF) <= 2 && ((pixels[0] >> 16) & 0xFF) <= 2);
    y.fill(41);
    cr.fill(110);
    cb.fill(240);
    CHECK(convert() == nullptr);
    CHECK((pixels[0] & 0xFF) <= 2 && ((pixels[0] >> 8) & 0xFF) <= 2 && ((pixels[0] >> 16) & 0xFF) >= 250);

    CHECK(d3d::ConvertBinkFrame(2, 2, {}, { cr, 1 }, { cb, 1 }, pixels) != nullptr);
    CHECK(Black(pixels));
    CHECK(d3d::ConvertBinkFrame(2, 2, { y, 1 }, { cr, 1 }, { cb, 1 }, pixels) != nullptr);
    CHECK(Black(pixels));
    CHECK(d3d::ConvertBinkFrame(2, 2, { std::span(y).first(3), 2 }, { cr, 1 }, { cb, 1 }, pixels) != nullptr);
    CHECK(Black(pixels));
    CHECK(d3d::ConvertBinkFrame(2, 2, { y, 2 }, {}, { cb, 1 }, pixels) != nullptr);
    CHECK(Black(pixels));
    CHECK(d3d::ConvertBinkFrame(2, 2, { y, 2 }, { cr, 1 }, {}, pixels) != nullptr);
    CHECK(Black(pixels));
    CHECK(d3d::ConvertBinkFrame(2, 2, { y, UINT32_MAX }, { cr, 1 }, { cb, 1 }, pixels) != nullptr);
    CHECK(Black(pixels));

    // Odd dimensions use ceiling-divided chroma, and neither require nor inspect trailing row padding.
    std::array<uint8_t, 11> oddY{ 16, 16, 16, 255, 16, 16, 16, 255, 16, 16, 16 };
    std::array<uint8_t, 5> oddChroma{ 128, 128, 255, 128, 128 };
    CHECK(d3d::ConvertBinkFrame(3, 3, { oddY, 4 }, { oddChroma, 3 }, { oddChroma, 3 }, pixels) == nullptr);
    CHECK(pixels.size() == 9 && Black(pixels));
    for (uint8_t& value : oddY)
        value = value == 255 ? 255 : 0;
    for (uint8_t& value : oddChroma)
        value = value == 255 ? 255 : 0;
    CHECK(d3d::ConvertBinkFrame(3, 3, { oddY, 4 }, { oddChroma, 3 }, { oddChroma, 3 }, pixels) != nullptr);
    CHECK(Black(pixels));
    oddY[10] = 16;
    CHECK(d3d::ConvertBinkFrame(3, 3, { oddY, 4 }, { oddChroma, 3 }, { oddChroma, 3 }, pixels) == nullptr);

    for (const auto& size : std::array<std::array<uint32_t, 2>, 5>{
             { { 0, 2 }, { 2, 0 }, { 4097, 2 }, { 2, 4097 }, { UINT32_MAX, UINT32_MAX } } })
    {
        CHECK(d3d::ConvertBinkFrame(size[0], size[1], {}, {}, {}, pixels) != nullptr);
        CHECK(pixels.size() == 1 && Black(pixels));
    }

    if (g_failures == 0)
        printf("ArkeyanBinkFrameTest: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
