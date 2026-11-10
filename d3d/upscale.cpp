#include "d3d/upscale.h"
#include "d3d/upscale_math.h"
#include "d3d/ultrawide.h"
#include "game.h"
#include "gpu/upscale.h"
#include "../host/env.h"
#include "gpu/gpu.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    bool g_active;
    gpu::Upscaler g_upscaler = gpu::Upscaler::Fsr3;
    uint32_t g_frameScale = 1, g_sceneScale = 1, g_phases = 8;
    // FSR's sharpening (RCAS) after the upscale: textures as sharp as at the frame's scale.
    constexpr float Sharpness = 0.5f;
    uint64_t g_frame;
    std::array<float, 2> g_jitter{};

    upscale::CameraVote g_vote;
    bool g_composited; // this frame's composite ran
    bool g_history;    // the frame before was upscaled: the upscaler's history follows on

    // The scene's depth targets this frame and their draws (the busiest is the scene's), with their viewports' depth
    // ranges by weight (upscale::SceneDepthRange picks one).
    struct DepthUse
    {
        gpu::Texture* target;
        uint32_t draws;
        std::array<upscale::DepthRange, 8> ranges;
        uint32_t rangeCount;
    };
    std::array<DepthUse, 4> g_depths;
    uint32_t g_depthCount;

    gpu::Texture* g_output; // the upscaled scene

    // The camera of this frame's heaviest scene draw with a perspective matrix, and the last one found (FSR's depth
    // tests; a guess until a frame has one).
    upscale::Camera g_camera{ 0.1f, 1000.0f, 1.0f, false };
    uint32_t g_cameraWeight;
    // The scene's depth tests this frame, by draw weight: mostly "greater" is reversed Z.
    uint64_t g_greaterWeight, g_lessWeight;
    bool g_depthReversed;

    // The log's tally (every 900 upscaled frames): frames whose camera motion the draws agreed on, frames that started
    // a new history, changes of the depth's direction.
    uint32_t g_upscaled, g_cameraFound, g_resets, g_depthFlips;
}

namespace
{
    // The upscaled picture's size in guest pixels: the whole frame where the composite stretches the scene over it
    // (Alchemy: FSR then does all of the stretch), else the scene's own size (Lux's composite reads by pixel position).
    uint32_t OutputGuestWidth()
    {
        return game::UltrawideCompositeVpos ? ultrawide::SceneWidth() : ultrawide::FrameWidth();
    }
    uint32_t OutputGuestHeight()
    {
        return game::UltrawideCompositeVpos ? ultrawide::SceneHeight() : ultrawide::FrameHeight;
    }
    // The output's width over the scene's: FSR's ratio for its jitter phases (ffxFsr3UpscalerGetJitterPhaseCount takes
    // the widths) and the texture LOD bias.
    float Ratio()
    {
        return float(OutputGuestWidth() * g_frameScale) / float(std::max(ultrawide::SceneWidth(), 1u) * g_sceneScale);
    }

    uint32_t g_windowFrameScale = 1, g_baseScale = 1; // the window's frame scale; the scale the player chose
    bool g_sceneIsFrame; // the scene at the frame's size this run: FSR waits for the next start
    // The menu's request (main thread) for the render thread's next swap: 0 none, else 0x100 | mode << 4 | quality.
    std::atomic<uint32_t> g_request;

    // Sets the scales for `mode` and `quality`; the render targets follow at their next creation.
    void Apply(upscalemode::Upscaler mode, upscalemode::Quality quality)
    {
        g_active = false;
        g_frameScale = g_sceneScale = g_baseScale;
        gpu::g_settings.resolutionScale = g_baseScale;
        if (mode == upscalemode::Upscaler::Off)
        {
            fprintf(stderr, "[upscale] off: the scene and the frame at scale %u\n", g_baseScale);
            return;
        }
        g_upscaler = mode == upscalemode::Upscaler::MetalFx ? gpu::Upscaler::MetalFx : gpu::Upscaler::Fsr3;
        const uint32_t frameScale = g_windowFrameScale, sceneScale = upscalemode::SceneScale(frameScale, quality);
        if (!gpu::UpscalerAvailable(g_upscaler))
        {
            fprintf(stderr, "[upscale] %s asked, but this GPU backend cannot run it: off\n", upscalemode::Name(mode));
            return;
        }
        if (g_upscaler == gpu::Upscaler::MetalFx && frameScale <= sceneScale)
        {
            fprintf(stderr, "[upscale] MetalFX off: the frame's scale %u is not above the scene's %u\n", frameScale,
                sceneScale);
            return;
        }
        g_frameScale = frameScale;
        g_sceneScale = sceneScale;
        gpu::g_settings.resolutionScale = sceneScale;
        g_active = true;
        g_phases = upscale::JitterPhases(Ratio());
        g_jitter = upscale::Jitter(g_frame, g_phases);
        fprintf(stderr, "[upscale] %s %s: the scene at scale %u, upscaled to the frame's scale %u (%u jitter phases)\n",
            upscalemode::Name(mode), upscalemode::Name(quality), g_sceneScale, g_frameScale, g_phases);
    }
}

