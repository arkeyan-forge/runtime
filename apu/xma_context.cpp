// The XMA decoder's context model: one frame at a time, as the hardware does. The packet and frame parsing, buffer
// swapping, output ring and loop handling follow Xenia's XmaContextNew (src/xenia/apu/xma_context_new.cc,
// BSD-3-Clause, Copyright 2024 Xenia Canary), with these differences, from the title's own code:
// - the loop end frame is decoded up to its end subframe, then playback jumps back to the loop start frame;
// - a frame split across input buffers waits for the next buffer instead of failing the context (error 4 makes
//   Wwise drop the voice);
// - a frame the codec can't output still takes its place as silence, so every frame gives 512 samples;
// - a starved context keeps the output it already wrote.
#include "apu/xma_context.h"
#include "memory.h"
#include <algorithm>
#include <byteswap.h>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr uint32_t BlockSize = 256;
    constexpr uint32_t SamplesPerBlock = BlockSize / sizeof(int16_t);
    constexpr uint32_t BlocksPerFrameChannel = XmaFrameDecoder::SamplesPerFrame * sizeof(int16_t) / BlockSize;

    constexpr uint32_t PacketSize = 2048;
    constexpr uint32_t PacketHeaderSize = 4;
    constexpr uint32_t PacketDataSize = PacketSize - PacketHeaderSize;
    constexpr uint32_t BitsPerPacket = PacketSize * 8;
    constexpr uint32_t PacketHeaderBits = PacketHeaderSize * 8;
    constexpr uint32_t FrameHeaderBits = 15;
    constexpr uint32_t InvalidFrameLength = 0x7FFF;
    constexpr uint32_t MaxFrameOffset = BitsPerPacket - PacketHeaderBits; // past it, no frame starts in the packet
    constexpr uint32_t PacketSkipAll = 0xFF;
    constexpr uint32_t InfiniteLoop = 255;
    constexpr uint32_t SampleRates[4] = { 24000, 32000, 44100, 48000 };

    uint8_t* TranslatePhysical(uint32_t physical)
    {
        uint32_t guest = PhysicalToGuest(physical);
        return guest != 0 ? static_cast<uint8_t*>(g_memory.Translate(guest)) : nullptr;
    }

    // XMA2 packet header: frame count (6 bits), bit offset of the first frame starting in the packet (15 bits, from
    // the end of the header), metadata (3 bits, 1 for XMA2), packets of other streams to skip after this one (8 bits).
    uint32_t PacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
    uint32_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }
    bool IsXma2Packet(const uint8_t* packet) { return (packet[2] & 7) == 1; }
    uint32_t FirstFrameOffset(const uint8_t* packet)
    {
        return PacketHeaderBits + (((packet[0] & 3u) << 13) | (packet[1] << 5) | (packet[2] >> 3));
    }

    // Big-endian bit reader over [0, size).
    struct BitReader
    {
        const uint8_t* data;
        uint32_t size;
        uint32_t offset = 0;

        uint32_t Remaining() const { return size > offset ? size - offset : 0; }

        uint32_t Peek(uint32_t count) const
        {
            uint32_t value = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t bit = offset + i;
                value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1);
            }
            return value;
        }

        uint32_t Read(uint32_t count)
        {
            uint32_t value = Peek(count);
            offset += count;
            return value;
        }
    };

    struct PacketInfo
    {
        uint32_t frameCount = 0;
        uint32_t currentFrame = 0;
        uint32_t currentFrameSize = 0; // 0: the frame's length header continues in the next packet

        bool CurrentFrameIsLast() const { return frameCount == 0 || currentFrame == frameCount - 1; }
    };

    // Which frame of the packet starts at `frameOffset`, its size, and how many frames the packet has.
    PacketInfo GetPacketInfo(const uint8_t* packet, uint32_t frameOffset)
    {
        PacketInfo info;
        uint32_t firstFrameOffset = FirstFrameOffset(packet);
        if (frameOffset < firstFrameOffset)
            info.currentFrameSize = firstFrameOffset - frameOffset; // the end of a frame split from the last packet

        BitReader stream{ packet, BitsPerPacket, firstFrameOffset };
        while (true)
        {
            if (stream.Remaining() < FrameHeaderBits)
            {
                // The length header runs into the next packet: count the frame, size unknown yet.
                if (stream.Remaining() > 0)
                {
                    if (stream.offset == frameOffset)
                    {
                        info.currentFrame = info.frameCount;
                        info.currentFrameSize = 0;
                    }
                    info.frameCount++;
                }
                break;
            }

            uint32_t frameSize = stream.Peek(FrameHeaderBits);
            if (frameSize == 0 || frameSize == InvalidFrameLength)
                break;
            if (stream.offset == frameOffset)
            {
                info.currentFrame = info.frameCount;
                info.currentFrameSize = frameSize;
            }
            info.frameCount++;
            if (frameSize > stream.Remaining())
                break;
            stream.offset += frameSize - 1;
            if (stream.Read(1) == 0) // trailing bit: does another frame follow in this packet?
                break;
        }

        if (IsXma2Packet(packet) && PacketFrameCount(packet) > info.frameCount)
        {
            if (info.currentFrameSize == 0)
                info.currentFrame = info.frameCount;
            info.frameCount = PacketFrameCount(packet);
        }
        return info;
    }

    // The current input buffer is used up: the decoder releases it and moves on to the other one.
    void SwapInputBuffer(XmaContextData& data)
    {
        if (data.current_buffer == 0)
            data.input_buffer_0_valid = 0;
        else
            data.input_buffer_1_valid = 0;
        data.current_buffer ^= 1;
        data.input_buffer_read_offset = PacketHeaderBits;
    }

    // Packet `index` of the current buffer, counting on into the other buffer past its end; null if not handed over.
    const uint8_t* FindPacket(const XmaContextData& data, uint32_t index, uint32_t* buffer = nullptr,
        uint32_t* bufferIndex = nullptr)
    {
        uint32_t which = data.current_buffer;
        uint32_t count = data.InputBufferPacketCount(which);
        if (index >= count)
        {
            which ^= 1;
            index -= count;
        }
        if (!data.InputBufferValid(which) || index >= data.InputBufferPacketCount(which))
            return nullptr;

        const uint8_t* input = TranslatePhysical(data.InputBufferAddress(which));
        if (input == nullptr)
            return nullptr;
        if (buffer != nullptr)
            *buffer = which;
        if (bufferIndex != nullptr)
            *bufferIndex = index;
        return input + index * PacketSize;
    }

    // Read offset (in the buffer it lives in) of the first frame starting at or after packet `index`, following the
    // skip counts; the end of a packet header when there is none.
    uint32_t NextFrameOffset(const XmaContextData& data, uint32_t index)
    {
        uint32_t buffer, bufferIndex;
        const uint8_t* packet = FindPacket(data, index, &buffer, &bufferIndex);
        if (packet == nullptr)
            return PacketHeaderBits;

        uint32_t count = data.InputBufferPacketCount(buffer);
        while (bufferIndex < count)
        {
            uint32_t offset = FirstFrameOffset(packet);
            if (offset <= MaxFrameOffset)
                return bufferIndex * BitsPerPacket + offset;
            uint32_t skip = PacketSkipCount(packet);
            if (skip == PacketSkipAll)
                break;
            bufferIndex += skip + 1;
            packet += (skip + 1) * PacketSize;
        }
        return PacketHeaderBits;
    }
}

