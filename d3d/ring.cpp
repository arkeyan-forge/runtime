#include "d3d/ring.h"
#include "d3d/command_buffers.h"
#include "d3d/device.h"
#include "game_hooks.h"
#include "memory.h"
#include "gpu/gpu.h"
#include "platform/clock.h"
#include <array>
#include <bit>
#include <mutex>
#include <utility>
#if defined(__APPLE__)
#include <os/lock.h>
#endif
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t FirstRegister = 0x2000;
    constexpr uint32_t RegisterCount = 0x4928 - FirstRegister;
    constexpr uint32_t AluConstants = 0x4000;  // vertex shader c0.x .. then pixel shader from 0x4400
    constexpr uint32_t FetchConstants = 0x4800;
    constexpr uint32_t BoolConstants = 0x4900; // then loop constants from 0x4908

    constexpr uint32_t WriteCursor = 0x30;       // device: the last word written

#if defined(__APPLE__)
    // Taken ~30 times a draw (GpuRegister, GpuRegisters, GpuFloatConstants): an unfair lock costs less than a pthread
    // mutex there.
    class RingMutex
    {
    public:
        void lock() { os_unfair_lock_lock(&m_lock); }
        void unlock() { os_unfair_lock_unlock(&m_lock); }

    private:
        os_unfair_lock m_lock = OS_UNFAIR_LOCK_INIT;
    };
#else
    using RingMutex = std::mutex;
