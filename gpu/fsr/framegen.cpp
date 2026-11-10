#include "gpu/fsr/framegen.h"
#include "gpu/context.h"
#include "gpu/fsr/backend.h"
#include "gpu/fsr/framegen_rules.h"
#include "host/env.h"
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

using namespace gpu::detail;
using namespace plume;

namespace
{
    std::atomic<bool> g_requested, g_blocked;
    std::atomic<bool> g_enabled; // requested and not blocked
    std::atomic<bool> g_uneven; // the frames shown were uneven (the GPU too slow): off until the setting is set again

    struct State
    {
        FfxInterface backend{};
        FfxFrameInterpolationContext fi{};
        FfxOpticalflowContext of{};
        bool created = false, failed = false;
        uint32_t renderWidth = 0, renderHeight = 0, displayWidth = 0, displayHeight = 0;
        RenderFormat format = RenderFormat::UNKNOWN; // the frame's (and the copy without its HUD)
        gpu::Texture* flowVector = nullptr; // optical flow's outputs, which interpolation reads
        gpu::Texture* flowChange = nullptr;
        gpu::Texture* generated = nullptr; // the frame between the last two real ones
        // The frame target right after the composite, and the GPU thread's frame it was copied in.
        gpu::Texture* hudless = nullptr;
        uint64_t hudlessFrame = UINT64_MAX;
        uint64_t menuFrame = UINT64_MAX; // a frame of the game's menus (CaptureHudless(nullptr))
        // This frame's upscale (gpu/fsr/fsr.cpp).
        gpu::framegen::detail::Scene scene{};
        uint64_t sceneFrame = UINT64_MAX;
        float lastShift = 0.0f; // the camera shift of the last frame with a scene (::framegen::CameraJump)
        bool history = false; // the last real frame went through optical flow and interpolation: they can interpolate
        uint64_t lastFrame = UINT64_MAX - 1; // the GPU thread's frame of the last Generate
        uint64_t frameId = 0;
        ::framegen::Pacing pacing;
        gpu::framegen::Stats stats;
        uint64_t logNs = 0;
        ::framegen::UnevenGuard uneven;
        uint64_t arrivalNs = 0;               // this real frame reached PresentNow (Generate)
        uint64_t heldNs = 0,
                 heldCount = 0;   // since the last log line: how long real frames waited behind a generated one
        bool saidNoHudless = false;
        double refreshHz = 0;
        std::function<void(const char*, uint64_t, const std::vector<uint32_t>&, uint32_t, uint32_t)> snapshot;
        uint64_t snapshotIndex = 0;
    } g_state;

    bool Fail(const char* what, FfxErrorCode code = FFX_OK)
    {
        fprintf(stderr, "[framegen] %s (%d); frame generation is off for this run\n", what, int(code));
        g_state.failed = true;
        return false;
    }

    void DestroyContexts()
    {
        State& s = g_state;
        if (!s.created)
            return;
        Submit(); // waits for the GPU: the contexts' resources may be in use
        ffxFrameInterpolationContextDestroy(&s.fi);
        ffxOpticalflowContextDestroy(&s.of);
        gpu::fsr::DestroyShared(s.flowVector);
        gpu::fsr::DestroyShared(s.flowChange);
        s.flowVector = s.flowChange = nullptr;
        if (s.generated != nullptr)
            DeferDestroy(s.generated);
        s.generated = nullptr;
        s.created = false;
        s.history = false;
    }

