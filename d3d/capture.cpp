// SKY_D3D_CAPTURE=<seconds> (or f<N>: after the N-th swap): logs every draw, clear, resolve and swap of the first frame
// that starts after that time ("[capture]" lines): bound shaders, render targets, vertex declaration and streams, textures and the main render
// registers, as the game's device holds them. A development aid for the Direct3D replacement.
#include "d3d/capture.h"
#include "d3d/device.h"
#include "d3d/ring.h"
#include "d3d/shaders.h"
#include "memory.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <ppc_config.h>
#include <ppc_context.h>
#include "../host/env.h"

namespace
{
    using Clock = std::chrono::steady_clock;

    std::mutex g_mutex;
    bool g_capturing;
    bool g_done;
    uint32_t g_drawIndex;
    std::set<uint32_t> g_declarationsShown;

    double CaptureTime()
    {
        static const char* value = skyenv::Get("SKY_D3D_CAPTURE");
        return value != nullptr ? atof(value[0] == 'f' ? "0" : value) : -1.0;
    }

    // "f<N>": the frame that starts after the N-th swap instead of a time.
    uint64_t CaptureFrame()
    {
        static const char* value = skyenv::Get("SKY_D3D_CAPTURE");
        return value != nullptr && value[0] == 'f' ? strtoull(value + 1, nullptr, 10) : 0;
    }

    uint64_t g_swaps;

    double SecondsSinceStart()
    {
        static const Clock::time_point start = Clock::now();
        return std::chrono::duration<double>(Clock::now() - start).count();
    }

    uint32_t Read32(uint32_t address) { return g_memory.Read<uint32_t>(address); }

    void LogShader(const char* name, uint32_t object)
    {
        const d3d::GuestShader* shader = d3d::FindGuestShader(object);
        if (shader != nullptr)
            fprintf(stderr, " %s=%08X(%016llX)", name, object, (unsigned long long)shader->hash);
        else
            fprintf(stderr, " %s=%08X(?)", name, object);
    }

    void LogDeclaration(uint32_t declaration)
    {
        fprintf(stderr, "[capture]   declaration %08X", declaration);
        if (declaration == 0 || !g_declarationsShown.insert(declaration).second)
        {
            fprintf(stderr, "\n");
            return;
        }
        fprintf(stderr, " raw:");
        for (uint32_t offset = 0; offset < 0x38 + 12 * 8; offset += 4)
            fprintf(stderr, " %08X", Read32(declaration + offset));
        fprintf(stderr, "\n");
    }

    void LogState(uint32_t device)
    {
        fprintf(stderr, "[capture]  ");
        LogShader("vs", Read32(device + d3d::Device::VertexShader));
        LogShader("ps", Read32(device + d3d::Device::PixelShader));
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t surface = Read32(device + d3d::Device::RenderTargets + i * 4);
            if (surface != 0)
                fprintf(stderr, " rt%u=%08X(%08X %08X %08X %08X %08X)", i, surface, Read32(surface + 0x18),
                    Read32(surface + 0x1C), Read32(surface + 0x20), Read32(surface + 0x24), Read32(surface + 0x28));
        }
        uint32_t depth = Read32(device + d3d::Device::DepthStencil);
        if (depth != 0)
            fprintf(stderr, " ds=%08X(%08X %08X %08X %08X %08X)", depth, Read32(depth + 0x18), Read32(depth + 0x1C),
                Read32(depth + 0x20), Read32(depth + 0x24), Read32(depth + 0x28));
        fprintf(stderr, "\n");

