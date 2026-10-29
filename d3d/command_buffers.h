#pragma once

// Recorded command buffers (D3DDevice_BeginCommandBuffer .. EndCommandBuffer, replayed by RunCommandBuffer): the
// engine records one per mesh and material and runs it every frame. See docs/runtime/gpu/command_buffers.md (section "workshop notes").
#include <cstdint>

namespace d3d
{
    // While the game records a command buffer, keeps a draw for its runs instead of drawing it. False when nothing
    // is being recorded.
    bool RecordDraw(
        uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, bool indexed, int32_t baseVertex);
    // After the guest's draw call and a ScanRing: the register writes it put into the buffer belong to the recorded draw.
    void FinishRecordedDraw();

    // render.cpp: draws with the device state as it is now.
    void Draw(uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, bool indexed, int32_t baseVertex);

    // A recorded draw is being replayed (the device's dirty masks are then the game's, not the draw's).
    bool Replaying();
}
