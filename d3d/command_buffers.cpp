// Recorded command buffers. Begin marks dirty every state group the buffer does not inherit, so the first recorded
// draw writes them into the buffer, and later draws write what the recording changed. A run therefore draws with the
// device state of the run, overlaid with the groups that were dirty at some recorded draw. Here each recorded draw keeps
// those groups' device bytes; a run writes them over the device, draws, and puts the device's own bytes back (the
// guest's run leaves the device shadows alone too and only marks the groups dirty).
#include "d3d/command_buffers.h"
#include "d3d/device.h"
#include "d3d/ring.h"
#include "game_hooks.h"
#include "memory.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>

using namespace d3d;

namespace
{
    struct Range
    {
        uint32_t offset;
        uint32_t size;
    };

    struct RecordedDraw
    {
        uint32_t primitive;
        uint32_t first;
        uint32_t count;
        bool indexed;
        int32_t baseVertex;
        std::vector<Range> ranges; // device state the buffer sets for this draw
        std::vector<uint8_t> state; // its recorded bytes, range after range
        std::vector<RegisterWrite> writes; // registers written into the buffer since the previous draw, and by this one
    };
#if defined(GAME_HOOK_D3DDevice_EndCommandBuffer) || defined(GAME_HOOK_D3DDevice_RunCommandBuffer)
    constexpr uint32_t WritesOnly =
        ~0u; // RecordedDraw::primitive of the buffer's last entry: what follows its last draw
#endif

    std::mutex g_mutex;
    uint32_t g_recording; // command buffer being recorded, 0 if none
    const struct RecordedDraw* g_replaying; // draw being replayed, nullptr if none (render thread)
    uint64_t g_recordedGroups[DirtyMaskCount];
    // Replaced, not cleared, when re-recorded: a run keeps the draws it started with.
    std::unordered_map<uint32_t, std::shared_ptr<std::vector<RecordedDraw>>> g_buffers;

    // The device bytes behind the given dirty groups, merged.
    std::vector<Range> RangesFor(const uint64_t groups[DirtyMaskCount])
    {
        auto dirty = [&](uint32_t mask, uint32_t bit) { return (groups[mask] >> bit) & 1; };
        std::vector<Range> ranges;

        // Float constants: bit 63 is c0-c3.
        for (uint32_t i = 0; i < 64; i++)
        {
            if (dirty(0, 63 - i))
                ranges.push_back({ Device::VertexConstants + i * 64, 64 });
            if (dirty(1, 63 - i))
                ranges.push_back({ Device::PixelConstants + i * 64, 64 });
        }

        for (const RegisterGroup& group : RegisterGroups)
            for (uint32_t i = 0; i < group.count; i++)
                if (dirty(group.mask, group.highBit - i))
                    ranges.push_back({ RegisterOffset(group.firstRegister + i), 4 });

        // The objects the renderer reads instead of registers: render targets (RB_COLOR_INFO), depth buffer
        // (RB_DEPTH_INFO), shaders and vertex declaration (the shader link bits 17-20).
        if (dirty(2, 57 - 1))
            ranges.push_back({ Device::RenderTargets, 16 });
        if (dirty(2, 57 - 2))
            ranges.push_back({ Device::DepthStencil, 4 });
        if ((groups[2] >> 17) & 0xF)
        {
            ranges.push_back({ Device::VertexDeclaration, 4 });
            ranges.push_back({ Device::PixelShader, 8 }); // and VertexShader
        }

        // Fetch constants (bit 31 is slot 0), with the strides of the vertex streams they hold (stream i fetches
        // from device+0x778 - 8i).
        for (uint32_t slot = 0; slot < 32; slot++)
        {
            if (!dirty(3, 31 - slot))
                continue;
            ranges.push_back({ Device::FetchConstants + slot * 24, 24 });
            for (uint32_t stream = 0; stream < 16; stream++)
                if ((0x778 - 8 * stream - Device::FetchConstants) / 24 == slot)
                    ranges.push_back({ Device::StreamStrides + stream, 1 });
        }

        // Bool and loop constants.
        if (dirty(4, 56))
            ranges.push_back({ Device::VertexBooleans, 0xA0 });

        // The index buffer is part of the draw packet.
        ranges.push_back({ Device::Indices, 4 });

        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.offset < b.offset; });
        std::vector<Range> merged;
        for (const Range& range : ranges)
        {
            if (!merged.empty() && merged.back().offset + merged.back().size >= range.offset)
                merged.back().size = std::max(merged.back().size, range.offset + range.size - merged.back().offset);
            else
                merged.push_back(range);
        }
        return merged;
    }
}

namespace d3d
{
    bool Replaying() { return g_replaying != nullptr; }

