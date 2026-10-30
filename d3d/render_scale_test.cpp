#include "render_scale.h"
#include <cmath>
#include <cstdio>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, int line)
    {
        g_checks++;
        if (!condition)
        {
            fprintf(stderr, "FAIL line %d\n", line);
            g_failures++;
        }
    }

#define CHECK(condition) Check((condition), __LINE__)

    void TestTableScale()
    {
        struct Size
        {
            uint32_t width, height;
            bool table;
        };
        constexpr Size Sizes[]{ { 32, 33, true }, { 33, 32, true }, { 32, 32, true }, { 33, 33, false },
            { 1024, 32, true }, { 256, 16, true }, { 64, 1, true }, { 64, 2, true }, { 1280, 720, false } };
        for (const Size& size : Sizes)
        {
            CHECK(d3d::IsTableSize(size.width, size.height) == size.table);
            CHECK(d3d::IsTableSize(size.height, size.width) == size.table);
            for (int32_t scale = 1; scale <= 3; scale++)
            {
                const int32_t expected = size.table ? 1 : scale;
                CHECK(d3d::TargetScale(size.width, size.height, scale) == expected);
                CHECK(d3d::TargetScale(size.height, size.width, scale) == expected);
                const uint32_t hostWidth = size.width * expected;
                const uint32_t hostHeight = size.height * expected;
                CHECK(d3d::TargetScale(hostWidth, hostHeight, scale) == expected);
            }
        }
    }

    void TestHostKernel()
    {
        constexpr d3d::HostKernel Guest{ { 1.0f / 1280, 1.0f / 720, -0.5f / 1280, -0.5f / 720, 2.0f / 1280, 2.0f / 720,
                                             -3.0f / 1280, -3.0f / 720 },
            0x80000005u };
        constexpr std::array<float, 8> Expected[]{ Guest.constants,
            { 1.0f / 2560, 1.0f / 1440, -0.5f / 2560, -0.5f / 1440, 2.0f / 2560, 2.0f / 1440, -3.0f / 2560,
                -3.0f / 1440 },
            { 1.0f / 3840, 1.0f / 2160, -0.5f / 3840, -0.5f / 2160, 2.0f / 3840, 2.0f / 2160, -3.0f / 3840,
                -3.0f / 2160 } };
        for (int32_t scale = 1; scale <= 3; scale++)
        {
            const auto kernel = d3d::ScaleHostKernel(Guest, scale);
            for (size_t i = 0; i < kernel.constants.size(); i++)
                CHECK(std::fabs(kernel.constants[i] - Expected[scale - 1][i]) < 1.0e-9f);
            CHECK(kernel.scaledTextures == (scale == 1 ? Guest.scaledTextures : 0u));
            // One normalized pixel step and a half-texel corner tap stay on the host pixel grid.
            CHECK(std::fabs(kernel.constants[0] * (1280 * scale) - 1.0f) < 1.0e-6f);
            CHECK(std::fabs(kernel.constants[1] * (720 * scale) - 1.0f) < 1.0e-6f);
            const float fetchScale = (kernel.scaledTextures & 1) != 0 ? float(scale) : 1.0f;
            CHECK(0.5f * fetchScale == 0.5f);
            CHECK(d3d::ScaleHostKernel({ Guest.constants, 0 }, scale).scaledTextures == 0);
        }
        CHECK(d3d::ScaleHostKernel(Guest, 1).constants == Guest.constants);
    }
}

int main()
{
    TestTableScale();
    TestHostKernel();
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
