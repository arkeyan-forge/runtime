#include "gpu/gpu.h"
#include "gpu/auto_scale.h"
#include "gpu/buffer_cache.h"
#include "gpu/context.h"
#include "gpu/fsr/fsr.h"
#include "gpu/fsr/framegen.h"
#include "gpu/frame_stats.h"
#include "gpu/shader_cache.h"
#include "../write_watch.h"
#include "../platform/platform.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <plume_render_interface.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include "../host/env.h"
#if defined(ARKEYAN_METAL)
#include <plume_metal.h>
#else
#if !defined(PLUME_D3D12_COREWINDOW)
// Vulkan Memory Allocator tests extension macros the headers leave undefined (-Wundef under SKY_WERROR on Linux).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef"
#include <plume_vulkan.h>
#pragma clang diagnostic pop
#endif
#if defined(_WIN32)
#include <plume_d3d12.h>
#endif
#endif

#if defined(ARKEYAN_METAL)
#include "shaders/present_metallib.h"
#endif
#if defined(_WIN32)
#include "shaders/present_OverlayPS_dxil.h"
#include "shaders/present_PresentPS_dxil.h"
#include "shaders/present_PresentVS_dxil.h"
#include "shaders/present_ReadbackCS_dxil.h"
#include "shaders/present_ReadbackRawCS_dxil.h"
#include "shaders/present_ResolveDepthPS_dxil.h"
#endif
#if !defined(ARKEYAN_METAL) && !defined(PLUME_D3D12_COREWINDOW)
#include "shaders/present_OverlayPS_spirv.h"
#include "shaders/present_PresentPS_spirv.h"
#include "shaders/present_PresentVS_spirv.h"
#include "shaders/present_ReadbackCS_spirv.h"
#include "shaders/present_ReadbackRawCS_spirv.h"
#include "shaders/present_ResolveDepthPS_spirv.h"
#endif

using namespace plume;

namespace plume
{
#if defined(ARKEYAN_METAL)
    std::unique_ptr<RenderInterface> CreateMetalInterface();
#endif
#if defined(_WIN32)
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
#if !defined(ARKEYAN_METAL) && !defined(PLUME_D3D12_COREWINDOW)
    std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
}

namespace
{
#if defined(ARKEYAN_METAL)
    const char* g_backendName = "Metal";
#elif defined(PLUME_D3D12_COREWINDOW)
    const char* g_backendName = "D3D12";
#else
    const char* g_backendName = "Vulkan"; // desktop Windows: or D3D12 (CreateDevice)
#endif

    constexpr RenderFormat OutputFormat = RenderFormat::B8G8R8A8_UNORM;
    constexpr uint32_t SwapChainBufferCount = 2;
    constexpr uint64_t OffscreenFrameNs = 1'000'000'000 / 60;

    // Buffer-to-texture copy placement, D3D12's rules (the strictest backend).
    constexpr uint64_t UploadRowAlignment = 256;
    constexpr uint64_t UploadOffsetAlignment = 512;
    constexpr uint64_t MinStagingSize = 4 << 20;

}

namespace gpu::detail
{
    std::unique_ptr<Context> g_context;
    std::recursive_mutex g_mutex;

    uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

    void Step(const char* what)
    {
#if defined(_WIN32)
        static std::atomic<uint32_t> steps = 0;
        if (g_context == nullptr || g_context->device == nullptr ||
            g_context->renderInterface->getCapabilities().shaderFormat != RenderShaderFormat::DXIL)
            return;
        uint32_t step = steps++;
        if (step >= 40)
            return;
        long reason = D3D12RemovedReason(*g_context->device);
        fprintf(stderr, "[gpu] step %u: %s (thread %llu)%s\n", step + 1, what,
            static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF),
            reason == 0 ? "" : " - device REMOVED");
#else
        (void)what;
#endif
    }

    bool DeviceLost() { return g_context != nullptr && g_context->lost; }

    void CheckDeviceLost()
    {
#if defined(_WIN32)
        Context& c = *g_context;
        if (c.lost || c.renderInterface->getCapabilities().shaderFormat != RenderShaderFormat::DXIL)
            return;
        static std::mutex reportMutex; // one report, even when several threads see the removal
        std::lock_guard lock(reportMutex);
        if (!c.lost && D3D12DeviceRemoved(*c.device))
        {
            D3D12LogMemory(*c.device, "at the removal");
            c.lost = true;
            fprintf(stderr, "[gpu] the GPU device is lost: no more rendering until the app restarts\n");
        }
#endif
    }

    // The debug screen's GPU time per frame (frame_stats.h): Metal reads its command buffers' own GPU start and end,
    // Vulkan and D3D12 a timestamp at each end of a command list. Nothing is done while nothing measures.
    bool MetalTiming()
    {
#if defined(ARKEYAN_METAL)
        return g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::METAL;
#else
        return false;
#endif
    }

    void StartTiming(Context::Submission& s)
    {
        s.timestamped = false;
        if (!framestats::Enabled() || MetalTiming() || !g_context->device->getCapabilities().queryPools)
            return;
        if (s.timestamps == nullptr)
            s.timestamps = g_context->device->createQueryPool(2);
        s.commandList->resetQueryPool(s.timestamps.get(), 0, 2);
        s.commandList->writeTimestamp(s.timestamps.get(), 0);
        s.timestamped = true;
    }

    // Before the list ends; `present`: it carries the frame's present pass, the frame's last list.
    void EndTiming(Context::Submission& s, bool present)
    {
        s.timedFrame = 0;
        if (!framestats::Enabled())
            return;
        s.timedFrame = ConsumerFrame();
        s.timedPresent = present;
        if (s.timestamped)
            s.commandList->writeTimestamp(s.timestamps.get(), 1);
    }

    // After the list ends, before its execution: plume releases a Metal command buffer when it commits it.
    void KeepMetalBuffer(Context::Submission& s)
    {
#if defined(ARKEYAN_METAL)
        if (s.timedFrame != 0 && MetalTiming())
        {
            MTL::CommandBuffer* buffer = static_cast<MetalCommandList&>(*s.commandList).mtl;
            buffer->retain();
            s.metalBuffer = buffer;
        }
#else
        (void)s;
#endif
    }

    // Once its fence was waited for.
    void ReadTiming(Context::Submission& s)
    {
        if (s.timedFrame == 0 && s.metalBuffer == nullptr)
            return;
        uint64_t costStart = SDL_GetTicksNS();
        uint64_t start = 0, end = 0;
        bool known = false;
#if defined(ARKEYAN_METAL)
        if (s.metalBuffer != nullptr)
        {
            auto* buffer = static_cast<MTL::CommandBuffer*>(s.metalBuffer);
            double from = buffer->GPUStartTime(), to = buffer->GPUEndTime();
            buffer->release();
            s.metalBuffer = nullptr;
            known = from > 0 && to >= from;
            start = uint64_t(from * 1e9);
            end = uint64_t(to * 1e9);
        }
#endif
        if (s.timedFrame == 0)
            return;
        if (s.timestamped)
        {
            s.timestamps->queryResults();
            const uint64_t* results = s.timestamps->getResults();
            start = results[0];
            end = results[1];
            known = start != 0 && end >= start;
            s.timestamped = false;
        }
        framestats::GpuSpan(s.timedFrame, start, end, known, s.timedPresent);
        s.timedFrame = 0;
        framestats::AddCost(SDL_GetTicksNS() - costStart);
    }

    RenderCommandList& Record()
    {
        Context& c = *g_context;
        if (!c.recording)
        {
            if (c.submitted == c.submissions.size())
            {
                Context::Submission submission;
                submission.commandList = c.queue->createCommandList();
                submission.fence = c.device->createCommandFence();
                c.submissions.push_back(std::move(submission));
            }
            c.submissions[c.submitted].commandList->begin();
            StartTiming(c.submissions[c.submitted]);
            c.recording = true;
            c.boundFramebuffer = nullptr;
            c.bound = {};
            c.drawsSinceFlush = 0;
        }
        return *c.submissions[c.submitted].commandList;
    }

    // `present`: the list carries the frame's present pass (the offscreen output's).
    void FlushList(bool present)
    {
        if (g_context->recording)
            g_context->perf.flushes++;
        Context& c = *g_context;
        if (!c.recording)
            return;
        Context::Submission& submission = c.submissions[c.submitted++];
        EndTiming(submission, present);
        submission.commandList->end();
        KeepMetalBuffer(submission);
        c.recording = false;
        c.queue->executeCommandLists(submission.commandList.get(), submission.fence.get());
        Step("command list executed");
    }

    void Flush() { FlushList(false); }

    // Waits for the frame before (Context::previous); its upload memory becomes free.
    static void WaitForPrevious()
    {
        Context::Frame& previous = g_context->previous;
        uint64_t start = SDL_GetTicksNS();
        for (uint32_t i = 0; i < previous.submitted; i++)
        {
            g_context->queue->waitForCommandFence(previous.submissions[i].fence.get());
            ReadTiming(previous.submissions[i]);
        }
        g_context->perf.gpuWaitNs += SDL_GetTicksNS() - start;
        previous.submitted = 0;
        previous.retiredStaging.clear();
        CheckDeviceLost();
    }

    // Waits for everything sent: the frame before and this one so far, the command list being recorded included (sent
    // first). Resetting `submitted` under a recording list made the next Record() return a list already sent while the
    // recorded one was never committed: a swap chain resize in the middle of a chunked frame hung the GPU (Metal "GPU
    // Timeout Error") and the game for good (problem_catalogue.md, "Resizing the window froze the game").
    static void WaitForSubmissions()
    {
        Context& c = *g_context;
        Flush();
        WaitForPrevious();
        uint64_t start = SDL_GetTicksNS();
        for (uint32_t i = 0; i < c.submitted; i++)
        {
            c.queue->waitForCommandFence(c.submissions[i].fence.get());
            ReadTiming(c.submissions[i]);
        }
        c.perf.gpuWaitNs += SDL_GetTicksNS() - start;
        c.submitted = 0;
        CheckDeviceLost();
    }

    void SetFramebuffer(RenderCommandList& list, const RenderFramebuffer* framebuffer)
    {
        if (g_context->boundFramebuffer == framebuffer)
            return;
        list.setFramebuffer(framebuffer);
        g_context->boundFramebuffer = framebuffer;
        g_context->targetSwitches++;
    }

    static void FrameDone()
    {
        g_context->retiredStaging.clear();
        g_context->stagingUsed = 0;
        DrawFrameDone();
    }

    // Runs what was recorded and waits for the whole frame.
    void Submit()
    {
        Flush();
        WaitForSubmissions();
        FrameDone();
        ConsumerFrameDone(false);
    }

    // Ends a frame whose parts were all sent without waiting for it, so the GPU finishes it while the next one is
    // recorded: waits for the frame before instead, and the next frame records into that one's command lists, upload
    // memory, present sets and acquire semaphore. A frame that released textures waits for itself (they are destroyed
    // now, and the frame may use them).
    // After EndFrame's wait: a read-back recorded two frames before (or earlier) has finished on the GPU; its pixels
    // go to `rawReady` for the producers.
    static void FinishRawReadbacks()
    {
        Context& c = *g_context;
        for (Context::RawReadback& slot : c.rawReadbacks)
        {
            if (!slot.pending || slot.frame + 2 > c.frames)
                continue;
            slot.pending = false;
            auto data = static_cast<const uint32_t*>(slot.cpu->map());
            if (data == nullptr)
                continue;
            std::vector<uint32_t> converted;
            slot.convert(data, slot.width, slot.height, converted);
            slot.cpu->unmap();
            std::lock_guard ready(c.rawReadyMutex);
            c.rawReady.swap(converted);
            c.rawReadyValid = true;
        }
    }

    static void EndFrame()
    {
        Context& c = *g_context;
        c.frames++;
        if (DrawHasPendingDestroys())
        {
            WaitForSubmissions();
            FrameDone();
            ConsumerFrameDone(true);
            FinishRawReadbacks();
            return;
        }
        WaitForPrevious();
        ConsumerFrameDone(false);
        FinishRawReadbacks();
        Context::Frame& previous = c.previous;
        std::swap(c.submissions, previous.submissions);
        std::swap(c.submitted, previous.submitted);
        std::swap(c.staging, previous.staging);
        std::swap(c.stagingMemory, previous.stagingMemory);
        std::swap(c.stagingCapacity, previous.stagingCapacity);
        std::swap(c.retiredStaging, previous.retiredStaging);
        std::swap(c.presentSet, previous.presentSet);
        std::swap(c.overlaySet, previous.overlaySet);
        std::swap(c.acquireSemaphore, previous.acquireSemaphore);
        c.stagingUsed = 0;
        DrawFrameDone();
    }

    void Transition(
        RenderCommandList& list, const gpu::Texture& texture, RenderTextureLayout layout, RenderBarrierStages stages)
    {
        if (texture.layout == layout)
            return;
        list.barriers(stages, RenderTextureBarrier(texture.texture.get(), layout));
        texture.layout = layout;
    }

    uint64_t AllocateStaging(uint64_t size, RenderBuffer*& buffer, uint64_t alignment)
    {
        Context& c = *g_context;
        uint64_t offset = AlignUp(c.stagingUsed, alignment);
        if (c.staging == nullptr || offset + size > c.stagingCapacity)
        {
            if (c.staging != nullptr)
            {
                if (c.stagingMemory != nullptr)
                    c.staging->unmap();
                c.retiredStaging.push_back(std::move(c.staging));
            }
            c.stagingCapacity = std::max({ size, c.stagingCapacity * 2, MinStagingSize });
            c.staging = c.device->createBuffer(RenderBufferDesc::UploadBuffer(
                c.stagingCapacity, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT |
                                       RenderBufferFlag::DEVICE_ADDRESSABLE));
            c.stagingMemory = c.staging != nullptr ? static_cast<uint8_t*>(c.staging->map()) : nullptr;
            offset = 0;
        }
        c.stagingUsed = offset + size;
        buffer = c.staging.get();
        return offset;
    }

    uint8_t* StagingMemory() { return g_context->stagingMemory; }
}

