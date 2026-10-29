#pragma once

// The game's Direct3D device (XDK D3D9LTCG, statically linked), as the replacement renderer reads it. The setters only
// write this structure (and many are inlined into game code), so state is read here at draw time. Layout: docs/runtime/gpu/d3d9ltcg_device.md (section "Earlier partial D3D map").
#include "game_hooks.h"
#include <cstdint>

namespace d3d
{
    // From the vertex declaration on, the fields sit game::DeviceStateShift bytes later in newer XDKs (Giants: D3D9LTCG
    // 2.0.21076, +0x80; games/giants/docs/boot.md). The constants, register shadows and fence words before it do not move.
    namespace Device
    {
        constexpr uint32_t FetchConstants = 0x480; // 32 x 6 dwords: texture fetch constants (vertex fetch shares them)
        constexpr uint32_t VertexConstants = 0x780; // 256 float4
        constexpr uint32_t PixelConstants = 0x1780; // 256 float4
        constexpr uint32_t VertexBooleans = 0x2780; // 128 bits
        constexpr uint32_t PixelBooleans = 0x2790; // 128 bits
        constexpr uint32_t LoopConstants = 0x27A0;
        constexpr uint32_t VertexDeclaration =
            game::DeviceVertexDeclaration; // SSA 0x2F58, Giants 0x2FD0 (+0x78, not +0x80)
        constexpr uint32_t Indices = 0x318C + game::DeviceStateShift;
        constexpr uint32_t RenderTargets = 0x3190 + game::DeviceStateShift; // 4 surfaces
        constexpr uint32_t DepthStencil = 0x31A0 + game::DeviceStateShift;
        constexpr uint32_t Streams = 0x31A4 + game::DeviceStateShift; // 16 vertex buffers
        constexpr uint32_t StreamStrides = 0x31E8 + game::DeviceStateShift; // 16 bytes
        constexpr uint32_t Textures = 0x31F8 + game::DeviceStateShift; // 26 textures
        constexpr uint32_t Viewport = 0x3260 + game::DeviceStateShift; // float x, y, width, height, minZ, maxZ
        constexpr uint32_t Scissor = 0x327C + game::DeviceStateShift; // left, top, right, bottom
        constexpr uint32_t PixelShader = 0x328C + game::DeviceStateShift;
        constexpr uint32_t VertexShader = 0x3290 + game::DeviceStateShift;
        constexpr uint32_t ScissorTestEnable = 0x2F80 + game::DeviceStateShift; // D3DRS_SCISSORTESTENABLE
        constexpr uint32_t RecordingBuffer =
            0x35E0 + game::DeviceStateShift; // command buffer being recorded, 0 if none
        constexpr uint32_t TileCount =
            0x32C4 + game::DeviceStateShift; // predicated tiling: tile count, then the tiles'
        constexpr uint32_t TileRects =
            0x32C8 + game::DeviceStateShift; // D3DRECTs (left, top, right, bottom); seen in Giants
        constexpr uint32_t PresentInterval = 0x3630 + game::DeviceStateShift; // D3DRS_PRESENTINTERVAL
    }

    // Resource header: +0 Common (low 4 bits: 1 VB, 2 IB, 3 texture, 4 surface, 5 declaration, 6 VS, 7 PS), +4 refcount.
    namespace Resource
    {
        constexpr uint32_t VertexBufferFetch = 0x18; // vertex fetch constant: address dword, size dword
        constexpr uint32_t IndexBufferAddress = 0x18;
        constexpr uint32_t TextureFetch = 0x1C; // 6-dword texture fetch constant
        constexpr uint32_t SurfaceInfo = 0x1C; // RB_COLOR_INFO / RB_DEPTH_INFO word
        constexpr uint32_t SurfaceSize = 0x24;
    }

    // Xenos registers the device keeps shadow copies of.
    namespace Reg
    {
        constexpr uint32_t RB_SURFACE_INFO = 0x2000;
        constexpr uint32_t RB_COLOR_INFO = 0x2001;
        constexpr uint32_t RB_DEPTH_INFO = 0x2002;
        constexpr uint32_t RB_COLOR_MASK = 0x2104;
        constexpr uint32_t RB_BLEND_RED = 0x2105; // to RB_BLEND_ALPHA (0x2108)
        constexpr uint32_t RB_STENCILREFMASK = 0x210D;
        constexpr uint32_t RB_ALPHA_REF = 0x210E;
        constexpr uint32_t SQ_PROGRAM_CNTL = 0x2180;
        constexpr uint32_t SQ_CONTEXT_MISC = 0x2181;
        constexpr uint32_t RB_DEPTHCONTROL = 0x2200;
        constexpr uint32_t RB_BLENDCONTROL0 = 0x2201;
        constexpr uint32_t RB_COLORCONTROL = 0x2202;
        constexpr uint32_t PA_CL_CLIP_CNTL = 0x2204;
        constexpr uint32_t PA_SU_SC_MODE_CNTL = 0x2205;
        constexpr uint32_t PA_CL_VTE_CNTL = 0x2206;
        constexpr uint32_t RB_MODECONTROL = 0x2208;
        constexpr uint32_t PA_SC_AA_CONFIG = 0x2301;
        constexpr uint32_t RB_COPY_CONTROL = 0x2318;
        constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
        constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
        constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
        constexpr uint32_t RB_DEPTH_CLEAR = 0x231D;
        constexpr uint32_t RB_COLOR_CLEAR = 0x231E;
    }

    uint32_t CurrentDevice();

    // Device offset of the shadow of Xenos register `reg`, 0 when the device does not shadow it.
    uint32_t RegisterOffset(uint32_t reg);

    // Shadow value of Xenos register `reg`, or 0 when the device does not shadow it.
    uint32_t ReadRegister(uint32_t device, uint32_t reg);

    // Dirty masks at device+0x00..+0x20 (VS constants, PS constants, then three masks of register groups and fetch
    // constants): the register groups, each listing its first register in its highest bit (docs/runtime/gpu/command_buffers.md (section "workshop notes")).
    constexpr uint32_t DirtyMaskCount = 5;
    struct RegisterGroup
    {
        uint32_t mask;
        uint32_t highBit;
        uint32_t firstRegister;
        uint32_t count;
    };
    constexpr RegisterGroup RegisterGroups[] = {
        { 2, 11, 0x2200, 12 },
        { 2, 16, 0x2180, 5 },
        { 2, 41, 0x2100, 21 },
        { 2, 57, 0x2000, 16 },
        { 3, 54, 0x2280, 21 },
        { 4, 37, 0x2300, 38 },
        { 4, 45, 0x2380, 8 },
    };
}
