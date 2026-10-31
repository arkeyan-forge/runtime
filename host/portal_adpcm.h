#pragma once

// The Traptanium portal's speaker stream on the Xbox 360: 4-bit ADPCM at 8 kHz (ITU-T G.721, 32 kbit/s), decoded to
// 16-bit mono PCM. Written from the recommendation's description of the algorithm (docs/runtime/host/portal.md, "Speaker").
// Not wired to the portal yet.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace portal_adpcm
{
    // One decoder per stream: it keeps its adaptive state from one block to the next, so blocks of any size can follow
    // each other. Reset() when the stream restarts (the game's speaker-on command).
    class Decoder
    {
    public:
        Decoder() { Reset(); }
        void Reset();
        // One 4-bit code (low 4 bits of `code`) to one sample.
        int16_t Decode(uint8_t code);

    private:
        int yl_, yu_, dms_, dml_, ap_, td_;
        int a_[2], b_[6], dq_[6], sr_[2], pk_[2];
    };

    // How the bytes of a packet hold the codes. [?] one source only (Cemu's 360 bridge: two codes per byte, low nibble
    // first); a capture settles it. Kept apart from the decoder, which only knows codes.
    constexpr bool LowNibbleFirst = true;

    // Decodes packed bytes (two codes each) with `decoder`, appending the samples to `out`. The packet header (`0B 17`
    // on the 360 [?]) is the caller's to strip.
    void DecodeBytes(Decoder& decoder, const uint8_t* bytes, size_t size, std::vector<int16_t>& out);
}
