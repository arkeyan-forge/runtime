#pragma once

// State shared by the gpu/ sources (not part of the gpu API).
#include "gpu/gpu.h"
#include <plume_render_interface.h>
#include <SDL3/SDL_metal.h>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

struct gpu::Texture
{
    TextureDesc desc;
    std::unique_ptr<plume::RenderTexture> texture{};
    mutable plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    uint32_t heapIndex = 0; // slot in the bindless heap of its kind (sampled textures only; 0 = none)

    // Swizzled views of it and their heap slots.
    struct SwizzledView
    {
        uint32_t mapping;
        std::unique_ptr<plume::RenderTextureView> view;
        uint32_t heapIndex;
    };
    std::vector<SwizzledView> swizzledViews{};
};

namespace gpu
{
    struct DrawDesc;
}

namespace gpu::detail
{
    struct Context
    {
        SDL_Window* window = nullptr;
        std::unique_ptr<plume::RenderInterface> renderInterface;
        std::unique_ptr<plume::RenderDevice> device;
        std::unique_ptr<plume::RenderCommandQueue> queue;

        // A frame goes to the GPU in parts as it is recorded (Flush), so the GPU works while the CPU records the rest.
        // Each part has its own command list and fence; Submit waits for all of them.
        struct Submission
        {
            std::unique_ptr<plume::RenderCommandList> commandList;
            std::unique_ptr<plume::RenderCommandFence> fence;
        };
        std::vector<Submission> submissions;
        uint32_t submitted = 0; // parts of the frame sent so far; submissions[submitted] records the next one
        bool recording = false;
        uint32_t drawsSinceFlush = 0;
        // The framebuffer the command list has bound (SetFramebuffer), nullptr at the start of each command list.
        const plume::RenderFramebuffer* boundFramebuffer = nullptr;
        // D3D12: the game draws' state the command list has bound (gpu::DrawNow sets only what changed). Reset with each
        // command list and by every other pass that binds graphics state (BoundGraphics{}).
        struct BoundGraphics
        {
            bool valid = false; // the root signature and the four descriptor heaps are bound
            const plume::RenderPipeline* pipeline = nullptr;
            bool hasViewport = false;
            plume::RenderViewport viewport;
            plume::RenderRect scissor;
            float blendFactor[4] = {};
            plume::RenderBufferReference roots[4];
        } bound;

        // Upload memory of the frame being recorded (texture data, vertices, indices, constants); reused once the GPU
        // has finished the frame.
        std::unique_ptr<plume::RenderBuffer> staging;
        // `staging` mapped once for its lifetime: a map and an unmap per upload were ~8 % of the GPU thread's draw time.
        uint8_t* stagingMemory = nullptr;
        uint64_t stagingCapacity = 0;
        uint64_t stagingUsed = 0;
        std::vector<std::unique_ptr<plume::RenderBuffer>> retiredStaging;

        // The frame before, which the GPU may still be running while this one is recorded: its parts, its upload
        // memory and what its present pass used. EndFrame swaps these with this frame's.
        struct Frame
        {
            std::vector<Submission> submissions;
            uint32_t submitted = 0;
            std::unique_ptr<plume::RenderBuffer> staging;
            uint8_t* stagingMemory = nullptr;
            uint64_t stagingCapacity = 0;
            std::vector<std::unique_ptr<plume::RenderBuffer>> retiredStaging;
            std::unique_ptr<plume::RenderDescriptorSet> presentSet;
            std::unique_ptr<plume::RenderDescriptorSet> overlaySet;
            std::unique_ptr<plume::RenderCommandSemaphore> acquireSemaphore;
        } previous;

        std::unique_ptr<plume::RenderSampler> linearClamp;
        std::unique_ptr<plume::RenderShader> presentVS;
        std::unique_ptr<plume::RenderShader> presentPS;
        std::unique_ptr<plume::RenderPipelineLayout> presentLayout;
        std::unique_ptr<plume::RenderPipeline> presentPipeline;
        std::unique_ptr<plume::RenderDescriptorSet> presentSet;