using namespace gpu::detail;

namespace
{
    // The present pass's shaders for the running backend. One metallib holds every entry point; DXIL (D3D12) and
    // SPIR-V (Vulkan) have one blob per entry point.
    std::unique_ptr<RenderShader> CreatePresentShader(const char* entryPoint)
    {
        RenderShaderFormat format = g_context->renderInterface->getCapabilities().shaderFormat;
#if defined(ARKEYAN_METAL)
        return g_context->device->createShader(g_presentMetallib, sizeof(g_presentMetallib), entryPoint, format);
#else
        std::span<const std::pair<const char*, std::span<const uint8_t>>> blobs;
#if defined(_WIN32)
        static const std::pair<const char*, std::span<const uint8_t>> dxil[] = {
            { "PresentVS", g_presentPresentVSDxil }, { "PresentPS", g_presentPresentPSDxil },
            { "ReadbackCS", g_presentReadbackCSDxil }, { "ReadbackRawCS", g_presentReadbackRawCSDxil },
            { "ResolveDepthPS", g_presentResolveDepthPSDxil }, { "OverlayPS", g_presentOverlayPSDxil }
        };
        if (format == RenderShaderFormat::DXIL)
            blobs = dxil;
#endif
#if !defined(PLUME_D3D12_COREWINDOW)
        static const std::pair<const char*, std::span<const uint8_t>> spirv[] = {
            { "PresentVS", g_presentPresentVSSpirv }, { "PresentPS", g_presentPresentPSSpirv },
            { "ReadbackCS", g_presentReadbackCSSpirv }, { "ReadbackRawCS", g_presentReadbackRawCSSpirv },
            { "ResolveDepthPS", g_presentResolveDepthPSSpirv }, { "OverlayPS", g_presentOverlayPSSpirv }
        };
        if (format == RenderShaderFormat::SPIRV)
            blobs = spirv;
#endif
        for (const auto& [name, blob] : blobs)
        {
            if (strcmp(name, entryPoint) == 0)
                return g_context->device->createShader(blob.data(), blob.size(), entryPoint, format);
        }
        return nullptr;
#endif
    }

    bool CreatePipelines()
    {
        Context& c = *g_context;
        RenderSamplerDesc samplerDesc;
        samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
        samplerDesc.addressU = RenderTextureAddressMode::CLAMP;
        samplerDesc.addressV = RenderTextureAddressMode::CLAMP;
        samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
        c.linearClamp = c.device->createSampler(samplerDesc);

        c.presentVS = CreatePresentShader("PresentVS");
        c.presentPS = CreatePresentShader("PresentPS");
        c.readbackCS = CreatePresentShader("ReadbackCS");
        c.readbackRawCS = CreatePresentShader("ReadbackRawCS");
        c.resolveDepthPS = CreatePresentShader("ResolveDepthPS");
        c.overlayPS = CreatePresentShader("OverlayPS");
        if (c.presentVS == nullptr || c.presentPS == nullptr || c.readbackCS == nullptr || c.readbackRawCS == nullptr ||
            c.resolveDepthPS == nullptr || c.overlayPS == nullptr)
            return false;

        // Present: set 0 = { image texture, immutable linear clamp sampler }.
        RenderDescriptorSetBuilder setBuilder;
        setBuilder.begin();
        setBuilder.addTexture(0);
        setBuilder.addImmutableSampler(1, c.linearClamp.get());
        setBuilder.end();
        c.presentSet = setBuilder.create(c.device.get());
        c.overlaySet = setBuilder.create(c.device.get());
        c.previous.presentSet = setBuilder.create(c.device.get());
        c.previous.overlaySet = setBuilder.create(c.device.get());
        for (uint32_t i = 0; i < 2; i++)
        {
            c.generatedSets[i] = setBuilder.create(c.device.get());
            c.generatedOverlaySets[i] = setBuilder.create(c.device.get());
        }

        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();
        c.presentLayout = layoutBuilder.create(c.device.get());

        RenderGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.pipelineLayout = c.presentLayout.get();
        pipelineDesc.vertexShader = c.presentVS.get();
        pipelineDesc.pixelShader = c.presentPS.get();
        pipelineDesc.renderTargetFormat[0] = OutputFormat;
        pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        c.presentPipeline = c.device->createGraphicsPipeline(pipelineDesc);
        pipelineDesc.pixelShader = c.overlayPS.get();
        pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        c.overlayPipeline = c.device->createGraphicsPipeline(pipelineDesc);
        pipelineDesc.pixelShader = c.presentPS.get();
        pipelineDesc.renderTargetBlend[0] = RenderBlendDesc::Copy();

        // Readback: set 0 = { output texture, RGBA8 words }.
        setBuilder.begin();
        setBuilder.addTexture(0);
        setBuilder.addReadWriteByteAddressBuffer(1);
        setBuilder.end();
        c.readbackSet = setBuilder.create(c.device.get());
        for (Context::RawReadback& slot : c.rawReadbacks)
            slot.set = setBuilder.create(c.device.get());

        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.end();
        c.readbackLayout = layoutBuilder.create(c.device.get());
        c.readbackPipeline = c.device->createComputePipeline(
            RenderComputePipelineDesc(c.readbackLayout.get(), c.readbackCS.get(), 8, 8, 1));
        // Raw read-back: the same set, push constants (binding 0, space 1) = the step.
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.addPushConstant(0, 1, 8, RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        c.rawReadbackLayout = layoutBuilder.create(c.device.get());
        c.readbackRawPipeline = c.device->createComputePipeline(
            RenderComputePipelineDesc(c.rawReadbackLayout.get(), c.readbackRawCS.get(), 8, 8, 1));

        // Depth resolve: set 0 = { depth texture }, push constants (binding 0, space 1) = the source offset.
        setBuilder.begin();
        setBuilder.addTexture(0);
        setBuilder.end();
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.addPushConstant(0, 1, 8, RenderShaderStageFlag::PIXEL);
        layoutBuilder.end();
        c.resolveDepthLayout = layoutBuilder.create(c.device.get());
        pipelineDesc.pipelineLayout = c.resolveDepthLayout.get();
        pipelineDesc.pixelShader = c.resolveDepthPS.get();
        pipelineDesc.renderTargetFormat[0] = RenderFormat::R32_FLOAT;
        c.resolveDepthPipeline = c.device->createGraphicsPipeline(pipelineDesc);
        return c.presentPipeline != nullptr && c.overlayPipeline != nullptr && c.readbackPipeline != nullptr &&
               c.readbackRawPipeline != nullptr && c.resolveDepthPipeline != nullptr;
    }

    // The native surface the backend presents to, when the window has one (not with SDL's offscreen driver).
    bool GetRenderWindow(SDL_Window* window, RenderWindow& renderWindow)
    {
        SDL_PropertiesID properties = SDL_GetWindowProperties(window);
#if defined(ARKEYAN_METAL)
        void* nsWindow = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (nsWindow == nullptr || (SDL_GetWindowFlags(window) & SDL_WINDOW_METAL) == 0)
            return false;
        g_context->metalView = SDL_Metal_CreateView(window);
        if (g_context->metalView == nullptr)
            return false;
        renderWindow = { nsWindow, SDL_Metal_GetLayer(g_context->metalView) };
        return true;
#elif defined(PLUME_D3D12_COREWINDOW)
        // UWP (host/uwp): the CoreWindow, at the size the host chose for the swap chain.
        int width = 0, height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        renderWindow = { static_cast<IUnknown*>(SDL_GetPointerProperty(properties, "SDL.window.winrt.window", nullptr)),
            uint32_t(width), uint32_t(height) };
        return renderWindow.coreWindow != nullptr;
#elif defined(_WIN32)
        renderWindow =
            static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        return renderWindow != nullptr;
#else
        // PLUME_SDL3_VULKAN_ENABLED: plume creates the surface through SDL.
        (void)properties;
        renderWindow = window;
#if defined(__APPLE__)
        // MoltenVK test build (ARKEYAN_MACOS_VULKAN): SDL's offscreen driver has no surface, so draw offscreen like the Metal
        // build does (SKY_GPU_SNAPSHOT reads frames back). Linux keeps SDL's headless swapchain (SKY_DUMP_FRAME).
        const char* driver = SDL_GetCurrentVideoDriver();
        // SKY_GPU_HEADLESS_SWAPCHAIN=1 keeps a swap chain there (MoltenVK's VK_EXT_headless_surface), for the swap
        // chain's own tests (SKY_GPU_TEST_ACQUIRE_FAIL below).
        if (driver != nullptr && SDL_strcmp(driver, "offscreen") == 0 &&
            skyenv::Get("SKY_GPU_HEADLESS_SWAPCHAIN") == nullptr)
            return false;
#endif
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN) != 0;
#endif
    }