bool upscale::SwitchesLive() { return !g_sceneIsFrame; }

void upscale::Configure(
    uint32_t windowHeight, upscalemode::Upscaler upscaler, upscalemode::Quality quality, bool sceneIsFrame)
{
    g_sceneIsFrame = sceneIsFrame;
    g_baseScale = std::max(gpu::g_settings.resolutionScale, 1u);
    g_windowFrameScale = upscalemode::FrameScale(windowHeight);
    if (const char* scale = skyenv::Get("SKY_UPSCALE_SCALE"))
        g_windowFrameScale = uint32_t(std::clamp(atoi(scale), 1, 8));
    if (upscaler != upscalemode::Upscaler::Off)
        Apply(upscaler, quality);
}

void upscale::Request(upscalemode::Upscaler upscaler, upscalemode::Quality quality)
{
    if (g_sceneIsFrame)
        return; // at the next start (SwitchesLive)
    g_request = 0x100u | uint32_t(upscaler) << 4 | uint32_t(quality);
}

uint32_t upscale::SceneScale() { return g_sceneScale; }

uint32_t upscale::WindowFrameScale() { return g_windowFrameScale; }

bool upscale::Active() { return g_active; }

uint32_t upscale::FrameScale() { return g_frameScale; }

std::array<float, 2> upscale::Jitter() { return g_jitter; }

float upscale::MipBias() { return -std::log2(Ratio()); }

void upscale::SceneDraw(uint64_t identity, const float* rows, uint32_t weight, gpu::Texture* depth, float minDepth,
    float maxDepth, bool greaterTest)
{
    (greaterTest ? g_greaterWeight : g_lessWeight) += weight;
    if (rows != nullptr)
    {
        const Matrix matrix = FromRowVectors(rows);
        if (identity != 0)
            g_vote.Add(identity, matrix, weight);
        upscale::Camera camera;
        if (weight > g_cameraWeight && CameraOf(matrix, camera))
            g_camera = camera, g_cameraWeight = weight;
        else if (static uint32_t logged; weight > g_cameraWeight && logged < 2)
        {
            logged++; // what a matrix that is not a perspective one looks like (the engine's conventions)
            fprintf(stderr,
                "[upscale] no camera in a scene matrix: rows %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g "
                "%.4g %.4g | %.4g %.4g %.4g %.4g\n",
                matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8],
                matrix[9], matrix[10], matrix[11], matrix[12], matrix[13], matrix[14], matrix[15]);
        }
    }
    uint32_t i = 0;
    while (i < g_depthCount && g_depths[i].target != depth)
        i++;
    if (i == g_depthCount)
    {
        if (g_depthCount == g_depths.size())
            return;
        g_depths[g_depthCount++] = { depth, 0, {}, 0 };
    }
    DepthUse& use = g_depths[i];
    use.draws++;
    uint32_t r = 0;
    while (r < use.rangeCount && (use.ranges[r].minDepth != minDepth || use.ranges[r].maxDepth != maxDepth))
        r++;
    if (r == use.rangeCount && r < use.ranges.size())
        use.ranges[use.rangeCount++] = { minDepth, maxDepth, 0 };
    if (r < use.rangeCount)
        use.ranges[r].weight += weight;
}

// game::Adapter::sceneMotionLayers; SKY_FSR_LAYERS=0 (test aid): the camera's motion alone, as in the other games.
static bool MotionLayers()
{
    static const bool off = skyenv::Get("SKY_FSR_LAYERS") != nullptr && atoi(skyenv::Get("SKY_FSR_LAYERS")) == 0;
    return game::Current().sceneMotionLayers && !off;
}