    // The contexts for these sizes and the frame's format.
    bool Prepare(
        uint32_t renderWidth, uint32_t renderHeight, uint32_t displayWidth, uint32_t displayHeight, RenderFormat format)
    {
        State& s = g_state;
        if (s.created && s.renderWidth == renderWidth && s.renderHeight == renderHeight &&
            s.displayWidth == displayWidth && s.displayHeight == displayHeight && s.format == format)
            return true;
        DestroyContexts();
        const FfxSurfaceFormat surface = gpu::fsr::SurfaceFormat(format);
        if (surface == FFX_SURFACE_FORMAT_UNKNOWN)
        {
            fprintf(stderr, "[framegen] the frame's format (%d) is not one frame interpolation takes\n", int(format));
            return Fail("unknown frame format");
        }
        gpu::fsr::MakeInterface(s.backend);

        FfxOpticalflowContextDescription flow{};
        flow.backendInterface = s.backend;
        flow.resolution = { displayWidth, displayHeight };
        FfxErrorCode error = ffxOpticalflowContextCreate(&s.of, &flow);
        if (error != FFX_OK)
            return Fail("the optical flow context could not be made", error);

        FfxFrameInterpolationContextDescription description{};
        description.backendInterface = s.backend;
        // Motion at the render size, depth 0 near (the upscaler's dilated depth), SDR.
        description.flags = 0;
        description.maxRenderSize = { renderWidth, renderHeight };
        description.displaySize = { displayWidth, displayHeight };
        description.backBufferFormat = surface;
        description.previousInterpolationSourceFormat = surface;
        error = ffxFrameInterpolationContextCreate(&s.fi, &description);
        if (error != FFX_OK)
        {
            ffxOpticalflowContextDestroy(&s.of);
            return Fail("the frame interpolation context could not be made", error);
        }
        FfxOpticalflowSharedResourceDescriptions shared{};
        ffxOpticalflowGetSharedResourceDescriptions(&s.of, &shared);
        s.flowVector = gpu::fsr::CreateShared(shared.opticalFlowVector);
        s.flowChange = gpu::fsr::CreateShared(shared.opticalFlowSCD);
        s.generated = gpu::CreateTexture({ .width = displayWidth,
            .height = displayHeight,
            .format = RenderFormat::R8G8B8A8_UNORM,
            .usage = gpu::TextureUsage::Storage });
        s.created = true; // DestroyContexts frees what was made
        s.renderWidth = renderWidth, s.renderHeight = renderHeight;
        s.displayWidth = displayWidth, s.displayHeight = displayHeight;
        s.format = format;
        if (s.flowVector == nullptr || s.flowChange == nullptr || s.generated == nullptr)
        {
            DestroyContexts();
            return Fail("frame generation's textures could not be made");
        }
        fprintf(stderr, "[framegen] FSR 3.1 frame generation: render %ux%u, frame %ux%u, optical flow %ux%u\n",
            renderWidth, renderHeight, displayWidth, displayHeight, s.flowVector->desc.width,
            s.flowVector->desc.height);
        return true;
    }

