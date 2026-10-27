#include "gpu/gpu.h"
#include "gpu/buffer_cache.h"
#include "gpu/context.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <plume_render_interface.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include "../host/env.h"

#if defined(__APPLE__)
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
#if !defined(__APPLE__) && !defined(PLUME_D3D12_COREWINDOW)
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
#if defined(__APPLE__)
    std::unique_ptr<RenderInterface> CreateMetalInterface();
#endif
#if defined(_WIN32)
    std::unique_ptr<RenderInterface> CreateD3D12Interface();
#endif
#if !defined(__APPLE__) && !defined(PLUME_D3D12_COREWINDOW)
    std::unique_ptr<RenderInterface> CreateVulkanInterface();
#endif
}


namespace
{
#if defined(__APPLE__)
    const char* BackendName = "Metal";
#elif defined(PLUME_D3D12_COREWINDOW)
    const char* BackendName = "D3D12";
#else
    const char* BackendName = "Vulkan"; // desktop Windows: or D3D12 (CreateDevice)
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

    uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

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

    bool DeviceLost()
    {
        return g_context != nullptr && g_context->lost;
    }

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

    RenderCommandList& Record()
    {
        Context& c = *g_context;
        if (!c.recording)
        {
            if (c.submitted == c.submissions.size())
                c.submissions.push_back({ c.queue->createCommandList(), c.device->createCommandFence() });
            c.submissions[c.submitted].commandList->begin();
            c.recording = true;
            c.boundFramebuffer = nullptr;
            c.bound = {};
            c.drawsSinceFlush = 0;
        }
        return *c.submissions[c.submitted].commandList;
    }

    void Flush()
    {
        if (g_context->recording)
            g_context->perf.flushes++;
        Context& c = *g_context;
        if (!c.recording)
            return;
        Context::Submission& submission = c.submissions[c.submitted++];
        submission.commandList->end();
        c.recording = false;
        c.queue->executeCommandLists(submission.commandList.get(), submission.fence.get());
        Step("command list executed");
    }

    // Waits for the frame before (Context::previous); its upload memory becomes free.
    static void WaitForPrevious()
    {
        Context::Frame& previous = g_context->previous;
        uint64_t start = SDL_GetTicksNS();
        for (uint32_t i = 0; i < previous.submitted; i++)
            g_context->queue->waitForCommandFence(previous.submissions[i].fence.get());
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
            c.queue->waitForCommandFence(c.submissions[i].fence.get());
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
        std::swap(c.stagingCapacity, previous.stagingCapacity);
        std::swap(c.retiredStaging, previous.retiredStaging);
        std::swap(c.presentSet, previous.presentSet);
        std::swap(c.overlaySet, previous.overlaySet);
        std::swap(c.acquireSemaphore, previous.acquireSemaphore);
        c.stagingUsed = 0;
        DrawFrameDone();
    }

    void Transition(RenderCommandList& list, const gpu::Texture& texture, RenderTextureLayout layout, RenderBarrierStages stages)
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
                c.retiredStaging.push_back(std::move(c.staging));
            c.stagingCapacity = std::max({ size, c.stagingCapacity * 2, MinStagingSize });
            c.staging = c.device->createBuffer(RenderBufferDesc::UploadBuffer(c.stagingCapacity,
                RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT | RenderBufferFlag::DEVICE_ADDRESSABLE));
            offset = 0;
        }
        c.stagingUsed = offset + size;
        buffer = c.staging.get();
        return offset;
    }
}

using namespace gpu::detail;