    // Framebuffers cache the image size: rebuild them after every resize, once the GPU is idle.
    void ResizeSwapChain()
    {
        Context& c = *g_context;
        WaitForSubmissions();
        c.swapChainFramebuffers.clear();
        if (!c.swapChain->resize())
            return;
        for (uint32_t i = 0; i < c.swapChain->getTextureCount(); i++)
        {
            const RenderTexture* color = c.swapChain->getTexture(i);
            RenderFramebufferDesc framebufferDesc;
            framebufferDesc.colorAttachments = &color;
            framebufferDesc.colorAttachmentsCount = 1;
            c.swapChainFramebuffers.push_back(c.device->createFramebuffer(framebufferDesc));
        }
        while (c.releaseSemaphores.size() < c.swapChain->getTextureCount())
            c.releaseSemaphores.push_back(c.device->createCommandSemaphore());
    }

    // Vulkan: whether the swap chain got its window's surface (plume leaves it null when SDL cannot make one).
    bool HasSurface(const RenderSwapChain& swapChain)
    {
#if !defined(ARKEYAN_METAL) && !defined(PLUME_D3D12_COREWINDOW)
        if (g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::SPIRV)
            return static_cast<const VulkanSwapChain&>(swapChain).surface != VK_NULL_HANDLE;
#endif
        (void)swapChain;
        return true;
    }

    void CreateSwapChain()
    {
        Context& c = *g_context;
        c.swapChain = c.queue->createSwapChain(RenderSwapChainDesc(c.renderWindow, OutputFormat, SwapChainBufferCount));
        c.swapChain->setVsyncEnabled(gpu::g_settings.vsync);
        c.swapChainNoSurface = !HasSurface(*c.swapChain);
        if (!c.swapChainNoSurface)
            ResizeSwapChain();
    }

    // SKY_GPU_TEST_ACQUIRE_FAIL=<n>[:lost]: once a frame has been shown, the next n acquires fail as
    // VK_ERROR_OUT_OF_DATE_KHR (VK_ERROR_SURFACE_LOST_KHR with :lost) without asking the driver, so a test run proves
    // that frames come back (docs/runtime/gpu/plume_backends.md).
    struct AcquireFailTest
    {
        uint32_t left = 0;
        int32_t result = gpu::swapchain::OutOfDate;
    };

    AcquireFailTest ReadAcquireFailTest()
    {
        AcquireFailTest test;
        if (const char* value = skyenv::Get("SKY_GPU_TEST_ACQUIRE_FAIL"))
        {
            test.left = uint32_t(std::max(atoi(value), 0));
            if (strstr(value, ":lost") != nullptr)
                test.result = gpu::swapchain::SurfaceLost;
            if (test.left > 0)
                fprintf(stderr, "[gpu] test: %u acquires will fail as %s once a frame is shown\n", test.left,
                    gpu::swapchain::ResultName(test.result));
        }
        return test;
    }

    // `result` is the backend's code when it fails (Vulkan: the VkResult; Metal and D3D12: 0).
    bool AcquireImage(RenderCommandSemaphore* signal, uint32_t* index, int32_t& result)
    {
        Context& c = *g_context;
        static AcquireFailTest test = ReadAcquireFailTest();
        if (c.presented && test.left > 0)
        {
            test.left--;
            result = test.result;
            return false;
        }
        if (c.swapChain->acquireTexture(signal, index))
            return true;
        result = c.swapChain->getLastResult();
        return false;
    }

    // An acquire or a present failed on a window that has a size: the swap chain, or the swap chain and its surface, are
    // rebuilt before the next frame (gpu/swapchain_recovery.h). Before, plume's size check alone decided, and a driver
    // that refused every image at an unchanged size (NVIDIA on Wayland, fullscreen) left the screen black for good.
    // D3D12 names the present's HRESULT the same way; Metal names no result (0) and keeps its behaviour.
    void SwapChainFailed(const char* what, int32_t result)
    {
        Context& c = *g_context;
        int width = 0, height = 0;
        if (result == gpu::swapchain::Success || c.window == nullptr ||
            !SDL_GetWindowSizeInPixels(c.window, &width, &height) || width <= 0 || height <= 0)
            return;
        // A removed or reset D3D12 device fails every present: a new swap chain would too.
        if (gpu::swapchain::DeviceGone(result))
        {
            static bool logged = false;
            if (!logged)
                fprintf(stderr, "[gpu] swap chain: %s failed (%s, 0x%08X): the device is gone, not rebuilt\n", what,
                    gpu::swapchain::ResultName(result), uint32_t(result));
            logged = true;
            CheckDeviceLost();
            return;
        }
        const gpu::swapchain::Action action = c.recovery.Failed(result, SDL_GetTicks());
        if (action == gpu::swapchain::Action::Wait)
            return;
        const uint32_t attempt = c.recovery.Failures();
        if (c.recovery.LogStreak() && (attempt == 1 || attempt == gpu::swapchain::Recovery::FastAttempts + 1))
        {
            char code[16]; // an HRESULT in hexadecimal, as Microsoft writes them
            if (gpu::swapchain::IsHresult(result))
                snprintf(code, sizeof(code), "0x%08X", uint32_t(result));
            else
                snprintf(code, sizeof(code), "%d", result);
            fprintf(stderr, "[gpu] swap chain: %s failed (%s, %s) on a %dx%d window: %s\n", what,
                gpu::swapchain::ResultName(result), code, width, height,
                attempt == 1 ? "rebuilding it" : "still failing after 5 rebuilds, trying again every 2 s");
        }
        if (action == gpu::swapchain::Action::Rebuild && !c.swapChainNoSurface)
        {
            ResizeSwapChain();
            return;
        }
        // The swap chain goes first, then its surface (plume's destructor), then both come back.
        WaitForSubmissions();
        c.swapChainFramebuffers.clear();
        c.swapChain.reset();
        CreateSwapChain();
    }

    void FramePresented()
    {
        Context& c = *g_context;
        if (uint32_t attempts = c.recovery.Presented(); attempts > 0 && c.recovery.LogStreak())
            fprintf(stderr, "[gpu] swap chain: frames again after %u rebuild%s\n", attempts, attempts == 1 ? "" : "s");
    }

    void ResizeOffscreen(uint32_t width, uint32_t height)
    {
        Context& c = *g_context;
        WaitForSubmissions();
        c.offscreenFramebuffer.reset();
        c.offscreen.desc = {
            .width = width, .height = height, .format = OutputFormat, .usage = gpu::TextureUsage::RenderTarget
        };
        c.offscreen.texture = c.device->createTexture(RenderTextureDesc::ColorTarget(width, height, OutputFormat));
        c.offscreen.layout = RenderTextureLayout::UNKNOWN;

        const RenderTexture* color = c.offscreen.texture.get();
        RenderFramebufferDesc framebufferDesc;
        framebufferDesc.colorAttachments = &color;
        framebufferDesc.colorAttachmentsCount = 1;
        c.offscreenFramebuffer = c.device->createFramebuffer(framebufferDesc);
    }