        // Overlay (gpu::SetOverlay): the present pass with alpha blending, its own set.
        std::unique_ptr<plume::RenderShader> overlayPS;
        std::unique_ptr<plume::RenderPipeline> overlayPipeline;
        std::unique_ptr<plume::RenderDescriptorSet> overlaySet;
        const gpu::Texture* overlay = nullptr;

        std::unique_ptr<plume::RenderShader> readbackCS;
        std::unique_ptr<plume::RenderPipelineLayout> readbackLayout;
        std::unique_ptr<plume::RenderPipeline> readbackPipeline;
        std::unique_ptr<plume::RenderShader> readbackRawCS;
        std::unique_ptr<plume::RenderPipeline> readbackRawPipeline;
        std::unique_ptr<plume::RenderDescriptorSet> readbackSet;
        std::unique_ptr<plume::RenderBuffer> readbackGpu;
        std::unique_ptr<plume::RenderBuffer> readbackCpu;
        uint64_t readbackCapacity = 0;
        // Asynchronous read-backs (gpu::RequestReadback), recorded on the GPU thread, one slot per frame parity: own
        // set and buffers, the size, the frame that recorded it (`frames` counts EndFrame calls). EndFrame copies a
        // finished one into `rawReady`, which producers take (CollectReadback) under `rawReadyMutex`.
        std::unique_ptr<plume::RenderPipelineLayout> rawReadbackLayout;
        struct RawReadback
        {
            std::unique_ptr<plume::RenderDescriptorSet> set;
            std::unique_ptr<plume::RenderBuffer> gpu;
            std::unique_ptr<plume::RenderBuffer> cpu;
            uint64_t capacity = 0;
            uint32_t width = 0, height = 0;
            bool pending = false;
            uint64_t frame = 0;
            gpu::ReadbackConvert convert;
        } rawReadbacks[2];
        uint64_t frames = 0;
        std::mutex rawReadyMutex;
        std::vector<uint32_t> rawReady;
        bool rawReadyValid = false;

        // Depth resolves (gpu::ResolveDepth): set 0 = { depth texture }, push constants = source offset.
        std::unique_ptr<plume::RenderShader> resolveDepthPS;
        std::unique_ptr<plume::RenderPipelineLayout> resolveDepthLayout;
        std::unique_ptr<plume::RenderPipeline> resolveDepthPipeline;

        SDL_MetalView metalView = nullptr;
        std::unique_ptr<plume::RenderSwapChain> swapChain;
        std::vector<std::unique_ptr<plume::RenderFramebuffer>> swapChainFramebuffers;
        std::unique_ptr<plume::RenderCommandSemaphore> acquireSemaphore;
        std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> releaseSemaphores;

        // The device was removed (D3D12): from then on the gpu functions do nothing (CheckDeviceLost). Atomic: the
        // pipeline precompile threads check it too.
        std::atomic<bool> lost = false;
        bool presented = false; // a frame reached the output

        // The frame's costs for the perf log (LogPerf in gpu.cpp), reset at every Present.
        struct Perf
        {
            uint64_t drawNs = 0;     // inside gpu::Draw
            uint32_t draws = 0;
            uint64_t gpuWaitNs = 0;  // waiting for the GPU (WaitForSubmissions)
            uint64_t presentNs = 0;  // the swap chain's Present, with the UI thread handoff and the vblank wait
            uint32_t flushes = 0;    // command lists sent during the frame
        } perf;

        gpu::Texture offscreen;
        std::unique_ptr<plume::RenderFramebuffer> offscreenFramebuffer;
        uint64_t nextOffscreenFrame = 0;
    };

    extern std::unique_ptr<Context> g_context;

    // Read-only draw-cache portion of ReadDebugSnapshot; caller holds g_mutex.
    void ReadDebugDrawSnapshot(gpu::DebugSnapshot& snapshot);

