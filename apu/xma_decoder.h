#pragma once

#include <cstdint>
#include <memory>

// Decodes single XMA frames, as the hardware does: every frame gives 512 samples per channel, with no extra latency
// beyond the codec's own overlap. A context's stream has 1 or 2 channels.
class XmaFrameDecoder
{
public:
    static constexpr uint32_t SamplesPerFrame = 512;

    virtual ~XmaFrameDecoder() = default;

    // Forgets the current stream (the overlap kept from the previous frame).
    virtual void Reset() = 0;
    // Decodes the frame whose bits start `bitOffset` bits into `data` and span `bitCount` bits (its 15-bit length
    // header included). Writes SamplesPerFrame * channels interleaved samples in host byte order; a frame the codec
    // can't output (the first one of a stream, a damaged one) comes out as silence. False if the frame was rejected.
    virtual bool DecodeFrame(const uint8_t* data, uint32_t bitOffset, uint32_t bitCount, uint32_t sampleRate,
        uint32_t channels, int16_t* samples) = 0;
};

// Null when the build has no XMA decoder (SSA_FFMPEG=OFF); contexts then stay silent.
std::unique_ptr<XmaFrameDecoder> CreateXmaFrameDecoder();