    RenderViewport Letterbox(const gpu::TextureDesc& image, uint32_t outputWidth, uint32_t outputHeight)
    {
        float scale = std::min(float(outputWidth) / image.width, float(outputHeight) / image.height);
        uint32_t width = std::clamp(uint32_t(std::lround(image.width * scale)), 1u, outputWidth);
        uint32_t height = std::clamp(uint32_t(std::lround(image.height * scale)), 1u, outputHeight);
        return RenderViewport(
            float((outputWidth - width) / 2), float((outputHeight - height) / 2), float(width), float(height));
    }
}

gpu::Settings gpu::g_settings;

namespace
{
    // The adapter's driver version as its vendor writes it: D3D12 gives the user-mode driver's four 16-bit parts,
    // Vulkan a packed number (NVIDIA's own packing, everyone else's VK_MAKE_VERSION); Metal none.
    void WriteDriverVersion(
        char* text, size_t capacity, const RenderDeviceDescription& description, RenderShaderFormat format)
    {
        unsigned long long v = description.driverVersion;
        snprintf(text, capacity, "n/a");
        if (format == RenderShaderFormat::DXIL)
            snprintf(text, capacity, "%llu.%llu.%llu.%llu", v >> 48, v >> 32 & 0xFFFF, v >> 16 & 0xFFFF, v & 0xFFFF);
        else if (format == RenderShaderFormat::SPIRV && description.vendor == RenderDeviceVendor::NVIDIA)
            snprintf(text, capacity, "%llu.%llu.%llu.%llu", v >> 22 & 0x3FF, v >> 14 & 0xFF, v >> 6 & 0xFF, v & 0x3F);
        else if (format == RenderShaderFormat::SPIRV)
            snprintf(text, capacity, "%llu.%llu.%llu", v >> 22 & 0x3FF, v >> 12 & 0x3FF, v & 0xFFF);
    }

    std::string DriverVersion(const RenderDeviceDescription& description, RenderShaderFormat format)
    {
        char text[64];
        WriteDriverVersion(text, sizeof(text), description, format);
        return text;
    }

    // resolution_scale=auto: 2 only where the GPU is known to keep up at 2240x1408 (Apple silicon, discrete GPUs with
    // 2 GB or more), else 1. Integrated GPUs are fill-bound with the pixel count: a Radeon 840M (shared memory) lags at
    // x3, a little at x2, and is fine at x1.
    uint32_t AutoScale(
        const RenderDeviceDescription& description, const RenderDeviceCapabilities& capabilities, const char*& why)
    {
        constexpr uint64_t MinimumVideoMemory = 2ull << 30;
        if (description.vendor == RenderDeviceVendor::APPLE)
            return why = "Apple GPU", 2;
        if (capabilities.uma || description.type == RenderDeviceType::INTEGRATED)
            return why = "integrated GPU (shared memory)", 1;
        if (description.type != RenderDeviceType::DISCRETE && description.type != RenderDeviceType::UNKNOWN)
            return why = "virtual or software GPU", 1;
        if (description.dedicatedVideoMemory < MinimumVideoMemory)
            return why = "discrete GPU with less than 2 GB", 1;
        return why = "discrete GPU", 2;
    }

    bool g_scaleAuto = false; // the player left the resolution scale on auto (gpu/auto_scale.h)
    const char* const AutoScaleFile = "auto_scale.txt";

    // The adapter, the window and the resolution scale (chosen here when it is auto), for the log.
    void LogAdapter(SDL_Window* window, uint32_t requestedScale)
    {
        Context& c = *g_context;
        const RenderDeviceDescription& description = c.device->getDescription();
        const RenderDeviceCapabilities& capabilities = c.device->getCapabilities();
        static const char* const types[] = { "unknown type", "integrated", "discrete", "virtual", "CPU" };
        fprintf(stderr, "[gpu] adapter %s: vendor %04X, %s%s, %llu MB of video memory, driver %s\n",
            description.name.c_str(), unsigned(description.vendor),
            types[std::min<size_t>(size_t(description.type), std::size(types) - 1)],
            capabilities.uma ? ", unified memory" : "",
            static_cast<unsigned long long>(description.dedicatedVideoMemory >> 20),
            DriverVersion(description, c.renderInterface->getCapabilities().shaderFormat).c_str());

        if (window != nullptr)
        {
            int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
            SDL_GetWindowSize(window, &width, &height);
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            fprintf(stderr, "[gpu] window %dx%d (%dx%d pixels), display %dx%d at %.0f%%\n", width, height, pixelWidth,
                pixelHeight, mode != nullptr ? mode->w : 0, mode != nullptr ? mode->h : 0,
                100.0 * SDL_GetWindowDisplayScale(window));
        }

        if (requestedScale == 0)
        {
            const char* why = "";
            gpu::g_settings.resolutionScale = AutoScale(description, capabilities, why);
            g_scaleAuto = true;
            // An earlier run on this adapter found the GPU the limit at a higher scale.
            std::string remembered;
            if (std::filesystem::path file = PlayerDataFile(AutoScaleFile); !file.empty())
                std::getline(std::ifstream(file), remembered);
            const uint32_t learned = gpu::autoscale::Parse(remembered, description.name);
            if (learned != 0 && learned < gpu::g_settings.resolutionScale)
            {
                gpu::g_settings.resolutionScale = learned;
                why = "an earlier run found the GPU the limit at a higher scale on this adapter: auto_scale.txt";
            }
            fprintf(stderr, "[gpu] resolution scale auto: %u (%s; resolution_scale=1 to 10 sets it)\n",
                gpu::g_settings.resolutionScale, why);
        }
        uint32_t scale = gpu::g_settings.resolutionScale;
        fprintf(stderr, "[gpu] resolution scale %u: the 3D renders at %ux%u\n", scale, 1120 * scale, 704 * scale);
    }
}

SDL_WindowFlags gpu::WindowFlags()
{
#if defined(ARKEYAN_METAL)
    return SDL_WINDOW_METAL;
#elif defined(_WIN32)
    return 0;
#else
    return SDL_WINDOW_VULKAN;
#endif
}

bool gpu::Init(SDL_Window* window, uint32_t width, uint32_t height)
{
    std::lock_guard lock(g_mutex);
    // Auto (0) is chosen once the device is known (LogAdapter), before anything is rendered at a scale.
    uint32_t requestedScale = gpu::g_settings.resolutionScale;
    if (requestedScale == 0)
        gpu::g_settings.resolutionScale = 1;
    g_context = std::make_unique<Context>();
    Context& c = *g_context;
    c.window = window;
#if defined(ARKEYAN_METAL)
    c.renderInterface = CreateMetalInterface();
#elif defined(PLUME_D3D12_COREWINDOW)
    D3D12EnableDred();
    c.renderInterface = CreateD3D12Interface();
#elif defined(_WIN32)
    // Desktop Windows: the chosen backend (gpu_backend, D3D12 by default), then the other when it has no device;
    // SKY_GPU_BACKEND=d3d12 or vulkan names one and allows no other (gpu/gpu_backend.h).
    const gpubackend::Plan plan = gpubackend::Choose(g_settings.backend, skyenv::Get("SKY_GPU_BACKEND"));
    // SKY_GPU_TEST_NO_DEVICE=d3d12 or vulkan: that backend reports no device, so a test run proves the fallback.
    const char* refused = skyenv::Get("SKY_GPU_TEST_NO_DEVICE");
    auto open = [&](gpubackend::Backend backend) -> const char* {
        c.device.reset();
        c.renderInterface.reset();
        g_backendName = gpubackend::Label(backend);
        gpubackend::Backend test;
        if (refused != nullptr && gpubackend::Parse(refused, test) && test == backend)
            return "SKY_GPU_TEST_NO_DEVICE";
#if defined(ARKEYAN_SHADER_CACHE)
        // A cache made without the D3D12 stream (an older or a foreign one) leaves D3D12 nothing to draw the game with.
        if (backend == gpubackend::Backend::D3D12 && g_shaderCacheEntryCount != 0 && g_dxilCacheDecompressedSize == 0)
            return "the shader cache has no DXIL";
#endif
        if (backend == gpubackend::Backend::D3D12)
        {
            D3D12EnableDred();
            c.renderInterface = CreateD3D12Interface();
        }
        else
            c.renderInterface = CreateVulkanInterface();
        if (c.renderInterface == nullptr)
            return "its interface could not be created";
        c.device = c.renderInterface->createDevice();
        return c.device == nullptr ? "no device was created" : nullptr;
    };
    gpubackend::Run(plan, open, [](const char* line) { fprintf(stderr, "%s\n", line); });
#else
    c.renderInterface = CreateVulkanInterface();
    if (c.renderInterface != nullptr)
        c.device = c.renderInterface->createDevice();
#endif
#if defined(ARKEYAN_METAL) || defined(PLUME_D3D12_COREWINDOW)
    if (c.renderInterface != nullptr)
        c.device = c.renderInterface->createDevice();
#endif
    if (c.device == nullptr)
    {
        fprintf(stderr, "[gpu] no %s device\n", g_backendName);
        g_context.reset();
        return false;
    }
    Step("device created");
    c.queue = c.device->createCommandQueue(RenderCommandListType::DIRECT);
    Step("queue created");
    if (!CreatePipelines())
    {
        g_context.reset();
        return false;
    }

    RenderWindow renderWindow{};
    if (window != nullptr && GetRenderWindow(window, renderWindow))
    {
        Step("present pipelines created");
        c.acquireSemaphore = c.device->createCommandSemaphore();
        c.previous.acquireSemaphore = c.device->createCommandSemaphore();
        for (auto& semaphore : c.generatedAcquire)
            semaphore = c.device->createCommandSemaphore();
        c.renderWindow = renderWindow;
        CreateSwapChain();
        Step("swap chain created");
    }
    else
    {
        int pixelWidth = int(width), pixelHeight = int(height);
        if (window != nullptr)
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        ResizeOffscreen(uint32_t(std::max(pixelWidth, 1)), uint32_t(std::max(pixelHeight, 1)));
    }

    fprintf(stderr, "[gpu] %s on %s, %s output\n", g_backendName, c.device->getDescription().name.c_str(),
        c.swapChain != nullptr ? "swapchain" : "offscreen");
    LogAdapter(window, requestedScale);
#if defined(_WIN32)
    if (c.renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::DXIL)
        D3D12LogMemory(*c.device, "at start");
#endif
    // The draw context before the GPU thread: creating it uploads placeholder textures through the public functions.
    DrawInit();
    StartWorker();
    Step(WorkerRunning() ? "GPU thread started" : "no GPU thread");
    return true;
}

