#pragma once

// FSR 3.1's backend over plume (docs/runtime/gpu/upscaling.md): the FidelityFX SDK's host interface (FfxInterface)
// implemented once for Metal, D3D12 and Vulkan. Its resources are plume textures, its passes plume compute pipelines
// built from the SDK's shaders (gpu/fsr/fsr.cmake, framegen.cmake), its jobs recorded into the frame's command list: the
// upscaler, frame interpolation and optical flow. Files including it are built with the SDK's options.
#include <FidelityFX/host/ffx_interface.h>
#include <cstdint>
#include <plume_render_interface_types.h>

namespace gpu
{
    struct Texture;
}

namespace gpu::fsr
{
    // One member of a Metal shader's argument buffer (gpu/fsr/msl_table.cmake).
    struct MslSlot
    {
        const char* name;
        char kind; // c constant buffer, s sampler, t texture, w written texture, b buffer, a atomics of a texture
        uint32_t id;
    };

    // The interface's callbacks (GPU thread, under g_mutex). The command list FFX gets is a plume::RenderCommandList*.
    void MakeInterface(FfxInterface& backend);

    // A texture of ours as FFX takes it (registered for one dispatch).
    FfxResource External(gpu::Texture* texture, bool writable);

    // One of the textures FSR 3.1 wants from the application every dispatch (dilated depth and motion, reconstructed
    // previous depth: ffxFsr3UpscalerGetSharedResourceDescriptions), made like FFX's own; ReleaseAll frees it.
    gpu::Texture* CreateShared(const FfxCreateResourceDescription& create);

    // FFX's name of a format of ours (FFX_SURFACE_FORMAT_UNKNOWN: one FFX is not given).
    FfxSurfaceFormat SurfaceFormat(plume::RenderFormat format);
    // A CreateShared texture goes before ReleaseAll (frame generation's, when its sizes change; the GPU must be idle).
    void DestroyShared(gpu::Texture* texture);

    // Whether the shaders for the running backend are built in.
    bool ShadersAvailable();

    // Every pipeline and resource FFX made goes (after the context is destroyed; the GPU must be idle).
    void ReleaseAll();
    // ReleaseAll and the backend's own pipelines (before the device goes).
    void ReleaseDevice();
}
