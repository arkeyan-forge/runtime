#pragma once

// The PM4 stream the game's Direct3D keeps writing (nothing executes it) is what the GPU would receive, and it is the
// reference for the GPU's registers: the engine copies prebuilt state blocks into it (material depth/stencil, blend,
// colour and cull control, stencil and alpha reference, built by 0x828FC670, and shader constants) and writes registers
// itself (0x8213BBB8), past the device's shadow copies. The register writes in it are mirrored here: context registers
// 0x2000-0x23FF, ALU constants 0x4000-0x47FF, fetch constants 0x4800-0x48BF, bool and loop constants 0x4900-0x4927.
#include <cstdint>
#include <vector>

namespace d3d
{
    // Reads the packets written since the last call. While a command buffer is being recorded they run later: their
    // register writes are kept for the recorded draw instead (TakeRecordedWrites).
    void ScanRing(uint32_t device);

    // The write position moved elsewhere (another ring segment, a command buffer recording began or ended): continue
    // from where it is now.
    void ResyncRing(uint32_t device);

    // Value of register `reg` the GPU uses for the draw the guest just sent: the draw hooks scan after the guest's call,
    // which writes the state it sends into the stream. For the registers the stream has right (ring.cpp FromRing): the
    // stream's value; for float and fetch constants written before this draw (the last DrawConsumed), the shadow while
    // it is still dirty outside a replay (D3D sends only the constants the shaders use). Others: the shadow.
    uint32_t GpuRegister(uint32_t device, uint32_t reg);
    // GpuRegister of `count` registers from `first`, into `values`.
    void GpuRegisters(uint32_t device, uint32_t first, uint32_t count, uint32_t* values);

    // The 512 float constants (vertex then pixel shader) the same way, in the device's layout (big-endian), valid until
    // the next call. `version`: the same number as long as the content is the same; `groupVersions`: the same per group
    // of 16 registers (128 of them, valid until the next call).
    const void* GpuFloatConstants(uint32_t device, uint64_t* version = nullptr, const uint32_t** groupVersions = nullptr);

    // Register writes a command buffer recorded since the last call, in order, and applying them when it runs.
    struct RegisterWrite
    {
        uint32_t reg;
        uint32_t value;
    };
    std::vector<RegisterWrite> TakeRecordedWrites();
    void ApplyRecordedWrites(uint32_t device, const std::vector<RegisterWrite>& writes);

    // RunCommandBuffer first sends the dirty groups the buffer inherits (its tags at +0x18) from the shadows.
    void SendInheritedShadows(uint32_t device, uint32_t commandBuffer);

    // The draw the last scan belongs to has been built: later writes belong to the next one.
    void DrawConsumed();

    // Scans that found the write position moved without a resync (packets lost), since the last call.
    uint32_t TakeRingJumps();
}