        using namespace d3d::Reg;
        fprintf(stderr,
            "[capture]   regs surface %08X color %08X depth %08X mask %08X depthctl %08X blend0 %08X colorctl %08X "
            "mode %08X vte %08X clip %08X modectl %08X alpharef %08X stencilref %08X\n",
            d3d::ReadRegister(device, RB_SURFACE_INFO), d3d::ReadRegister(device, RB_COLOR_INFO),
            d3d::ReadRegister(device, RB_DEPTH_INFO), d3d::ReadRegister(device, RB_COLOR_MASK),
            d3d::ReadRegister(device, RB_DEPTHCONTROL), d3d::ReadRegister(device, RB_BLENDCONTROL0),
            d3d::ReadRegister(device, RB_COLORCONTROL), d3d::ReadRegister(device, PA_SU_SC_MODE_CNTL),
            d3d::ReadRegister(device, PA_CL_VTE_CNTL), d3d::ReadRegister(device, PA_CL_CLIP_CNTL),
            d3d::ReadRegister(device, RB_MODECONTROL), d3d::ReadRegister(device, RB_ALPHA_REF),
            d3d::ReadRegister(device, RB_STENCILREFMASK));

        // What the GPU gets where the engine writes past the shadows (d3d/ring.h); the draw hooks scan the ring first.
        fprintf(stderr,
            "[capture]   gpu depthctl %08X stencilref %08X blend0 %08X colorctl %08X mode %08X alpharef %08X mask "
            "%08X\n",
            d3d::GpuRegister(device, RB_DEPTHCONTROL), d3d::GpuRegister(device, RB_STENCILREFMASK),
            d3d::GpuRegister(device, RB_BLENDCONTROL0), d3d::GpuRegister(device, RB_COLORCONTROL),
            d3d::GpuRegister(device, PA_SU_SC_MODE_CNTL), d3d::GpuRegister(device, RB_ALPHA_REF),
            d3d::GpuRegister(device, RB_COLOR_MASK));

        float viewport[6];
        for (uint32_t i = 0; i < 6; i++)
            viewport[i] = g_memory.Read<float>(device + d3d::Device::Viewport + i * 4);
        fprintf(stderr, "[capture]   viewport %.1f %.1f %.1f %.1f z %.2f..%.2f scissor %u %u %u %u\n", viewport[0],
            viewport[1], viewport[2], viewport[3], viewport[4], viewport[5], Read32(device + d3d::Device::Scissor),
            Read32(device + d3d::Device::Scissor + 4), Read32(device + d3d::Device::Scissor + 8),
            Read32(device + d3d::Device::Scissor + 12));

        LogDeclaration(Read32(device + d3d::Device::VertexDeclaration));
        for (uint32_t stream = 0; stream < 16; stream++)
        {
            uint32_t buffer = Read32(device + d3d::Device::Streams + stream * 4);
            if (buffer != 0)
                fprintf(stderr, "[capture]   stream %u vb %08X fetch %08X %08X stride %u\n", stream, buffer,
                    Read32(buffer + d3d::Resource::VertexBufferFetch),
                    Read32(buffer + d3d::Resource::VertexBufferFetch + 4),
                    g_memory.Read<uint8_t>(device + d3d::Device::StreamStrides + stream));
        }
        for (uint32_t slot = 0; slot < 26; slot++)
        {
            uint32_t texture = Read32(device + d3d::Device::Textures + slot * 4);
            if (texture == 0)
                continue;
            fprintf(stderr, "[capture]   tex %u %08X common %08X fetch", slot, texture, Read32(texture));
            for (uint32_t i = 0; i < 6; i++)
                fprintf(stderr, " %08X", Read32(device + d3d::Device::FetchConstants + slot * 24 + i * 4));
            fprintf(stderr, "\n");
        }
    }

    // Draws, resolves and swaps of the captured frame are logged from the game's render thread, one at a time.
    template <typename Body>
    void Capture(Body&& body)
    {
        if (CaptureTime() < 0)
            return;
        std::lock_guard lock(g_mutex);
        if (g_capturing)
            body();
    }
}

// D3DDevice_DrawVertices(device, primitiveType, startVertex, vertexCount)
void d3d::CaptureDrawVertices(const PPCContext& ctx)
{
    Capture([&] {
        fprintf(stderr, "[capture] draw %u DrawVertices prim %u start %u count %u\n", g_drawIndex++, ctx.r4.u32,
            ctx.r5.u32, ctx.r6.u32);
        LogState(ctx.r3.u32);
    });
}