void XmaContextData::Load(const void* guest)
{
    uint32_t* words = reinterpret_cast<uint32_t*>(this);
    memcpy(words, guest, sizeof(*this));
    for (uint32_t i = 0; i < sizeof(*this) / 4; i++)
        words[i] = ByteSwap(words[i]);
}

void XmaContextData::Store(void* guest) const
{
    uint32_t words[sizeof(*this) / 4];
    memcpy(words, this, sizeof(*this));
    for (uint32_t& word : words)
        word = ByteSwap(word);
    memcpy(guest, words, sizeof(*this));
}

void XmaContext::Reset()
{
    if (m_decoder != nullptr)
        m_decoder->Reset();
    m_frameBlocksLeft = 0;
    m_loopFrameBlockLimit = 0;
    m_loopStartSkipPending = false;
}

void XmaContext::Clear(uint8_t* guestData)
{
    XmaContextData data;
    data.Load(guestData);
    data.input_buffer_0_valid = 0;
    data.input_buffer_1_valid = 0;
    data.output_buffer_valid = 0;
    data.input_buffer_read_offset = PacketHeaderBits;
    data.output_buffer_read_offset = 0;
    data.output_buffer_write_offset = 0;
    data.Store(guestData);
    Reset();
}

void XmaContext::Kick(uint8_t* guestData)
{
    XmaContextData data;
    data.Load(guestData);
    const XmaContextData initial = data;

    // The title sets output_buffer_valid when there is room to decode into; the decoder clears it once the ring is full.
    uint32_t blocks = data.output_buffer_block_count;
    uint8_t* output = TranslatePhysical(data.output_buffer_ptr);
    if (!data.output_buffer_valid || blocks == 0 || output == nullptr)
        return;

    if (m_decoder == nullptr)
        m_decoder = CreateXmaFrameDecoder();
    if (m_decoder == nullptr)
        return;

    uint32_t read = data.output_buffer_read_offset % blocks;
    uint32_t write = data.output_buffer_write_offset % blocks;
    int32_t blocksLeft = int32_t(read == write ? blocks : (read + blocks - write) % blocks);

    if (data.input_buffer_0_packet_count == 0 && data.input_buffer_1_packet_count == 0)
    {
        // No input at all: only the rest of the current frame can still come out.
        if (m_frameBlocksLeft == 0)
            return;
        Consume(data, output, write, blocksLeft);
    }
    else
    {
        // Each pass outputs up to subframe_decode_count blocks, plus the padding the title reserves per frame.
        int32_t passBlocks = int32_t(std::max<uint32_t>(1, data.subframe_decode_count) + data.output_buffer_padding);
        while (blocksLeft >= passBlocks)
        {
            uint32_t readOffset = data.input_buffer_read_offset;
            uint32_t frameBlocksLeft = m_frameBlocksLeft;
            Decode(data);
            Consume(data, output, write, blocksLeft);

            // The frame being output is finished even when the input ran out: nothing else would deliver its end.
            if (!data.AnyInputBufferValid() && m_frameBlocksLeft == 0)
                break;
            if (frameBlocksLeft == 0 && m_frameBlocksLeft == 0 && data.input_buffer_read_offset == readOffset)
                break; // no progress: waiting for input
        }
    }

    data.output_buffer_write_offset = write;
    if (blocksLeft <= 0 && write == read)
        data.output_buffer_valid = 0; // full

    // Another thread may have changed the context meanwhile: write back only what the decoder owns.
    XmaContextData fresh;
    fresh.Load(guestData);
    fresh.loop_count = data.loop_count;
    fresh.output_buffer_write_offset = data.output_buffer_write_offset;
    if (initial.input_buffer_0_valid && !data.input_buffer_0_valid)
        fresh.input_buffer_0_valid = 0;
    if (initial.input_buffer_1_valid && !data.input_buffer_1_valid)
        fresh.input_buffer_1_valid = 0;
    if (initial.output_buffer_valid && !data.output_buffer_valid)
        fresh.output_buffer_valid = 0;
    fresh.input_buffer_read_offset = data.input_buffer_read_offset;
    fresh.error_status = data.error_status;
    fresh.current_buffer = data.current_buffer;
    fresh.Store(guestData);
}