    // Every public gpu function takes it: the game's render thread draws while the main thread may still present
    // movie frames.
    extern std::recursive_mutex g_mutex;

    uint64_t AlignUp(uint64_t value, uint64_t alignment);
    // True once the device is lost: the public gpu functions return at once (no device calls, no log spam).
    bool DeviceLost();
    // A file of the player's data for this game (SKY_SAVE_DIR, else the game's data folder), next to pipelines.bin;
    // empty without one (no shader cache: the GPU test).
    std::filesystem::path PlayerDataFile(const char* name);
    // D3D12: "[gpu] step N: <what> (thread T)" for the first 40 steps of the run, with the device state, so a console
    // log shows which call came first after a removal.
    void Step(const char* what);
    // Checks for a removed device (D3D12) after a frame or any failed D3D12 call (a resource, a pipeline, a null map);
    // the first time, logs why (reason, DRED, memory) and stops GPU work. Thread-safe.
    void CheckDeviceLost();
#if defined(_WIN32)
    // d3d12_diagnostics.cpp: DRED before the device is created; the removal report.
    void D3D12EnableDred();
    bool D3D12DeviceRemoved(plume::RenderDevice& device);
    // d3d12_pipeline_cache.cpp: the persistent pipeline library (pipelines.d3d12), opened before the pipelines are
    // built and saved when new ones were added.
    void D3D12OpenPipelineLibrary(plume::RenderDevice& device, const std::filesystem::path& path, uint64_t identity);
    void D3D12SavePipelineLibrary(plume::RenderDevice& device, const char* when);
    // The removal reason (S_OK while the device works).
    long D3D12RemovedReason(plume::RenderDevice& device);
    // Logs the adapter's local memory budget and usage (IDXGIAdapter3::QueryVideoMemoryInfo).
    void D3D12LogMemory(plume::RenderDevice& device, const char* when);
#endif
    plume::RenderCommandList& Record();
    // Sends what was recorded so far to the GPU without waiting for it.
    void Flush();
    // Binds `framebuffer` unless the command list already has it bound: plume ends the render pass at every
    // setFramebuffer, and every pass loads and stores all of its attachments.
    void SetFramebuffer(plume::RenderCommandList& list, const plume::RenderFramebuffer* framebuffer);
    void Submit();
    void Transition(plume::RenderCommandList& list, const gpu::Texture& texture, plume::RenderTextureLayout layout,
        plume::RenderBarrierStages stages);
    // Upload memory for this frame, `alignment`-aligned; usable as vertex, index and constant data and by address.
    uint64_t AllocateStaging(uint64_t size, plume::RenderBuffer*& buffer, uint64_t alignment = 512);
    // The mapped memory of the buffer the last AllocateStaging returned (nullptr if it could not be mapped).
    uint8_t* StagingMemory();

    // Called at the end of every frame: destroys the textures released during the frame (the GPU has finished it when
    // there are some, EndFrame) and forgets the frame's constant uploads.
    void DrawFrameDone();
    bool DrawHasPendingDestroys();
    // Creates the draw context (pipeline list, heaps, placeholders); gpu::Init calls it.
    void DrawInit();
    // Stops the threads building listed pipelines (before the device goes away).
    void DrawShutdown();
    void DeferDestroy(gpu::Texture* texture);

    // --- The GPU thread (worker.cpp) ---------------------------------------------------------------------------------
    // Producers (the callers of the public gpu functions) hold this; the GPU thread never takes it.
    extern std::recursive_mutex g_producerMutex;

    // UWP: the UI thread issues the swap chain's Presents (gpu::ServicePresents). While it waits for the GPU thread, or
    // for the producer lock, it keeps taking the Present the GPU thread may be handing it, or the two would wait for
    // each other until the handoff's 200 ms timeout.
    bool IsPresentThread();
    void ServicePendingPresent(); // non-blocking; does nothing off the present thread