const char* gpu::BackendName() { return g_backendName; }

void gpu::Shutdown()
{
    ProducerLock producer;
    StopWorker();
    std::lock_guard lock(g_mutex);
    if (g_context == nullptr)
        return;
    if (g_context->recording || g_context->submitted != 0 || g_context->previous.submitted != 0)
        Submit();
    fsr::Shutdown();
    DrawShutdown();
    BufferCacheShutdown();
    ShutdownUploads();
    for (auto [buffer, memory] : { std::pair(g_context->staging.get(), g_context->stagingMemory),
             std::pair(g_context->previous.staging.get(), g_context->previous.stagingMemory) })
        if (buffer != nullptr && memory != nullptr)
            buffer->unmap();
    SDL_MetalView metalView = g_context->metalView;
    g_context.reset();
    if (metalView != nullptr)
        SDL_Metal_DestroyView(metalView);
}

gpu::Texture* gpu::CreateTexture(const TextureDesc& desc)
{
    // Not under g_mutex: the GPU thread may be recording. Creating a resource is thread-safe in every backend (Metal's
    // residency set, D3D12MA and VMA lock), and the new texture is not shared yet.
    if (g_context == nullptr)
        return nullptr; // headless: no device
    RenderTextureFlags flags = RenderTextureFlag::NONE;
    if (desc.usage & TextureUsage::RenderTarget)
        flags |= RenderTextureFlag::RENDER_TARGET;
    if (desc.usage & TextureUsage::DepthStencil)
        flags |= RenderTextureFlag::DEPTH_TARGET;
    if (desc.usage & TextureUsage::Storage) // UNORDERED_ACCESS for D3D12 and Metal, STORAGE for Vulkan's usage bit
        flags |= RenderTextureFlag::UNORDERED_ACCESS | RenderTextureFlag::STORAGE;
    if (desc.cube)
        flags |= RenderTextureFlag::CUBE;

    RenderTextureDesc textureDesc =
        desc.depth > 1
            ? RenderTextureDesc::Texture3D(desc.width, desc.height, desc.depth, desc.mipLevels, desc.format, flags)
            : RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, desc.width, desc.height, 1, desc.mipLevels,
                  desc.arraySize, desc.format, flags);

    // Render and depth targets are committed resources: D3D12 then initializes their compression metadata. A placed one
    // must be cleared, discarded or copied to before any other use, or the GPU may read garbage metadata and fault (AMD).
    textureDesc.committed = (desc.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)) != 0;

    auto texture = new Texture{ .desc = desc };
#if defined(_WIN32)
    // D3D12: the big allocations, for the memory budget (the console renders at scale 2).
    uint64_t bytes = uint64_t(desc.width) * desc.height * desc.depth * desc.arraySize * RenderFormatSize(desc.format) /
                     (RenderFormatBlockWidth(desc.format) * RenderFormatBlockWidth(desc.format));
    bool big =
        bytes >= (16u << 20) && g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::DXIL;
    if (big)
        fprintf(stderr, "[gpu] texture %ux%u x%u, format %d, %u mips: %.0f MB\n", desc.width, desc.height,
            desc.depth * desc.arraySize, int(desc.format), desc.mipLevels, double(bytes) / (1 << 20));
#endif
    if (!DeviceLost())
    {
        texture->texture = g_context->device->createTexture(textureDesc);
        CheckDeviceLost(); // a failed creation is often the first sign of a removed device
#if defined(_WIN32)
        if (big && !DeviceLost())
            D3D12LogMemory(*g_context->device, "after it");
#endif
    }
    return texture;
}

const gpu::TextureDesc& gpu::Desc(const Texture* texture) { return texture->desc; }

void gpu::DestroyTexture(Texture* texture)
{
    ProducerLock producer;
    if (texture != nullptr)
        Enqueue([texture] {
            std::lock_guard lock(g_mutex);
            DeferDestroy(texture);
        });
}

namespace
{
    // Texture uploads queued since the start, while frame_stats.h measures (under the producer lock).
    uint64_t g_uploads, g_uploadBytes;

    // Where a subresource upload goes: producer upload memory with D3D12's row alignment, and the copy's footprint.
    struct UploadPlace
    {
        RenderBuffer* buffer;
        uint64_t offset;
        uint8_t* mapped; // of the upload, unmapped by Commit
        uint32_t rowBytes, rows, pitch;
        uint32_t footprintWidth, footprintHeight, footprintRow;
    };

    bool PlaceUpload(const gpu::Texture& texture, uint32_t mipLevel, UploadPlace& place)
    {
        const gpu::TextureDesc& desc = texture.desc;
        uint32_t width = std::max(desc.width >> mipLevel, 1u);
        uint32_t height = std::max(desc.height >> mipLevel, 1u);
        uint32_t blockWidth = RenderFormatBlockWidth(desc.format);
        uint32_t blockSize = RenderFormatSize(desc.format);
        place.rowBytes = (width + blockWidth - 1) / blockWidth * blockSize;
        place.rows = (height + blockWidth - 1) / blockWidth;
        place.pitch = uint32_t(AlignUp(place.rowBytes, UploadRowAlignment));
        place.offset = AllocateUpload(uint64_t(place.pitch) * place.rows, place.buffer, UploadOffsetAlignment);
        if (gpu::framestats::Enabled())
        {
            g_uploads++;
            g_uploadBytes += uint64_t(place.pitch) * place.rows;
        }
        auto mapped = static_cast<uint8_t*>(place.buffer->map());
        if (mapped == nullptr)
        {
            CheckDeviceLost();
            return false;
        }
        place.mapped = mapped + place.offset;
        // D3D12 wants the footprint of a block-compressed mip in whole blocks (a 2x2 BC mip is copied as 4x4); Metal and
        // Vulkan want the mip's own size.
        place.footprintWidth = width;
        place.footprintHeight = height;
        if (g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::DXIL)
        {
            place.footprintWidth = uint32_t(AlignUp(width, blockWidth));
            place.footprintHeight = uint32_t(AlignUp(height, blockWidth));
        }
        place.footprintRow = place.pitch / blockSize * blockWidth;
        return true;
    }

    // Queues the copy into the texture, after `done` (a texture job) when given.
    void QueueUploadCopy(gpu::Texture* texture, uint32_t mipLevel, uint32_t arraySlice, const UploadPlace& place,
        std::shared_ptr<std::atomic<bool>> done)
    {
        Enqueue([=] {
            if (done != nullptr)
                WaitTextureJob(*done);
            std::lock_guard lock(g_mutex);
            if (DeviceLost())
                return;
            SettleCopies(texture);
            RenderCommandList& list = Record();
            Transition(list, *texture, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
            list.copyTextureRegion(RenderTextureCopyLocation::Subresource(texture->texture.get(), mipLevel, arraySlice),
                RenderTextureCopyLocation::PlacedFootprint(place.buffer, texture->desc.format, place.footprintWidth,
                    place.footprintHeight, 1, place.footprintRow, place.offset));
        });
    }
}

void gpu::UploadTexture(Texture* texture, const void* data, uint32_t rowPitch, uint32_t mipLevel, uint32_t arraySlice)
{
    // The data goes into upload memory now (the caller's buffer is temporary); the GPU thread records the copy.
    ProducerLock producer;
    UploadPlace place;
    if (g_context == nullptr || texture == nullptr || DeviceLost() || !PlaceUpload(*texture, mipLevel, place))
        return;
    auto source = static_cast<const uint8_t*>(data);
    for (uint32_t row = 0; row < place.rows; row++)
        memcpy(place.mapped + uint64_t(row) * place.pitch, source + uint64_t(row) * rowPitch, place.rowBytes);
    place.buffer->unmap();
    QueueUploadCopy(texture, mipLevel, arraySlice, place, nullptr);
}

void gpu::UploadTextureAsync(Texture* texture, uint32_t mipLevel, uint32_t arraySlice,
    std::function<void(uint8_t* destination, uint32_t rowPitch)> fill)
{
    ProducerLock producer;
    UploadPlace place;
    if (g_context == nullptr || texture == nullptr || DeviceLost() || !PlaceUpload(*texture, mipLevel, place))
        return;
    auto done = std::make_shared<std::atomic<bool>>(false);
    RunTextureJob(
        [fill = std::move(fill), place] {
            fill(place.mapped, place.pitch);
            place.buffer->unmap();
        },
        done);
    QueueUploadCopy(texture, mipLevel, arraySlice, place, done);
}

namespace
{
    std::atomic<uint64_t> g_frameTimes[uint32_t(gpu::FrameTime::Count)];
}

void gpu::AddFrameTime(FrameTime what, uint64_t ns)
{
    g_frameTimes[uint32_t(what)].fetch_add(ns, std::memory_order_relaxed);
}

namespace
{
    // gpu::ServicePresents: the frame the game thread hands to the UI thread to present.
    std::mutex g_handoffMutex;
    std::condition_variable g_handoffChanged;
    std::atomic<bool> g_hasPresentThread;
    std::thread::id g_presentThread;
    struct
    {
        bool pending = false;
        bool done = false;
        bool result = false;
        uint32_t index = 0;
        RenderCommandSemaphore* wait = nullptr;
    } g_handoff;

