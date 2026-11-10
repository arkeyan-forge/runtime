// Temporal upscaling of the 3D scene with FSR 3.1 (docs/runtime/gpu/upscaling.md): [<game>.video] upscaler = fsr3.
//
// The scene renders at the resolution scale with a sub-pixel jitter. At the composite (the draw that reads the scene
// into the 1280x720 frame: where the 3D ends and the HUD begins, d3d/ultrawide.h) the upscaler turns the scene into a
// picture at the frame's scale, with the scene's depth and the camera's motion, and the composite reads that instead.
// The frame, and the HUD the game draws into it, are at the frame's scale. Nothing changes while it is off.
#pragma once

#include "gpu/upscale_mode.h"
#include <array>
#include <cstdint>

namespace gpu
{
    struct Texture;
}

namespace upscale
{
    // The frame's scale is the window's height over 720, rounded (SKY_UPSCALE_SCALE=<n> sets it); the scene's comes from
    // the preset (upscalemode::SceneScale) and replaces gpu::g_settings.resolutionScale. On only when the backend has
    // the upscaler. Once, after gpu::Init, before the first frame.
    // `sceneIsFrame`: the game renders its scene at the frame's own size this run (game::Adapter::fullScene), where FSR
    // cannot tell the two apart: it can then only be turned on at the next start (SwitchesLive).
    void Configure(
        uint32_t windowHeight, upscalemode::Upscaler upscaler, upscalemode::Quality quality, bool sceneIsFrame = false);
    // Whether the F3 menu's Upscaling row applies at once (else at the next start).
    bool SwitchesLive();
    // The F3 menu: this upscaler and preset from the next frame on. The scales change at the swap (FrameEnd).
    void Request(upscalemode::Upscaler upscaler, upscalemode::Quality quality);

    bool Active();
    uint32_t SceneScale();
    // The window's frame scale (what FSR upscales to), whether or not it is on.
    uint32_t WindowFrameScale();
    uint32_t FrameScale();

    // This frame's jitter, in host pixels of the scene (x right, y down).
    std::array<float, 2> Jitter();

    // The scene's texture LOD bias: log2(scene scale / frame scale), negative (what temporal upscalers advise).
    float MipBias();

    // A draw into the scene with the depth test on (`greaterTest`: it passes the farther window depth, reversed Z). `identity` (0: none) names the same draw across frames; `rows` are
    // the four constants of its position matrix in the row-vector convention (nullptr: unknown); `weight` its size.
    void SceneDraw(uint64_t identity, const float* rows, uint32_t weight, gpu::Texture* depth, float minDepth,
        float maxDepth, bool greaterTest = false);

    // The composite reads `scene`: queues its upscale and returns the texture to read instead (nullptr: keep `scene`).
    gpu::Texture* Composite(gpu::Texture* scene);

    // The swap: the next frame begins. True when the scales changed (a request): the caller drops its render targets,
    // which the next frame makes again at the new scales.
    bool FrameEnd();
}
