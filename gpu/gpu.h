#pragma once

// Host renderer over plume: Metal on Apple, D3D12 on Windows (and later Xbox), Vulkan elsewhere. Today it only presents
// CPU-made images (Bink movies) and, through gpu/draw.h, the game's own draws. Thread-safe. The command list work runs on
// a GPU thread of its own (worker.cpp), in call order: the calls queue it and return (SKY_GPU_THREAD=0: on the caller).
#include "gpu/gpu_backend.h"
#include <SDL3/SDL_video.h>
#include <plume_render_interface_types.h>
#include <cstdint>
#include <array>
#include <functional>
#include <vector>

namespace gpu
{
    using Format = plume::RenderFormat;

    // Xenos limits, as the game's D3D exposes them.
    constexpr uint32_t MaxTextureSlots = 32;     // texture fetch constants tf0-tf31, shared by both stages
    constexpr uint32_t MaxRenderTargets = 4;     // EDRAM color targets
    constexpr uint32_t FloatConstantCount = 256; // float4 c0-c255 per stage (512 in the register file)
    constexpr uint32_t BoolConstantCount = 128;  // per stage (256 bits in the register file)
    constexpr uint32_t LoopConstantCount = 16;   // per stage, packed count/start/step (32 in the register file)
    constexpr uint32_t MaxVertexStreams = 16;
    constexpr uint32_t MaxVertexElements = 32;

    namespace TextureUsage
    {
        enum : uint32_t
        {
            Sampled = 1u << 0,
            RenderTarget = 1u << 1,
            DepthStencil = 1u << 2,
            Storage = 1u << 3, // written by compute work (the upscaler's output)
        };
    }