#endif
    RingMutex g_mutex;
    uint32_t g_cursor;
    uint32_t g_jumps;
    std::array<uint32_t, RegisterCount> g_values;
    std::array<bool, RegisterCount> g_written;
    std::array<uint32_t, RegisterCount> g_epochOf; // g_epoch when written
    uint32_t g_epoch = 1;                          // advanced after each draw (DrawConsumed)
    // Written from an indirect buffer (a shader's own packet) or loaded from memory (LOAD_ALU_CONSTANT): a shader's
    // literal constants, in effect while the shader is used even if the device's shadow of that group is pending.
    std::array<bool, RegisterCount> g_shaderOwned;
    std::vector<d3d::RegisterWrite> g_recorded; // writes into the command buffer being recorded
    std::array<uint32_t, 2 * 256 * 4> g_constants; // GpuFloatConstants, big-endian
    // Per group of 16 float constant registers: g_constants holds the stream's values of a clean group whose registers
    // were all written (it then depends on nothing else), and no write came since.
    // As the device's dirty masks: group g is bit 63 - g % 64 of word g / 64. GpuFloatConstants visits only the groups
    // dirty or not cached (~10 a draw of 128).
    uint64_t g_constantsCached[2];
    uint32_t g_constantsDevice;
    uint64_t g_constantsVersion = 1; // changes with g_constants' content
    std::array<uint32_t, 2 * 256 * 4 / 16> g_groupVersions; // the same per group of 16 registers

    uint32_t Read32(uint32_t address) { return g_memory.Read<uint32_t>(address); }

    void Write(uint32_t reg, uint32_t value, bool recording, uint32_t depth = 0)
    {
        if (reg < FirstRegister || reg >= FirstRegister + RegisterCount)
            return;
        if (recording)
        {
            g_recorded.push_back({ reg, value });
            return;
        }
        g_values[reg - FirstRegister] = value;
        g_written[reg - FirstRegister] = true;
        g_epochOf[reg - FirstRegister] = g_epoch;
        g_shaderOwned[reg - FirstRegister] = depth != 0;
        if (reg >= AluConstants && reg < FetchConstants)
        {
            const uint32_t group = (reg - AluConstants) / 16;
            g_constantsCached[group / 64] &= ~(1ull << (63 - group % 64));
        }
    }

    // Constant packets address their first register by type and offset (as Xenia's command processor reads them).
    uint32_t ConstantBase(uint32_t offsetType)
    {
        switch ((offsetType >> 16) & 0xFF)
        {
        case 0: return AluConstants;
        case 1: return FetchConstants;
        case 2: return BoolConstants;
        case 3: return BoolConstants + 8; // loop constants
        case 4: return FirstRegister;
        default: return 0;
        }
    }

    // Type-0 packets write `count` registers from `reg` (or `count` times `reg` with bit 15); type-3 packets carry
    // `count` data words after an opcode (SET_CONSTANT, SET_CONSTANT2 and LOAD_ALU_CONSTANT write registers); type-2 is
    // a one-word filler; type-1 writes two registers.
    void Parse(uint32_t from, uint32_t to, bool recording, uint32_t depth = 0)
    {
        for (uint32_t at = from; at <= to;)
        {
            uint32_t header = Read32(at);
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            switch (header >> 30)
            {
            case 0:
            {
                uint32_t reg = header & 0x7FFF;
                for (uint32_t i = 0; i < count && at + 4 + i * 4 <= to; i++)
                    Write((header & 0x8000) ? reg : reg + i, Read32(at + 4 + i * 4), recording, depth);
                at += 4 + count * 4;
                break;
            }
            case 1: at += 12; break;
            case 2: at += 4; break;
            default:
            {
                uint32_t opcode = (header >> 8) & 0x7F;
                if (opcode == 0x2D && count >= 2 && at + 4 * count <= to)
                {
                    // SET_CONSTANT: type and 11-bit offset, then the values.
                    uint32_t offsetType = Read32(at + 4);
                    uint32_t base = ConstantBase(offsetType);
                    if (base != 0)
                        for (uint32_t i = 0; i + 1 < count; i++)
                            Write(base + (offsetType & 0x7FF) + i, Read32(at + 8 + i * 4), recording, depth);
                }
                else if (opcode == 0x55 && count >= 2 && at + 4 * count <= to)
                {
                    // SET_CONSTANT2: register index, then the values.
                    uint32_t reg = Read32(at + 4) & 0xFFFF;
                    for (uint32_t i = 0; i + 1 < count; i++)
                        Write(reg + i, Read32(at + 8 + i * 4), recording, depth);
                }
                else if ((opcode == 0x3F || opcode == 0x37) && count >= 2 && at + 8 <= to && depth < 2)
                {
                    // INDIRECT_BUFFER(_PFD): physical address, size in dwords. D3D runs prebuilt packets this way, such
                    // as a shader's (its literal constants).
                    uint32_t target = PhysicalToGuest(Read32(at + 4) & ~3u);
                    uint32_t size = Read32(at + 8) & 0xFFFFF;
                    if (target != 0 && size != 0)
                        Parse(target, target + (size - 1) * 4, recording, depth + 1);
                }
                else if (opcode == 0x2F && count >= 3 && at + 12 <= to)
                {
                    // LOAD_ALU_CONSTANT: physical address, type and offset, size in dwords.
                    uint32_t source = PhysicalToGuest(Read32(at + 4) & 0x3FFFFFFF);
                    uint32_t offsetType = Read32(at + 8);
                    uint32_t size = Read32(at + 12) & 0xFFF;
                    uint32_t base = ConstantBase(offsetType);
                    if (source != 0 && base != 0)
                        for (uint32_t i = 0; i < size; i++)
                            Write(base + (offsetType & 0x7FF) + i, Read32(source + i * 4), recording,
                                depth + 1); // a shader's literals
                }
                at += 4 + count * 4;
                break;
            }
            }
        }
    }

    // Where the device keeps its shadow of `reg` (0 if it has none) and whether it sends it with the next draw.
    uint32_t ShadowOffset(uint32_t reg)
    {
        if (reg >= AluConstants && reg < FetchConstants)
            return d3d::Device::VertexConstants + (reg - AluConstants) * 4;
        if (reg >= FetchConstants && reg < FetchConstants + 0xC0)
            return d3d::Device::FetchConstants + (reg - FetchConstants) * 4;
        if (reg >= BoolConstants && reg < BoolConstants + 0x28)
            return d3d::Device::VertexBooleans + (reg - BoolConstants) * 4;
        return d3d::RegisterOffset(reg);
    }

    // Whether `reg`'s group is set in the five state-group masks at `masks` (the device's dirty masks at +0x00, or a
    // command buffer's inherit tags at +0x18).
    bool GroupBit(uint32_t masks, uint32_t reg)
    {
        auto mask = [&](uint32_t index) { return g_memory.Read<uint64_t>(masks + index * 8); };
        if (reg >= AluConstants && reg < FetchConstants)
        {
            // One bit per 4 float4 (16 registers), bit 63 first; vertex then pixel shader constants.
            uint32_t group = (reg - AluConstants) / 16;
            return (mask(group / 64) >> (63 - group % 64)) & 1;
        }
        if (reg >= FetchConstants && reg < FetchConstants + 0xC0)
            return (mask(3) >> (31 - (reg - FetchConstants) / 6)) & 1; // one bit per 6-dword slot, slot 0 in bit 31
        if (reg >= BoolConstants && reg < BoolConstants + 0x28)
            return (mask(4) >> 56) & 1;
        for (const d3d::RegisterGroup& group : d3d::RegisterGroups)
            if (reg >= group.firstRegister && reg < group.firstRegister + group.count)
                return (mask(group.mask) >> (group.highBit - (reg - group.firstRegister))) & 1;
        return false;
    }

    bool ShadowDirty(uint32_t device, uint32_t reg) { return GroupBit(device, reg); }

    // Registers only the stream has right: those the engine's state blocks and 0x8213BBB8 write (depth control, blend
    // control 0, colour control, mode control, colour mask, blend colour, stencil reference and alpha reference), and
    // the float constants: D3D writes each shader's literal constants (from c255 down) only there, and the engine loads
    // the constant ranges it owns there directly (the device neither shadows nor re-sends them); and the texture fetch
    // constants (slots 0-25), which a command buffer run leaves on the GPU where the buffer inherits them. D3D keeps its
    // shadows right for the other registers, and its own writes into the stream (a resolve's copy draw) would go stale.
    bool FromRing(uint32_t reg)
    {
        return (reg >= 0x2200 && reg <= 0x2202) || reg == 0x2205 || (reg >= 0x2104 && reg <= 0x2108) || reg == 0x210D ||
               reg == 0x210E || (reg >= AluConstants && reg < FetchConstants) ||
               (reg >= FetchConstants && reg < FetchConstants + 26 * 6);
    }

    // The stream's value is the GPU's when the draw hooks scan, after the guest's call: D3D's flush sends every dirty
    // register group. It sends only the float and fetch constants the shaders use, though, and the translated shaders
    // may read others: for those, a value written before this draw yields to a shadow still pending (dirty), except
    // during a replay (the dirty masks are the game's) and for a shader's literal constants.
    bool UseStream(uint32_t device, uint32_t reg)
    {
        uint32_t index = reg - FirstRegister;
        if (!g_written[index])
            return false;
        bool constant = reg >= AluConstants && reg < BoolConstants;
        return !constant || g_epochOf[index] == g_epoch || g_shaderOwned[index] || d3d::Replaying() ||
               !ShadowDirty(device, reg);
    }

    // ShadowOffset and FromRing of every register, computed once (Value runs ~30 times per draw).
    struct RegisterInfo
    {
        uint32_t shadowOffset;
        bool fromRing;
    };
    const std::array<RegisterInfo, RegisterCount> g_registerInfo = [] {
        std::array<RegisterInfo, RegisterCount> table;
        for (uint32_t i = 0; i < RegisterCount; i++)
            table[i] = { ShadowOffset(FirstRegister + i), FromRing(FirstRegister + i) };
        return table;
    }();

    uint32_t Value(uint32_t device, uint32_t reg)
    {
        if (reg < FirstRegister || reg >= FirstRegister + RegisterCount)
        {
            uint32_t offset = ShadowOffset(reg);
            return offset != 0 ? Read32(device + offset) : 0;
        }
        const RegisterInfo& info = g_registerInfo[reg - FirstRegister];
        if (info.fromRing && UseStream(device, reg))
            return g_values[reg - FirstRegister];
        return info.shadowOffset != 0 ? Read32(device + info.shadowOffset) : 0;
    }
}