    // g_producerMutex, taken so (above) on the present thread.
    class ProducerLock
    {
    public:
        ProducerLock()
        {
            if (!IsPresentThread())
            {
                g_producerMutex.lock();
                return;
            }
            while (!g_producerMutex.try_lock())
            {
                ServicePendingPresent();
                std::this_thread::yield();
            }
        }
        ~ProducerLock() { g_producerMutex.unlock(); }
        ProducerLock(const ProducerLock&) = delete;
        ProducerLock& operator=(const ProducerLock&) = delete;
    };

    // condition_variable::wait, servicing the Present handoff on the present thread.
    template <typename Predicate>
    void WaitServicingPresents(
        std::unique_lock<std::mutex>& lock, std::condition_variable& condition, Predicate predicate)
    {
        if (!IsPresentThread())
        {
            condition.wait(lock, predicate);
            return;
        }
        while (!condition.wait_for(lock, std::chrono::milliseconds(1), predicate))
        {
            lock.unlock();
            ServicePendingPresent();
            lock.lock();
        }
    }
    bool WorkerRunning();
    void StartWorker();
    void StopWorker(); // runs what is queued first
    // Runs `command` on the GPU thread after what is queued (at once without one).
    void Enqueue(std::function<void()> command);
    void RunAndWait(const std::function<void()>& command);
    // Memory for the producer frame's queued commands, valid until the GPU thread has run its Present.
    void* FrameAllocate(size_t bytes, size_t alignment);
    const void* FrameCopy(const void* data, size_t bytes);
    // Producer frames: the one being recorded, and the last one the GPU has finished.
    uint64_t ProducerFrame();
    uint64_t CompletedFrame();
    // The GPU thread waited for the GPU: the frame before the one it records is finished, and that one if `finished`.
    void ConsumerFrameDone(bool finished);
    // Upload memory written by producers (vertex data, textures), reused once the GPU finished the producer frame.
    uint64_t AllocateUpload(uint64_t size, plume::RenderBuffer*& buffer, uint64_t alignment);
    // Queues the frame's Present and starts the next producer frame; waits while the GPU thread is a frame behind.
    void EndProducerFrame(std::function<void()> present);
    // Since the last call: time the GPU thread spent running commands, producers spent waiting for it, and the texture
    // workers spent decoding (all of them together).
    struct WorkerTimes
    {
        uint64_t busyNs, producerWaitNs;
        uint32_t textureThreads, textureJobs;
        uint64_t textureBusyNs;
    };
    void TakeWorkerTimes(WorkerTimes& times);
    void ShutdownUploads();
    // Runs `job` on a texture worker thread (at once without the GPU thread); `done` is set after it.
    void RunTextureJob(std::function<void()> job, std::shared_ptr<std::atomic<bool>> done);
    void WaitTextureJob(const std::atomic<bool>& done);
    // What the public functions queue (gpu.cpp, draw.cpp).
    void PresentNow(const gpu::Texture* image);
    bool ReadbackNow(
        const gpu::Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, bool raw = false);
    void DrawNow(const gpu::DrawDesc& draw);
    void ClearColorNow(gpu::Texture* target, const float rgba[4], const plume::RenderRect* rect);
    void ClearDepthStencilNow(
        gpu::Texture* target, bool depth, bool stencil, float z, uint8_t stencilValue, const plume::RenderRect* rect);
    void CopyTextureNow(gpu::Texture* destination, uint32_t x, uint32_t y, gpu::Texture* source,
        const plume::RenderRect& region, uint32_t slice);
    void ResolveDepthNow(
        gpu::Texture* destination, uint32_t x, uint32_t y, gpu::Texture* source, const plume::RenderRect& region);
    // Records the deferred resolve copies (draw.cpp, CopyTextureNow) that read or write `texture`, every one for nullptr:
    // before anything else on the GPU thread reads or writes it. Under g_mutex.
    void SettleCopies(const gpu::Texture* texture);
}
