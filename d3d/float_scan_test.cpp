// Checks the frame dump's float scan: NaN, infinities, negatives, denormals, the finite range. usage: ArkeyanFloatScanTest
#include "d3d/float_scan.h"
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

    uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }
}

int main()
{
    const float inf = std::numeric_limits<float>::infinity();
    const uint32_t clean[] = { Bits(0.0f), Bits(0.5f), Bits(1.0f), Bits(-0.0f) };
    d3d::FloatScan scan = d3d::ScanFloats(clean, 4);
    CHECK(!scan.Bad() && scan.negative == 0 && scan.denormal == 0 && scan.firstBad == -1);
    CHECK(scan.min == 0.0f && scan.max == 1.0f && scan.mean == 0.375);

    // A quiet NaN, a NaN with a payload, both infinities, a negative, a denormal.
    const uint32_t bad[] = { Bits(2.0f), 0x7FC00000u, Bits(inf), Bits(-inf), Bits(-3.0f), 0x00000001u, 0xFFFFFFFFu };
    scan = d3d::ScanFloats(bad, 7);
    CHECK(scan.Bad());
    CHECK(scan.nan == 2 && scan.positiveInf == 1 && scan.negativeInf == 1);
    CHECK(scan.negative == 1 && scan.denormal == 1);
    CHECK(scan.firstBad == 1);
    CHECK(scan.min == -3.0f && scan.max == 2.0f);
    CHECK(scan.mean > -0.334 && scan.mean < -0.333); // 2, -3 and the denormal

    // Only NaN: no finite range.
    const uint32_t onlyNan[] = { 0x7FC00000u };
    scan = d3d::ScanFloats(onlyNan, 1);
    CHECK(!scan.anyFinite && scan.min == 0.0f && scan.max == 0.0f && scan.firstBad == 0);

    if (g_failures == 0)
        printf("ArkeyanFloatScanTest: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
