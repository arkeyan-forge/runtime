// The Traptanium speaker's route and the computer's stream (no game data, no audio device): ./SSAPortalSpeakerTest
#include "host/portal_identity.h"
#include "host/portal_speaker.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

    // Each portal model: the emulated portal and speakerless real ones leave the sound to the computer, a real
    // Traptanium portal whose backend takes the blocks plays them, and the switch silences both.
    void TestRoute()
    {
        const uint8_t ssaWii[] = { 0x01, 0x29 }, giants360[] = { 0x01, 0x48 }, swapForce[] = { 0x02, 0x03 },
                      trapWii[] = { 0x02, 0x18 }, trap360[] = { 0x02, 0x1B }, trapOne[] = { 0x02, 0x27 },
                      superChargers[] = { 0x02, 0x14, 0x03, 0x02 }, unknown[] = { 0x02, 0x7F };
        CHECK(PortalModelHasSpeaker(trapWii, 2) && PortalModelHasSpeaker(trap360, 2) &&
              PortalModelHasSpeaker(trapOne, 2));
        CHECK(!PortalModelHasSpeaker(ssaWii, 2) && !PortalModelHasSpeaker(swapForce, 2) &&
              !PortalModelHasSpeaker(superChargers, 4) && !PortalModelHasSpeaker(unknown, 2));

        // The games whose own portal has a speaker (the F3 menu's row): Trap Team's, the Xbox 360 Traptanium portal.
        auto gameHasSpeaker = [](const char* game) {
            const PortalModel& model = EmulatedPortalModel(game);
            return PortalModelHasSpeaker(model.id, model.idLength);
        };
        CHECK(gameHasSpeaker("trapteam") && !gameHasSpeaker("ssa") && !gameHasSpeaker("giants") &&
              !gameHasSpeaker("swapforce"));

        // Emulated (whatever model it answers, Trap Team's is the Xbox 360 Traptanium portal).
        CHECK(ChooseSpeakerRoute(true, false, trap360, 2, false) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, false, trap360, 2, true) == SpeakerRoute::Host);
        // Real portals with a speaker, their backend taking the blocks (Wii HID, Xbox 360 USB): never on the computer too.
        CHECK(ChooseSpeakerRoute(true, true, trapWii, 2, true) == SpeakerRoute::Device);
        CHECK(ChooseSpeakerRoute(true, true, trap360, 2, true) == SpeakerRoute::Device);
        // A Traptanium portal whose backend cannot send sound (the Xbox One's GIP, a HID portal that refused a block).
        CHECK(ChooseSpeakerRoute(true, true, trapOne, 2, false) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, true, trapWii, 2, false) == SpeakerRoute::Host);
        // Real portals without a speaker, an unknown one, one whose id is not known yet.
        CHECK(ChooseSpeakerRoute(true, true, ssaWii, 2, true) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, true, giants360, 2, true) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, true, swapForce, 2, true) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, true, unknown, 2, true) == SpeakerRoute::Host);
        CHECK(ChooseSpeakerRoute(true, true, nullptr, 0, true) == SpeakerRoute::Host);
        // The switch off: nowhere, on any portal.
        CHECK(ChooseSpeakerRoute(false, false, trap360, 2, false) == SpeakerRoute::Off);
        CHECK(ChooseSpeakerRoute(false, true, trapWii, 2, true) == SpeakerRoute::Off);
        CHECK(ChooseSpeakerRoute(false, true, ssaWii, 2, true) == SpeakerRoute::Off);
    }

    // The encoder side by synthesis, as in portal_adpcm_test.cpp; like the recommendation's coder, it never sends code 0
    // (the stream from the game has none: its smallest positive difference goes as 15).
    std::vector<uint8_t> Encode(const std::vector<int16_t>& input)
    {
        portal_adpcm::Decoder decoder;
        std::vector<uint8_t> codes;
        for (int16_t sample : input)
        {
            int best = 1, bestError = 1 << 30;
            for (int code = 1; code < 16; code++)
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

    // A vowel-like test signal: a 180 Hz voice with two harmonics, its loudness rising and falling.
    std::vector<int16_t> Voice(size_t count)
    {
        std::vector<int16_t> samples(count);
        for (size_t i = 0; i < count; i++)
        {
            const double t = double(i) / 8000.0;
            const double envelope = 0.5 - 0.5 * std::cos(2 * M_PI * 2.0 * t);
            const double wave =
                std::sin(2 * M_PI * 180 * t) + 0.5 * std::sin(2 * M_PI * 360 * t) + 0.25 * std::sin(2 * M_PI * 540 * t);
            samples[i] = int16_t(std::lround(5000 * envelope * wave));
        }
        return samples;
    }

    // What the game sends: 30 payload bytes per block after `0B 17`, two codes per byte, the low nibble first.
    constexpr size_t BlockBytes = 30;
    std::vector<std::vector<uint8_t>> Blocks(const std::vector<uint8_t>& codes)
    {
        std::vector<std::vector<uint8_t>> blocks;
        for (size_t at = 0; at + 2 * BlockBytes <= codes.size(); at += 2 * BlockBytes)
        {
            std::vector<uint8_t> block;
            for (size_t i = at; i < at + 2 * BlockBytes; i += 2)
                block.push_back(uint8_t(codes[i] | codes[i + 1] << 4));
            blocks.push_back(block);
        }
        return blocks;
    }

    // The output, left channel only (checking that right matches), in chunks of the audio driver's 256 frames.
    std::vector<float> Drain(SpeakerStream& stream, size_t chunks, bool* stereoMatches = nullptr)
    {
        std::vector<float> left;
        bool same = true;
        for (size_t c = 0; c < chunks; c++)
        {
            float stereo[256 * 2] = {};
            stream.MixInto(stereo, 256);
            for (int i = 0; i < 256; i++)
            {
                left.push_back(stereo[i * 2]);
                same = same && stereo[i * 2] == stereo[i * 2 + 1];
            }
        }
        if (stereoMatches != nullptr)
            *stereoMatches = same;
        return left;
    }

    size_t Sounding(const std::vector<float>& samples)
    {
        size_t count = 0;
        for (float sample : samples)
            count += sample != 0;
        return count;
    }

    // The framing seen in a run (Trap Team, 2026-11-02): one second of blocks, through the stream, back at 48 kHz.
    void TestFramedVoice()
    {
        const std::vector<int16_t> voice = Voice(8000);
        const auto blocks = Blocks(Encode(voice));
        CHECK(blocks.size() == 133); // 133 blocks of 60 samples a second: the 7.5 ms rhythm the game keeps

        // Pushed as the game does, a block every 7.5 ms, while the audio thread takes 256 frames at a time.
        auto stream = std::make_unique<SpeakerStream>();
        std::vector<float> out;
        bool stereo = true, same = false;
        size_t pushed = 0;
        stream->Start();
        for (const auto& block : blocks)
        {
            stream->Push(block.data(), block.size());
            pushed += block.size() * 2;
            while (pushed * 6 >= out.size() + 256 + SpeakerStream::Prebuffer * 6)
            {
                const std::vector<float> chunk = Drain(*stream, 1, &same);
                out.insert(out.end(), chunk.begin(), chunk.end());
                stereo = stereo && same;
            }
        }
        stream->Stop();
        const std::vector<float> rest = Drain(*stream, SpeakerStream::Prebuffer * 6 / 256 + 2, &same);
        out.insert(out.end(), rest.begin(), rest.end());
        CHECK(stereo && same);

        // Every sixth output sample is a decoded one, one input sample late (the interpolation's delay); compare with the
        // input once the predictor has settled.
        double signal = 0, noise = 0;
        for (size_t k = 1000; k < 7900; k++)
        {
            const double decoded = out[(k + 1) * 6] * 32768.0;
            signal += double(voice[k]) * voice[k];
            noise += (voice[k] - decoded) * (voice[k] - decoded);
        }
        const double snr = 10 * std::log10(signal / std::max(noise, 1.0));
        printf("framed voice through the stream: SNR %.1f dB\n", snr);
        CHECK(snr > 30.0);
        CHECK(stream->Queued() == 0);
    }

    void TestLifecycle()
    {
        auto stream = std::make_unique<SpeakerStream>();
        std::vector<int16_t> loud(SpeakerStream::Prebuffer / 2, 8000);

        // Nothing before `M 01`.
        uint8_t block[BlockBytes];
        std::fill(block, block + BlockBytes, uint8_t(0x77));
        stream->Push(block, sizeof(block));
        CHECK(stream->Queued() == 0);

        // Less than the prebuffer waits, then plays once the line ends.
        stream->Start();
        stream->PushSamples(loud.data(), loud.size());
        CHECK(Sounding(Drain(*stream, 4)) == 0);
        stream->Stop();
        CHECK(Sounding(Drain(*stream, 4)) >= loud.size() * 6 - 6);
        // Stopped: blocks are refused until the next `M 01`.
        stream->Push(block, sizeof(block));
        CHECK(stream->Queued() == 0);

        // A trap leaving: what is queued goes at once.
        stream->Start();
        std::vector<int16_t> line(2000, 8000);
        stream->PushSamples(line.data(), line.size());
        stream->Flush();
        CHECK(Sounding(Drain(*stream, 2)) == 0 && stream->Queued() == 0);
        stream->Push(block, sizeof(block));
        CHECK(stream->Queued() == 0);

        // A producer far ahead of the output: the consumer skips to the newest samples, the latency stays short.
        stream->Start();
        std::vector<int16_t> flood(SpeakerStream::MaxQueued + 1000, 1000);
        stream->PushSamples(flood.data(), flood.size());
        Drain(*stream, 1);
        CHECK(stream->Queued() <= SpeakerStream::Prebuffer);
        // The ring never takes more than it holds.
        std::vector<int16_t> more(SpeakerStream::Capacity * 2, 1000);
        stream->PushSamples(more.data(), more.size());
        CHECK(stream->Queued() == SpeakerStream::Capacity);

        // Mixed into what is there, not over it.
        stream->Flush();
        Drain(*stream, 1);
        stream->Start();
        std::vector<int16_t> half(SpeakerStream::Prebuffer, 16384);
        stream->PushSamples(half.data(), half.size());
        float stereo[256 * 2];
        std::fill(stereo, stereo + 512, 0.25f);
        stream->MixInto(stereo, 256);
        CHECK(std::fabs(stereo[200] - 0.75f) < 1e-6f && std::fabs(stereo[201] - 0.75f) < 1e-6f);
    }
}

int main()
{
    TestRoute();
    TestFramedVoice();
    TestLifecycle();
    printf("portal speaker test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