    // Presents swap chain image `index` once `rendered` is signalled, on the present thread when there is one.
    bool PresentSwapChain(uint32_t index, RenderCommandSemaphore* rendered)
    {
        Context& c = *g_context;
        if (!g_hasPresentThread || std::this_thread::get_id() == g_presentThread)
            return c.swapChain->present(index, &rendered, 1);
        std::unique_lock lock(g_handoffMutex);
        g_handoff.pending = true;
        g_handoff.done = false;
        g_handoff.index = index;
        g_handoff.wait = rendered;
        g_handoffChanged.notify_all();
        if (g_handoffChanged.wait_for(lock, std::chrono::milliseconds(200), [] { return g_handoff.done; }))
            return g_handoff.result;
        // The UI thread did not come (busy, or closing): present here. On the console such a Present is not shown.
        g_handoff.pending = false;
        lock.unlock();
        static uint32_t timeouts = 0;
        if (++timeouts <= 5 || timeouts % 100 == 0)
            fprintf(stderr,
                "[gpu] warning: the UI thread did not take the Present within 200 ms (%u times): presenting from this "
                "thread\n",
                timeouts);
        return c.swapChain->present(index, &rendered, 1);
    }

    // Every 10 s for the whole session (SKY_PERF_LOG=0 turns it off): frame rate, frame time median and p95,
    // and where a frame's time goes on average, on the GPU thread (worker.cpp) when there is one: "rest" is then its idle
    // time and its other commands; without it, the guest's CPU work, the renderer's state translation and the guest's
    // waits. A second line gives the GPU thread's busy share and how long the game's render thread waited for it.
    std::atomic<uint64_t> g_framesPresented;
    std::vector<uint64_t> g_perfFrameNs;