    bool RecordDraw(
        uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, bool indexed, int32_t baseVertex)
    {
        std::lock_guard lock(g_mutex);
        if (g_recording == 0)
            return false;

        // What is dirty now is what this draw writes into the buffer.
        for (uint32_t i = 0; i < DirtyMaskCount; i++)
            g_recordedGroups[i] |= g_memory.Read<uint64_t>(device + i * 8);

        RecordedDraw& draw = g_buffers[g_recording]->emplace_back();
        draw = { primitive, first, count, indexed, baseVertex, RangesFor(g_recordedGroups), {}, TakeRecordedWrites() };
        for (const Range& range : draw.ranges)
        {
            auto bytes = static_cast<const uint8_t*>(g_memory.Translate(device + range.offset));
            draw.state.insert(draw.state.end(), bytes, bytes + range.size);
        }
        return true;
    }

    void FinishRecordedDraw()
    {
        std::vector<RegisterWrite> writes = TakeRecordedWrites();
        std::lock_guard lock(g_mutex);
        if (g_recording != 0 && !g_buffers[g_recording]->empty())
        {
            std::vector<RegisterWrite>& own = g_buffers[g_recording]->back().writes;
            own.insert(own.end(), writes.begin(), writes.end());
        }
    }
}

#ifdef GAME_HOOK_D3DDevice_BeginCommandBuffer
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_BeginCommandBuffer));
// D3DDevice_BeginCommandBuffer(device, commandBuffer)
PPC_FUNC(GAME_HOOK(D3DDevice_BeginCommandBuffer))
{
    uint32_t device = ctx.r3.u32, commandBuffer = ctx.r4.u32;
    ScanRing(device);
    GAME_IMP(D3DDevice_BeginCommandBuffer)(ctx, base);
    ResyncRing(device); // now writing into the command buffer, which runs later
    TakeRecordedWrites();
    std::lock_guard lock(g_mutex);
    g_recording = commandBuffer;
    std::fill(std::begin(g_recordedGroups), std::end(g_recordedGroups), 0);
    g_buffers[commandBuffer] = std::make_shared<std::vector<RecordedDraw>>();
}
#endif

#ifdef GAME_HOOK_D3DDevice_EndCommandBuffer
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_EndCommandBuffer));
// D3DDevice_EndCommandBuffer(device)
PPC_FUNC(GAME_HOOK(D3DDevice_EndCommandBuffer))
{
    uint32_t device = ctx.r3.u32;
    ScanRing(device); // the state the last draw sent: it stays on the GPU after a run where the buffer inherits it
    GAME_IMP(D3DDevice_EndCommandBuffer)(ctx, base);
    ResyncRing(device); // back in the ring
    std::lock_guard lock(g_mutex);
    if (g_recording != 0)
        g_buffers[g_recording]->push_back({ WritesOnly, 0, 0, false, 0, {}, {}, TakeRecordedWrites() });
    g_recording = 0;
}
#endif

#ifdef GAME_HOOK_D3DDevice_RunCommandBuffer
// D3DDevice_RunCommandBuffer(device, commandBuffer): the guest's version emits INDIRECT_BUFFER packets nobody executes,
// so it is replaced. Like it, afterwards the device marks dirty every group the buffer does not persist (the tags at
// +0x40..+0x60), to send them again before the next draw; the groups it inherits keep what the buffer wrote on the
// GPU (d3d/ring.h: the buffer's register writes are applied to the mirror), such as the texture the autosave panel
// draws with.
PPC_FUNC(GAME_HOOK(D3DDevice_RunCommandBuffer))
{
    uint32_t device = ctx.r3.u32, commandBuffer = ctx.r4.u32;
    ScanRing(device);
    std::shared_ptr<const std::vector<RecordedDraw>> draws;
    {
        std::lock_guard lock(g_mutex);
        auto buffer = g_buffers.find(commandBuffer);
        if (buffer == g_buffers.end())
        {
            static bool reported;
            if (!reported)
                fprintf(stderr, "[render] run of command buffer 0x%08X, which was not recorded\n", commandBuffer);
            reported = true;
            return;
        }
        draws = buffer->second;
    }
    SendInheritedShadows(device, commandBuffer);

    std::vector<uint8_t> saved;
    for (const RecordedDraw& draw : *draws)
    {
        if (draw.primitive == WritesOnly)
        {
            ApplyRecordedWrites(device, draw.writes);
            continue;
        }
        saved.resize(draw.state.size());
        size_t at = 0;
        for (const Range& range : draw.ranges)
        {
            void* bytes = g_memory.Translate(device + range.offset);
            memcpy(saved.data() + at, bytes, range.size);
            memcpy(bytes, draw.state.data() + at, range.size);
            at += range.size;
        }
        ApplyRecordedWrites(device, draw.writes);
        g_replaying = &draw;
        Draw(device, draw.primitive, draw.first, draw.count, draw.indexed, draw.baseVertex);
        DrawConsumed();
        g_replaying = nullptr;
        at = 0;
        for (const Range& range : draw.ranges)
        {
            memcpy(g_memory.Translate(device + range.offset), saved.data() + at, range.size);
            at += range.size;
        }
    }
    for (uint32_t i = 0; i < DirtyMaskCount; i++)
        g_memory.Write<uint64_t>(device + i * 8, ~g_memory.Read<uint64_t>(commandBuffer + 0x40 + i * 8));
}
#endif