    void LogStats(uint64_t now)
    {
        State& s = g_state;
        if (s.logNs == 0)
            s.logNs = now;
        if (now - s.logNs < 10'000'000'000ull)
            return;
        const double seconds = double(now - s.logNs) / 1e9;
        static gpu::framegen::Stats last;
        const gpu::framegen::Stats& n = s.stats;
        const ::framegen::Pacing::Summary pacing = s.pacing.TakeSummary();
        const uint64_t real = n.real - last.real;
        if (s.uneven.Window(pacing.deviationMs, real != 0 ? double(n.generated - last.generated) / double(real) : 0.0))
        {
            fprintf(stderr,
                "[framegen] the frames shown are uneven (%.1f ms deviation: the GPU cannot interpolate in time at this "
                "rate): off until it is switched on again\n",
                pacing.deviationMs);
            g_uneven = true;
        }
        fprintf(stderr,
            "[framegen] last %.0f s: %.1f real fps, %.1f shown; %llu generated; without one: %llu (no scene) %llu "
            "(reset) %llu (no HUD copy) %llu (display: VSync at %.0f Hz); shown frame spacing %.2f ms mean, %.2f ms "
            "deviation; the real frame held %.1f ms\n",
            seconds, double(n.real - last.real) / seconds,
            double(n.real - last.real + n.generated - last.generated) / seconds,
            (unsigned long long)(n.generated - last.generated), (unsigned long long)(n.noScene - last.noScene),
            (unsigned long long)(n.reset - last.reset), (unsigned long long)(n.noHudless - last.noHudless),
            (unsigned long long)(n.display - last.display), s.refreshHz, pacing.meanMs, pacing.deviationMs,
            s.heldCount != 0 ? double(s.heldNs) / double(s.heldCount) / 1e6 : 0.0);
        s.heldNs = s.heldCount = 0;
        last = n;
        s.logNs = now;
    }
}

void gpu::framegen::SetEnabled(bool on)
{
    g_uneven = false; // a new try
    g_requested = on;
    g_enabled = on && !g_blocked;
}

bool gpu::framegen::Enabled() { return g_enabled; }

void gpu::framegen::SetBlocked(bool blocked)
{
    g_blocked = blocked;
    g_enabled = g_requested && !blocked;
}

bool gpu::framegen::Blocked() { return g_blocked; }

bool gpu::framegen::Preview()
{
    static const bool preview = skyenv::Get("SKY_FSR_FRAME_GENERATION_PREVIEW") != nullptr;
    return preview;
}

bool gpu::framegen::Available()
{
    std::lock_guard lock(g_mutex);
    if (g_context == nullptr || g_context->device == nullptr || !gpu::fsr::ShadersAvailable())
        return false;
    // D3D12: not proven yet (this Mac runs it on Mesa's lavapipe only, which fails in the interpolation pass on every
    // backend): off for players; SKY_FSR_FRAME_GENERATION_D3D12=1 lets a tester on a Windows PC try it.
    static const bool d3d12Allowed = skyenv::Get("SKY_FSR_FRAME_GENERATION_D3D12") != nullptr;
    return g_context->renderInterface->getCapabilities().shaderFormat != RenderShaderFormat::DXIL || d3d12Allowed;
}

gpu::framegen::Stats gpu::framegen::Counts()
{
    std::lock_guard lock(g_mutex);
    return g_state.stats;
}

void gpu::framegen::SetSnapshotHook(
    std::function<void(const char*, uint64_t, const std::vector<uint32_t>&, uint32_t, uint32_t)> hook)
{
    std::lock_guard lock(g_mutex);
    g_state.snapshot = std::move(hook);
}

void gpu::framegen::CaptureHudless(Texture* frame)
{
    if (!g_enabled)
        return;
    ProducerLock producer;
    Enqueue([frame] {
        std::lock_guard lock(g_mutex);
        State& s = g_state;
        if (frame == nullptr)
            s.menuFrame = g_context->frames;
        if (frame == nullptr || DeviceLost() || s.failed)
            return;
        SettleCopies(frame);
        const TextureDesc& d = frame->desc;
        if (s.hudless == nullptr || s.hudless->desc.width != d.width || s.hudless->desc.height != d.height ||
            s.hudless->desc.format != d.format)
        {
            if (s.hudless != nullptr)
                DeferDestroy(s.hudless);
            s.hudless = gpu::CreateTexture({ .width = d.width, .height = d.height, .format = d.format });
            if (s.hudless == nullptr)
                return;
        }
        // In a command list of its own, after the composite's and before the HUD's (Metal raced FSR's work recorded in
        // the frame's list: gpu/fsr/fsr.cpp).
        Flush();
        RenderCommandList& list = Record();
        Transition(list, *frame, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
        Transition(list, *s.hudless, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
        list.copyTexture(s.hudless->texture.get(), frame->texture.get());
        Flush();
        s.hudlessFrame = g_context->frames;
    });
}

void gpu::framegen::detail::Upscaled(const Scene& scene)
{
    State& s = g_state;
    if (!g_enabled)
        return;
    s.scene = scene;
    s.sceneFrame = g_context->frames;
}

const gpu::Texture* gpu::framegen::detail::Generate(const Texture* image)
{
    State& s = g_state;
    const uint64_t now = SDL_GetTicksNS();
    const uint64_t frame = g_context->frames;
    s.arrivalNs = now;
    if (!g_enabled || g_uneven || s.failed || image == nullptr)
    {
        if (g_uneven)
            s.uneven.Reset();
        s.history = false;
        s.pacing.Reset();
        return nullptr;
    }
    // SKY_FSR_FG_FROM_SECONDS=<s>: off until that many seconds after the first frame (a measure of off and on in one
    // run: tools/regress/fsr_framegen.sh, docs/runtime/gpu/upscaling.md).
    static const double fromSeconds =
        skyenv::Get("SKY_FSR_FG_FROM_SECONDS") != nullptr ? atof(skyenv::Get("SKY_FSR_FG_FROM_SECONDS")) : 0.0;
    static const uint64_t firstNs = now;
    if (double(now - firstNs) / 1e9 < fromSeconds)
        return nullptr; // the gap below starts the history and the pacing again
    // A gap (frame generation was off, or a present without an image): the history and the pacing start again.
    if (frame != s.lastFrame + 1)
    {
        s.history = false;
        s.pacing.Reset();
    }
    s.lastFrame = frame;
    s.stats.real++;
    LogStats(now);
    const float frameMs = s.pacing.RealFrame(now);
    const bool hudless = s.hudless != nullptr && s.hudlessFrame == frame &&
                         s.hudless->desc.width == image->desc.width && s.hudless->desc.height == image->desc.height &&
                         s.hudless->desc.format == image->desc.format;
    // The display's refresh rate (VSync on): read again every 300 real frames (the window may move to another display).
    if (s.stats.real % 300 == 1 && g_context->window != nullptr)
    {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_context->window));
        s.refreshHz = mode != nullptr ? double(mode->refresh_rate) : 0.0;
    }
    // Offscreen, the stand-in for VSync shows 60 frames a second (gpu.cpp).
    const double refresh = g_context->swapChain != nullptr ? s.refreshHz : 60.0;
    const bool scene = s.sceneFrame == frame && s.menuFrame != frame;
    // The upscaler's own reset is not one here: it follows the swaps (one that presents nothing breaks its history),
    // this follows the frames shown, and the interpolation's inputs (dilated depth and motion) hold on its reset too.
    const bool cut = scene && (s.scene.shift > s.scene.cutShift || ::framegen::CameraJump(s.scene.shift, s.lastShift));
    if (scene)
        s.lastShift = s.scene.shift;
    const ::framegen::Outcome outcome = ::framegen::Decide({ true, scene, hudless, s.history, cut,
        ::framegen::DisplayHasRoom(gpu::g_settings.vsync, refresh, s.pacing.IntervalMs()) });
    // SKY_FSR_FG_DEBUG=3: the decision of each of the first 3000 real frames.
    static const bool trace = skyenv::Get("SKY_FSR_FG_DEBUG") != nullptr && atoi(skyenv::Get("SKY_FSR_FG_DEBUG")) == 3;
    if (trace && s.stats.real < 3000)
        fprintf(stderr,
            "[framegen] frame %llu: outcome %d, scene %d, hudless %d, history %d, upscaler reset %d, camera shift "
            "%.3f, "
            "%.1f ms\n",
            (unsigned long long)s.stats.real, int(outcome), int(scene), int(hudless), int(s.history),
            int(scene && s.scene.reset), scene ? double(s.scene.shift) : 0.0, double(frameMs));
    if (outcome == ::framegen::Outcome::Display)
    {
        // The real frame alone, and no history after it (the optical flow and the interpolation did not see it).
        s.stats.display++;
        s.history = false;
        return nullptr;
    }
    if (outcome == ::framegen::Outcome::NoScene)
    {
        // A movie, a loading screen, the game's menus: the real frame alone, and no history after it.
        s.stats.noScene++;
        s.history = false;
        return nullptr;
    }
    if (outcome == ::framegen::Outcome::NoHudless)
    {
        if (!s.saidNoHudless)
            fprintf(stderr, "[framegen] no copy of this frame without its HUD (%s): the real frame alone\n",
                s.hudless == nullptr || s.hudlessFrame != frame ? "no composite copied" : "its size or format differs");
        s.saidNoHudless = true;
        s.stats.noHudless++;
        s.history = false;
        return nullptr;
    }
    if (!Prepare(s.scene.renderWidth, s.scene.renderHeight, image->desc.width, image->desc.height, image->desc.format))
        return nullptr;
    const bool reset = outcome == ::framegen::Outcome::Reset || !s.history; // Prepare may have made the contexts again
    // Optical flow and interpolation in a command list of their own, after the frame's work (gpu/fsr/fsr.cpp: Metal
    // raced FSR's passes recorded with the frame's draws); PresentGenerated sends it before its present pass.
    Flush();
    RenderCommandList& list = Record();

    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = &list;
    flow.color = gpu::fsr::External(s.hudless, false);
    flow.opticalFlowVector = gpu::fsr::External(s.flowVector, true);
    flow.opticalFlowSCD = gpu::fsr::External(s.flowChange, true);
    flow.reset = reset;
    flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    flow.minMaxLuminance = { 0.0f, 1.0f };
    FfxErrorCode error = ffxOpticalflowContextDispatch(&s.of, &flow);
    if (error != FFX_OK)
        return Fail("an optical flow dispatch failed", error), nullptr;

    // SKY_FSR_FG_DEBUG=1: the SDK's debug view in the generated frame; 2: its tear and reset markers.
    static const int debug = skyenv::Get("SKY_FSR_FG_DEBUG") != nullptr ? atoi(skyenv::Get("SKY_FSR_FG_DEBUG")) : 0;
    FfxFrameInterpolationDispatchDescription d{};
    d.flags = debug == 1   ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW
              : debug == 2 ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES |
                                 FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS
                           : 0;
    d.commandList = &list;
    d.displaySize = { s.displayWidth, s.displayHeight };
    d.renderSize = { s.scene.renderWidth, s.scene.renderHeight };
    d.currentBackBuffer = gpu::fsr::External(const_cast<Texture*>(image), false);
    d.currentBackBuffer_HUDLess = gpu::fsr::External(s.hudless, false);
    d.output = gpu::fsr::External(s.generated, true);
    d.interpolationRect = { 0, 0, int32_t(s.displayWidth), int32_t(s.displayHeight) };
    d.opticalFlowVector = gpu::fsr::External(s.flowVector, false);
    d.opticalFlowSceneChangeDetection = gpu::fsr::External(s.flowChange, false);
    d.opticalFlowBufferSize = { s.flowVector->desc.width, s.flowVector->desc.height };
    d.opticalFlowScale = { 1.0f / float(s.displayWidth), 1.0f / float(s.displayHeight) };
    d.opticalFlowBlockSize = 8;
    d.cameraNear = s.scene.cameraNear;
    d.cameraFar = s.scene.cameraFar;
    d.cameraFovAngleVertical = s.scene.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.frameTimeDelta = frameMs;
    d.reset = reset;
    d.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.minMaxLuminance[0] = 0.0f;
    d.minMaxLuminance[1] = 1.0f;
    d.frameID = ++s.frameId;
    d.dilatedDepth = gpu::fsr::External(s.scene.dilatedDepth, false);
    d.dilatedMotionVectors = gpu::fsr::External(s.scene.dilatedMotion, false);
    d.reconstructedPrevDepth = gpu::fsr::External(s.scene.previousDepth, false);
    error = ffxFrameInterpolationDispatch(&s.fi, &d);
    if (error != FFX_OK)
        return Fail("a frame interpolation dispatch failed", error), nullptr;
    s.history = true;
    if (reset)
    {
        // The interpolation has only this frame: it is shown alone, the next one gets a frame before it.
        s.stats.reset++;
        return nullptr;
    }
    s.stats.generated++;
    // The present pass reads it next.
    Transition(list, *s.generated, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
    return s.generated;
}

uint64_t gpu::framegen::detail::RealFrameDelayNs()
{
    // SKY_FSR_FG_NO_PACING=1: no wait (ArkeyanRecompGpuTest --framegen-bench measures the GPU's work alone).
    // With VSync on the display spaces the two presents itself (each waits for its blank): a wait here would only add
    // latency.
    static const bool noPacing = skyenv::Get("SKY_FSR_FG_NO_PACING") != nullptr;
    return noPacing || gpu::g_settings.vsync ? 0 : g_state.pacing.HalfIntervalNs();
}

void gpu::framegen::detail::Shown()
{
    if (g_enabled)
        g_state.pacing.Shown(SDL_GetTicksNS());
}

void gpu::framegen::detail::RealPresented(const Texture* generated, const Texture* real)
{
    State& s = g_state;
    // The latency frame generation adds: the real frame went out this long after it was ready.
    s.heldNs += SDL_GetTicksNS() - s.arrivalNs;
    s.heldCount++;
    if (!s.snapshot)
        return;
    // SKY_FSR_FG_SNAPSHOT_EVERY=<n> (default 300) from SKY_FSR_FG_SNAPSHOT_FROM=<n> generated frames: the generated
    // frame and the real one after it.
    static const uint64_t every = std::max(
        skyenv::Get("SKY_FSR_FG_SNAPSHOT_EVERY") != nullptr ? atoi(skyenv::Get("SKY_FSR_FG_SNAPSHOT_EVERY")) : 300, 1);
    static const uint64_t from =
        skyenv::Get("SKY_FSR_FG_SNAPSHOT_FROM") != nullptr ? atoi(skyenv::Get("SKY_FSR_FG_SNAPSHOT_FROM")) : 0;
    const uint64_t index = s.stats.generated;
    // SKY_FSR_FG_SNAPSHOT_COUNT=<n>: at most n of them (consecutive frames with EVERY=1).
    static const uint64_t count = skyenv::Get("SKY_FSR_FG_SNAPSHOT_COUNT") != nullptr
                                      ? uint64_t(atoll(skyenv::Get("SKY_FSR_FG_SNAPSHOT_COUNT")))
                                      : UINT64_MAX;
    if (index < from || (index - from) % every != 0 || (index - from) / every >= count)
        return;
    std::vector<uint32_t> rgba;
    uint32_t width = 0, height = 0;
    if (ReadbackNow(generated, rgba, width, height))
        s.snapshot("gen", index, rgba, width, height);
    if (ReadbackNow(real, rgba, width, height))
        s.snapshot("real", index, rgba, width, height);
    if (s.hudless != nullptr && ReadbackNow(s.hudless, rgba, width, height))
        s.snapshot("hudless", index, rgba, width, height);
}

void gpu::framegen::detail::Release()
{
    State& s = g_state;
    DestroyContexts();
    if (s.hudless != nullptr)
        DeferDestroy(s.hudless);
    s.hudless = nullptr;
    s.hudlessFrame = s.sceneFrame = UINT64_MAX;
    s.failed = false;
}