void d3d::ScanRing(uint32_t device)
{
    // The perf log's ring translation time (gpu::AddFrameTime).
    struct Timer
    {
        uint64_t start = PlatformNowNs();
        ~Timer() { gpu::AddFrameTime(gpu::FrameTime::Ring, PlatformNowNs() - start); }
    } timer;
    std::lock_guard lock(g_mutex);
    uint32_t cursor = Read32(device + WriteCursor);
    uint32_t from = g_cursor;
    g_cursor = cursor;
    if (from == 0 || cursor == from)
        return;
    if (cursor < from || cursor - from > 0x400000)
    {
        g_jumps++;
        return;
    }
    Parse(from + 4, cursor, Read32(device + d3d::Device::RecordingBuffer) != 0);
}

void d3d::ResyncRing(uint32_t device)
{
    std::lock_guard lock(g_mutex);
    g_cursor = Read32(device + WriteCursor);
}

uint32_t d3d::GpuRegister(uint32_t device, uint32_t reg)
{
    std::lock_guard lock(g_mutex);
    return Value(device, reg);
}

void d3d::GpuRegisters(uint32_t device, uint32_t first, uint32_t count, uint32_t* values)
{
    std::lock_guard lock(g_mutex);
    if (first < FetchConstants || first + count > FetchConstants + 26 * 6)
    {
        for (uint32_t i = 0; i < count; i++)
            values[i] = Value(device, first + i);
        return;
    }
    // Fetch constants (all from the stream, FromRing): Value with the slots' dirty bits read once.
    bool replaying = d3d::Replaying();
    uint64_t dirtyMask = g_memory.Read<uint64_t>(device + 3 * 8);
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t reg = first + i, index = reg - FirstRegister;
        bool dirty = (dirtyMask >> (31 - (reg - FetchConstants) / 6)) & 1;
        bool stream = g_written[index] && (g_epochOf[index] == g_epoch || g_shaderOwned[index] || replaying || !dirty);
        values[i] = stream ? g_values[index] : Read32(device + g_registerInfo[index].shadowOffset);
    }
}

