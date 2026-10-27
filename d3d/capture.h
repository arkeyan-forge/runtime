#pragma once

// SSA_D3D_CAPTURE=<seconds> or f<N>: logs one frame's draws, clears, resolves and swap with the device state
// (d3d/capture.cpp). The renderer's hooks call these on entry, with the guest arguments.
#include <ppc_config.h>
#include <ppc_context.h>

namespace d3d
{
    void CaptureDrawVertices(const PPCContext& ctx);
    void CaptureDrawIndexedVertices(const PPCContext& ctx);
    void CaptureBeginVertices(const PPCContext& ctx);
    void CaptureClear(const PPCContext& ctx);
    void CaptureResolve(const PPCContext& ctx);
    void CaptureSwap(const PPCContext& ctx);
}
