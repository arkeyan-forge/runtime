// The "Speakers" setting: its words, the auto rule, the channel order of the 5.1 output and the stereo fold, which must
// stay bit for bit the 0.9.3 one (no game data, no audio device): ./ArkeyanSpeakersTest
#include "host/speakers.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
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

    void TestWords()
    {
        speakers::Mode mode = speakers::Stereo;
        CHECK(speakers::Parse("auto", mode) && mode == speakers::Auto);
        CHECK(speakers::Parse("stereo", mode) && mode == speakers::Stereo);
        CHECK(speakers::Parse("5.1", mode) && mode == speakers::Surround);
        for (const char* bad : { "", "Auto", "5,1", "51", "7.1", "surround", "mono" })
            CHECK(!speakers::Parse(bad, mode) && mode == speakers::Surround);
        for (speakers::Mode m : { speakers::Auto, speakers::Stereo, speakers::Surround })
            CHECK(speakers::Parse(speakers::Name(m), mode) && mode == m);
    }

    // Auto: 5.1 from 6 device channels (5.1, 7.1), stereo below and without a device; a choice is kept whatever the device.
    void TestAutoRule()
    {
        for (int channels : { 0, 1, 2, 3, 4, 5 })
            CHECK(speakers::Resolve(speakers::Auto, channels) == speakers::Stereo);
        for (int channels : { 6, 7, 8 })
            CHECK(speakers::Resolve(speakers::Auto, channels) == speakers::Surround);
        for (int channels : { 0, 2, 6, 8 })
        {
            CHECK(speakers::Resolve(speakers::Stereo, channels) == speakers::Stereo);
            CHECK(speakers::Resolve(speakers::Surround, channels) == speakers::Surround);
        }
        CHECK(speakers::Channels(speakers::Stereo) == 2 && speakers::Channels(speakers::Surround) == 6);
        // What the title is told (XAudioGetSpeakerConfig): Dolby Digital in the high half for 5.1 only; bit 31, the one the
        // titles' XAudio2 reads, never set.
        CHECK(speakers::GuestSpeakerConfig(speakers::Surround) == 0x00010001);
        CHECK(speakers::GuestSpeakerConfig(speakers::Stereo) == 0x00000001);
        CHECK((speakers::GuestSpeakerConfig(speakers::Surround) & 0x80000000) == 0 &&
              (speakers::GuestSpeakerConfig(speakers::Stereo) & 0x80000000) == 0);
    }

    // Each guest channel lands at its own place of the interleaved frame: FL FR C LFE BL BR, SDL 3's 6-channel order
    // (SDL_audio.h: "FL, FR, FC, LFE, BL, BR") and XAudio2's SPEAKER_5POINT1 (bits FL 0x1, FR 0x2, FC 0x4, LFE 0x8, BL 0x10,
    // BR 0x20, in that order).
    void TestSurroundOrder()
    {
        CHECK(speakers::FrontLeft == 0 && speakers::FrontRight == 1 && speakers::Center == 2 && speakers::Lfe == 3 &&
              speakers::BackLeft == 4 && speakers::BackRight == 5);
        std::vector<float> planar(AudioFrameChannels * AudioFrameSamples), out(AudioFrameChannels * AudioFrameSamples);
        for (uint32_t c = 0; c < AudioFrameChannels; c++)
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
                planar[c * AudioFrameSamples + i] =
                    float(c + 1) * 1000.0f + float(i); // channel and sample in the value
        speakers::Interleave(planar.data(), out.data());
        bool exact = true;
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
            for (uint32_t c = 0; c < AudioFrameChannels; c++)
                exact = exact && out[i * AudioFrameChannels + c] == float(c + 1) * 1000.0f + float(i);
        CHECK(exact);
        // A tone on one channel only reaches that channel: the LFE stays on the LFE, the back pair is not swapped.
        for (uint32_t only = 0; only < AudioFrameChannels; only++)
        {
            std::fill(planar.begin(), planar.end(), 0.0f);
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
                planar[only * AudioFrameSamples + i] = std::sin(float(i) * 0.1f);
            speakers::Interleave(planar.data(), out.data());
            double energy[AudioFrameChannels] = {};
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
                for (uint32_t c = 0; c < AudioFrameChannels; c++)
                    energy[c] += double(out[i * AudioFrameChannels + c]) * out[i * AudioFrameChannels + c];
            bool alone = energy[only] > 1.0;
            for (uint32_t c = 0; c < AudioFrameChannels; c++)
                alone = alone && (c == only || energy[c] == 0.0);
            CHECK(alone);
        }
        // The portal speaker's stereo voice goes to the front pair only.
        std::fill(out.begin(), out.end(), 0.0f);
        std::vector<float> voice(AudioFrameSamples * 2);
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
            voice[i * 2] = voice[i * 2 + 1] = 0.25f;
        speakers::AddToFront(voice.data(), out.data());
        bool front = true;
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
            for (uint32_t c = 0; c < AudioFrameChannels; c++)
                front = front && out[i * AudioFrameChannels + c] == (c < 2 ? 0.25f : 0.0f);
        CHECK(front);
    }

    // The 0.9.3 stereo output, verbatim (host/audio.cpp's HostAudioSubmit before the setting), without the portal voice.
    void StereoBefore(const float* planarFrame, uint32_t volume, float* stereo)
    {
        constexpr float Side = 0.7071f;
        const float gain = float(volume) / 100.0f;
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
        {
            auto channel = [&](uint32_t c) { return planarFrame[c * AudioFrameSamples + i]; };
            float center = channel(2) * Side;
            stereo[i * 2 + 0] = channel(0) + center + channel(4) * Side;
            stereo[i * 2 + 1] = channel(1) + center + channel(5) * Side;
        }
        for (uint32_t i = 0; i < AudioFrameSamples * 2; i++)
            stereo[i] = std::clamp(stereo[i] * gain, -1.0f, 1.0f);
    }

    // The stereo output now: the same bytes for every frame and volume, including frames that clip.
    void TestStereoUnchanged()
    {
        std::mt19937 random(94);
        std::uniform_real_distribution<float> quiet(-0.3f, 0.3f), loud(-2.0f, 2.0f);
        std::vector<float> planar(AudioFrameChannels * AudioFrameSamples);
        std::vector<float> before(AudioFrameSamples * 2), now(AudioFrameSamples * 2);
        bool same = true;
        uint32_t frames = 0;
        for (uint32_t frame = 0; frame < 4000; frame++)
        {
            for (float& sample : planar)
                sample = frame % 2 == 0 ? quiet(random) : loud(random);
            if (frame % 97 == 0) // denormals and signed zeros
                for (uint32_t i = 0; i < planar.size(); i++)
                    planar[i] = i % 3 == 0 ? 1e-40f : i % 3 == 1 ? -0.0f : 0.0f;
            const uint32_t volume = frame % 101;
            StereoBefore(planar.data(), volume, before.data());
            speakers::FoldToStereo(planar.data(), now.data());
            speakers::ApplyGain(now.data(), AudioFrameSamples * 2, float(volume) / 100.0f);
            same = same && std::memcmp(before.data(), now.data(), before.size() * sizeof(float)) == 0;
            frames++;
        }
        CHECK(same && frames == 4000);
    }

    // The LFE low-pass: the bass through (40 Hz), -3 dB at 120 Hz, a voice's band gone (1 kHz: -70 dB and more), steady
    // across frames; the other five channels untouched.
    double LfeGain(double hz)
    {
        speakers::LfeLowPass filter;
        std::vector<float> frame(AudioFrameChannels * AudioFrameSamples);
        double peak = 0;
        bool othersKept = true;
        constexpr double Pi = 3.14159265358979323846;
        for (uint32_t f = 0; f < 400; f++) // ~2 s: past the filter's settling
        {
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
                for (uint32_t c = 0; c < AudioFrameChannels; c++)
                    frame[i * AudioFrameChannels + c] =
                        float(0.5 * std::sin(2 * Pi * hz * double(f * AudioFrameSamples + i) / 48000.0));
            filter.Process(frame.data());
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
            {
                const float in = float(0.5 * std::sin(2 * Pi * hz * double(f * AudioFrameSamples + i) / 48000.0));
                for (uint32_t c = 0; c < AudioFrameChannels; c++)
                    othersKept = othersKept && (c == speakers::Lfe || frame[i * AudioFrameChannels + c] == in);
                if (f >= 200)
                    peak = std::max(peak, double(std::fabs(frame[i * AudioFrameChannels + speakers::Lfe])));
            }
        }
        CHECK(othersKept);
        return peak / 0.5;
    }

    void TestLfeLowPass()
    {
        const double bass = LfeGain(40), corner = LfeGain(120), voice = LfeGain(1000);
        printf("LFE low-pass: 40 Hz %.4f, 120 Hz %.3f, 1 kHz %.6f\n", bass, corner, voice);
        CHECK(bass > 0.995 && bass < 1.005);
        CHECK(corner > 0.68 && corner < 0.73);
        CHECK(voice < 0.0003);
        // Silence after sound ends at zero, not in denormals.
        speakers::LfeLowPass filter;
        std::vector<float> frame(AudioFrameChannels * AudioFrameSamples, 0.0f);
        frame[speakers::Lfe] = 1.0f;
        for (uint32_t f = 0; f < 400; f++) // an impulse, then ~2 s of silence
        {
            filter.Process(frame.data());
            std::fill(frame.begin(), frame.end(), 0.0f);
        }
        filter.Process(frame.data());
        CHECK(frame[speakers::Lfe] == 0.0f &&
              frame[(AudioFrameSamples - 1) * AudioFrameChannels + speakers::Lfe] == 0.0f);
    }

    void TestGain()
    {
        float samples[] = { 0.5f, -0.5f, 1.5f, -3.0f, 0.0f, 1.0f };
        speakers::ApplyGain(samples, 6, 1.0f);
        CHECK(samples[0] == 0.5f && samples[1] == -0.5f && samples[2] == 1.0f && samples[3] == -1.0f &&
              samples[4] == 0.0f && samples[5] == 1.0f);
        speakers::ApplyGain(samples, 6, 0.5f);
        CHECK(samples[0] == 0.25f && samples[2] == 0.5f && samples[3] == -0.5f);
    }
}

int main()
{
    TestWords();
    TestAutoRule();
    TestSurroundOrder();
    TestStereoUnchanged();
    TestLfeLowPass();
    TestGain();
    printf("speakers test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
