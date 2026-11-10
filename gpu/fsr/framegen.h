#pragma once

// FSR 3.1 frame generation (docs/runtime/gpu/upscaling.md, "Frame generation"): one frame made between two real ones by
// the FidelityFX SDK's frame interpolation and optical flow, from the upscaler's dilated depth and motion, the frame
// without its HUD (copied right after the composite) and the frame as presented. gpu.cpp presents it, then the real
// frame half a real frame later. [<game>.video] fsr_frame_generation = off | on; it needs upscaler = fsr3.
#include "gpu/gpu.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace gpu::framegen
{
    // The setting (any thread): applied at the next frame.
    void SetEnabled(bool on);
    bool Enabled();
    // Off whatever the setting (framegen::HeldBack: held back for players, or not clean in this game yet).
    void SetBlocked(bool blocked);
    bool Blocked();
    // SKY_FSR_FRAME_GENERATION_PREVIEW is set: a test may run it (framegen::HeldBack).
    bool Preview();
    // Whether this device can (after gpu::Init): FSR's backend and the frame generation shaders are there.
    bool Available();

    // The producer's side, queued like a draw: the frame target right after the composite (where the HUD begins);
    // nullptr: this frame is one of the game's menus, where no frame is made.
    void CaptureHudless(Texture* frame);

    // What one generated frame showed, for the log and the tests: the counts since the start.
    struct Stats
    {
        uint64_t real = 0, generated = 0;
        // Frames presented without one before them, by reason.
        uint64_t noScene = 0, reset = 0, noHudless = 0, display = 0;
    };
    Stats Counts();

    // SKY_FSR_FG_SNAPSHOT: called on the GPU thread with each kept frame (`kind`: "gen", or "real" for the real frame
    // after it), RGBA8. The host writes the files (host/display.cpp).
    void SetSnapshotHook(std::function<void(
            const char* kind, uint64_t index, const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height)>
            hook);
}

namespace gpu::framegen::detail
{
    // FSR's upscale of this frame (gpu/fsr/fsr.cpp, GPU thread): the inputs frame interpolation shares with it.
    struct Scene
    {
        uint32_t renderWidth, renderHeight;
        float jitter[2];
        float cameraNear, cameraFar, fovY;
        bool reset;
        float shift;    // how far the camera moved the picture (::framegen::CameraShift); a cut above cutShift
        float cutShift; // gpu::UpscaleDesc::cutShift
        Texture* dilatedDepth;
        Texture* dilatedMotion;
        Texture* previousDepth;
    };
    void Upscaled(const Scene& scene);

    // gpu.cpp's PresentNow (GPU thread, under g_mutex), before it presents `image`: records the optical flow and the
    // interpolation (in a command list of their own) and returns the frame to present first, or nullptr (no scene this frame, no copy without
    // the HUD, a reset).
    const Texture* Generate(const Texture* image);
    // After the generated frame is presented: how long to wait before the real one (half the real frames' interval,
    // measured), in nanoseconds.
    uint64_t RealFrameDelayNs();
    // A frame went out (generated or real), while frame generation is on.
    void Shown();
    // After the real frame that followed a generated one: how long it was held (the latency added, in the log), and
    // SKY_FSR_FG_SNAPSHOT's read-backs of both (offscreen only).
    void RealPresented(const Texture* generated, const Texture* real);

    // Before FSR's context goes (its shared textures with it) and before the device goes: the GPU must be idle.
    void Release();
}
