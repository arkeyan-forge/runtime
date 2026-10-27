#pragma once

#include "apu/xma_decoder.h"
#include <array>
#include <cstdint>
#include <memory>

// XMA_CONTEXT_DATA: the 64-byte block through which the title drives one hardware decoder context. It lives in
// guest memory as 16 big-endian dwords; the bitfields below are their host-order view (allocated from the least
// significant bit, as on every compiler and ABI this runtime targets). Field layout from Xenia
// (src/xenia/apu/xma_context.h, BSD-3-Clause, Copyright 2021 Ben Vanik), checked against the title's code: its
// XMASetLoopData (0x82617390) writes the loop subframe end to bits 12-13 and the subframe skip to bits 17-19 of dword 1.
struct XmaContextData
{
    // dword 0
    uint32_t input_buffer_0_packet_count : 12; // 2048-byte packets
    uint32_t loop_count : 8;                   // 255: forever
    uint32_t input_buffer_0_valid : 1;
    uint32_t input_buffer_1_valid : 1;
    uint32_t output_buffer_block_count : 5; // 256-byte blocks
    uint32_t output_buffer_write_offset : 5; // in blocks
    // dword 1
    uint32_t input_buffer_1_packet_count : 12;
    uint32_t loop_subframe_end : 2;  // last subframe played in the loop end frame (Xenia calls these bits "start")
    uint32_t : 3;
    uint32_t loop_subframe_skip : 3; // subframes skipped in the loop start frame
    uint32_t subframe_decode_count : 4;
    uint32_t output_buffer_padding : 3;
    uint32_t sample_rate : 2; // 24, 32, 44.1, 48 kHz
    uint32_t is_stereo : 1;
    uint32_t unknown_dword_1 : 1;
    uint32_t output_buffer_valid : 1;
    // dword 2
    uint32_t input_buffer_read_offset : 26; // in bits, from the start of the current input buffer
    uint32_t error_status : 5;
    uint32_t error_set : 1;
    // dword 3
    uint32_t loop_start : 26; // in bits
    uint32_t parser_error_status : 5;
    uint32_t parser_error_set : 1;
    // dword 4
    uint32_t loop_end : 26; // in bits
    uint32_t packet_metadata : 5;
    uint32_t current_buffer : 1;
    // dwords 5-8: physical addresses
    uint32_t input_buffer_0_ptr;
    uint32_t input_buffer_1_ptr;
    uint32_t output_buffer_ptr;
    uint32_t work_buffer_ptr;
    // dword 9
    uint32_t output_buffer_read_offset : 5; // in blocks
    uint32_t : 25;
    uint32_t stop_when_done : 1;
    uint32_t interrupt_when_done : 1;
    // dwords 10-15
    uint32_t reserved[6];

    void Load(const void* guest);
    void Store(void* guest) const;

    bool InputBufferValid(uint32_t buffer) const { return buffer == 0 ? input_buffer_0_valid : input_buffer_1_valid; }
    bool AnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
    uint32_t InputBufferPacketCount(uint32_t buffer) const
    {
        return buffer == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
    }
    uint32_t InputBufferAddress(uint32_t buffer) const { return buffer == 0 ? input_buffer_0_ptr : input_buffer_1_ptr; }
};
static_assert(sizeof(XmaContextData) == 64);

// One hardware decoder context. Like Xenia with its decoder thread disabled, a kick decodes synchronously: when the
// kick returns, the output ring holds as much audio as fits (or as the input allows) and the context reflects it.
class XmaContext
{
public:
    // The title (re)starts using the context: XMACreateContext, XMAReleaseContext.
    void Reset();
    // Kick register: decode into the output ring.
    void Kick(uint8_t* guestData);
    // Clear register: back to the state of a fresh context.
    void Clear(uint8_t* guestData);

private:
    static constexpr uint32_t MaxChannels = 2;

    std::unique_ptr<XmaFrameDecoder> m_decoder;
    // The decoded frame, as the hardware writes it: big-endian samples, interleaved.
    std::array<int16_t, XmaFrameDecoder::SamplesPerFrame * MaxChannels> m_frame{};
    uint32_t m_frameBlocksLeft = 0;    // 256-byte blocks of m_frame not yet in the output ring
    uint32_t m_loopFrameBlockLimit = 0; // the loop end frame only outputs up to loop_subframe_end
    bool m_loopStartSkipPending = false;
    bool m_errorReported = false;

    void Decode(XmaContextData& data);
    void Consume(XmaContextData& data, uint8_t* output, uint32_t& write, int32_t& blocksLeft);
    bool UpdateLoop(XmaContextData& data);
};
