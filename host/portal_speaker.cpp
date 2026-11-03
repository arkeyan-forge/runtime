#include "host/portal_speaker.h"
#include "host/portal_identity.h"
#include <algorithm>

SpeakerRoute ChooseSpeakerRoute(bool enabled, bool real, const uint8_t* id, size_t idLength, bool deviceTakesSound)
{
    if (!enabled)
        return SpeakerRoute::Off;
    return real && deviceTakesSound && PortalModelHasSpeaker(id, idLength) ? SpeakerRoute::Device : SpeakerRoute::Host;
}

void SpeakerStream::Start()
{
    m_decoder.Reset();
    m_accepting = true;
    m_ending = false;
}

void SpeakerStream::Stop()
{
    m_accepting = false;
    m_ending = true;
}

void SpeakerStream::Flush()
{
    m_accepting = false;
    m_flush = true;
}

void SpeakerStream::Push(const uint8_t* adpcm, size_t size)
{
    if (!m_accepting)
        return;
    int16_t samples[128];
    size_t count = 0;
    for (size_t i = 0; i < size; i++)
    {
        const uint8_t byte = adpcm[i];
        samples[count++] = m_decoder.Decode(portal_adpcm::LowNibbleFirst ? byte & 15 : byte >> 4);
        samples[count++] = m_decoder.Decode(portal_adpcm::LowNibbleFirst ? byte >> 4 : byte & 15);
        if (count == std::size(samples))
        {
            PushSamples(samples, count);
            count = 0;
        }
    }
    PushSamples(samples, count);
}

void SpeakerStream::PushSamples(const int16_t* samples, size_t count)
{
    size_t write = m_write.load(std::memory_order_relaxed);
    const size_t read = m_read.load(std::memory_order_acquire);
    // Full: the newest samples are dropped (the consumer owns the read side and skips ahead itself).
    count = std::min(count, Capacity - (write - read));
    for (size_t i = 0; i < count; i++)
        m_ring[(write + i) % Capacity] = samples[i];
    m_write.store(write + count, std::memory_order_release);
}

size_t SpeakerStream::Queued() const
{
    return m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire);
}

void SpeakerStream::MixInto(float* stereo, uint32_t frames)
{
    size_t read = m_read.load(std::memory_order_relaxed);
    const size_t write = m_write.load(std::memory_order_acquire);
    if (m_flush.exchange(false))
    {
        read = write;
        m_primed = false;
        m_previous = m_current = 0;
    }
    if (write - read > MaxQueued)
        read = write - Prebuffer;
    // A line starts once a little is queued, or at once when it already ended (a short one).
    if (!m_primed && (write - read >= Prebuffer || (m_ending && write != read)))
        m_primed = true;
    if (!m_primed && m_current == 0)
    {
        m_read.store(read, std::memory_order_release);
        return;
    }

    constexpr uint32_t Step = OutputRate / InputRate;
    constexpr float Scale = 1.0f / 32768.0f;
    for (uint32_t i = 0; i < frames; i++)
    {
        if (m_phase == 0)
        {
            m_previous = m_current;
            if (m_primed && read != write)
                m_current = float(m_ring[read++ % Capacity]) * Scale;
            else
            {
                // Ran dry: down to silence over one input sample, then wait for the next prebuffer.
                m_current = 0;
                m_primed = false;
            }
        }
        // Linear interpolation between two input samples.
        const float sample = m_previous + (m_current - m_previous) * float(m_phase) / float(Step);
        stereo[i * 2 + 0] += sample;
        stereo[i * 2 + 1] += sample;
        m_phase = (m_phase + 1) % Step;
    }
    m_read.store(read, std::memory_order_release);
}

SpeakerStream& HostSpeakerStream()
{
    static SpeakerStream stream;
    return stream;
}