    void LogPerf()
    {
        g_framesPresented++;
        Context& c = *g_context;
        static const bool enabled = skyenv::Get("SKY_PERF_LOG") == nullptr || atoi(skyenv::Get("SKY_PERF_LOG")) != 0;
        static uint32_t slowWindows = 0; // consecutive windows under the frame rate with the GPU as the limit
        static uint64_t windowStart = SDL_GetTicksNS(), lastFrame = windowStart;
        auto& frameNs = g_perfFrameNs;
        static Context::Perf total;
        static uint64_t windowTimes[uint32_t(gpu::FrameTime::Count)];
        uint64_t now = SDL_GetTicksNS();
        frameNs.push_back(now - lastFrame);
        if (gpu::framestats::Enabled())
            gpu::framestats::Presented(
                ConsumerFrame(), now, now - lastFrame, { c.perf.presentNs, DrawTimeBuilds(), c.targetSwitches });
        lastFrame = now;
        total.drawNs += c.perf.drawNs;
        total.draws += c.perf.draws;
        total.gpuWaitNs += c.perf.gpuWaitNs;
        total.presentNs += c.perf.presentNs;
        total.flushes += c.perf.flushes;
        c.perf = {};
        if (now - windowStart < 10'000'000'000ull)
            return;
        WorkerTimes workers;
        TakeWorkerTimes(workers);
        if (enabled)
        {
            size_t frames = frameNs.size();
            double seconds = double(now - windowStart) / 1e9;
            uint64_t sum = 0;
            for (uint64_t ns : frameNs)
                sum += ns;
            std::sort(frameNs.begin(), frameNs.end());
            auto ms = [frames](uint64_t ns) { return double(ns) / 1e6 / double(frames); };
            double meanMs = double(sum) / 1e6 / double(frames);
            double accounted = ms(total.drawNs) + ms(total.gpuWaitNs) + ms(total.presentNs);
            fprintf(stderr,
                "[perf] %.1f fps; frame median %.1f ms, p95 %.1f ms; per frame: %u draws taking %.1f ms, GPU wait %.1f "
                "ms, "
                "present %.1f ms, %.1f command lists, rest (guest CPU, renderer, waits) %.1f ms\n",
                double(frames) / seconds, double(frameNs[frames / 2]) / 1e6,
                double(frameNs[std::min(frames - 1, frames * 95 / 100)]) / 1e6, uint32_t(total.draws / frames),
                ms(total.drawNs), ms(total.gpuWaitNs), ms(total.presentNs), double(total.flushes) / double(frames),
                std::max(meanMs - accounted, 0.0));
            // GPU-bound: the frame rate stays under the limit while the GPU thread (or the caller, without it) spends
            // much of each frame waiting for the GPU or the present. A larger scale is then what costs: say so once,
            // after 30 s. The scale is not changed in play (render targets and resolved textures have it).
            double fps = double(frames) / seconds, gpuShare = (ms(total.gpuWaitNs) + ms(total.presentNs)) / meanMs;
            slowWindows = fps < 0.9 * double(gpu::g_settings.fpsLimit) && gpuShare >= 0.4 ? slowWindows + 1 : 0;
            if (slowWindows == 3 && gpu::g_settings.resolutionScale > 1)
            {
                fprintf(stderr,
                    "[perf] hint: the GPU limits the frame rate (%.1f fps, %.0f%% of each frame waiting for it) at "
                    "resolution scale %u; resolution_scale=1 renders %u times fewer pixels\n",
                    fps, 100.0 * gpuShare, gpu::g_settings.resolutionScale,
                    gpu::g_settings.resolutionScale * gpu::g_settings.resolutionScale);
                // Auto steps down at the next start (the scale cannot change in play); a chosen scale stays.
                std::filesystem::path file = g_scaleAuto ? PlayerDataFile(AutoScaleFile) : std::filesystem::path();
                if (!file.empty() && (std::ofstream(file) << gpu::autoscale::Format(
                                          gpu::g_settings.resolutionScale - 1, c.device->getDescription().name)))
                    fprintf(stderr,
                        "[perf] resolution scale auto: the GPU limits the frame rate at scale %u; the next start "
                        "renders at scale %u (%s)\n",
                        gpu::g_settings.resolutionScale, gpu::g_settings.resolutionScale - 1, AutoScaleFile);
            }
            // "rest" split: the renderer's own work on the game's render thread, the vblank wait, and what remains: the
            // guest's own code (game logic, its D3D calls) and the render thread's waits for the GPU thread or the guest.
            // The totals keep counting (frame_stats.h reads the renderer's per frame): the window is their difference.
            uint64_t times[uint32_t(gpu::FrameTime::Count)];
            for (uint32_t i = 0; i < uint32_t(gpu::FrameTime::Count); i++)
            {
                uint64_t counted = g_frameTimes[i].load(std::memory_order_relaxed);
                times[i] = counted - windowTimes[i];
                windowTimes[i] = counted;
            }
            double renderer = ms(times[uint32_t(gpu::FrameTime::Renderer)]);
            double vblank = ms(times[uint32_t(gpu::FrameTime::VblankWait)]);
            double presentPath = WorkerRunning() ? double(workers.producerWaitNs) / 1e6 / double(frames)
                                                 : ms(total.gpuWaitNs) + ms(total.presentNs);
            fprintf(stderr,
                "[perf] render thread per frame: renderer %.1f ms (ring %.1f, textures %.1f, gpu::Draw %.1f), vblank "
                "wait %.1f ms, "
                "%s %.1f ms, guest code and other %.1f ms\n",
                renderer, ms(times[uint32_t(gpu::FrameTime::Ring)]), ms(times[uint32_t(gpu::FrameTime::Textures)]),
                ms(total.drawNs), vblank, WorkerRunning() ? "waiting for the GPU thread" : "GPU wait and present",
                presentPath, std::max(meanMs - renderer - vblank - presentPath, 0.0));
            if (WorkerRunning())
                fprintf(stderr,
                    "[perf] GPU thread busy %.0f%% (recording, submitting, GPU waits, present); the game's render "
                    "thread "
                    "waited %.1f ms per frame for it; %u texture threads busy %.1f%% (%u decodes)\n",
                    100.0 * double(workers.busyNs) / double(now - windowStart),
                    double(workers.producerWaitNs) / 1e6 / double(frames), workers.textureThreads,
                    100.0 * double(workers.textureBusyNs) / double(now - windowStart) /
                        std::max(workers.textureThreads, 1u),
                    workers.textureJobs);
        }
        else
        {
            for (uint32_t i = 0; i < uint32_t(gpu::FrameTime::Count); i++)
                windowTimes[i] = g_frameTimes[i].load(std::memory_order_relaxed);
        }
        frameNs.clear();
        total = {};
        windowStart = now;
    }

    // For the first minute: how many presents succeeded in each 5 s, and from which thread (are frames still coming?).
    void LogPresents(bool presented)
    {
        static uint64_t start = SDL_GetTicksNS() / 1000000, windowStart = start;
        static uint32_t count = 0;
        count += presented;
        uint64_t now = SDL_GetTicksNS() / 1000000;
        if (now - windowStart < 5000 || windowStart - start >= 60000)
            return;
        fprintf(stderr, "[gpu] presents: %u in the last 5 s (last from thread %llu)\n", count,
            static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF));
        count = 0;
        windowStart = now;
    }
}

void gpu::Present(const Texture* image)
{
    ProducerLock producer;
    if (g_context == nullptr)
        return;
    if (framestats::Enabled())
    {
        // The game's render thread's CPU time (from any thread: a movie presents from the main thread).
        uint64_t gameCpu = UINT64_MAX;
        uint64_t clock = framestats::ThreadClock(framestats::Thread::Game);
        if (clock == 0 || !PlatformThreadCpuNs(clock, gameCpu))
            gameCpu = UINT64_MAX;
        framestats::ProducerFrame(ProducerFrame(),
            { g_frameTimes[uint32_t(FrameTime::Renderer)].load(std::memory_order_relaxed), g_uploads, g_uploadBytes,
                writewatch::FaultCount(), g_frameTimes[uint32_t(FrameTime::VblankWait)].load(std::memory_order_relaxed),
                gameCpu });
    }
    BufferCacheFrameDone(CompletedFrame());
    EndProducerFrame([image] { PresentNow(image); });
}

namespace
{
    // Frame generation: the generated frame before the real one, through the same present pass with its own sets and
    // acquire semaphore. False when there is nothing to present to (the real frame then goes as usual).
    bool PresentGenerated(const gpu::Texture& generated)
    {
        Context& c = *g_context;
        const uint32_t slot = uint32_t(c.frames & 1);
        Flush(); // the optical flow and the interpolation go first, without waiting for a swap chain image
        uint32_t imageIndex = 0;
        RenderTexture* target;
        const RenderFramebuffer* framebuffer;
        uint32_t width, height;
        if (c.swapChain != nullptr)
        {
            if (c.swapChainNoSurface || c.swapChain->needsResize() || c.swapChain->isEmpty() ||
                c.swapChainFramebuffers.empty())
                return false;
            if (int32_t result = 0; !AcquireImage(c.generatedAcquire[slot].get(), &imageIndex, result))
            {
                SwapChainFailed("acquire", result);
                return false;
            }
            target = c.swapChain->getTexture(imageIndex);
            framebuffer = c.swapChainFramebuffers[imageIndex].get();
            width = c.swapChain->getWidth();
            height = c.swapChain->getHeight();
        }
        else
        {
            target = c.offscreen.texture.get();
            framebuffer = c.offscreenFramebuffer.get();
            width = c.offscreen.desc.width;
            height = c.offscreen.desc.height;
        }
        RenderCommandList& list = Record();
        Transition(list, generated, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
        if (c.swapChain != nullptr)
            list.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
        else
            Transition(list, c.offscreen, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
        SetFramebuffer(list, framebuffer);
        c.bound = {};
        list.setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
        list.setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
        list.clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        c.generatedSets[slot]->setTexture(0, generated.texture.get(), RenderTextureLayout::SHADER_READ);
        list.setViewports(Letterbox(generated.desc, width, height));
        list.setGraphicsPipelineLayout(c.presentLayout.get());
        list.setPipeline(c.presentPipeline.get());
        list.setGraphicsDescriptorSet(c.generatedSets[slot].get(), 0);
        list.drawInstanced(3, 1, 0, 0);
        if (c.overlay != nullptr)
        {
            // The F3 menu on both frames: it would flicker otherwise.
            Transition(list, *c.overlay, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
            c.generatedOverlaySets[slot]->setTexture(0, c.overlay->texture.get(), RenderTextureLayout::SHADER_READ);
            list.setViewports(Letterbox(c.overlay->desc, width, height));
            list.setPipeline(c.overlayPipeline.get());
            list.setGraphicsDescriptorSet(c.generatedOverlaySets[slot].get(), 0);
            list.drawInstanced(3, 1, 0, 0);
        }
        bool presented = true;
        if (c.swapChain != nullptr)
        {
            list.barriers(RenderBarrierStage::NONE, RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
            EndTiming(c.submissions[c.submitted], false);
            list.end();
            KeepMetalBuffer(c.submissions[c.submitted]);
            c.recording = false;
            const RenderCommandList* lists = &list;
            RenderCommandSemaphore* acquired = c.generatedAcquire[slot].get();
            RenderCommandSemaphore* rendered = c.releaseSemaphores[imageIndex].get();
            c.queue->executeCommandLists(
                &lists, 1, &acquired, 1, &rendered, 1, c.submissions[c.submitted++].fence.get());
            const uint64_t presentStart = SDL_GetTicksNS();
            presented = PresentSwapChain(imageIndex, rendered);
            c.perf.presentNs += SDL_GetTicksNS() - presentStart;
            if (presented)
                FramePresented();
            else
                SwapChainFailed("present", c.swapChain->getLastResult());
        }
        else
        {
            FlushList(false);
            const uint64_t now = SDL_GetTicksNS();
            if (gpu::g_settings.vsync && c.nextOffscreenFrame > now)
                SDL_DelayPrecise(c.nextOffscreenFrame - now);
            c.nextOffscreenFrame = std::max(c.nextOffscreenFrame, now) + OffscreenFrameNs;
        }
        if (presented)
            g_framesPresented++; // the FPS counter counts the frames shown
        return presented;
    }
}

void gpu::detail::PresentNow(const Texture* image)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
    {
        SDL_Delay(16);
        return;
    }
    if (image != nullptr)
        SettleCopies(image);
    Context& c = *g_context;
    // Frame generation: the frame between the last real one and this one goes first, the real one half a real frame
    // later. The lock stays held: nothing may record into this frame's command lists between the two presents.
    const Texture* generated = image != nullptr && framegen::Enabled() ? framegen::detail::Generate(image) : nullptr;
    if (generated != nullptr && !PresentGenerated(*generated))
        generated = nullptr;
    if (generated != nullptr)
    {
        framegen::detail::Shown();
        SDL_DelayPrecise(framegen::detail::RealFrameDelayNs());
    }
    uint32_t imageIndex = 0;
    RenderTexture* target;
    const RenderFramebuffer* framebuffer;
    uint32_t width, height;
    if (c.swapChain != nullptr)
    {
        int32_t result = 0;
        if (c.swapChainNoSurface)
            SwapChainFailed("making the surface", gpu::swapchain::SurfaceLost);
        else if (c.swapChain->needsResize())
            ResizeSwapChain();
        // Minimised or zero-sized window: nothing to present to.
        if (c.swapChainNoSurface || c.swapChain->isEmpty() || c.swapChainFramebuffers.empty() ||
            !AcquireImage(c.acquireSemaphore.get(), &imageIndex, result))
        {
            SwapChainFailed("acquire", result);
            if (c.recording || c.submitted != 0)
                Submit();
            SDL_Delay(16);
            return;
        }
        target = c.swapChain->getTexture(imageIndex);
        framebuffer = c.swapChainFramebuffers[imageIndex].get();
        width = c.swapChain->getWidth();
        height = c.swapChain->getHeight();
    }
    else
    {
        int windowWidth, windowHeight;
        if (c.window != nullptr && SDL_GetWindowSizeInPixels(c.window, &windowWidth, &windowHeight) &&
            windowWidth > 0 && windowHeight > 0 &&
            (uint32_t(windowWidth) != c.offscreen.desc.width || uint32_t(windowHeight) != c.offscreen.desc.height))
            ResizeOffscreen(uint32_t(windowWidth), uint32_t(windowHeight));
        target = c.offscreen.texture.get();
        framebuffer = c.offscreenFramebuffer.get();
        width = c.offscreen.desc.width;
        height = c.offscreen.desc.height;
    }

    RenderCommandList& list = Record();
    if (image != nullptr)
        Transition(list, *image, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
    if (c.swapChain != nullptr)
        list.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target, RenderTextureLayout::COLOR_WRITE));
    else
        Transition(list, c.offscreen, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);

    SetFramebuffer(list, framebuffer);
    c.bound = {}; // the game draws' state tracking (gpu::DrawNow): this pass binds its own
    list.setViewports(RenderViewport(0.0f, 0.0f, float(width), float(height)));
    list.setScissors(RenderRect(0, 0, int32_t(width), int32_t(height)));
    list.clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    if (image != nullptr)
    {
        c.presentSet->setTexture(0, image->texture.get(), RenderTextureLayout::SHADER_READ);
        list.setViewports(Letterbox(image->desc, width, height));
        c.bound = {};
        list.setGraphicsPipelineLayout(c.presentLayout.get());
        list.setPipeline(c.presentPipeline.get());
        list.setGraphicsDescriptorSet(c.presentSet.get(), 0);
        list.drawInstanced(3, 1, 0, 0);
    }
    if (c.overlay != nullptr)
    {
        Transition(list, *c.overlay, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
        c.overlaySet->setTexture(0, c.overlay->texture.get(), RenderTextureLayout::SHADER_READ);
        list.setViewports(Letterbox(c.overlay->desc, width, height));
        c.bound = {};
        list.setGraphicsPipelineLayout(c.presentLayout.get());
        list.setPipeline(c.overlayPipeline.get());
        list.setGraphicsDescriptorSet(c.overlaySet.get(), 0);
        list.drawInstanced(3, 1, 0, 0);
    }

    if (c.swapChain != nullptr)
    {
        list.barriers(RenderBarrierStage::NONE, RenderTextureBarrier(target, RenderTextureLayout::PRESENT));
        EndTiming(c.submissions[c.submitted], true);
        list.end();
        KeepMetalBuffer(c.submissions[c.submitted]);
        c.recording = false;
        const RenderCommandList* lists = &list;
        RenderCommandSemaphore* acquired = c.acquireSemaphore.get();
        RenderCommandSemaphore* rendered = c.releaseSemaphores[imageIndex].get();
        c.queue->executeCommandLists(&lists, 1, &acquired, 1, &rendered, 1, c.submissions[c.submitted++].fence.get());
        Step("frame submitted, presenting");
        uint64_t presentStart = SDL_GetTicksNS();
        bool presented = PresentSwapChain(imageIndex, rendered);
        Step(presented ? "presented" : "present failed");
        if (presented)
            FramePresented();
        else
            SwapChainFailed("present", c.swapChain->getLastResult());
        c.perf.presentNs += SDL_GetTicksNS() - presentStart;
        EndFrame();
        if (presented && !c.presented)
            fprintf(stderr, "[gpu] first present %ux%u\n", width, height);
        c.presented |= presented;
        LogPresents(presented);
    }
    else
    {
        FlushList(true);
        EndFrame();
        // Stand-in for vsync.
        uint64_t now = SDL_GetTicksNS();
        if (gpu::g_settings.vsync && c.nextOffscreenFrame > now)
            SDL_DelayPrecise(c.nextOffscreenFrame - now);
        c.nextOffscreenFrame = std::max(c.nextOffscreenFrame, now) + OffscreenFrameNs;
    }
    framegen::detail::Shown();
    if (generated != nullptr)
        framegen::detail::RealPresented(generated, image);
    LogPerf();
}

uint64_t gpu::FramesPresented() { return g_framesPresented; }

void gpu::SetOverlay(const Texture* overlay)
{
    ProducerLock producer;
    if (g_context != nullptr)
        Enqueue([overlay] {
            std::lock_guard lock(g_mutex);
            g_context->overlay = overlay;
        });
}

bool gpu::ReadbackOutput(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height)
{
    ProducerLock producer;
    bool result = false;
    RunAndWait([&] {
        std::lock_guard lock(g_mutex);
        Context& c = *g_context;
        if (c.swapChain == nullptr && c.offscreen.texture != nullptr)
            result = ReadbackNow(&c.offscreen, rgba, width, height);
    });
    return result;
}

bool gpu::ReadbackTexture(
    const Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, bool raw, uint32_t channel)
{
    ProducerLock producer;
    bool result = false;
    RunAndWait([&] { result = ReadbackNow(texture, rgba, width, height, raw, channel); });
    return result;
}

bool gpu::detail::ReadbackNow(
    const Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, bool raw, uint32_t channel)
{
    std::lock_guard lock(g_mutex);
    Context& c = *g_context;
    if (texture == nullptr || DeviceLost())
        return false;
    SettleCopies(texture);

    width = texture->desc.width;
    height = texture->desc.height;
    uint64_t size = uint64_t(width) * height * 4;
    if (c.readbackCapacity < size)
    {
        c.readbackGpu = c.device->createBuffer(
            RenderBufferDesc::DefaultBuffer(size, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
        c.readbackCpu = c.device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
        c.readbackCapacity = size;
        c.readbackSet->setBuffer(1, c.readbackGpu.get(), size);
    }

    // plume copies textures only into textures on Metal and Vulkan, so a compute pass packs the pixels into a buffer.
    RenderCommandList& list = Record();
    Transition(list, *texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
    list.barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(c.readbackGpu.get(), RenderBufferAccess::WRITE));
    // A cube or array texture is read through a 2D view of its first slice: bound whole to the shader's 2D texture, Metal
    // returned garbage (Trap Team's cube map read as one flat grey) and D3D12 face 0.
    std::unique_ptr<RenderTextureView> slice;
    if (texture->desc.cube || texture->desc.arraySize > 1)
    {
        RenderTextureViewDesc view = RenderTextureViewDesc::Texture2D(texture->desc.format);
        view.mipLevels = 1;
        view.arraySize = 1;
        slice = texture->texture->createTextureView(view);
    }
    c.readbackSet->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ, slice.get());
    c.bound = {}; // a compute pipeline replaces the draws' pipeline state on D3D12
    // `raw`: ReadbackRawCS at step 1 (its layout has the same set, plus the step and the channel as push constants).
    list.setComputePipelineLayout(raw ? c.rawReadbackLayout.get() : c.readbackLayout.get());
    list.setPipeline(raw ? c.readbackRawPipeline.get() : c.readbackPipeline.get());
    list.setComputeDescriptorSet(c.readbackSet.get(), 0);
    if (raw)
    {
        int32_t constants[2] = { 1, int32_t(channel) };
        list.setComputePushConstants(0, constants);
    }
    list.dispatch((width + 7) / 8, (height + 7) / 8, 1);
    list.barriers(RenderBarrierStage::COPY, RenderBufferBarrier(c.readbackGpu.get(), RenderBufferAccess::READ));
    list.copyBufferRegion(c.readbackCpu->at(0), c.readbackGpu->at(0), size);
    Submit();

    auto pixels = static_cast<const uint32_t*>(c.readbackCpu->map());
    if (pixels == nullptr)
    {
        CheckDeviceLost();
        return false;
    }
    rgba.assign(pixels, pixels + uint64_t(width) * height);
    c.readbackCpu->unmap();
    return true;
}

void gpu::RequestReadback(const Texture* texture, uint32_t step, ReadbackConvert convert)
{
    ProducerLock producer;
    Enqueue([texture, step, convert = std::move(convert)] { // after the producer's earlier commands (the resolve)
        std::lock_guard lock(g_mutex);
        Context& c = *g_context;
        Context::RawReadback& slot = c.rawReadbacks[c.frames & 1];
        if (texture == nullptr || slot.pending || DeviceLost())
            return;
        SettleCopies(texture);

        slot.width = texture->desc.width / step;
        slot.height = texture->desc.height / step;
        uint64_t size = std::max(uint64_t(slot.width) * slot.height * 4, uint64_t(4));
        if (slot.capacity < size)
        {
            slot.gpu = c.device->createBuffer(
                RenderBufferDesc::DefaultBuffer(size, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
            slot.cpu = c.device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
            slot.capacity = size;
            slot.set->setBuffer(1, slot.gpu.get(), size);
        }

        RenderCommandList& list = Record();
        Transition(list, *texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        list.barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(slot.gpu.get(), RenderBufferAccess::WRITE));
        slot.set->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ);
        c.bound = {}; // a compute pipeline replaces the draws' pipeline state on D3D12
        list.setComputePipelineLayout(c.rawReadbackLayout.get());
        list.setPipeline(c.readbackRawPipeline.get());
        list.setComputeDescriptorSet(slot.set.get(), 0);
        int32_t constants[2] = { int32_t(step), 0 };
        list.setComputePushConstants(0, constants);
        list.dispatch((slot.width + 7) / 8, (slot.height + 7) / 8, 1);
        list.barriers(RenderBarrierStage::COPY, RenderBufferBarrier(slot.gpu.get(), RenderBufferAccess::READ));
        list.copyBufferRegion(slot.cpu->at(0), slot.gpu->at(0), size);
        slot.pending = true;
        slot.frame = c.frames;
        slot.convert = convert;
    });
}

bool gpu::CollectReadback(std::vector<uint32_t>& data)
{
    Context& c = *g_context;
    std::lock_guard ready(c.rawReadyMutex);
    if (!c.rawReadyValid)
        return false;
    c.rawReadyValid = false;
    data.swap(c.rawReady);
    return true;
}

bool gpu::detail::IsPresentThread() { return g_hasPresentThread && std::this_thread::get_id() == g_presentThread; }

void gpu::detail::ServicePendingPresent()
{
    if (!IsPresentThread())
        return;
    std::unique_lock lock(g_handoffMutex);
    if (!g_handoff.pending)
        return;
    g_handoff.result = g_context->swapChain->present(g_handoff.index, &g_handoff.wait, 1);
    g_handoff.pending = false;
    g_handoff.done = true;
    g_handoffChanged.notify_all();
}

void gpu::ServicePresents(uint32_t timeoutMs)
{
    if (!g_hasPresentThread)
    {
        g_presentThread = std::this_thread::get_id();
        g_hasPresentThread = true;
    }
    std::unique_lock lock(g_handoffMutex);
    if (!g_handoffChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return g_handoff.pending; }))
        return;
    // The game thread holds g_mutex and waits for this: the swap chain cannot change meanwhile.
    g_handoff.result = g_context->swapChain->present(g_handoff.index, &g_handoff.wait, 1);
    g_handoff.pending = false;
    g_handoff.done = true;
    g_handoffChanged.notify_all();
}

// Read only: sampling never resets the perf log or creates renderer resources.
bool gpu::ReadDebugSnapshot(DebugSnapshot& result, bool wait)
{
    std::unique_lock lock(g_mutex, std::defer_lock);
    if (wait)
        lock.lock();
    else if (!lock.try_lock())
        return false;
    result = {};
    if (g_context == nullptr)
        return true;
    const auto& c = *g_context;
    const auto& description = c.device->getDescription();
    auto format = c.renderInterface->getCapabilities().shaderFormat;
    snprintf(result.adapter, sizeof(result.adapter), "%s", description.name.c_str());
    snprintf(result.backend, sizeof(result.backend), "%s",
        format == RenderShaderFormat::METAL   ? "Metal"
        : format == RenderShaderFormat::DXIL  ? "D3D12"
        : format == RenderShaderFormat::SPIRV ? "Vulkan"
                                              : "n/a");
    WriteDriverVersion(result.driver, sizeof(result.driver), description, format);
    result.width = c.swapChain ? c.swapChain->getWidth() : c.offscreen.desc.width;
    result.height = c.swapChain ? c.swapChain->getHeight() : c.offscreen.desc.height;
#if defined(ARKEYAN_METAL)
    if (format == RenderShaderFormat::METAL)
    {
        auto* device = static_cast<plume::MetalDevice&>(*c.device).mtl;
        result.videoUsed = device->currentAllocatedSize();
        result.videoBudget = device->recommendedMaxWorkingSetSize();
        result.videoUsageKnown = true;
        result.videoBudgetKnown = result.videoBudget != 0;
    }
#else
#if defined(_WIN32)
    // Windows can choose Vulkan or D3D12. Dispatch by the live interface, never by the host OS alone.
    if (format == RenderShaderFormat::DXIL)
    {
        auto* adapter = static_cast<plume::D3D12Device&>(*c.device).adapter;
        IDXGIAdapter3* query = nullptr;
        if (adapter && SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&query))))
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            if (SUCCEEDED(query->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)))
            {
                result.videoUsed = memory.CurrentUsage;
                result.videoBudget = memory.Budget;
                result.videoUsageKnown = result.videoBudgetKnown = true;
            }
            query->Release();
        }
    }
#endif
#if !defined(PLUME_D3D12_COREWINDOW)
    if (format == RenderShaderFormat::SPIRV)
    {
        const auto& device = static_cast<plume::VulkanDevice&>(*c.device);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(device.physicalDevice, &properties);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(device.allocator, budgets);
        for (uint32_t heap = 0; heap < properties.memoryHeapCount; heap++)
            if (properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                result.videoUsed += budgets[heap].usage;
                result.videoBudget += budgets[heap].budget;
            }
        result.videoUsageKnown = result.videoBudgetKnown = result.videoBudget != 0;
    }
#endif
#endif
    result.scale = g_settings.resolutionScale;
    result.frameCount = std::min(result.frameNs.size(), g_perfFrameNs.size());
    std::copy(g_perfFrameNs.end() - result.frameCount, g_perfFrameNs.end(), result.frameNs.begin());
    result.available = true;
    ReadDebugDrawSnapshot(result);
    return true;
}