    struct TextureDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        uint32_t mipLevels = 1;
        uint32_t arraySize = 1;
        Format format = Format::UNKNOWN;
        uint32_t usage = TextureUsage::Sampled;
        bool cube = false;
    };

    struct Texture;

    // --- Implemented: device, output, uploads, present ---------------------------------------------------------------

    // Flags the SDL window needs so the backend can render to it (SDL_WINDOW_METAL, SDL_WINDOW_VULKAN or none).
    SDL_WindowFlags WindowFlags();

    // Creates the device and the output. Output goes to a swapchain when the window has a native surface for the
    // backend; otherwise (no window, or SDL's offscreen video driver) to an offscreen target that follows the window's
    // pixel size, or is width x height without a window.
    bool Init(SDL_Window* window, uint32_t width = 0, uint32_t height = 0);

    // Host rendering settings (host/settings.h), set before Init.
    struct Settings
    {
        uint32_t resolutionScale = 1; // render targets and what is resolved from them are this multiple of the guest's;
        // 0: auto, chosen by Init from the GPU (1 on integrated GPUs, 2 on discrete ones
        // and Apple's)
        uint32_t anisotropy = 1; // samplers with linear filtering
        bool vsync = true; // off: presents don't wait for the display (nor the offscreen output's 60 Hz)
        bool gpuThread = true; // the GPU thread (worker.cpp); off: the callers record and submit (SKY_GPU_THREAD=0)
        bool pipelineLibrary = true; // D3D12: the persistent pipeline cache (d3d12_pipeline_cache.cpp)
        uint32_t fpsLimit = 30; // the game's frame rate (fps_limit), for the [perf] log's GPU-bound hint
        uint32_t edgeSmoothing =
            0; // edgesmoothing::Mode of the game's FXAA pass (gpu/edge_smoothing.h); changed while playing
        // Desktop Windows: [<game>.video] gpu_backend, the one tried first (gpu/gpu_backend.h); ignored elsewhere.
        gpubackend::Backend backend = gpubackend::RuntimeDefault;
    };
    extern Settings g_settings;
    // The backend that runs, once Init has made its device: "Metal", "D3D12" or "Vulkan".
    const char* BackendName();
    void Shutdown();

    // Textures may be created at any time; a destroyed one goes once the frames queued before have used it.
    Texture* CreateTexture(const TextureDesc& desc);
    void DestroyTexture(Texture* texture);
    const TextureDesc& Desc(const Texture* texture);

    // Copies a whole subresource through a staging buffer; the copy runs on the GPU before the next Present.
    void UploadTexture(
        Texture* texture, const void* data, uint32_t rowPitch, uint32_t mipLevel = 0, uint32_t arraySlice = 0);
    // The same, with the data made on a worker thread: `fill(destination, rowPitch)` writes the subresource's rows (of
    // blocks), `rowPitch` bytes apart; it must use only data it owns (it runs later, while the caller goes on).
    void UploadTextureAsync(Texture* texture, uint32_t mipLevel, uint32_t arraySlice,
        std::function<void(uint8_t* destination, uint32_t rowPitch)> fill);

    // Ends the frame: clears the output to black, draws the image (if any) scaled to fit with its aspect ratio kept
    // (letterbox: scale = min(outW / w, outH / h), size rounded to whole pixels, centred with the offset rounded down),
    // presents. Paced by vsync, or at 60 Hz for an offscreen output. Queued: the caller waits only while the GPU thread
    // is still on the frame before.
    void Present(const Texture* image);

    // The perf log's split of a frame (LogPerf, every 10 s): time the renderer (d3d/) spent on the game's render thread
    // in its hooks (without the guest's own D3D code, the vblank wait and the present), of which the ring translation and
    // the texture hashing and decoding, and the time spent waiting for the vertical blank.
    enum class FrameTime : uint32_t
    {
        Renderer,
        Ring,
        Textures,
        VblankWait,
        Count
    };
    void AddFrameTime(FrameTime what, uint64_t ns);

    // UWP: called in the UI thread's loop, which then becomes the thread that presents. A Present from another thread
    // (the game's) records and submits the frame there but leaves the swap chain's Present call to the UI thread (as
    // Microsoft's template presents), waiting at most 200 ms for it. Waits up to `timeoutMs` for such a frame.
    void ServicePresents(uint32_t timeoutMs);

    // Host UI over the game (the Toy Box menu): an RGBA8 image with straight alpha, blended over every presented image
    // and letterboxed the same way, until set to null. Keep the texture alive while it is set.
    void SetOverlay(const Texture* overlay);

    // Reads back the last presented image (RGBA8 in memory order R, G, B, A). Offscreen output only: swapchain images
    // cannot be read.
    // Frames the game has presented since the start (any thread): the F3 menu's FPS counter divides its difference by time.
    uint64_t FramesPresented();
    // A read-only copy of the perf window already collected by LogPerf (at most 10 seconds).
    // No device strings or mutable renderer containers escape their lock. Unavailable data stays explicit.
    struct DebugSnapshot
    {
        char adapter[192]{}, backend[16]{}, driver[64]{};
        uint32_t width = 0, height = 0, scale = 1;
        uint64_t videoUsed = 0, videoBudget = 0;
        std::array<uint64_t, 2048> frameNs{};
        size_t frameCount = 0;
        size_t shaders = 0, missingShaders = 0, pipelinesReady = 0;
        uint32_t drawTimeBuilds = 0;
        bool available = false, videoUsageKnown = false, videoBudgetKnown = false;
    };
    // Without `wait`: false, and nothing read, while the GPU thread holds the renderer (a GPU-bound frame waits for the
    // GPU there, the offscreen output paces itself there): the debug screen tries again at its next update instead of
    // stalling the main thread, and waits only when it could not read for a quarter of a second.
    bool ReadDebugSnapshot(DebugSnapshot& snapshot, bool wait);
    bool ReadbackOutput(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height);

    // Reads back a colour texture's base level as RGBA8 (finishes the frame's GPU work first); with `raw`, the bits of
    // one channel instead (0 red to 3 alpha; float targets such as R32_FLOAT). For debugging.
    bool ReadbackTexture(const Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height,
        bool raw = false, uint32_t channel = 0);

    // Reads back the bits of a texture's red channel (R32_FLOAT depth resolves), one texel per `step` x `step` block,
    // without stalling either thread: RequestReadback queues the copy after the caller's earlier commands;
    // CollectReadback returns the latest one the GPU has finished (two frames after its request, once the GPU thread's
    // frame end has waited for it), false when there is none new. `convert` runs on the GPU thread when it finishes:
    // it turns the pixels (width x height, row-major) into what CollectReadback returns. One request per frame.
    using ReadbackConvert =
        std::function<void(const uint32_t* pixels, uint32_t width, uint32_t height, std::vector<uint32_t>& out)>;
    void RequestReadback(const Texture* texture, uint32_t step, ReadbackConvert convert);
    bool CollectReadback(std::vector<uint32_t>& data);

    // --- Render state, as the Direct3D replacement describes it (see gpu/draw.h) -------------------------------------

    // Sampler state; equal states share one sampler. Compared bytewise: zero-initialise before filling.
    struct SamplerState
    {
        plume::RenderFilter minFilter = plume::RenderFilter::LINEAR;
        plume::RenderFilter magFilter = plume::RenderFilter::LINEAR;
        plume::RenderMipmapMode mipFilter = plume::RenderMipmapMode::LINEAR;
        plume::RenderTextureAddressMode addressU = plume::RenderTextureAddressMode::WRAP;
        plume::RenderTextureAddressMode addressV = plume::RenderTextureAddressMode::WRAP;
        plume::RenderTextureAddressMode addressW = plume::RenderTextureAddressMode::WRAP;
        plume::RenderBorderColor borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
        uint32_t maxAnisotropy = 1;
        float mipLodBias = 0.0f;
        float minLod = 0.0f;
        float maxLod = 1000.0f;
    };

    struct BlendState
    {
        bool enabled;
        plume::RenderBlend srcColor, dstColor;
        plume::RenderBlendOperation colorOp;
        plume::RenderBlend srcAlpha, dstAlpha;
        plume::RenderBlendOperation alphaOp;
        uint8_t writeMask; // plume::RenderColorWriteEnable bits
    };

    struct DepthStencilState
    {
        bool depthEnabled;
        bool depthWrite;
        plume::RenderComparisonFunction depthFunction;
        bool stencilEnabled;
        uint8_t stencilReadMask, stencilWriteMask, stencilReference;
        plume::RenderStencilFaceDesc front, back;
    };

    struct RasterState
    {
        plume::RenderCullMode cullMode;
        plume::RenderFrontFace frontFace;
        int32_t depthBias;
        float slopeScaledDepthBias;
    };
}
