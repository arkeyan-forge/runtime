// XMA frame decoding with FFmpeg's libavcodec (LGPL, linked as a library).
//
// XMA frames are WMA Pro frames (FFmpeg's xma1/xma2 decoders run the WMA Pro frame decoder), but those decoders take
// whole 2048-byte packets and hold 4096 samples back until the end of the stream. The hardware works frame by frame,
// so each XMA frame is wrapped in a packet of its own for the plain WMA Pro decoder, configured with XMA's stream
// parameters: 512-sample frames, 15-bit frame lengths, the same decode flags.
#include "apu/xma_decoder.h"
#include "host/env.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
}

namespace
{
    // WMA Pro packet: 4-bit sequence number, 2 unused bits, then the length of the previous packet's split frame in
    // log2(block_align) + 4 = 15 bits (the width of an XMA frame length). The frame follows right after.
    constexpr int BlockAlign = 4095;
    constexpr uint32_t PacketHeaderBits = 21;
    constexpr uint32_t MaxFrameBits = BlockAlign * 8 - PacketHeaderBits;

    // WAVEFORMATEX extension of WMA Pro as the decoder reads it: bits per sample, channel mask, decode flags.
    constexpr int ExtradataSize = 18;
    constexpr uint16_t XmaDecodeFlags = 0x10D6; // what FFmpeg's XMA decoders use: length-prefixed 512-sample frames
    constexpr uint32_t ChannelMaskStereo = 3;   // front left | front right
    constexpr uint32_t ChannelMaskMono = 4;     // front center

    void CopyBits(
        uint8_t* destination, uint32_t destinationBit, const uint8_t* source, uint32_t sourceBit, uint32_t count)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t from = sourceBit + i;
            uint32_t to = destinationBit + i;
            if (source[from >> 3] & (0x80 >> (from & 7)))
                destination[to >> 3] |= uint8_t(0x80 >> (to & 7));
        }
    }

    class FfmpegXmaDecoder final : public XmaFrameDecoder
    {
    public:
        FfmpegXmaDecoder()
        {
            m_codec = avcodec_find_decoder(AV_CODEC_ID_WMAPRO);
            m_packet = av_packet_alloc();
            m_frame = av_frame_alloc();
        }

        ~FfmpegXmaDecoder() override
        {
            avcodec_free_context(&m_context);
            av_frame_free(&m_frame);
            av_packet_free(&m_packet);
        }

        void Reset() override
        {
            if (m_context != nullptr)
                avcodec_flush_buffers(m_context);
            m_sequence = 0;
        }

        bool DecodeFrame(const uint8_t* data, uint32_t bitOffset, uint32_t bitCount, uint32_t sampleRate,
            uint32_t channels, int16_t* samples) override
        {
            std::fill_n(samples, SamplesPerFrame * channels, int16_t(0));
            if (bitCount == 0 || bitCount > MaxFrameBits || !Open(sampleRate, channels))
                return false;
            if (av_new_packet(m_packet, BlockAlign) < 0)
                return false;

            memset(m_packet->data, 0, BlockAlign);
            m_packet->data[0] = uint8_t(m_sequence << 4); // the previous-frame length stays 0
            m_sequence = (m_sequence + 1) & 0xF;
            CopyBits(m_packet->data, PacketHeaderBits, data, bitOffset, bitCount);

            int result = avcodec_send_packet(m_context, m_packet);
            av_packet_unref(m_packet);
            static const bool trace = skyenv::Get("SKY_XMA_TRACE") != nullptr;
            if (result < 0)
            {
                if (trace)
                    fprintf(
                        stderr, "[xma-dec] %p send failed %d bits=%u\n", static_cast<void*>(this), result, bitCount);
                return false;
            }

            int received = 0;
            while (avcodec_receive_frame(m_context, m_frame) >= 0)
            {
                received++;
                if (trace && m_frame->nb_samples != int(SamplesPerFrame))
                    fprintf(stderr, "[xma-dec] %p frame of %d samples (seq %u)\n", static_cast<void*>(this),
                        m_frame->nb_samples, m_sequence);
                Convert(samples, channels);
                av_frame_unref(m_frame);
            }
            if (trace && received == 0)
                fprintf(stderr, "[xma-dec] %p no frame out (seq %u bits=%u)\n", static_cast<void*>(this), m_sequence,
                    bitCount);
            return true;
        }

    private:
        const AVCodec* m_codec{};
        AVCodecContext* m_context{};
        AVPacket* m_packet{};
        AVFrame* m_frame{};
        uint32_t m_sampleRate{};
        uint32_t m_channels{};
        uint32_t m_sequence{};

        bool Open(uint32_t sampleRate, uint32_t channels)
        {
            if (m_context != nullptr && sampleRate == m_sampleRate && channels == m_channels)
                return true;
            if (m_codec == nullptr || m_packet == nullptr || m_frame == nullptr)
                return false;

            avcodec_free_context(&m_context);
            m_context = avcodec_alloc_context3(m_codec);
            if (m_context == nullptr)
                return false;

            uint32_t mask = channels == 2 ? ChannelMaskStereo : ChannelMaskMono;
            m_context->sample_rate = int(sampleRate);
            m_context->block_align = BlockAlign;
            av_channel_layout_from_mask(&m_context->ch_layout, mask);
            auto* extradata = static_cast<uint8_t*>(av_mallocz(ExtradataSize + AV_INPUT_BUFFER_PADDING_SIZE));
            extradata[0] = 16; // bits per sample
            for (int i = 0; i < 4; i++)
                extradata[2 + i] = uint8_t(mask >> (8 * i));
            extradata[14] = uint8_t(XmaDecodeFlags);
            extradata[15] = uint8_t(XmaDecodeFlags >> 8);
            m_context->extradata = extradata;
            m_context->extradata_size = ExtradataSize;

            if (avcodec_open2(m_context, m_codec, nullptr) < 0)
            {
                fprintf(stderr, "[xma] cannot open FFmpeg's WMA Pro decoder\n");
                avcodec_free_context(&m_context);
                return false;
            }

            m_sampleRate = sampleRate;
            m_channels = channels;
            m_sequence = 0;
            return true;
        }

        // Like the hardware's 16-bit output: planar float to interleaved, saturated, rounded.
        void Convert(int16_t* samples, uint32_t channels)
        {
            uint32_t count = std::min<uint32_t>(uint32_t(m_frame->nb_samples), SamplesPerFrame);
            uint32_t decoded = std::min<uint32_t>(uint32_t(m_frame->ch_layout.nb_channels), channels);
            uint32_t first = SamplesPerFrame - count; // a trimmed frame keeps its place at the end
            for (uint32_t c = 0; c < channels; c++)
            {
                const auto* in = reinterpret_cast<const float*>(m_frame->extended_data[std::min(c, decoded - 1)]);
                for (uint32_t i = 0; i < count; i++)
                {
                    float value = std::clamp(in[i], -1.0f, 1.0f) * 32767.0f;
                    samples[(first + i) * channels + c] = int16_t(std::lrintf(value));
                }
            }
        }
    };
}

std::unique_ptr<XmaFrameDecoder> CreateXmaFrameDecoder()
{
    av_log_set_level(AV_LOG_ERROR);
    return std::make_unique<FfmpegXmaDecoder>();
}