// D3DDevice_DrawIndexedVertices(device, primitiveType, baseVertex, startIndex, indexCount)
void d3d::CaptureDrawIndexedVertices(const PPCContext& ctx)
{
    Capture([&] {
        uint32_t indices = Read32(ctx.r3.u32 + d3d::Device::Indices);
        fprintf(stderr,
            "[capture] draw %u DrawIndexedVertices prim %u base %u start %u count %u ib %08X (common %08X addr %08X)\n",
            g_drawIndex++, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, indices, indices ? Read32(indices) : 0,
            indices ? Read32(indices + d3d::Resource::IndexBufferAddress) : 0);
        LogState(ctx.r3.u32);
    });
}

// D3DDevice_BeginVertices(device, primitiveType, vertexCount, stride) -> pointer the game fills
void d3d::CaptureBeginVertices(const PPCContext& ctx)
{
    Capture([&] {
        fprintf(stderr, "[capture] draw %u BeginVertices prim %u count %u stride %u\n", g_drawIndex++, ctx.r4.u32,
            ctx.r5.u32, ctx.r6.u32);
        LogState(ctx.r3.u32);
    });
}

// D3DDevice_ClearF's worker (device, flags, rect, color float4, -, stencil, f1 = z)
void d3d::CaptureClear(const PPCContext& ctx)
{
    Capture([&] {
        uint32_t rect = ctx.r5.u32, color = ctx.r6.u32;
        fprintf(stderr, "[capture] clear flags %08X rect (%d %d %d %d) color", ctx.r4.u32, Read32(rect),
            Read32(rect + 4), Read32(rect + 8), Read32(rect + 12));
        for (uint32_t i = 0; i < 4; i++)
            fprintf(stderr, " %g", color != 0 ? g_memory.Read<float>(color + i * 4) : 0.0f);
        fprintf(stderr, " z %g stencil %u\n", ctx.f1.f64, ctx.r8.u32);
        LogState(ctx.r3.u32);
    });
}

// D3DDevice_Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, f1 = clearZ, ...)
void d3d::CaptureResolve(const PPCContext& ctx)
{
    Capture([&] {
        uint32_t rect = ctx.r5.u32;
        uint32_t texture = ctx.r6.u32;
        fprintf(stderr, "[capture] resolve flags %08X rect %08X", ctx.r4.u32, rect);
        if (rect != 0)
            fprintf(stderr, "(%d %d %d %d)", Read32(rect), Read32(rect + 4), Read32(rect + 8), Read32(rect + 12));
        fprintf(stderr, " dest %08X", texture);
        if (texture != 0)
        {
            fprintf(stderr, "(common %08X fetch", Read32(texture));
            for (uint32_t i = 0; i < 6; i++)
                fprintf(stderr, " %08X", Read32(texture + d3d::Resource::TextureFetch + i * 4));
            fprintf(stderr, ")");
        }
        fprintf(stderr, " point %08X level %u slice %u clearColor %08X clearZ %f\n", ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
            ctx.r10.u32, ctx.f1.f64);
        LogState(ctx.r3.u32);
    });
}

// D3DDevice_Swap(device, frontBufferTexture, ...): the frame boundary.
void d3d::CaptureSwap(const PPCContext& ctx)
{
    if (CaptureTime() >= 0)
    {
        std::lock_guard lock(g_mutex);
        if (g_capturing)
        {
            fprintf(stderr, "[capture] swap front %08X r5 %08X; end of capture (%u draws)\n", ctx.r4.u32, ctx.r5.u32,
                g_drawIndex);
            g_capturing = false;
            g_done = true;
        }
        else if (!g_done && ++g_swaps >= CaptureFrame() && SecondsSinceStart() >= CaptureTime())
        {
            fprintf(stderr, "[capture] frame starts after swap %llu at %.1f s\n", (unsigned long long)g_swaps,
                SecondsSinceStart());
            g_capturing = true;
        }
    }
}
