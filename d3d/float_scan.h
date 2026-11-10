// The frame dump's float scan (SKY_DUMP_FLOATS): counts the values a render target should not hold, from the raw bits of
// one channel. A 16-bit float target is read back as 32-bit floats: its NaN and infinities stay, its denormals become
// normal numbers (so `denormal` counts only 32-bit ones).
#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace d3d
{
    struct FloatScan
    {
        uint64_t nan = 0, positiveInf = 0, negativeInf = 0, negative = 0, denormal = 0;
        float min = 0.0f, max = 0.0f; // of the finite values (0 when there is none)
        double mean = 0.0; // of the finite values
        int64_t firstBad = -1; // the index of the first NaN or infinity
        bool anyFinite = false;

        bool Bad() const { return nan + positiveInf + negativeInf != 0; }
    };

    inline FloatScan ScanFloats(const uint32_t* bits, size_t count)
    {
        FloatScan scan;
        double sum = 0.0;
        uint64_t finite = 0;
        for (size_t i = 0; i < count; i++)
        {
            const float value = std::bit_cast<float>(bits[i]);
            if (std::isnan(value) || std::isinf(value))
            {
                if (std::isnan(value))
                    scan.nan++;
                else if (value > 0.0f)
                    scan.positiveInf++;
                else
                    scan.negativeInf++;
                if (scan.firstBad < 0)
                    scan.firstBad = int64_t(i);
                continue;
            }
            if (value < 0.0f)
                scan.negative++;
            if (value != 0.0f && std::fpclassify(value) == FP_SUBNORMAL)
                scan.denormal++;
            if (!scan.anyFinite || value < scan.min)
                scan.min = value;
            if (!scan.anyFinite || value > scan.max)
                scan.max = value;
            scan.anyFinite = true;
            sum += value;
            finite++;
        }
        scan.mean = finite != 0 ? sum / double(finite) : 0.0;
        return scan;
    }
}