// Writes the next blocks of the decoded frame into the output ring.
void XmaContext::Consume(XmaContextData& data, uint8_t* output, uint32_t& write, int32_t& blocksLeft)
{
    if (m_frameBlocksLeft == 0)
        return;

    uint32_t frameBlocks = BlocksPerFrameChannel << data.is_stereo;
    uint32_t done = frameBlocks - m_frameBlocksLeft;
    uint32_t count = std::min<uint32_t>(m_frameBlocksLeft, std::max<uint32_t>(1, data.subframe_decode_count));
    if (m_loopFrameBlockLimit != 0)
    {
        if (done >= m_loopFrameBlockLimit)
        {
            // The loop ends inside this frame: drop the rest.
            blocksLeft -= int32_t(data.output_buffer_padding);
            m_frameBlocksLeft = 0;
            m_loopFrameBlockLimit = 0;
            return;
        }
        count = std::min(count, m_loopFrameBlockLimit - done);
    }

    uint32_t blocks = data.output_buffer_block_count;
    for (uint32_t block = 0; block < count; block++)
    {
        auto* destination = reinterpret_cast<int16_t*>(output + write * BlockSize);
        const int16_t* source = m_frame.data() + (done + block) * SamplesPerBlock;
        for (uint32_t i = 0; i < SamplesPerBlock; i++)
            destination[i] = ByteSwap(source[i]);
        write = (write + 1) % blocks;
    }

    m_frameBlocksLeft -= count;
    blocksLeft -= int32_t(count + (m_frameBlocksLeft == 0 ? data.output_buffer_padding : 0));
}

// After the loop end frame, with loops left, playback jumps back to the loop start. True when it did.
bool XmaContext::UpdateLoop(XmaContextData& data)
{
    if (data.loop_count == 0 || data.input_buffer_read_offset != std::max(PacketHeaderBits, uint32_t(data.loop_end)))
        return false;

    data.input_buffer_read_offset = std::max(PacketHeaderBits, uint32_t(data.loop_start));
    m_loopStartSkipPending = true;
    if (data.loop_count != InfiniteLoop)
        data.loop_count--;
    return true;
}

