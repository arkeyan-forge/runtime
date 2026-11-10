// The portal speaker's ADPCM decoder against an encoder that lives only here (no game data): ./ArkeyanPortalAdpcmTest
#include "portal_adpcm.h"
#include <cmath>
#include <cstdio>
#include <vector>

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

    // The encoder side by synthesis: try the 16 codes on a copy of the decoder and keep the one whose output is
    // closest to the input, then advance the real decoder with it. So the encoder never needs the decoder's tables.
    std::vector<uint8_t> Encode(const std::vector<int16_t>& input)
    {
        portal_adpcm::Decoder decoder;
        std::vector<uint8_t> codes;
        for (int16_t sample : input)
        {
            int best = 0, bestError = 1 << 30;
            for (int code = 0; code < 16; code++)
            {
                portal_adpcm::Decoder trial = decoder;
                int error = std::abs(trial.Decode(uint8_t(code)) - sample);
                if (error < bestError)
                    best = code, bestError = error;
            }
            decoder.Decode(uint8_t(best));
            codes.push_back(uint8_t(best));
        }
        return codes;
    }

    std::vector<uint8_t> Pack(const std::vector<uint8_t>& codes)
    {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < codes.size(); i += 2)
        {
            uint8_t first = codes[i], second = codes[i + 1];
            bytes.push_back(portal_adpcm::LowNibbleFirst ? uint8_t(first | second << 4) : uint8_t(second | first << 4));
        }
        return bytes;
    }

    std::vector<int16_t> Sine(size_t count, double hertz, double amplitude)
    {
        std::vector<int16_t> samples(count);
        for (size_t i = 0; i < count; i++)
            samples[i] = int16_t(std::lround(amplitude * std::sin(2 * M_PI * hertz * double(i) / 8000.0)));
        return samples;
    }

    double SnrDb(const std::vector<int16_t>& original, const std::vector<int16_t>& decoded, size_t from)
    {
        double signal = 0, noise = 0;
        for (size_t i = from; i < original.size() && i < decoded.size(); i++)
        {
            signal += double(original[i]) * original[i];
            noise += double(original[i] - decoded[i]) * (original[i] - decoded[i]);
        }
        return 10 * std::log10(signal / std::max(noise, 1.0));
    }

    void TestSine()
    {
        for (double hertz : { 440.0, 1000.0, 2500.0 })
        {
            std::vector<int16_t> input = Sine(8000, hertz, 8000);
            std::vector<uint8_t> bytes = Pack(Encode(input));
            portal_adpcm::Decoder decoder;
            std::vector<int16_t> output;
            portal_adpcm::DecodeBytes(decoder, bytes.data(), bytes.size(), output);
            double snr = SnrDb(input, output, 1000); // after the predictor has settled
            printf("sine %.0f Hz: SNR %.1f dB\n", hertz, snr);
            // 34 dB: the pole predictor's a1 adapted the wrong way round before 2026-11-02 (33.5, 27.6, 26.1 dB).
            CHECK(output.size() == input.size() && snr > 34.0);
        }
    }

    void TestSilence()
    {
        std::vector<int16_t> silence(2000, 0);
        std::vector<uint8_t> bytes = Pack(Encode(silence));
        portal_adpcm::Decoder decoder;
        std::vector<int16_t> output;
        portal_adpcm::DecodeBytes(decoder, bytes.data(), bytes.size(), output);
        int peak = 0;
        for (int16_t sample : output)
            peak = std::max(peak, std::abs(int(sample)));
        CHECK(output.size() == silence.size() && peak <= 16); // a few LSB of the 16-bit scale, no drift
    }

    void TestBlocks()
    {
        std::vector<uint8_t> bytes = Pack(Encode(Sine(1024, 1000, 6000)));
        portal_adpcm::Decoder whole, blocks, restarted;
        std::vector<int16_t> a, b, c;
        portal_adpcm::DecodeBytes(whole, bytes.data(), bytes.size(), a);
        // 16 bytes = 32 samples per block; the same decoder carries on across the boundaries.
        for (size_t at = 0; at < bytes.size(); at += 16)
        {
            portal_adpcm::DecodeBytes(blocks, bytes.data() + at, 16, b);
            restarted.Reset();
            portal_adpcm::DecodeBytes(restarted, bytes.data() + at, 16, c);
        }
        CHECK(a == b);
        CHECK(a != c); // a decoder that forgot its state between blocks would differ
        // The first block is the same either way, the second only if the state is kept.
        CHECK(std::equal(a.begin(), a.begin() + 32, c.begin()) &&
              !std::equal(a.begin() + 32, a.begin() + 64, c.begin() + 32));
    }

    void TestNibbles()
    {
        portal_adpcm::Decoder one, two;
        std::vector<int16_t> packed;
        const uint8_t byte = 0x52;
        portal_adpcm::DecodeBytes(one, &byte, 1, packed);
        const uint8_t first = portal_adpcm::LowNibbleFirst ? 2 : 5, second = portal_adpcm::LowNibbleFirst ? 5 : 2;
        CHECK(packed.size() == 2 && packed[0] == two.Decode(first) && packed[1] == two.Decode(second));
    }
}

int main()
{
    TestSine();
    TestSilence();
    TestBlocks();
    TestNibbles();
    printf("portal adpcm test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