gpu::Texture* upscale::Composite(gpu::Texture* scene)
{
    if (!g_active || scene == nullptr)
        return nullptr;
    if (g_composited)
        return g_output; // drawn again this frame: the same picture
    const gpu::TextureDesc& size = gpu::Desc(scene);
    const DepthUse* depth = nullptr;
    for (uint32_t i = 0; i < g_depthCount; i++)
        if (gpu::Desc(g_depths[i].target).width == size.width && gpu::Desc(g_depths[i].target).height == size.height &&
            (depth == nullptr || g_depths[i].draws > depth->draws))
            depth = &g_depths[i];
    if (depth == nullptr)
        return nullptr; // no 3D drawn this frame

    const uint32_t width = OutputGuestWidth() * g_frameScale, height = OutputGuestHeight() * g_frameScale;
    if (g_output == nullptr || gpu::Desc(g_output).width != width || gpu::Desc(g_output).height != height ||
        gpu::Desc(g_output).format != size.format)
    {
        gpu::DestroyTexture(g_output);
        g_output = gpu::CreateTexture({ .width = width,
            .height = height,
            .format = size.format,
            .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Storage });
        g_history = false;
        if (g_output == nullptr)
            return nullptr;
    }

    Matrix reprojection = Identity;
    const bool camera = g_vote.Close(reprojection);
    // ndc z is reversed when the test keeps the farther window depth, unless the viewport's range is reversed too (the
    // inputs pass undoes the range itself).
    const upscale::DepthRange range = upscale::SceneDepthRange(depth->ranges.data(), depth->rangeCount);
    const bool reversed = (g_greaterWeight > g_lessWeight) != (range.maxDepth < range.minDepth);
    g_depthFlips += g_upscaled != 0 && reversed != g_depthReversed;
    g_depthReversed = reversed;
    gpu::UpscaleDesc desc{ .upscaler = g_upscaler,
        .color = scene,
        .depth = depth->target,
        .output = g_output,
        .jitter = { g_jitter[0], g_jitter[1] },
        .reprojection = {},
        .cameraFound = camera,
        .depthMin = range.minDepth,
        .depthMax = range.maxDepth,
        .depthReversed = g_depthReversed,
        .reset = !g_history,
        .sharpness = Sharpness,
        .cameraNear = g_camera.nearZ,
        .cameraFar = g_camera.farZ,
        .fovY = g_camera.fovY,
        .motionLayers = MotionLayers(),
        .cutShift = game::Current().frameGenerationCutShift,
        .otherMotionCount = 0,
        .otherMotions = {} };
    if (desc.motionLayers && camera)
    {
        Matrix others[2];
        desc.otherMotionCount = g_vote.Others(others);
        for (uint32_t i = 0; i < desc.otherMotionCount; i++)
            std::copy(others[i].begin(), others[i].end(), desc.otherMotions[i]);
    }
    std::copy(reprojection.begin(), reprojection.end(), desc.reprojection);
    gpu::Upscale(desc);
    g_composited = true;

    g_upscaled++;
    g_cameraFound += camera;
    g_resets += desc.reset;
    if (g_upscaled == 900)
    {
        fprintf(stderr,
            "[upscale] last 900 frames: the camera's motion found in %u; camera near %.3g far %.3g fov %.1f, depth "
            "%s (direction changed %u times), %u new histories\n",
            g_cameraFound, g_camera.nearZ, g_camera.farZ, g_camera.fovY * 57.2958f,
            g_depthReversed ? "reversed" : "0 near", g_depthFlips, g_resets);
        g_upscaled = g_cameraFound = g_resets = g_depthFlips = 0;
    }
    return g_output;
}

bool upscale::FrameEnd()
{
    bool changed = false;
    if (const uint32_t request = g_request.exchange(0); request != 0)
    {
        const bool wasActive = g_active;
        const uint32_t frameScale = g_frameScale, sceneScale = g_sceneScale;
        Apply(upscalemode::Upscaler((request >> 4) & 0xF), upscalemode::Quality(request & 0xF));
        changed = g_active != wasActive || g_frameScale != frameScale || g_sceneScale != sceneScale;
        g_composited = g_history = false;
        g_vote.Forget();
        g_depthCount = 0;
    }
    if (!g_active)
        return changed;
    if (!g_composited)
        g_vote.Forget(); // no scene: the next frame starts a new history
    g_history = g_composited;
    g_composited = false;
    g_depthCount = 0;
    g_cameraWeight = 0;
    g_greaterWeight = g_lessWeight = 0;
    g_frame++;
    g_jitter = upscale::Jitter(g_frame, g_phases);
    return changed;
}