// Decodes the frame at the read offset and moves the read offset to the next one.
void XmaContext::Decode(XmaContextData& data)
{
    if (!data.AnyInputBufferValid() || m_frameBlocksLeft > 0)
        return;
    if (!data.InputBufferValid(data.current_buffer))
    {
        SwapInputBuffer(data);
        if (!data.InputBufferValid(data.current_buffer))
            return;
    }

    uint32_t packetCount = data.InputBufferPacketCount(data.current_buffer);
    const uint8_t* input = TranslatePhysical(data.InputBufferAddress(data.current_buffer));
    if (data.input_buffer_read_offset < PacketHeaderBits)
        data.input_buffer_read_offset = PacketHeaderBits;
    uint32_t packetIndex = data.input_buffer_read_offset / BitsPerPacket;
    if (input == nullptr || packetIndex >= packetCount)
    {
        if (input == nullptr && !m_errorReported)
        {
            fprintf(stderr, "[xma] input buffer 0x%08X is not in physical memory\n",
                data.InputBufferAddress(data.current_buffer));
            m_errorReported = true;
        }
        SwapInputBuffer(data);
        return;
    }

    const uint8_t* packet = input + packetIndex * PacketSize;
    uint32_t relativeOffset = data.input_buffer_read_offset % BitsPerPacket;
    if (relativeOffset < FirstFrameOffset(packet))
    {
        // Pointing at the end of a frame that started in an earlier packet: skip to the first whole frame.
        relativeOffset = FirstFrameOffset(packet);
        data.input_buffer_read_offset = packetIndex * BitsPerPacket + relativeOffset;
    }

    if (PacketSkipCount(packet) == PacketSkipAll)
    {
        // No frame of this stream starts here.
        uint32_t nextOffset = NextFrameOffset(data, packetIndex + 1);
        if (packetIndex + 1 >= packetCount || nextOffset == PacketHeaderBits)
            SwapInputBuffer(data);
        data.input_buffer_read_offset = nextOffset;
        return;
    }
    uint32_t nextPacketIndex = packetIndex + 1 + PacketSkipCount(packet);

    // The frame, copied from its packet and, when split, the start of the stream's next packet.
    uint8_t frame[PacketDataSize * 2] = {};
    PacketInfo info = GetPacketInfo(packet, relativeOffset);
    uint32_t frameBits = info.currentFrameSize;
    uint32_t bitsInPacket = BitsPerPacket - relativeOffset;
    if (frameBits == 0 || (info.CurrentFrameIsLast() && bitsInPacket < frameBits))
    {
        const uint8_t* nextPacket = FindPacket(data, nextPacketIndex);
        if (nextPacket == nullptr)
            return; // the rest is in a buffer the title hasn't handed over yet
        memcpy(frame + PacketDataSize, nextPacket + PacketHeaderSize, PacketDataSize);
    }
    memcpy(frame, packet + PacketHeaderSize, PacketDataSize);

    uint32_t frameStart = relativeOffset - PacketHeaderBits;
    if (frameBits == 0)
        frameBits = BitReader{ frame, sizeof(frame) * 8, frameStart }.Peek(FrameHeaderBits);
    if (frameBits == InvalidFrameLength || frameStart + frameBits > sizeof(frame) * 8)
    {
        SwapInputBuffer(data);
        return;
    }

    bool loopEndFrame =
        data.loop_count != 0 && data.input_buffer_read_offset == std::max(PacketHeaderBits, uint32_t(data.loop_end));
    m_decoder->DecodeFrame(frame, frameStart, frameBits, SampleRates[data.sample_rate], data.is_stereo ? 2 : 1,
        m_frame.data());
    m_frameBlocksLeft = BlocksPerFrameChannel << data.is_stereo;
    m_loopFrameBlockLimit = loopEndFrame ? (data.loop_subframe_end + 1u) << data.is_stereo : 0;
    if (m_loopStartSkipPending)
    {
        uint32_t skip = uint32_t(data.loop_subframe_skip) << data.is_stereo;
        if (skip < m_frameBlocksLeft)
            m_frameBlocksLeft -= skip;
        m_loopStartSkipPending = false;
    }

    if (UpdateLoop(data))
        return;

    if (!info.CurrentFrameIsLast())
    {
        data.input_buffer_read_offset = packetIndex * BitsPerPacket + relativeOffset + frameBits;
        return;
    }

    // The next frame starts in the stream's next packet, possibly in the other buffer.
    uint32_t nextOffset = NextFrameOffset(data, nextPacketIndex);
    if (nextPacketIndex >= packetCount || nextOffset == PacketHeaderBits)
        SwapInputBuffer(data);
    if (nextOffset == PacketHeaderBits && data.AnyInputBufferValid())
    {
        const uint8_t* first = TranslatePhysical(data.InputBufferAddress(data.current_buffer));
        nextOffset = first != nullptr ? FirstFrameOffset(first) : PacketHeaderBits;
        if (nextOffset > MaxFrameOffset)
        {
            SwapInputBuffer(data);
            return;
        }
    }
    data.input_buffer_read_offset = nextOffset;
}