const void* d3d::GpuFloatConstants(uint32_t device, uint64_t* version, const uint32_t** groupVersions)
{
    std::lock_guard lock(g_mutex);
    // Value in bulk, per group of 16 registers (4 float4, one dirty bit).
    auto shadow = static_cast<const uint32_t*>(g_memory.Translate(device + d3d::Device::VertexConstants));
    // The dirty bits of the 128 groups (ShadowDirty): masks 0 and 1, bit 63 first.
    bool replaying = d3d::Replaying();
    uint64_t masks[2] = { g_memory.Read<uint64_t>(device), g_memory.Read<uint64_t>(device + 8) };
    if (device != g_constantsDevice)
    {
        g_constantsCached[0] = g_constantsCached[1] = 0;
        g_constantsDevice = device;
        g_constantsVersion++;
        for (uint32_t& groupVersion : g_groupVersions)
            groupVersion++;
    }
    for (uint32_t word = 0; word < 2; word++)
    {
        uint64_t visit = (replaying ? 0 : masks[word]) | ~g_constantsCached[word];
        while (visit != 0)
        {
            const uint32_t bit = 63 - uint32_t(std::countl_zero(visit));
            visit &= ~(1ull << bit);
            const uint32_t group = word * 64 + (63 - bit);
            const uint32_t first = group * 16;
            const bool dirty = !replaying && ((masks[word] >> bit) & 1);
            bool all = true;
            uint32_t changed = 0;
            for (uint32_t i = first; i < first + 16; i++)
            {
                uint32_t index = AluConstants + i - FirstRegister;
                bool stream = g_written[index] && (!dirty || g_epochOf[index] == g_epoch || g_shaderOwned[index]);
                uint32_t value = stream ? ByteSwap(g_values[index]) : shadow[i];
                changed |= value ^ g_constants[i];
                g_constants[i] = value;
                all &= stream;
            }
            if (!dirty && all)
                g_constantsCached[word] |= 1ull << bit;
            else
                g_constantsCached[word] &= ~(1ull << bit);
            g_constantsVersion += changed != 0;
            g_groupVersions[group] += changed != 0;
        }
    }
    if (version != nullptr)
        *version = g_constantsVersion;
    if (groupVersions != nullptr)
        *groupVersions = g_groupVersions.data();
    return g_constants.data();
}

std::vector<d3d::RegisterWrite> d3d::TakeRecordedWrites()
{
    std::lock_guard lock(g_mutex);
    return std::exchange(g_recorded, {});
}

void d3d::ApplyRecordedWrites(uint32_t device, const std::vector<RegisterWrite>& writes)
{
    std::lock_guard lock(g_mutex);
    bool recording =
        Read32(device + d3d::Device::RecordingBuffer) != 0; // a run inside a recording goes into that buffer
    for (const RegisterWrite& write : writes)
        Write(write.reg, write.value, recording);
}

void d3d::SendInheritedShadows(uint32_t device, uint32_t commandBuffer)
{
    std::lock_guard lock(g_mutex);
    // The float and fetch constants the stream has right (FromRing), a dirty-bit group at a time: 16 float constant
    // registers, or one 6-dword fetch slot.
    bool recording = Read32(device + d3d::Device::RecordingBuffer) != 0;
    auto send = [&](uint32_t first, uint32_t end, uint32_t step) {
        for (uint32_t group = first; group < end; group += step)
        {
            if (!GroupBit(device, group) || !GroupBit(commandBuffer + 0x18, group))
                continue;
            for (uint32_t reg = group; reg < group + step; reg++)
                Write(reg, Read32(device + ShadowOffset(reg)), recording);
        }
    };
    send(AluConstants, FetchConstants, 16);
    send(FetchConstants, FetchConstants + 26 * 6, 6);
}

void d3d::DrawConsumed()
{
    std::lock_guard lock(g_mutex);
    g_epoch++;
}

uint32_t d3d::TakeRingJumps()
{
    std::lock_guard lock(g_mutex);
    return std::exchange(g_jumps, 0u);
}

#ifdef GAME_HOOK_D3DDevice_NextRingSegment
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_NextRingSegment));
// The ring is written in segments: this (device) moves the write position to the next one, from
// D3DDevice_ReserveCommandSpace, kick-offs and Swap.
PPC_FUNC(GAME_HOOK(D3DDevice_NextRingSegment))
{
    uint32_t device = ctx.r3.u32;
    d3d::ScanRing(device);
    GAME_IMP(D3DDevice_NextRingSegment)(ctx, base);
    d3d::ResyncRing(device);
}
#endif