namespace
{
    // The present pass's shaders for the running backend. One metallib holds every entry point; DXIL (D3D12) and
    // SPIR-V (Vulkan) have one blob per entry point.
    std::unique_ptr<RenderShader> CreatePresentShader(const char* entryPoint)
    {
        RenderShaderFormat format = g_context->renderInterface->getCapabilities().shaderFormat;
#if defined(__APPLE__)
        return g_context->device->createShader(g_presentMetallib, sizeof(g_presentMetallib), entryPoint, format);
#else
        std::span<const std::pair<const char*, std::span<const uint8_t>>> blobs;
#if defined(_WIN32)
        static const std::pair<const char*, std::span<const uint8_t>> dxil[] = {
            { "PresentVS", g_presentPresentVSDxil }, { "PresentPS", g_presentPresentPSDxil }, { "ReadbackCS", g_presentReadbackCSDxil },
            { "ReadbackRawCS", g_presentReadbackRawCSDxil }, { "ResolveDepthPS", g_presentResolveDepthPSDxil }, { "OverlayPS", g_presentOverlayPSDxil } };
        if (format == RenderShaderFormat::DXIL)
            blobs = dxil;
#endif
#if !defined(PLUME_D3D12_COREWINDOW)
        static const std::pair<const char*, std::span<const uint8_t>> spirv[] = {
            { "PresentVS", g_presentPresentVSSpirv }, { "PresentPS", g_presentPresentPSSpirv }, { "ReadbackCS", g_presentReadbackCSSpirv },
            { "ReadbackRawCS", g_presentReadbackRawCSSpirv }, { "ResolveDepthPS", g_presentResolveDepthPSSpirv }, { "OverlayPS", g_presentOverlayPSSpirv } };
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
        if (c.presentVS == nullptr || c.presentPS == nullptr || c.readbackCS == nullptr || c.readbackRawCS == nullptr || c.resolveDepthPS == nullptr || c.overlayPS == nullptr)
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
        c.readbackPipeline = c.device->createComputePipeline(RenderComputePipelineDesc(c.readbackLayout.get(), c.readbackCS.get(), 8, 8, 1));
        // Raw read-back: the same set, push constants (binding 0, space 1) = the step.
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(setBuilder);
        layoutBuilder.addPushConstant(0, 1, 8, RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        c.rawReadbackLayout = layoutBuilder.create(c.device.get());
        c.readbackRawPipeline = c.device->createComputePipeline(RenderComputePipelineDesc(c.rawReadbackLayout.get(), c.readbackRawCS.get(), 8, 8, 1));

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
        return c.presentPipeline != nullptr && c.overlayPipeline != nullptr && c.readbackPipeline != nullptr && c.readbackRawPipeline != nullptr && c.resolveDepthPipeline != nullptr;
    }

    // The native surface the backend presents to, when the window has one (not with SDL's offscreen driver).
    bool GetRenderWindow(SDL_Window* window, RenderWindow& renderWindow)
    {
        SDL_PropertiesID properties = SDL_GetWindowProperties(window);
#if defined(__APPLE__)
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
        renderWindow = static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        return renderWindow != nullptr;
#else
        // PLUME_SDL3_VULKAN_ENABLED: plume creates the surface through SDL.
        (void)properties;
        renderWindow = window;
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

    void ResizeOffscreen(uint32_t width, uint32_t height)
    {
        Context& c = *g_context;
        WaitForSubmissions();
        c.offscreenFramebuffer.reset();
        c.offscreen.desc = { .width = width, .height = height, .format = OutputFormat, .usage = gpu::TextureUsage::RenderTarget };
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
        return RenderViewport(float((outputWidth - width) / 2), float((outputHeight - height) / 2), float(width), float(height));
    }
}

gpu::Settings gpu::g_settings;

namespace
{
    // The adapter's driver version as its vendor writes it: D3D12 gives the user-mode driver's four 16-bit parts,
    // Vulkan a packed number (NVIDIA's own packing, everyone else's VK_MAKE_VERSION); Metal none.
    std::string DriverVersion(const RenderDeviceDescription& description, RenderShaderFormat format)
    {
        unsigned long long v = description.driverVersion;
        char text[64] = "n/a";
        if (format == RenderShaderFormat::DXIL)
            snprintf(text, sizeof(text), "%llu.%llu.%llu.%llu", v >> 48, v >> 32 & 0xFFFF, v >> 16 & 0xFFFF, v & 0xFFFF);
        else if (format == RenderShaderFormat::SPIRV && description.vendor == RenderDeviceVendor::NVIDIA)
            snprintf(text, sizeof(text), "%llu.%llu.%llu.%llu", v >> 22 & 0x3FF, v >> 14 & 0xFF, v >> 6 & 0xFF, v & 0x3F);
        else if (format == RenderShaderFormat::SPIRV)
            snprintf(text, sizeof(text), "%llu.%llu.%llu", v >> 22 & 0x3FF, v >> 12 & 0x3FF, v & 0xFFF);
        return text;
    }

    // resolution_scale=auto: 2 only where the GPU is known to keep up at 2240x1408 (Apple silicon, discrete GPUs with
    // 2 GB or more), else 1. Integrated GPUs are fill-bound with the pixel count: a Radeon 840M (shared memory) lags at
    // x3, a little at x2, and is fine at x1.
    uint32_t AutoScale(const RenderDeviceDescription& description, const RenderDeviceCapabilities& capabilities, const char*& why)
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

    // The adapter, the window and the resolution scale (chosen here when it is auto), for the log.
    void LogAdapter(SDL_Window* window, uint32_t requestedScale)
    {
        Context& c = *g_context;
        const RenderDeviceDescription& description = c.device->getDescription();
        const RenderDeviceCapabilities& capabilities = c.device->getCapabilities();
        static const char* const types[] = { "unknown type", "integrated", "discrete", "virtual", "CPU" };
        fprintf(stderr, "[gpu] adapter %s: vendor %04X, %s%s, %llu MB of video memory, driver %s\n", description.name.c_str(),
            unsigned(description.vendor), types[std::min<size_t>(size_t(description.type), std::size(types) - 1)],
            capabilities.uma ? ", unified memory" : "", static_cast<unsigned long long>(description.dedicatedVideoMemory >> 20),
            DriverVersion(description, c.renderInterface->getCapabilities().shaderFormat).c_str());

        if (window != nullptr)
        {
            int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
            SDL_GetWindowSize(window, &width, &height);
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
            fprintf(stderr, "[gpu] window %dx%d (%dx%d pixels), display %dx%d at %.0f%%\n", width, height, pixelWidth,
                pixelHeight, mode != nullptr ? mode->w : 0, mode != nullptr ? mode->h : 0, 100.0 * SDL_GetWindowDisplayScale(window));
        }

        if (requestedScale == 0)
        {
            const char* why = "";
            gpu::g_settings.resolutionScale = AutoScale(description, capabilities, why);
            fprintf(stderr, "[gpu] resolution scale auto: %u (%s; resolution_scale=1 to 10 sets it)\n",
                gpu::g_settings.resolutionScale, why);
        }
        uint32_t scale = gpu::g_settings.resolutionScale;
        fprintf(stderr, "[gpu] resolution scale %u: the 3D renders at %ux%u\n", scale, 1120 * scale, 704 * scale);
    }
}

SDL_WindowFlags gpu::WindowFlags()
{
#if defined(__APPLE__)
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
#if defined(__APPLE__)
    c.renderInterface = CreateMetalInterface();
#elif defined(PLUME_D3D12_COREWINDOW)
    D3D12EnableDred();
    c.renderInterface = CreateD3D12Interface();
#else
    // Desktop Windows: Vulkan, then D3D12 when there is no Vulkan device. SKY_GPU_BACKEND=vulkan or d3d12 picks one.
    const char* requested = skyenv::Get("SKY_GPU_BACKEND");
#if defined(_WIN32)
    bool tryVulkan = requested == nullptr || SDL_strcasecmp(requested, "d3d12") != 0;
#else
    bool tryVulkan = true;
#endif
    if (tryVulkan)
        c.renderInterface = CreateVulkanInterface();
    if (c.renderInterface != nullptr)
        c.device = c.renderInterface->createDevice();
#if defined(_WIN32)
    if (c.device == nullptr && (requested == nullptr || SDL_strcasecmp(requested, "vulkan") != 0))
    {
        if (tryVulkan)
            fprintf(stderr, "[gpu] no Vulkan device, trying D3D12\n");
        BackendName = "D3D12";
        D3D12EnableDred();
        c.renderInterface = CreateD3D12Interface();
        if (c.renderInterface != nullptr)
            c.device = c.renderInterface->createDevice();
    }
#endif
#endif
#if defined(__APPLE__) || defined(PLUME_D3D12_COREWINDOW)
    if (c.renderInterface != nullptr)
        c.device = c.renderInterface->createDevice();
#endif
    if (c.device == nullptr)
    {
        fprintf(stderr, "[gpu] no %s device\n", BackendName);
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
        c.swapChain = c.queue->createSwapChain(RenderSwapChainDesc(renderWindow, OutputFormat, SwapChainBufferCount));
        Step("swap chain created");
        c.swapChain->setVsyncEnabled(gpu::g_settings.vsync);
        c.acquireSemaphore = c.device->createCommandSemaphore();
        c.previous.acquireSemaphore = c.device->createCommandSemaphore();
        ResizeSwapChain();
    }
    else
    {
        int pixelWidth = int(width), pixelHeight = int(height);
        if (window != nullptr)
            SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        ResizeOffscreen(uint32_t(std::max(pixelWidth, 1)), uint32_t(std::max(pixelHeight, 1)));
    }

    fprintf(stderr, "[gpu] %s on %s, %s output\n", BackendName, c.device->getDescription().name.c_str(),
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

void gpu::Shutdown()
{
    ProducerLock producer;
    StopWorker();
    std::lock_guard lock(g_mutex);
    if (g_context == nullptr)
        return;
    if (g_context->recording || g_context->submitted != 0 || g_context->previous.submitted != 0)
        Submit();
    DrawShutdown();
    BufferCacheShutdown();
    ShutdownUploads();
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
    if (desc.cube)
        flags |= RenderTextureFlag::CUBE;

    RenderTextureDesc textureDesc = desc.depth > 1
        ? RenderTextureDesc::Texture3D(desc.width, desc.height, desc.depth, desc.mipLevels, desc.format, flags)
        : RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D, desc.width, desc.height, 1, desc.mipLevels, desc.arraySize, desc.format, flags);

    // Render and depth targets are committed resources: D3D12 then initializes their compression metadata. A placed one
    // must be cleared, discarded or copied to before any other use, or the GPU may read garbage metadata and fault (AMD).
    textureDesc.committed = (desc.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)) != 0;

    auto texture = new Texture{ desc };
#if defined(_WIN32)
    // D3D12: the big allocations, for the memory budget (the console renders at scale 2).
    uint64_t bytes = uint64_t(desc.width) * desc.height * desc.depth * desc.arraySize * RenderFormatSize(desc.format) /
        (RenderFormatBlockWidth(desc.format) * RenderFormatBlockWidth(desc.format));
    bool big = bytes >= (16u << 20) && g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::DXIL;
    if (big)
        fprintf(stderr, "[gpu] texture %ux%u x%u, format %d, %u mips: %.0f MB\n", desc.width, desc.height, desc.depth * desc.arraySize,
            int(desc.format), desc.mipLevels, double(bytes) / (1 << 20));
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

const gpu::TextureDesc& gpu::Desc(const Texture* texture)
{
    return texture->desc;
}

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
    RunTextureJob([fill = std::move(fill), place] {
        fill(place.mapped, place.pitch);
        place.buffer->unmap();
    }, done);
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
            fprintf(stderr, "[gpu] warning: the UI thread did not take the Present within 200 ms (%u times): presenting from this thread\n", timeouts);
        return c.swapChain->present(index, &rendered, 1);
    }

    // Every 10 s for the whole session (SKY_PERF_LOG=0 turns it off): frame rate, frame time median and p95,
    // and where a frame's time goes on average, on the GPU thread (worker.cpp) when there is one: "rest" is then its idle
    // time and its other commands; without it, the guest's CPU work, the renderer's state translation and the guest's
    // waits. A second line gives the GPU thread's busy share and how long the game's render thread waited for it.
    void LogPerf()
    {
        Context& c = *g_context;
        static const bool enabled = skyenv::Get("SKY_PERF_LOG") == nullptr || atoi(skyenv::Get("SKY_PERF_LOG")) != 0;
        static uint32_t slowWindows = 0; // consecutive windows under the frame rate with the GPU as the limit
        static uint64_t windowStart = SDL_GetTicksNS(), lastFrame = windowStart;
        static std::vector<uint64_t> frameNs;
        static Context::Perf total;
        uint64_t now = SDL_GetTicksNS();
        frameNs.push_back(now - lastFrame);
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
            fprintf(stderr, "[perf] %.1f fps; frame median %.1f ms, p95 %.1f ms; per frame: %u draws taking %.1f ms, GPU wait %.1f ms, "
                "present %.1f ms, %.1f command lists, rest (guest CPU, renderer, waits) %.1f ms\n",
                double(frames) / seconds, double(frameNs[frames / 2]) / 1e6, double(frameNs[std::min(frames - 1, frames * 95 / 100)]) / 1e6,
                uint32_t(total.draws / frames), ms(total.drawNs), ms(total.gpuWaitNs), ms(total.presentNs), double(total.flushes) / double(frames),
                std::max(meanMs - accounted, 0.0));
            // GPU-bound: the frame rate stays under the limit while the GPU thread (or the caller, without it) spends
            // much of each frame waiting for the GPU or the present. A larger scale is then what costs: say so once,
            // after 30 s. The scale is not changed in play (render targets and resolved textures have it).
            double fps = double(frames) / seconds, gpuShare = (ms(total.gpuWaitNs) + ms(total.presentNs)) / meanMs;
            slowWindows = fps < 0.9 * double(gpu::g_settings.fpsLimit) && gpuShare >= 0.4 ? slowWindows + 1 : 0;
            if (slowWindows == 3 && gpu::g_settings.resolutionScale > 1)
                fprintf(stderr, "[perf] hint: the GPU limits the frame rate (%.1f fps, %.0f%% of each frame waiting for it) at "
                    "resolution scale %u; resolution_scale=1 renders %u times fewer pixels\n", fps, 100.0 * gpuShare,
                    gpu::g_settings.resolutionScale, gpu::g_settings.resolutionScale * gpu::g_settings.resolutionScale);
            // "rest" split: the renderer's own work on the game's render thread, the vblank wait, and what remains: the
            // guest's own code (game logic, its D3D calls) and the render thread's waits for the GPU thread or the guest.
            uint64_t times[uint32_t(gpu::FrameTime::Count)];
            for (uint32_t i = 0; i < uint32_t(gpu::FrameTime::Count); i++)
                times[i] = g_frameTimes[i].exchange(0, std::memory_order_relaxed);
            double renderer = ms(times[uint32_t(gpu::FrameTime::Renderer)]);
            double vblank = ms(times[uint32_t(gpu::FrameTime::VblankWait)]);
            double presentPath = WorkerRunning() ? double(workers.producerWaitNs) / 1e6 / double(frames) : ms(total.gpuWaitNs) + ms(total.presentNs);
            fprintf(stderr, "[perf] render thread per frame: renderer %.1f ms (ring %.1f, textures %.1f, gpu::Draw %.1f), vblank wait %.1f ms, "
                "%s %.1f ms, guest code and other %.1f ms\n", renderer, ms(times[uint32_t(gpu::FrameTime::Ring)]),
                ms(times[uint32_t(gpu::FrameTime::Textures)]), ms(total.drawNs), vblank,
                WorkerRunning() ? "waiting for the GPU thread" : "GPU wait and present", presentPath,
                std::max(meanMs - renderer - vblank - presentPath, 0.0));
            if (WorkerRunning())
                fprintf(stderr, "[perf] GPU thread busy %.0f%% (recording, submitting, GPU waits, present); the game's render thread "
                    "waited %.1f ms per frame for it; %u texture threads busy %.1f%% (%u decodes)\n",
                    100.0 * double(workers.busyNs) / double(now - windowStart), double(workers.producerWaitNs) / 1e6 / double(frames),
                    workers.textureThreads, 100.0 * double(workers.textureBusyNs) / double(now - windowStart) / std::max(workers.textureThreads, 1u),
                    workers.textureJobs);
        }
        else
        {
            for (auto& time : g_frameTimes)
                time.store(0, std::memory_order_relaxed);
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
    BufferCacheFrameDone(CompletedFrame());
    EndProducerFrame([image] { PresentNow(image); });
}

void gpu::detail::PresentNow(const Texture* image)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
    {
        SDL_Delay(16);
        return;
    }
    Context& c = *g_context;
    uint32_t imageIndex = 0;
    RenderTexture* target;
    const RenderFramebuffer* framebuffer;
    uint32_t width, height;
    if (c.swapChain != nullptr)
    {
        if (c.swapChain->needsResize())
            ResizeSwapChain();
        // Minimised or zero-sized window: nothing to present to.
        if (c.swapChain->isEmpty() || c.swapChainFramebuffers.empty() || !c.swapChain->acquireTexture(c.acquireSemaphore.get(), &imageIndex))
        {
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
        if (c.window != nullptr && SDL_GetWindowSizeInPixels(c.window, &windowWidth, &windowHeight) && windowWidth > 0
            && windowHeight > 0 && (uint32_t(windowWidth) != c.offscreen.desc.width || uint32_t(windowHeight) != c.offscreen.desc.height))
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
        list.end();
        c.recording = false;
        const RenderCommandList* lists = &list;
        RenderCommandSemaphore* acquired = c.acquireSemaphore.get();
        RenderCommandSemaphore* rendered = c.releaseSemaphores[imageIndex].get();
        c.queue->executeCommandLists(&lists, 1, &acquired, 1, &rendered, 1, c.submissions[c.submitted++].fence.get());
        Step("frame submitted, presenting");
        uint64_t presentStart = SDL_GetTicksNS();
        bool presented = PresentSwapChain(imageIndex, rendered);
        Step(presented ? "presented" : "present failed");
        c.perf.presentNs += SDL_GetTicksNS() - presentStart;
        EndFrame();
        if (presented && !c.presented)
            fprintf(stderr, "[gpu] first present %ux%u\n", width, height);
        c.presented |= presented;
        LogPresents(presented);
    }
    else
    {
        Flush();
        EndFrame();
        // Stand-in for vsync.
        uint64_t now = SDL_GetTicksNS();
        if (gpu::g_settings.vsync && c.nextOffscreenFrame > now)
            SDL_DelayPrecise(c.nextOffscreenFrame - now);
        c.nextOffscreenFrame = std::max(c.nextOffscreenFrame, now) + OffscreenFrameNs;
    }
    LogPerf();
}

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

bool gpu::ReadbackTexture(const Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, bool raw)
{
    ProducerLock producer;
    bool result = false;
    RunAndWait([&] { result = ReadbackNow(texture, rgba, width, height, raw); });
    return result;
}

bool gpu::detail::ReadbackNow(const Texture* texture, std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height, bool raw)
{
    std::lock_guard lock(g_mutex);
    Context& c = *g_context;
    if (texture == nullptr || DeviceLost())
        return false;

    width = texture->desc.width;
    height = texture->desc.height;
    uint64_t size = uint64_t(width) * height * 4;
    if (c.readbackCapacity < size)
    {
        c.readbackGpu = c.device->createBuffer(RenderBufferDesc::DefaultBuffer(size, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
        c.readbackCpu = c.device->createBuffer(RenderBufferDesc::ReadbackBuffer(size));
        c.readbackCapacity = size;
        c.readbackSet->setBuffer(1, c.readbackGpu.get(), size);
    }

    // plume copies textures only into textures on Metal and Vulkan, so a compute pass packs the pixels into a buffer.
    RenderCommandList& list = Record();
    Transition(list, *texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
    list.barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(c.readbackGpu.get(), RenderBufferAccess::WRITE));
    c.readbackSet->setTexture(0, texture->texture.get(), RenderTextureLayout::SHADER_READ);
    c.bound = {}; // a compute pipeline replaces the draws' pipeline state on D3D12
    // `raw`: ReadbackRawCS at step 1 (its layout has the same set, plus the step as a push constant).
    list.setComputePipelineLayout(raw ? c.rawReadbackLayout.get() : c.readbackLayout.get());
    list.setPipeline(raw ? c.readbackRawPipeline.get() : c.readbackPipeline.get());
    list.setComputeDescriptorSet(c.readbackSet.get(), 0);
    if (raw)
    {
        int32_t constants[2] = { 1, 0 };
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

        slot.width = texture->desc.width / step;
        slot.height = texture->desc.height / step;
        uint64_t size = std::max(uint64_t(slot.width) * slot.height * 4, uint64_t(4));
        if (slot.capacity < size)
        {
            slot.gpu = c.device->createBuffer(RenderBufferDesc::DefaultBuffer(size, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
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

bool gpu::detail::IsPresentThread()
{
    return g_hasPresentThread && std::this_thread::get_id() == g_presentThread;
}

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
