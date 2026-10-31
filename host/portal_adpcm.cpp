#include "portal_adpcm.h"
#include <algorithm>
#include <cstdlib>

namespace portal_adpcm
{
    namespace
    {
        // The recommendation's tables for the 4-bit coder, indexed by the code (0-7 positive, 15-8 the same magnitudes
        // negative, so entry i and 15 - i agree): the logarithm of the quantised difference, the scale factor's
        // multiplier, and the speed-control function. [?] typed from the recommendation's tables, not yet checked
        // against a real stream: the test proves the decoder against our own encoder only.
        constexpr int LogMagnitude[16] = { -2048, 4, 135, 213, 273, 323, 373, 425, 425, 373, 323, 273, 213, 135, 4,
            -2048 };
        constexpr int ScaleMultiplier[16] = { -12, 18, 41, 64, 112, 198, 355, 1122, 1122, 355, 198, 112, 64, 41, 18,
            -12 };
        constexpr int SpeedControl[16] = { 0, 0, 0, 0x200, 0x200, 0x200, 0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200,
            0x200, 0, 0, 0 };

        int Sign(int value) { return value < 0 ? -1 : 1; }
    }

    void Decoder::Reset()
    {
        yl_ = 34816; // slow scale factor
        yu_ = 544;   // fast scale factor
        dms_ = dml_ = ap_ = td_ = 0;
        a_[0] = a_[1] = 0;
        std::fill(b_, b_ + 6, 0);
        std::fill(dq_, dq_ + 6, 0);
        sr_[0] = sr_[1] = 0;
        pk_[0] = pk_[1] = 0;
    }

    int16_t Decoder::Decode(uint8_t code)
    {
        code &= 15;
        // Scale factor: the fast and slow ones mixed by the speed-control weight.
        const int weight = ap_ >= 256 ? 64 : ap_ >> 2;
        const int y = (yl_ >> 6) + (((yu_ - (yl_ >> 6)) * weight) >> 6);

        // Inverse quantiser: the difference signal, 2^(log / 128) with the sign of the code.
        int dq = 0;
        const int log = LogMagnitude[code] + (y >> 2);
        if (log >= 0)
        {
            const int exponent = (log >> 7) & 15;
            dq = (((128 + (log & 127)) << 7) >> (14 - exponent));
        }
        if (code & 8)
            dq = -dq;

        // Predictor: six zeros on the past differences, two poles on the past outputs (coefficients in 1/16384).
        int zeros = 0;
        for (int i = 0; i < 6; i++)
            zeros += (b_[i] * dq_[i]) >> 14;
        const int poles = (a_[0] * sr_[0] + a_[1] * sr_[1]) >> 14;
        const int estimate = zeros + poles;
        const int sr = std::clamp(estimate + dq, -8192, 8191); // 14-bit signal
        const int sez = zeros;
        const int dqsez = dq + sez; // the sign of this drives the pole update
        const int pk0 = dqsez < 0 ? 1 : 0;

        // Tone and transition detection: a strongly resonant pair of poles marks a pure tone; a sudden large difference
        // after one means the signal changed, so the predictor restarts.
        bool transition = false;
        if (td_)
        {
            const int lint = yl_ >> 15, frac = (yl_ >> 10) & 31;
            const int base = (32 + frac) << lint;
            const int limit = lint > 9 ? 31 << 10 : base;
            transition = std::abs(dq) > ((limit + (limit >> 1)) >> 1);
        }

        // Scale factor adaptation.
        yu_ = std::clamp(y + (((ScaleMultiplier[code] << 5) - y) >> 5), 544, 5120);
        yl_ += yu_ + ((-yl_) >> 6);

        // Speed control: short and long averages of how large the codes are.
        dms_ += (SpeedControl[code] - dms_) >> 5;
        dml_ += ((SpeedControl[code] << 2) - dml_) >> 7;
        if (transition)
            ap_ = 256;
        else if (y < 1536 || td_ || std::abs((dms_ << 2) - dml_) >= (dml_ >> 3))
            ap_ += (0x200 - ap_) >> 4;
        else
            ap_ += (-ap_) >> 4;

        // Predictor adaptation: sign-sign LMS with leakage, the poles limited to keep the filter stable.
        if (transition)
        {
            a_[0] = a_[1] = 0;
            std::fill(b_, b_ + 6, 0);
        }
        else
        {
            const int differs1 = pk0 ^ pk_[0], differs2 = pk0 ^ pk_[1];
            int a2 = a_[1] - (a_[1] >> 7);
            if (dqsez != 0)
            {
                const int a1 = differs1 ? a_[0] : -a_[0];
                a2 += std::clamp(a1 >> 5, -256, 255); // 4 * a1 while |a1| <= 1/2, else 2 * sign(a1)
                a2 += differs2 ? -128 : 128;
            }
            a_[1] = std::clamp(a2, -12288, 12288);
            int a1 = a_[0] - (a_[0] >> 8);
            if (dqsez != 0)
                a1 += differs1 ? 192 : -192;
            const int limit = 15360 - a_[1];
            a_[0] = std::clamp(a1, -limit, limit);
            for (int i = 0; i < 6; i++)
            {
                int b = b_[i] - (b_[i] >> 8);
                if (dq != 0)
                    b += Sign(dq) == Sign(dq_[i]) ? 128 : -128;
                b_[i] = b;
            }
        }
        td_ = !transition && a_[1] < -11776;

        for (int i = 5; i > 0; i--)
            dq_[i] = dq_[i - 1];
        dq_[0] = dq;
        sr_[1] = sr_[0];
        sr_[0] = sr;
        pk_[1] = pk_[0];
        pk_[0] = pk0;
        return int16_t(sr * 4); // 14 bits to 16
    }

    void DecodeBytes(Decoder& decoder, const uint8_t* bytes, size_t size, std::vector<int16_t>& out)
    {
        for (size_t i = 0; i < size; i++)
        {
            const uint8_t first = LowNibbleFirst ? bytes[i] & 15 : bytes[i] >> 4;
            const uint8_t second = LowNibbleFirst ? bytes[i] >> 4 : bytes[i] & 15;
            out.push_back(decoder.Decode(first));
            out.push_back(decoder.Decode(second));
        }
    }
}
