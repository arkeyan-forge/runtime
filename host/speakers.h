#pragma once

// The "Speakers" setting ([<game>.audio] speakers, SKY_SPEAKERS; docs/runtime/xbox360/audio_xma_xaudio.md, "5.1"): the
// layout of the audio output. The game always mixes six channels (the console's render driver frame, host/audio.h);
// stereo folds them down as the runtime always did, 5.1 gives them to the device in the same order, the LFE band-limited
// as on the console. Pure functions of the frame (no SDL, no allocation): host/audio.cpp and host/audio_xaudio2.cpp share them, speakers_test.cpp checks them.
#include "host/audio.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace speakers
{
    enum Mode : uint32_t
    {
        Auto, // not chosen: 5.1 when the output device has 6 channels or more, else stereo
        Stereo,
        Surround // 5.1
    };

    // The setting's text: "auto", "stereo", "5.1".
    constexpr const char* Name(Mode mode) { return mode == Stereo ? "stereo" : mode == Surround ? "5.1" : "auto"; }

    constexpr bool Parse(std::string_view text, Mode& mode)
    {
        for (Mode m : { Auto, Stereo, Surround })
            if (text == Name(m))
            {
                mode = m;
                return true;
            }
        return false;
    }

    // The layout of the run (never Auto): `deviceChannels` is what the default output device reports (0: unknown, no
    // device).
    constexpr Mode Resolve(Mode mode, int deviceChannels)
    {
        return mode != Auto ? mode : deviceChannels >= 6 ? Surround : Stereo;
    }

    constexpr uint32_t Channels(Mode layout) { return layout == Surround ? 6 : 2; }

    // What XAudioGetSpeakerConfig reports (kernel/audio.cpp): 0x00010001 as the runtime always did, Dolby Digital in the
    // high half (the console's digital output setting), for 5.1; 0x00000001, no Dolby Digital, for stereo. The six titles
    // read bit 31 only (their XAudio2, at its render client's registration) and always mix 5.1 (a 6-channel master voice,
    // mask 0x3F), so the word changes nothing in their mix; it is set for a title that would ask.
    constexpr uint32_t GuestSpeakerConfig(Mode layout) { return layout == Surround ? 0x00010001 : 0x00000001; }

    // The guest's channel order (FL, FR, C, LFE, BL, BR) is SDL 3's for 6 channels and XAudio2's SPEAKER_5POINT1 order.
    enum Channel : uint32_t
    {
        FrontLeft,
        FrontRight,
        Center,
        Lfe,
        BackLeft,
        BackRight
    };

    // Stereo: L = FL + 0.7071 C + 0.7071 BL, R = FR + 0.7071 C + 0.7071 BR, the LFE dropped. Interleaved L R.
    inline void FoldToStereo(const float* planar, float* stereo)
    {
        constexpr float Side = 0.7071f;
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
        {
            auto channel = [&](uint32_t c) { return planar[c * AudioFrameSamples + i]; };
            float center = channel(Center) * Side;
            stereo[i * 2 + 0] = channel(FrontLeft) + center + channel(BackLeft) * Side;
            stereo[i * 2 + 1] = channel(FrontRight) + center + channel(BackRight) * Side;
        }
    }

    // 5.1: the six planar channels interleaved in the same order.
    inline void Interleave(const float* planar, float* surround)
    {
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
            for (uint32_t c = 0; c < AudioFrameChannels; c++)
                surround[i * AudioFrameChannels + c] = planar[c * AudioFrameSamples + i];
    }

    // Adds an interleaved stereo frame (the portal speaker's voice) to the front pair of an interleaved 5.1 frame: where
    // the stereo output plays it, the same level.
    inline void AddToFront(const float* stereo, float* surround)
    {
        for (uint32_t i = 0; i < AudioFrameSamples; i++)
        {
            surround[i * AudioFrameChannels + FrontLeft] += stereo[i * 2 + 0];
            surround[i * AudioFrameChannels + FrontRight] += stereo[i * 2 + 1];
        }
    }

    // The LFE band-limited to 120 Hz, as the console's Dolby Digital encoder does (ATSC A/52): the Alchemy games send it
    // full-band effects (Giants: 90 % of its power above 120 Hz), which a PCM output would hand the subwoofer as they are.
    // A 4th-order Butterworth low-pass (two biquads), its state kept from frame to frame. One per output.
    class LfeLowPass
    {
    public:
        static constexpr double Cutoff = 120.0, Rate = 48000.0;

        LfeLowPass()
        {
            constexpr double Pi = 3.14159265358979323846;
            const double w = 2.0 * Pi * Cutoff / Rate, cosW = std::cos(w);
            const double qs[2] = { 0.54119610014619698, 1.3065629648763766 }; // 1 / (2 cos(pi/8)), 1 / (2 cos(3pi/8))
            for (int s = 0; s < 2; s++)
            {
                const double alpha = std::sin(w) / (2.0 * qs[s]), a0 = 1.0 + alpha;
                m_stage[s].b0 = m_stage[s].b2 = (1.0 - cosW) / 2.0 / a0;
                m_stage[s].b1 = (1.0 - cosW) / a0;
                m_stage[s].a1 = -2.0 * cosW / a0;
                m_stage[s].a2 = (1.0 - alpha) / a0;
            }
        }

        // Filters the LFE of an interleaved 5.1 frame in place.
        void Process(float* surround)
        {
            for (uint32_t i = 0; i < AudioFrameSamples; i++)
            {
                double x = surround[i * AudioFrameChannels + Lfe];
                for (Stage& stage : m_stage)
                {
                    const double y = stage.b0 * x + stage.b1 * stage.x1 + stage.b2 * stage.x2 - stage.a1 * stage.y1 -
                                     stage.a2 * stage.y2;
                    stage.x2 = stage.x1;
                    stage.x1 = x;
                    stage.y2 = stage.y1;
                    stage.y1 = std::fabs(y) < 1e-30 ? 0.0 : y; // no denormals in the tail of a silence
                    x = y;
                }
                surround[i * AudioFrameChannels + Lfe] = float(x);
            }
        }

    private:
        struct Stage
        {
            double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        };
        Stage m_stage[2];
    };

    // The master volume (linear: 1 leaves the samples as the game mixed them), then the clamp to [-1, 1].
    inline void ApplyGain(float* samples, uint32_t count, float gain)
    {
        for (uint32_t i = 0; i < count; i++)
            samples[i] = std::clamp(samples[i] * gain, -1.0f, 1.0f);
    }
}
