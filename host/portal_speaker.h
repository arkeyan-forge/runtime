#pragma once

// The Traptanium portal's speaker: a trap's voice (docs/runtime/host/portal.md, "The speaker"). The title turns it on
// with `M 01`, sends sound blocks (4-bit ADPCM at 8 kHz under the Xbox 360's `0B 17` header, which kernel/portal.cpp
// strips) and turns it off with `M 00`. A real portal with a speaker plays them itself; any other portal, emulated or
// real, leaves them to the computer, which mixes them into the game's audio at the game's volume. Never both.
#include "host/portal_adpcm.h"
#include <atomic>
#include <cstddef>
#include <cstdint>

enum class SpeakerRoute
{
    Off,    // the "Portal sound" switch is off: the blocks go nowhere
    Device, // the real portal's own speaker
    Host,   // decoded and mixed into the computer's audio
};

// `enabled`: the switch ([<game>.audio] portal_sound). `real`: a real portal is connected, `id` is its answer to 'R'
// (empty while unknown). `deviceTakesSound`: its backend can send it sound blocks.
SpeakerRoute ChooseSpeakerRoute(bool enabled, bool real, const uint8_t* id, size_t idLength, bool deviceTakesSound);

// The computer's side: the decoded samples between the title's portal thread (one producer) and the audio output's
// thread (one consumer), resampled from 8 kHz to the output's 48 kHz. Never allocates, never blocks.
class SpeakerStream
{
public:
    static constexpr uint32_t InputRate = 8000;
    static constexpr uint32_t OutputRate = 48000;
    static constexpr size_t Capacity = 8192; // samples, about 1 s
    static constexpr size_t Prebuffer =
        320; // 40 ms queued before a line starts, so the portal thread's jitter is not heard
    static constexpr size_t MaxQueued = 4000; // a producer faster than the output: past half a second, it skips ahead

    // Producer. `M 01`: a new line; the decoder starts over.
    void Start();
    // `M 00`: what is queued plays out; blocks are refused until the next Start.
    void Stop();
    // Drops everything now (the trap left the portal, the switch went off).
    void Flush();
    // Decodes the block's ADPCM bytes and queues the samples. Ignored while stopped.
    void Push(const uint8_t* adpcm, size_t size);
    // Queues decoded samples (Push's second half; tests).
    void PushSamples(const int16_t* samples, size_t count);

    // Consumer: adds `frames` stereo frames at 48 kHz (interleaved L R) to `stereo`.
    void MixInto(float* stereo, uint32_t frames);

    // Samples waiting (either side; approximate while the other side runs).
    size_t Queued() const;

private:
    portal_adpcm::Decoder m_decoder;         // producer only
    std::atomic<bool> m_accepting{ false }; // Flush also comes from the menu's thread (the switch)

    int16_t m_ring[Capacity];
    std::atomic<size_t> m_write{ 0 }, m_read{ 0 };
    std::atomic<bool> m_flush{ false }, m_ending{ false };

    // Consumer only.
    bool m_primed = false;
    float m_previous = 0, m_current = 0;
    uint32_t m_phase = 0;
};

// The one the computer plays (host/portal.cpp feeds it, host/audio.cpp mixes it).
SpeakerStream& HostSpeakerStream();
