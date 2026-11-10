#pragma once

// FSR 3.1 upscaling on every backend (docs/runtime/gpu/upscaling.md): the game's picture, depth and camera motion in,
// an upscaled, anti-aliased picture out. gpu/upscale.cpp calls it on the GPU thread.
#include "gpu/upscale.h"

namespace gpu::fsr
{
    // The running backend has FSR's shaders (after gpu::Init).
    bool Available();

    // Records the inputs pass and FSR's passes into the frame's command list (GPU thread, under g_mutex). False if FSR
    // failed: it then stays off for the run, and the caller keeps the picture as it is.
    bool Upscale(const UpscaleDesc& desc);

    // Before the device goes away.
    void Shutdown();
}
