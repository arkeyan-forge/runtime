#include "gpu/upscale.h"
#include "gpu/context.h"
#include "gpu/fsr/fsr.h"
#include <atomic>
#include <cstdio>
#include <mutex>

#if defined(ARKEYAN_METAL)
#define MTLFX_PRIVATE_IMPLEMENTATION
#include <MetalFX/MetalFX.hpp>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function" // two helpers of plume's header
#include <plume_metal.h>
#pragma clang diagnostic pop
#endif

using namespace gpu::detail;

#if defined(ARKEYAN_METAL)
namespace
{
    // The camera's motion of each pixel: its position without this frame's jitter, at its depth, taken back to the
    // previous frame's clip space by the reprojection; the difference in texture coordinates (MetalFX scales it to
    // pixels). Written to an RG16F texture of the picture's size.
    constexpr const char* MotionSource = R"(
#include <metal_stdlib>
using namespace metal;
struct MotionParams
{
    float4x4 reprojection;
    float2 size;
    float2 jitter; // in clip space
    float depthMin, depthScale;
};
kernel void CameraMotion(depth2d<float, access::read> depth [[texture(0)]],
    texture2d<float, access::write> motion [[texture(1)]], constant MotionParams& p [[buffer(0)]],
    uint2 id [[thread_position_in_grid]])
{
    if (id.x >= uint(p.size.x) || id.y >= uint(p.size.y))
        return;
    float2 ndc = float2((float(id.x) + 0.5) / p.size.x * 2.0 - 1.0, 1.0 - (float(id.y) + 0.5) / p.size.y * 2.0) - p.jitter;
    float z = (depth.read(id) - p.depthMin) * p.depthScale;
    float4 previous = p.reprojection * float4(ndc, z, 1.0);
    float2 delta = previous.w > 0.0 ? previous.xy / previous.w - ndc : float2(0.0);
    motion.write(float4(delta * float2(0.5, -0.5), 0.0, 0.0), id);
}
)";

    struct MotionParams
    {
        float reprojection[16]; // column-major (MSL float4x4)
        float size[2];
        float jitter[2];
        float depthMin, depthScale;
        float padding[2];
    };
    static_assert(sizeof(MotionParams) == 96);

    struct Upscaler
    {
        MTLFX::TemporalScaler* scaler = nullptr;
        MTL::ComputePipelineState* motionPipeline = nullptr;
        MTL::Texture* motion = nullptr;
        NS::UInteger inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
        MTL::PixelFormat color = MTL::PixelFormatInvalid, depth = MTL::PixelFormatInvalid,
                         output = MTL::PixelFormatInvalid;
        bool failed = false; // said once; Upscale does nothing after
    } g_upscaler;

    MTL::Texture* MetalTexture(const gpu::Texture* texture)
    {
        return static_cast<const plume::MetalTexture*>(texture->texture.get())->mtl;
    }

    bool Fail(const char* what)
    {
        fprintf(stderr, "[upscale] MetalFX: %s; the scene is no longer upscaled\n", what);
        g_upscaler.failed = true;
        return false;
    }

    // The scaler and the motion texture for these textures; made again when a size or a format changes.
    bool Prepare(MTL::Device* device, MTL::Texture* color, MTL::Texture* depth, MTL::Texture* output)
    {
        Upscaler& u = g_upscaler;
        if (u.motionPipeline == nullptr)
        {
            NS::Error* error = nullptr;
            MTL::Library* library =
                device->newLibrary(NS::String::string(MotionSource, NS::UTF8StringEncoding), nullptr, &error);
            if (library == nullptr)
                return Fail("the motion kernel does not compile");
            MTL::Function* function = library->newFunction(NS::String::string("CameraMotion", NS::UTF8StringEncoding));
            u.motionPipeline = device->newComputePipelineState(function, &error);
            function->release();
            library->release();
            if (u.motionPipeline == nullptr)
                return Fail("no motion pipeline");
        }
        if (u.scaler != nullptr && u.inputWidth == color->width() && u.inputHeight == color->height() &&
            u.outputWidth == output->width() && u.outputHeight == output->height() && u.color == color->pixelFormat() &&
            u.depth == depth->pixelFormat() && u.output == output->pixelFormat())
            return true;

        if (u.scaler != nullptr)
            u.scaler->release();
        if (u.motion != nullptr)
            u.motion->release();
        u.scaler = nullptr;
        u.motion = nullptr;
        MTLFX::TemporalScalerDescriptor* descriptor = MTLFX::TemporalScalerDescriptor::alloc()->init();
        descriptor->setColorTextureFormat(color->pixelFormat());
        descriptor->setDepthTextureFormat(depth->pixelFormat());
        descriptor->setMotionTextureFormat(MTL::PixelFormatRG16Float);
        descriptor->setOutputTextureFormat(output->pixelFormat());
        descriptor->setInputWidth(color->width());
        descriptor->setInputHeight(color->height());
        descriptor->setOutputWidth(output->width());
        descriptor->setOutputHeight(output->height());
        u.scaler = descriptor->newTemporalScaler(device);
        descriptor->release();
        if (u.scaler == nullptr)
            return Fail("the temporal scaler refuses these formats or sizes");
        const MTL::TextureUsage needed = u.scaler->outputTextureUsage();
        if ((output->usage() & needed) != needed)
            return Fail("the output texture lacks a usage the scaler needs");

        MTL::TextureDescriptor* motion = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatRG16Float, color->width(), color->height(), false);
        motion->setUsage(MTL::TextureUsageShaderWrite | MTL::TextureUsageShaderRead | u.scaler->motionTextureUsage());
        motion->setStorageMode(MTL::StorageModePrivate);
        u.motion = device->newTexture(motion); // the descriptor is autoreleased
        if (u.motion == nullptr)
            return Fail("no motion texture");

        u.inputWidth = color->width(), u.inputHeight = color->height();
        u.outputWidth = output->width(), u.outputHeight = output->height();
        u.color = color->pixelFormat(), u.depth = depth->pixelFormat(), u.output = output->pixelFormat();
        fprintf(stderr, "[upscale] MetalFX temporal scaler %lux%lu -> %lux%lu (color format %lu, depth %lu)\n",
            (unsigned long)u.inputWidth, (unsigned long)u.inputHeight, (unsigned long)u.outputWidth,
            (unsigned long)u.outputHeight, (unsigned long)u.color, (unsigned long)u.depth);
        return true;
    }

    void UpscaleNow(const gpu::UpscaleDesc& desc)
    {
        std::lock_guard lock(g_mutex);
        if (DeviceLost() || g_upscaler.failed)
            return;
        Context& c = *g_context;
        MTL::Device* device = static_cast<plume::MetalDevice*>(c.device.get())->mtl;
        auto* queue = static_cast<plume::MetalCommandQueue*>(c.queue.get());
        MTL::Texture* color = MetalTexture(desc.color);
        MTL::Texture* depth = MetalTexture(desc.depth);
        MTL::Texture* output = MetalTexture(desc.output);
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        if (!Prepare(device, color, depth, output))
        {
            pool->release();
            return;
        }
        Upscaler& u = g_upscaler;

        // What was recorded goes first; this command buffer takes its place in the queue's order (plume_queue_order.patch):
        // it waits for the last one sent and the next one waits for it.
        Flush();
        MTL::CommandBuffer* buffer = queue->mtl->commandBuffer();
        buffer->setLabel(NS::String::string("Upscale", NS::UTF8StringEncoding));
        buffer->encodeWait(queue->orderEvent, queue->orderValue);

        MotionParams params{};
        for (uint32_t row = 0; row < 4; row++)
            for (uint32_t column = 0; column < 4; column++)
                params.reprojection[column * 4 + row] = float(desc.reprojection[row * 4 + column]);
        const float width = float(color->width()), height = float(color->height());
        params.size[0] = width, params.size[1] = height;
        params.jitter[0] = 2.0f * desc.jitter[0] / width;
        params.jitter[1] = -2.0f * desc.jitter[1] / height;
        params.depthMin = desc.depthMin;
        params.depthScale = desc.depthMax != desc.depthMin ? 1.0f / (desc.depthMax - desc.depthMin) : 1.0f;
        MTL::ComputeCommandEncoder* encoder = buffer->computeCommandEncoder();
        encoder->setComputePipelineState(u.motionPipeline);
        encoder->setTexture(depth, 0);
        encoder->setTexture(u.motion, 1);
        encoder->setBytes(&params, sizeof(params), 0);
        encoder->dispatchThreads(MTL::Size(color->width(), color->height(), 1), MTL::Size(8, 8, 1));
        encoder->endEncoding();

        u.scaler->setColorTexture(color);
        u.scaler->setDepthTexture(depth);
        u.scaler->setMotionTexture(u.motion);
        u.scaler->setOutputTexture(output);
        u.scaler->setInputContentWidth(color->width());
        u.scaler->setInputContentHeight(color->height());
        // FSR's conventions, checked on Giants (docs/runtime/gpu/upscaling.md): the jitter as the shaders add it, the
        // motion from the current position to the previous one.
        u.scaler->setJitterOffsetX(desc.jitter[0]);
        u.scaler->setJitterOffsetY(desc.jitter[1]);
        u.scaler->setMotionVectorScaleX(width);
        u.scaler->setMotionVectorScaleY(height);
        u.scaler->setDepthReversed(desc.depthMin > desc.depthMax);
        u.scaler->setReset(desc.reset);
        u.scaler->encodeToCommandBuffer(buffer);

        buffer->encodeSignalEvent(queue->orderEvent, ++queue->orderValue);
        buffer->addCompletedHandler([](MTL::CommandBuffer* done) {
            // The log's GPU time of the pass (motion and MetalFX), every 900 upscales.
            static std::atomic<uint64_t> ns, count;
            ns += uint64_t((done->GPUEndTime() - done->GPUStartTime()) * 1e9);
            if (++count == 900)
            {
                fprintf(
                    stderr, "[upscale] GPU time per upscale (motion and MetalFX): %.2f ms\n", ns.exchange(0) / 900e6);
                count = 0;
            }
        });
        buffer->commit();
        pool->release();
    }
}

bool gpu::UpscalerAvailable(Upscaler upscaler)
{
    std::lock_guard lock(g_mutex);
    if (g_context == nullptr || g_context->device == nullptr)
        return false;
    if (upscaler == Upscaler::Fsr3)
        return fsr::Available();
    return MTLFX::TemporalScalerDescriptor::supportsDevice(
        static_cast<plume::MetalDevice*>(g_context->device.get())->mtl);
}
#else
bool gpu::UpscalerAvailable(Upscaler upscaler)
{
    std::lock_guard lock(g_mutex);
    return upscaler == Upscaler::Fsr3 && g_context != nullptr && g_context->device != nullptr && fsr::Available();
}
#endif

void gpu::Upscale(const UpscaleDesc& desc)
{
    ProducerLock producer;
    Enqueue([=] {
#if defined(ARKEYAN_METAL)
        if (desc.upscaler == Upscaler::MetalFx)
        {
            UpscaleNow(desc);
            return;
        }
#endif
        std::lock_guard lock(g_mutex);
        if (DeviceLost())
            return;
        SettleCopies(desc.color);
        SettleCopies(desc.depth);
        SettleCopies(desc.output);
        fsr::Upscale(desc);
    });
}
