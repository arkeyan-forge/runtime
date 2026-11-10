#include "gpu/fsr/backend.h"
#include "gpu/context.h"
#include "gpu/fsr/spirv_reflect.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <fsr3upscaler/ffx_fsr3upscaler_private.h> // the shader permutation bits, as the SDK's own backends use them
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <frameinterpolation/ffx_frameinterpolation_private.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if defined(ARKEYAN_METAL)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function" // two helpers of plume's header
#include <plume_metal.h>
#pragma clang diagnostic pop
#elif !defined(PLUME_D3D12_COREWINDOW)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundef" // the Vulkan Memory Allocator's extension macros (gpu/gpu.cpp)
#include <plume_vulkan.h>
#pragma clang diagnostic pop
#endif

#include "shaders/fsr_shaders.h"

using namespace gpu::detail;
using namespace plume;

namespace
{
    // The SPIR-V binding of each register class (gpu/fsr/fsr.cmake's -fvk-*-shift): D3D12 takes the register itself.
    constexpr uint32_t ShiftS = 8;
#if !defined(ARKEYAN_METAL)
    constexpr uint32_t ShiftB = 0, ShiftT = 16, ShiftU = 48;
#endif

    struct Shader
    {
        FfxEffect effect;
        FfxPass pass;
        bool hdr;
        std::span<const uint8_t> spirv;
#if defined(ARKEYAN_METAL)
        std::span<const uint8_t> metallib;
        std::span<const gpu::fsr::MslSlot> msl;
#elif defined(_WIN32)
        std::span<const uint8_t> dxil;
#endif
    };

#if defined(ARKEYAN_METAL)
#define FFX_SHADER(effect, pass, name, hdr)                                                                            \
    { effect, pass, hdr, g_fsr_##name##_spirv, g_fsr_##name##_metallib, g_fsr_##name##_msl }
#elif defined(_WIN32)
#define FFX_SHADER(effect, pass, name, hdr) { effect, pass, hdr, g_fsr_##name##_spirv, g_fsr_##name##_dxil }
#else
#define FFX_SHADER(effect, pass, name, hdr) { effect, pass, hdr, g_fsr_##name##_spirv }
#endif
#define FSR_SHADER(pass, name, hdr) FFX_SHADER(FFX_EFFECT_FSR3UPSCALER, pass, name, hdr)
#define FI_SHADER(pass, name)                                                                                          \
    FFX_SHADER(FFX_EFFECT_FRAMEINTERPOLATION, FFX_FRAMEINTERPOLATION_PASS_##pass, fi_##name, false)
#define OF_SHADER(pass, name) FFX_SHADER(FFX_EFFECT_OPTICALFLOW, FFX_OPTICALFLOW_PASS_##pass, of_##name, false)

    // Each pass for SDR colour and for HDR colour (fsr.cmake).
    const Shader g_shaders[] = {
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, prepare_inputs, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_PREPARE_INPUTS, prepare_inputs_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID, luma_pyramid, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_LUMA_PYRAMID, luma_pyramid_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, shading_change_pyramid, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE_PYRAMID, shading_change_pyramid_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE, shading_change, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_SHADING_CHANGE, shading_change_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, prepare_reactivity, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_PREPARE_REACTIVITY, prepare_reactivity_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY, luma_instability, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_LUMA_INSTABILITY, luma_instability_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_ACCUMULATE, accumulate, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_ACCUMULATE, accumulate_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN, accumulate_sharpen, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_ACCUMULATE_SHARPEN, accumulate_sharpen_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_RCAS, rcas, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_RCAS, rcas_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_DEBUG_VIEW, debug_view, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_DEBUG_VIEW, debug_view_hdr, true),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE, autogen_reactive, false),
        FSR_SHADER(FFX_FSR3UPSCALER_PASS_GENERATE_REACTIVE, autogen_reactive_hdr, true),
        // Frame generation (gpu/fsr/framegen.cmake): frame interpolation and optical flow, one permutation each.
        FI_SHADER(RECONSTRUCT_AND_DILATE, reconstruct_and_dilate),
        FI_SHADER(SETUP, setup),
        FI_SHADER(RECONSTRUCT_PREV_DEPTH, reconstruct_previous_depth),
        FI_SHADER(GAME_MOTION_VECTOR_FIELD, game_motion_vector_field),
        FI_SHADER(OPTICAL_FLOW_VECTOR_FIELD, optical_flow_vector_field),
        FI_SHADER(DISOCCLUSION_MASK, disocclusion_mask),
        FI_SHADER(INTERPOLATION, pass),
        FI_SHADER(INPAINTING_PYRAMID, compute_inpainting_pyramid),
        FI_SHADER(INPAINTING, inpainting),
        FI_SHADER(GAME_VECTOR_FIELD_INPAINTING_PYRAMID, compute_game_vector_field_inpainting_pyramid),
        FI_SHADER(DEBUG_VIEW, debug_view),
        OF_SHADER(PREPARE_LUMA, prepare_luma),
        OF_SHADER(GENERATE_OPTICAL_FLOW_INPUT_PYRAMID, compute_luminance_pyramid),
        OF_SHADER(GENERATE_SCD_HISTOGRAM, generate_scd_histogram),
        OF_SHADER(COMPUTE_SCD_DIVERGENCE, compute_scd_divergence),
        OF_SHADER(COMPUTE_OPTICAL_FLOW_ADVANCED_V5, compute_optical_flow_advanced_v5),
        OF_SHADER(FILTER_OPTICAL_FLOW_V5, filter_optical_flow_v5),
        OF_SHADER(SCALE_OPTICAL_FLOW_ADVANCED_V5, scale_optical_flow_advanced_v5),
    };

    // The permutation the shaders are built in (fsr.cmake); the sharpening bit picks the pass's own shader.
    constexpr uint32_t PermutationAllowed = FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS |
                                            FSR3UPSCALER_SHADER_PERMUTATION_ENABLE_SHARPENING |
                                            FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT;

    // An FFX resource: a texture of ours (registered for a dispatch) or one FFX made.
    struct Resource
    {
        gpu::Texture* texture = nullptr;
        bool owned = false; // made by CreateResource (gpu::CreateTexture, or the Metal atomics pair below)
        FfxResourceDescription desc{};
        std::vector<std::unique_ptr<RenderTextureView>> mips; // a view per mip, for UAVs of a mip chain
        // Metal: a uint texture FFX updates with atomics is a linear texture over this buffer, which the shaders' atomics
        // use (MoltenVK's converter emulates texture atomics with a buffer of the same layout).
        std::unique_ptr<RenderBuffer> atomics;
        RenderBuffer* borrowedAtomics = nullptr; // a registered texture's: the atomics of the shared texture it is
        std::vector<uint8_t> init; // data to copy in at the next execution
        std::unique_ptr<RenderBuffer> buffer; // FFX_RESOURCE_TYPE_BUFFER (frame interpolation's counters)
    };

    // How a pipeline's descriptor set is laid out: per binding, what FFX binds there.
    enum class SlotKind : uint8_t
    {
        Constant,
        Sampler,
        Srv,
        Uav,
        Atomics, // Metal: the atomics buffer of the UAV at `index`
        SrvBuffer,
        UavBuffer
    };

    struct Slot
    {
        SlotKind kind;
        uint32_t index; // into FFX's list of that kind (srv/uav texture and buffer bindings, constantBufferBindings)
    };

    struct Pipeline
    {
        std::unique_ptr<RenderShader> shader;
        std::vector<std::unique_ptr<RenderSampler>> samplers;
        RenderDescriptorSetBuilder setBuilder;
        std::unique_ptr<RenderPipelineLayout> layout;
        std::unique_ptr<RenderPipeline> pipeline;
        std::vector<Slot> slots; // by descriptor index
        std::string name; // FFX's, for the log
        uint32_t constants = 0;
        // Descriptor sets and constant buffers per frame in flight; a dispatch takes the next free one of its frame.
        struct Instance
        {
            std::unique_ptr<RenderDescriptorSet> set;
            std::vector<std::unique_ptr<RenderBuffer>> constants;
            std::vector<uint8_t*> mapped;
        };
        static constexpr uint32_t Frames = 3;
        std::vector<std::unique_ptr<Instance>> instances[Frames];
        uint32_t used[Frames] = {};
        uint64_t frame[Frames] = {};
    };

    constexpr int32_t RegisteredBase = 1 << 20;
    constexpr uint32_t ConstantSize = 256; // D3D12's constant buffer alignment

    struct Backend
    {
        std::vector<std::unique_ptr<Resource>> resources; // index = FfxResourceInternal
        std::vector<Resource> registered; // index - RegisteredBase
        std::vector<std::unique_ptr<Pipeline>> pipelines;
        std::vector<FfxGpuJobDescription> jobs;
        alignas(16) uint8_t constantData[16384];
        uint32_t constantUsed = 0;

        // The clears (fsr_inputs.hlsl / .metal): set 0 = { the target }, push constants (binding 0, set 1).
        std::unique_ptr<RenderShader> clearFloatShader, clearUintShader;
        std::unique_ptr<RenderPipelineLayout> clearLayout;
        std::unique_ptr<RenderPipeline> clearFloat, clearUint;
        RenderDescriptorSetBuilder clearSetBuilder;
        std::vector<std::unique_ptr<RenderDescriptorSet>> clearSets[Pipeline::Frames];
        uint32_t clearUsed[Pipeline::Frames] = {};
        uint64_t clearFrame[Pipeline::Frames] = {};
        bool failed = false;
    } g_backend;

    RenderShaderFormat ShaderFormat() { return g_context->renderInterface->getCapabilities().shaderFormat; }

#if !defined(ARKEYAN_METAL) && !defined(PLUME_D3D12_COREWINDOW)
    // Vulkan: a shader's SPIR-V with its storage images' formats Unknown (gpu::spirv::WithoutStorageFormats: DXC's
    // formats are the HLSL types', not the views'), when the device reads and writes storage images without a format
    // (desktop drivers and MoltenVK do; plume enables every feature the device has). Else the SPIR-V as built.
    std::vector<uint32_t> VulkanSpirv(std::span<const uint8_t> blob)
    {
        static const bool withoutFormat = [] {
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(static_cast<VulkanDevice&>(*g_context->device).physicalDevice, &features);
            fprintf(stderr, "[fsr] storage images without a format: %s\n",
                features.shaderStorageImageReadWithoutFormat && features.shaderStorageImageWriteWithoutFormat
                    ? "yes"
                    : "no (the shaders keep their formats)");
            return features.shaderStorageImageReadWithoutFormat && features.shaderStorageImageWriteWithoutFormat;
        }();
        const auto* words = reinterpret_cast<const uint32_t*>(blob.data());
        if (!withoutFormat)
            return std::vector<uint32_t>(words, words + blob.size() / 4);
        return gpu::spirv::WithoutStorageFormats(words, blob.size() / 4);
    }
#endif

    void Fail(const char* what)
    {
        if (!g_backend.failed)
            fprintf(stderr, "[fsr] %s\n", what);
        g_backend.failed = true;
    }

    Resource* Find(FfxResourceInternal resource)
    {
        Backend& b = g_backend;
        if (resource.internalIndex >= RegisteredBase)
        {
            const size_t i = size_t(resource.internalIndex - RegisteredBase);
            return i < b.registered.size() ? &b.registered[i] : nullptr;
        }
        if (resource.internalIndex < 0 || size_t(resource.internalIndex) >= b.resources.size())
            return nullptr;
        return b.resources[size_t(resource.internalIndex)].get();
    }

    RenderFormat ToRender(FfxSurfaceFormat format)
    {
        switch (format)
        {
        case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return RenderFormat::R32G32B32A32_FLOAT;
        case FFX_SURFACE_FORMAT_R32G32B32A32_UINT: return RenderFormat::R32G32B32A32_UINT;
        case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return RenderFormat::R16G16B16A16_FLOAT;
        case FFX_SURFACE_FORMAT_R32G32_FLOAT: return RenderFormat::R32G32_FLOAT;
        case FFX_SURFACE_FORMAT_R32_UINT: return RenderFormat::R32_UINT;
        case FFX_SURFACE_FORMAT_R32_FLOAT: return RenderFormat::R32_FLOAT;
        case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return RenderFormat::R8G8B8A8_UNORM;
        case FFX_SURFACE_FORMAT_R8G8B8A8_SNORM: return RenderFormat::R8G8B8A8_SNORM;
        case FFX_SURFACE_FORMAT_B8G8R8A8_UNORM: return RenderFormat::B8G8R8A8_UNORM;
        case FFX_SURFACE_FORMAT_R16G16_FLOAT: return RenderFormat::R16G16_FLOAT;
        case FFX_SURFACE_FORMAT_R16G16_UINT: return RenderFormat::R16G16_UINT;
        case FFX_SURFACE_FORMAT_R16G16_SINT: return RenderFormat::R16G16_SINT;
        case FFX_SURFACE_FORMAT_R16_FLOAT: return RenderFormat::R16_FLOAT;
        case FFX_SURFACE_FORMAT_R16_UINT: return RenderFormat::R16_UINT;
        case FFX_SURFACE_FORMAT_R16_UNORM: return RenderFormat::R16_UNORM;
        case FFX_SURFACE_FORMAT_R16_SNORM: return RenderFormat::R16_SNORM;
        case FFX_SURFACE_FORMAT_R8_UNORM: return RenderFormat::R8_UNORM;
        case FFX_SURFACE_FORMAT_R8_UINT: return RenderFormat::R8_UINT;
        case FFX_SURFACE_FORMAT_R8G8_UNORM: return RenderFormat::R8G8_UNORM;
        case FFX_SURFACE_FORMAT_R8G8_UINT: return RenderFormat::R8G8_UINT;
        default: return RenderFormat::UNKNOWN;
        }
    }

    FfxSurfaceFormat FromRender(RenderFormat format)
    {
        switch (format)
        {
        case RenderFormat::R32G32B32A32_FLOAT: return FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT;
        case RenderFormat::R16G16B16A16_FLOAT: return FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        case RenderFormat::R32G32_FLOAT: return FFX_SURFACE_FORMAT_R32G32_FLOAT;
        case RenderFormat::R32_UINT: return FFX_SURFACE_FORMAT_R32_UINT;
        case RenderFormat::R32_FLOAT: return FFX_SURFACE_FORMAT_R32_FLOAT;
        case RenderFormat::R8G8B8A8_UNORM: return FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case RenderFormat::B8G8R8A8_UNORM: return FFX_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case RenderFormat::R16G16_FLOAT: return FFX_SURFACE_FORMAT_R16G16_FLOAT;
        case RenderFormat::R16_FLOAT: return FFX_SURFACE_FORMAT_R16_FLOAT;
        case RenderFormat::R8_UNORM: return FFX_SURFACE_FORMAT_R8_UNORM;
        default: return FFX_SURFACE_FORMAT_UNKNOWN;
        }
    }

    bool IsUint(RenderFormat format)
    {
        return format == RenderFormat::R32_UINT || format == RenderFormat::R16_UINT ||
               format == RenderFormat::R8_UINT || format == RenderFormat::R16G16_UINT ||
               format == RenderFormat::R8G8_UINT || format == RenderFormat::R32G32B32A32_UINT;
    }

    void Copy(wchar_t* destination, size_t size, const char* source)
    {
        size_t i = 0;
        for (; i + 1 < size && source[i] != '\0'; i++)
            destination[i] = wchar_t(source[i]);
        destination[i] = 0;
    }

    // FFX's wide name equals `name`.
    bool SameName(const wchar_t* wide, const char* name)
    {
        size_t i = 0;
        for (; name[i] != '\0'; i++)
            if (wide[i] != wchar_t(name[i]))
                return false;
        return wide[i] == 0;
    }

    const Shader* FindShader(FfxEffect effect, FfxPass pass, bool hdr)
    {
        for (const Shader& shader : g_shaders)
            if (shader.effect == effect && shader.pass == pass && shader.hdr == hdr)
                return &shader;
        return nullptr;
    }

    uint32_t FrameSlot(uint64_t frame) { return uint32_t(frame % Pipeline::Frames); }

#if defined(ARKEYAN_METAL)
    // A linear R32Uint texture over a buffer of the same rows: what the converter's texture atomics expect (their rows
    // are padded to the device's linear texture alignment, which the pipelines get as function constant 65535).
    NS::UInteger LinearAlignment()
    {
        auto* device = static_cast<MetalDevice*>(g_context->device.get());
        return device->mtl->minimumLinearTextureAlignmentForPixelFormat(MTL::PixelFormatR32Uint);
    }

    gpu::Texture* CreateAtomicTexture(uint32_t width, uint32_t height, std::unique_ptr<RenderBuffer>& buffer)
    {
        auto* device = static_cast<MetalDevice*>(g_context->device.get());
        const NS::UInteger alignment = LinearAlignment();
        const NS::UInteger rowBytes = (NS::UInteger(width) * 4 + alignment - 1) / alignment * alignment;
        buffer = device->createBuffer(RenderBufferDesc::DefaultBuffer(
            rowBytes * height, RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
        if (buffer == nullptr)
            return nullptr;
        MTL::TextureDescriptor* descriptor =
            MTL::TextureDescriptor::texture2DDescriptor(MTL::PixelFormatR32Uint, width, height, false);
        descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
        descriptor->setStorageMode(MTL::StorageModePrivate);
        MTL::Texture* mtl = static_cast<MetalBuffer*>(buffer.get())->mtl->newTexture(descriptor, 0, rowBytes);
        if (mtl == nullptr)
            return nullptr;
        auto wrapper = std::make_unique<MetalTexture>();
        wrapper->device = device;
        wrapper->mtl = mtl;
        wrapper->desc = RenderTextureDesc::Texture2D(
            width, height, 1, RenderFormat::R32_UINT, RenderTextureFlag::UNORDERED_ACCESS | RenderTextureFlag::STORAGE);
        device->addResource(mtl);
        auto* texture = new gpu::Texture{ .desc = { .width = width,
                                              .height = height,
                                              .format = RenderFormat::R32_UINT,
                                              .usage = gpu::TextureUsage::Storage } };
        texture->texture = std::move(wrapper);
        return texture;
    }
#endif

    // --- The interface's callbacks ------------------------------------------------------------------------------------

    FfxVersionNumber GetSdkVersion(FfxInterface*) { return FFX_SDK_MAKE_VERSION(1, 1, 4); }

    FfxErrorCode GetEffectGpuMemoryUsage(FfxInterface*, FfxUInt32, FfxEffectMemoryUsage* usage)
    {
        *usage = {};
        return FFX_OK;
    }

    FfxErrorCode CreateBackendContext(FfxInterface*, FfxEffect, FfxEffectBindlessConfig*, FfxUInt32* id)
    {
        *id = 0;
        return FFX_OK;
    }

    FfxErrorCode GetDeviceCapabilities(FfxInterface*, FfxDeviceCapabilities* capabilities)
    {
        *capabilities = {};
        capabilities->maximumSupportedShaderModel = FFX_SHADER_MODEL_6_2;
        capabilities->waveLaneCountMin = 32;
        capabilities->waveLaneCountMax = 32;
        return FFX_OK;
    }

    FfxErrorCode DestroyBackendContext(FfxInterface*, FfxUInt32) { return FFX_OK; }

    FfxErrorCode CreateResource(
        FfxInterface*, const FfxCreateResourceDescription* create, FfxUInt32, FfxResourceInternal* out)
    {
        Backend& b = g_backend;
        auto resource = std::make_unique<Resource>();
        resource->owned = true;
        resource->desc = create->resourceDescription;
        const FfxResourceDescription& d = create->resourceDescription;
        if (d.type == FFX_RESOURCE_TYPE_BUFFER)
        {
            // A structured buffer: width bytes, height the stride. Zeroed at the next execution (FI's counters start at 0).
            resource->buffer = g_context->device->createBuffer(RenderBufferDesc::DefaultBuffer(
                std::max(d.width, 16u), RenderBufferFlag::STORAGE | RenderBufferFlag::UNORDERED_ACCESS));
            if (resource->buffer == nullptr)
            {
                Fail("a buffer of FFX's could not be made");
                return FFX_ERROR_OUT_OF_MEMORY;
            }
            resource->init.assign(std::max(d.width, 16u), 0);
            out->internalIndex = int32_t(b.resources.size());
            b.resources.push_back(std::move(resource));
            return FFX_OK;
        }
        if (d.type != FFX_RESOURCE_TYPE_TEXTURE2D)
        {
            Fail("FFX asked for a resource that is not a 2D texture");
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        const RenderFormat format = ToRender(d.format);
        if (format == RenderFormat::UNKNOWN)
        {
            Fail("FFX asked for an unknown texture format");
            return FFX_ERROR_INVALID_ARGUMENT;
        }
        uint32_t mips = d.mipCount;
        if (mips == 0)
            mips = uint32_t(std::floor(std::log2(double(std::max(d.width, d.height))))) + 1;
        resource->desc.mipCount = mips;
        const bool uav = (d.usage & FFX_RESOURCE_USAGE_UAV) != 0;
#if defined(ARKEYAN_METAL)
        if (uav && format == RenderFormat::R32_UINT && mips == 1)
            resource->texture = CreateAtomicTexture(d.width, d.height, resource->atomics);
        else
#endif
            resource->texture = gpu::CreateTexture({ .width = d.width,
                .height = d.height,
                .mipLevels = mips,
                .format = format,
                // Storage for all: FFX clears and writes some it calls read-only (Vulkan checks the usage).
                .usage = gpu::TextureUsage::Storage });
        if (resource->texture == nullptr || resource->texture->texture == nullptr)
        {
            Fail("a texture of FFX's could not be made");
            return FFX_ERROR_OUT_OF_MEMORY;
        }
        if (uav && mips > 1)
            for (uint32_t mip = 0; mip < mips; mip++)
            {
                RenderTextureViewDesc view = RenderTextureViewDesc::Texture2D(format);
                view.mipSlice = mip;
                view.mipLevels = 1;
                resource->mips.push_back(resource->texture->texture->createTextureView(view));
            }
        const FfxResourceInitData& init = create->initData;
        if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_BUFFER && init.buffer != nullptr)
            resource->init.assign(
                static_cast<const uint8_t*>(init.buffer), static_cast<const uint8_t*>(init.buffer) + init.size);
        else if (init.type == FFX_RESOURCE_INIT_DATA_TYPE_VALUE)
            resource->init.assign(init.size, init.value);
        out->internalIndex = int32_t(b.resources.size());
        b.resources.push_back(std::move(resource));
        return FFX_OK;
    }

    FfxErrorCode RegisterResource(FfxInterface*, const FfxResource* in, FfxUInt32, FfxResourceInternal* out)
    {
        Backend& b = g_backend;
        Resource resource;
        resource.texture = static_cast<gpu::Texture*>(in->resource);
        resource.desc = in->description;
        // One of our shared textures (CreateShared): its atomics buffer and mip views come along.
        for (const auto& own : b.resources)
            if (own != nullptr && own->texture == resource.texture && own->texture != nullptr)
                resource.borrowedAtomics = own->atomics.get();
        out->internalIndex = RegisteredBase + int32_t(b.registered.size());
        b.registered.push_back(std::move(resource));
        return FFX_OK;
    }

    FfxResource GetResource(FfxInterface*, FfxResourceInternal internal)
    {
        FfxResource resource{};
        if (const Resource* r = Find(internal))
        {
            resource.resource = r->texture;
            resource.description = r->desc;
        }
        return resource;
    }

    FfxErrorCode UnregisterResources(FfxInterface*, FfxCommandList, FfxUInt32)
    {
        g_backend.registered.clear();
        return FFX_OK;
    }

    FfxResourceDescription GetResourceDescription(FfxInterface*, FfxResourceInternal internal)
    {
        const Resource* r = Find(internal);
        return r != nullptr ? r->desc : FfxResourceDescription{};
    }

    void Free(Resource& resource)
    {
        resource.mips.clear();
        if (resource.atomics != nullptr)
            delete resource.texture; // ours (CreateAtomicTexture): the GPU is idle (ReleaseAll)
        else if (resource.owned)
            if (resource.texture != nullptr)
                DeferDestroy(resource.texture); // the GPU thread: gpu::DestroyTexture is a producer's call
        resource.atomics.reset();
        resource.buffer.reset(); // the GPU is idle (ReleaseAll)
        resource.texture = nullptr;
    }

    FfxErrorCode DestroyResource(FfxInterface*, FfxResourceInternal internal, FfxUInt32)
    {
        Resource* r = Find(internal);
        if (r != nullptr && r->owned)
            Free(*r);
        return FFX_OK;
    }

    FfxErrorCode StageConstantBufferData(FfxInterface*, void* data, FfxUInt32 size, FfxConstantBuffer* constantBuffer)
    {
        Backend& b = g_backend;
        const uint32_t aligned = (size + 15) & ~15u;
        if (b.constantUsed + aligned > sizeof(b.constantData) || size > ConstantSize)
            return FFX_ERROR_OUT_OF_MEMORY;
        memcpy(b.constantData + b.constantUsed, data, size);
        constantBuffer->data = reinterpret_cast<uint32_t*>(b.constantData + b.constantUsed);
        constantBuffer->num32BitEntries = size / 4;
        b.constantUsed += aligned;
        return FFX_OK;
    }

    // The descriptor set layout from the shader: FFX's names, our bindings per backend.
    FfxErrorCode CreatePipeline(FfxInterface*, FfxEffect effect, FfxPass pass, uint32_t permutation,
        const FfxPipelineDescription* description, FfxUInt32, FfxPipelineState* out)
    {
        Backend& b = g_backend;
        // The permutations built (fsr.cmake, framegen.cmake): the upscaler's, frame interpolation with motion at the
        // render size, optical flow's plain one.
        const bool built = (effect == FFX_EFFECT_FSR3UPSCALER && (permutation & ~PermutationAllowed) == 0 &&
                               (permutation & FSR3UPSCALER_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS)) ||
                           (effect == FFX_EFFECT_FRAMEINTERPOLATION &&
                               permutation == FRAMEINTERPOLATION_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS) ||
                           (effect == FFX_EFFECT_OPTICALFLOW && permutation == 0);
        if (!built)
        {
            Fail("FFX asked for a shader permutation that is not built");
            return FFX_ERROR_BACKEND_API_ERROR;
        }
        const bool hdr =
            effect == FFX_EFFECT_FSR3UPSCALER && (permutation & FSR3UPSCALER_SHADER_PERMUTATION_HDR_COLOR_INPUT) != 0;
        const Shader* shader = FindShader(effect, pass, hdr);
        gpu::spirv::Module module;
        if (shader == nullptr || !gpu::spirv::Reflect(reinterpret_cast<const uint32_t*>(shader->spirv.data()),
                                     shader->spirv.size() / 4, module))
        {
            Fail("no shader for an FSR pass");
            return FFX_ERROR_BACKEND_API_ERROR;
        }

        auto pipeline = std::make_unique<Pipeline>();
        const RenderShaderFormat format = ShaderFormat();
        std::span<const uint8_t> blob = shader->spirv;
#if defined(ARKEYAN_METAL)
        blob = shader->metallib;
#elif defined(_WIN32)
        if (format == RenderShaderFormat::DXIL)
            blob = shader->dxil;
#endif
#if !defined(ARKEYAN_METAL) && !defined(PLUME_D3D12_COREWINDOW)
        std::vector<uint32_t> vulkan;
        if (format == RenderShaderFormat::SPIRV)
        {
            vulkan = VulkanSpirv(shader->spirv);
            blob = std::span(reinterpret_cast<const uint8_t*>(vulkan.data()), vulkan.size() * 4);
        }
#endif
        pipeline->shader = g_context->device->createShader(blob.data(), blob.size(), "CS", format);

        // FFX's lists: SRVs, UAVs and constant buffers in the shader's order, with their names.
        *out = {};
        Copy(out->name, FFX_RESOURCE_NAME_SIZE, "FSR3");
        for (size_t i = 0; i < 64 && description->name[i] != 0; i++)
            out->name[i] = description->name[i];
        out->passId = pass;
        auto ffxIndex = [&](const gpu::spirv::Binding& binding, SlotKind& kind) -> uint32_t {
            FfxResourceBinding* list = nullptr;
            uint32_t* count = nullptr;
            switch (binding.kind)
            {
            case gpu::spirv::Kind::ConstantBuffer:
                kind = SlotKind::Constant, list = out->constantBufferBindings, count = &out->constCount;
                break;
            case gpu::spirv::Kind::Texture:
                kind = SlotKind::Srv, list = out->srvTextureBindings, count = &out->srvTextureCount;
                break;
            case gpu::spirv::Kind::StorageTexture:
                kind = SlotKind::Uav, list = out->uavTextureBindings, count = &out->uavTextureCount;
                break;
            case gpu::spirv::Kind::Buffer:
                kind = SlotKind::SrvBuffer, list = out->srvBufferBindings, count = &out->srvBufferCount;
                break;
            case gpu::spirv::Kind::StorageBuffer:
                kind = SlotKind::UavBuffer, list = out->uavBufferBindings, count = &out->uavBufferCount;
                break;
            case gpu::spirv::Kind::Sampler: kind = SlotKind::Sampler; return binding.binding - ShiftS;
            default: return UINT32_MAX;
            }
            for (uint32_t i = 0; i < *count; i++)
            {
                if (SameName(list[i].name, binding.name.c_str()))
                    return i;
            }
            FfxResourceBinding& entry = list[(*count)++];
            entry.slotIndex = 0;
            Copy(entry.name, FFX_RESOURCE_NAME_SIZE, binding.name.c_str());
            return *count - 1;
        };
        auto samplerFor = [&](uint32_t index) -> const RenderSampler* {
            if (index >= description->samplerCount)
                return nullptr;
            const FfxSamplerDescription& s = description->samplers[index];
            RenderSamplerDesc desc;
            const bool point = s.filter == FFX_FILTER_TYPE_MINMAGMIP_POINT;
            desc.minFilter = desc.magFilter = point ? RenderFilter::NEAREST : RenderFilter::LINEAR;
            desc.mipmapMode = point ? RenderMipmapMode::NEAREST : RenderMipmapMode::LINEAR;
            desc.addressU = desc.addressV = desc.addressW = RenderTextureAddressMode::CLAMP;
            pipeline->samplers.push_back(g_context->device->createSampler(desc));
            return pipeline->samplers.back().get();
        };

        // D3D12 and Vulkan: the immutable samplers go last: plume's D3D12 sets keep a heap slot for each that their
        // descriptor table does not have, which would move every view after it by one.
        pipeline->setBuilder.begin();
#if defined(ARKEYAN_METAL)
        // Metal: the argument buffer's ids (msl_table.cmake, 0 to n-1), matched to the SPIR-V by name.
        for (uint32_t i = 0; i < shader->msl.size(); i++)
            if (shader->msl[i].id != i)
            {
                Fail("an FSR shader's argument buffer has a gap");
                return FFX_ERROR_BACKEND_API_ERROR;
            }
        // In id order, samplers in place: plume's Metal set layout sorts its ranges by binding and takes a descriptor's
        // kind from that sorted list by the order they were added (a buffer after a moved sampler was taken for a texture).
        for (const gpu::fsr::MslSlot& slot : shader->msl)
        {
            if (slot.kind == 'a')
            {
                const std::string texture = std::string(slot.name, strlen(slot.name) - strlen("_atomic"));
                uint32_t uav = UINT32_MAX;
                for (uint32_t i = 0; i < out->uavTextureCount; i++)
                {
                    if (SameName(out->uavTextureBindings[i].name, texture.c_str()))
                        uav = i;
                }
                if (uav == UINT32_MAX)
                {
                    Fail("an FSR shader's atomics have no texture");
                    return FFX_ERROR_BACKEND_API_ERROR;
                }
                pipeline->setBuilder.addReadWriteStructuredBuffer(slot.id);
                pipeline->slots.push_back({ SlotKind::Atomics, uav });
                continue;
            }
            const gpu::spirv::Binding* binding = nullptr;
            for (const gpu::spirv::Binding& candidate : module.bindings)
                if (candidate.name == slot.name)
                    binding = &candidate;
            SlotKind kind;
            const uint32_t index = binding != nullptr ? ffxIndex(*binding, kind) : UINT32_MAX;
            if (index == UINT32_MAX)
            {
                Fail("an FSR shader binds something the backend does not know");
                return FFX_ERROR_BACKEND_API_ERROR;
            }
            if (kind == SlotKind::Constant)
                pipeline->setBuilder.addConstantBuffer(slot.id);
            else if (kind == SlotKind::SrvBuffer || kind == SlotKind::UavBuffer)
                pipeline->setBuilder.addReadWriteStructuredBuffer(slot.id);
            else if (kind == SlotKind::Sampler)
                pipeline->setBuilder.addImmutableSampler(slot.id, samplerFor(index));
            else if (slot.kind == 'w')
                pipeline->setBuilder.addReadWriteTexture(slot.id);
            else
                pipeline->setBuilder.addTexture(slot.id);
            pipeline->slots.push_back({ kind, index });
        }
#else
        const bool dxil = format == RenderShaderFormat::DXIL;
        for (int round = 0; round < 2; round++)
            for (const gpu::spirv::Binding& binding : module.bindings)
            {
                if ((round == 1) != (binding.kind == gpu::spirv::Kind::Sampler))
                    continue;
                SlotKind kind;
                const uint32_t index = ffxIndex(binding, kind);
                if (index == UINT32_MAX || binding.set != 0)
                {
                    Fail("an FSR shader binds something the backend does not know");
                    return FFX_ERROR_BACKEND_API_ERROR;
                }
                // D3D12: each register class has its own numbers; Vulkan: one number space (the shifts).
                switch (kind)
                {
                case SlotKind::Constant:
                    pipeline->setBuilder.addConstantBuffer(dxil ? binding.binding - ShiftB : binding.binding);
                    break;
                case SlotKind::Sampler:
                    pipeline->setBuilder.addImmutableSampler(
                        dxil ? binding.binding - ShiftS : binding.binding, samplerFor(index));
                    break;
                case SlotKind::Srv:
                    pipeline->setBuilder.addTexture(dxil ? binding.binding - ShiftT : binding.binding);
                    break;
                case SlotKind::SrvBuffer:
                    pipeline->setBuilder.addStructuredBuffer(dxil ? binding.binding - ShiftT : binding.binding);
                    break;
                case SlotKind::UavBuffer:
                    pipeline->setBuilder.addReadWriteStructuredBuffer(
                        dxil ? binding.binding - ShiftU : binding.binding);
                    break;
                default:
                    pipeline->setBuilder.addReadWriteTexture(dxil ? binding.binding - ShiftU : binding.binding);
                    break;
                }
                pipeline->slots.push_back({ kind, index });
            }
#endif
        pipeline->setBuilder.end();
        for (uint32_t i = 0; i < out->srvTextureCount; i++)
            out->srvTextureBindings[i].slotIndex = i;
        for (uint32_t i = 0; i < out->uavTextureCount; i++)
            out->uavTextureBindings[i].slotIndex = i;
        for (uint32_t i = 0; i < out->constCount; i++)
            out->constantBufferBindings[i].slotIndex = i;
        for (uint32_t i = 0; i < out->srvBufferCount; i++)
            out->srvBufferBindings[i].slotIndex = i;
        for (uint32_t i = 0; i < out->uavBufferCount; i++)
            out->uavBufferBindings[i].slotIndex = i;
        pipeline->constants = out->constCount;

        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(pipeline->setBuilder);
        layoutBuilder.end();
        pipeline->layout = layoutBuilder.create(g_context->device.get());
        RenderComputePipelineDesc desc(pipeline->layout.get(), pipeline->shader.get(), module.localSize[0],
            module.localSize[1], module.localSize[2]);
#if defined(ARKEYAN_METAL)
        const RenderSpecConstant alignment(65535, uint32_t(LinearAlignment()));
        desc.specConstants = &alignment;
        desc.specConstantsCount = 1;
#endif
        if (pipeline->shader == nullptr || pipeline->layout == nullptr)
        {
            Fail("an FSR pipeline could not be made");
            return FFX_ERROR_BACKEND_API_ERROR;
        }
        pipeline->pipeline = g_context->device->createComputePipeline(desc);
        if (pipeline->pipeline == nullptr)
        {
            Fail("an FSR pipeline could not be made");
            return FFX_ERROR_BACKEND_API_ERROR;
        }
        for (size_t i = 0; i < 64 && description->name[i] != 0; i++)
            pipeline->name += char(description->name[i] < 128 ? description->name[i] : '?');
        out->pipeline = pipeline.get();
        out->rootSignature = pipeline.get();
        b.pipelines.push_back(std::move(pipeline));
        return FFX_OK;
    }

    FfxErrorCode DestroyPipeline(FfxInterface*, FfxPipelineState* state, FfxUInt32)
    {
        Backend& b = g_backend;
        auto it = std::find_if(b.pipelines.begin(), b.pipelines.end(),
            [&](const std::unique_ptr<Pipeline>& p) { return p.get() == state->pipeline; });
        if (it != b.pipelines.end())
            b.pipelines.erase(it);
        state->pipeline = nullptr;
        state->rootSignature = nullptr;
        return FFX_OK;
    }

    FfxErrorCode ScheduleGpuJob(FfxInterface*, const FfxGpuJobDescription* job)
    {
        g_backend.jobs.push_back(*job);
        return FFX_OK;
    }

    // --- Execution --------------------------------------------------------------------------------------------------

    void Upload(RenderCommandList& list, Resource& resource)
    {
        gpu::Texture& texture = *resource.texture;
        const uint32_t width = texture.desc.width, height = texture.desc.height;
        const uint32_t texel = RenderFormatSize(texture.desc.format);
        const uint32_t rowBytes = width * texel, pitch = uint32_t(AlignUp(rowBytes, 256));
        RenderBuffer* staging = nullptr;
        const uint64_t offset = AllocateStaging(uint64_t(pitch) * height, staging, 512);
        uint8_t* memory = StagingMemory();
        if (staging == nullptr || memory == nullptr)
            return;
        for (uint32_t row = 0; row < height; row++)
        {
            const size_t from = size_t(row) * rowBytes;
            const size_t bytes =
                from < resource.init.size() ? std::min<size_t>(rowBytes, resource.init.size() - from) : 0;
            memset(memory + offset + size_t(row) * pitch, 0, rowBytes);
            if (bytes > 0)
                memcpy(memory + offset + size_t(row) * pitch, resource.init.data() + from, bytes);
        }
        Transition(list, texture, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
        list.copyTextureRegion(RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, 0),
            RenderTextureCopyLocation::PlacedFootprint(
                staging, texture.desc.format, width, height, 1, pitch / texel, offset));
        resource.init.clear();
    }

    void Clear(RenderCommandList& list, Resource& resource, const float value[4], uint64_t frame)
    {
        Backend& b = g_backend;
        gpu::Texture& texture = *resource.texture;
        const bool uint = IsUint(texture.desc.format);
        const uint32_t slot = FrameSlot(frame);
        if (b.clearFrame[slot] != frame)
            b.clearFrame[slot] = frame, b.clearUsed[slot] = 0;
        Transition(list, texture, RenderTextureLayout::GENERAL, RenderBarrierStage::COMPUTE);
        list.barriers(
            RenderBarrierStage::COMPUTE, RenderTextureBarrier(texture.texture.get(), RenderTextureLayout::GENERAL));
        g_context->bound = {};
        list.setComputePipelineLayout(b.clearLayout.get());
        list.setPipeline(uint ? b.clearUint.get() : b.clearFloat.get());
        const uint32_t mips = std::max(texture.desc.mipLevels, 1u);
        for (uint32_t mip = 0; mip < mips; mip++)
        {
            if (b.clearUsed[slot] == b.clearSets[slot].size())
                b.clearSets[slot].push_back(b.clearSetBuilder.create(g_context->device.get()));
            RenderDescriptorSet* set = b.clearSets[slot][b.clearUsed[slot]++].get();
            set->setTexture(0, texture.texture.get(), RenderTextureLayout::GENERAL,
                mip < resource.mips.size() ? resource.mips[mip].get() : nullptr);
            list.setComputeDescriptorSet(set, 0);
            const uint32_t width = std::max(texture.desc.width >> mip, 1u),
                           height = std::max(texture.desc.height >> mip, 1u);
            float constants[24] = {};
            memcpy(constants, value, sizeof(float) * 4);
            constants[16] = float(width);
            constants[17] = float(height);
            list.setComputePushConstants(0, constants);
            list.dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }
    }

    void Compute(RenderCommandList& list, const FfxComputeJobDescription& job, uint64_t frame)
    {
        auto* pipeline = static_cast<Pipeline*>(job.pipeline.pipeline);
        if (pipeline == nullptr)
            return;
        const uint32_t slot = FrameSlot(frame);
        if (pipeline->frame[slot] != frame)
            pipeline->frame[slot] = frame, pipeline->used[slot] = 0;
        if (pipeline->used[slot] == pipeline->instances[slot].size())
        {
            auto instance = std::make_unique<Pipeline::Instance>();
            instance->set = pipeline->setBuilder.create(g_context->device.get());
            for (uint32_t i = 0; i < pipeline->constants; i++)
            {
                instance->constants.push_back(g_context->device->createBuffer(
                    RenderBufferDesc::UploadBuffer(ConstantSize, RenderBufferFlag::CONSTANT)));
                instance->mapped.push_back(static_cast<uint8_t*>(instance->constants.back()->map()));
            }
            pipeline->instances[slot].push_back(std::move(instance));
        }
        Pipeline::Instance& instance = *pipeline->instances[slot][pipeline->used[slot]++];

        // Layouts first: what the pass writes in GENERAL (also where it reads it), the rest it reads in SHADER_READ;
        // then a barrier on what is written, so each pass sees the one before.
        std::vector<RenderTextureBarrier> written;
        std::vector<const gpu::Texture*> writes;
        for (uint32_t i = 0; i < job.pipeline.uavTextureCount; i++)
            if (Resource* r = Find(job.uavTextures[i].resource); r != nullptr && r->texture != nullptr)
            {
                Transition(list, *r->texture, RenderTextureLayout::GENERAL, RenderBarrierStage::COMPUTE);
                written.push_back(RenderTextureBarrier(r->texture->texture.get(), RenderTextureLayout::GENERAL));
                writes.push_back(r->texture);
            }
        for (uint32_t i = 0; i < job.pipeline.srvTextureCount; i++)
            if (Resource* r = Find(job.srvTextures[i].resource);
                r != nullptr && r->texture != nullptr &&
                std::find(writes.begin(), writes.end(), r->texture) == writes.end())
                Transition(list, *r->texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        std::vector<RenderBufferBarrier> buffers;
        for (uint32_t i = 0; i < job.pipeline.uavBufferCount; i++)
            if (Resource* r = Find(job.uavBuffers[i].resource); r != nullptr && r->buffer != nullptr)
                buffers.push_back(
                    RenderBufferBarrier(r->buffer.get(), RenderBufferAccess::READ | RenderBufferAccess::WRITE));
        for (uint32_t i = 0; i < job.pipeline.srvBufferCount; i++)
            if (Resource* r = Find(job.srvBuffers[i].resource); r != nullptr && r->buffer != nullptr)
                buffers.push_back(RenderBufferBarrier(r->buffer.get(), RenderBufferAccess::READ));
        if (!written.empty() || !buffers.empty())
            list.barriers(RenderBarrierStage::COMPUTE, buffers.data(), uint32_t(buffers.size()), written.data(),
                uint32_t(written.size()));

        for (uint32_t index = 0; index < pipeline->slots.size(); index++)
        {
            const Slot& s = pipeline->slots[index];
            switch (s.kind)
            {
            case SlotKind::Constant:
            {
                const FfxConstantBuffer& cb = job.cbs[s.index];
                if (instance.mapped[s.index] != nullptr && cb.data != nullptr)
                    memcpy(instance.mapped[s.index], cb.data, std::min<size_t>(cb.num32BitEntries * 4, ConstantSize));
                instance.set->setBuffer(index, instance.constants[s.index].get(), ConstantSize);
                break;
            }
            case SlotKind::Srv:
                if (Resource* r = Find(job.srvTextures[s.index].resource); r != nullptr && r->texture != nullptr)
                    instance.set->setTexture(index, r->texture->texture.get(), r->texture->layout);
                break;
            case SlotKind::Uav:
                if (Resource* r = Find(job.uavTextures[s.index].resource); r != nullptr && r->texture != nullptr)
                {
                    const uint32_t mip = job.uavTextures[s.index].mip;
                    static bool warned = false;
                    if (!(r->texture->desc.usage & gpu::TextureUsage::Storage) && !warned && (warned = true))
                        fprintf(stderr, "[fsr] %s writes a texture made without storage usage (%ux%u, format %d)\n",
                            pipeline->name.c_str(), r->texture->desc.width, r->texture->desc.height,
                            int(r->texture->desc.format));
                    // A pyramid's UAVs past its last mip (frame interpolation binds 12 on a smaller pyramid; its passes
                    // write only the mips there are) take the last one: a storage view of every mip is invalid on Vulkan.
                    instance.set->setTexture(index, r->texture->texture.get(), RenderTextureLayout::GENERAL,
                        r->mips.empty() ? nullptr : r->mips[std::min<size_t>(mip, r->mips.size() - 1)].get());
                }
                break;
            case SlotKind::Atomics:
                if (Resource* r = Find(job.uavTextures[s.index].resource); r != nullptr)
                {
                    RenderBuffer* atomics = r->atomics != nullptr ? r->atomics.get() : r->borrowedAtomics;
                    if (atomics != nullptr)
                        instance.set->setBuffer(index, atomics);
                }
                break;
            case SlotKind::SrvBuffer:
            case SlotKind::UavBuffer:
            {
                const FfxResourceInternal internal =
                    s.kind == SlotKind::SrvBuffer ? job.srvBuffers[s.index].resource : job.uavBuffers[s.index].resource;
                if (Resource* r = Find(internal); r != nullptr && r->buffer != nullptr)
                {
                    const RenderBufferStructuredView view(sizeof(uint32_t));
                    instance.set->setBuffer(index, r->buffer.get(), r->desc.width, &view);
                }
                break;
            }
            case SlotKind::Sampler: break;
            }
        }
        g_context->bound = {}; // a compute pipeline replaces the draws' pipeline state on D3D12
        list.setComputePipelineLayout(pipeline->layout.get());
        list.setPipeline(pipeline->pipeline.get());
        list.setComputeDescriptorSet(instance.set.get(), 0);
        list.dispatch(job.dimensions[0], job.dimensions[1], job.dimensions[2]);
    }

    FfxErrorCode ExecuteGpuJobs(FfxInterface*, FfxCommandList commandList, FfxUInt32)
    {
        Backend& b = g_backend;
        auto& list = *static_cast<RenderCommandList*>(commandList);
        const uint64_t frame = g_context->frames;
        for (const auto& resource : b.resources)
            if (resource != nullptr && !resource->init.empty() && resource->buffer != nullptr)
            {
                // Zeroes through the staging memory.
                RenderBuffer* staging = nullptr;
                const uint64_t offset = AllocateStaging(resource->init.size(), staging, 256);
                if (staging != nullptr && StagingMemory() != nullptr)
                {
                    memcpy(StagingMemory() + offset, resource->init.data(), resource->init.size());
                    list.barriers(RenderBarrierStage::COPY,
                        RenderBufferBarrier(resource->buffer.get(), RenderBufferAccess::WRITE));
                    list.copyBufferRegion(resource->buffer->at(0), staging->at(offset), resource->init.size());
                }
                resource->init.clear();
            }
            else if (resource != nullptr && !resource->init.empty() && resource->texture != nullptr)
            {
                if (resource->atomics != nullptr)
                {
                    const float zero[4] = {};
                    Clear(list, *resource, zero, frame);
                    resource->init.clear();
                }
                else
                    Upload(list, *resource);
            }
        for (const FfxGpuJobDescription& job : b.jobs)
        {
            switch (job.jobType)
            {
            case FFX_GPU_JOB_CLEAR_FLOAT:
                if (Resource* r = Find(job.clearJobDescriptor.target); r != nullptr && r->texture != nullptr)
                    Clear(list, *r, job.clearJobDescriptor.color, frame);
                break;
            case FFX_GPU_JOB_COPY:
            {
                Resource* from = Find(job.copyJobDescriptor.src);
                Resource* to = Find(job.copyJobDescriptor.dst);
                if (from != nullptr && to != nullptr && from->texture != nullptr && to->texture != nullptr)
                {
                    Transition(list, *from->texture, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
                    Transition(list, *to->texture, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
                    list.copyTexture(to->texture->texture.get(), from->texture->texture.get());
                }
                break;
            }
            case FFX_GPU_JOB_COMPUTE: Compute(list, job.computeJobDescriptor, frame); break;
            default: // barriers and discards: the transitions above do what is needed
                break;
            }
        }
        b.jobs.clear();
        b.constantUsed = 0;
        return FFX_OK;
    }

    bool CreateClears()
    {
        Backend& b = g_backend;
        if (b.clearFloat != nullptr)
            return true;
        const RenderShaderFormat format = ShaderFormat();
#if defined(ARKEYAN_METAL)
        b.clearFloatShader = g_context->device->createShader(
            g_fsr_inputs_metallib, sizeof(g_fsr_inputs_metallib), "ClearFloatCS", format);
        b.clearUintShader = g_context->device->createShader(
            g_fsr_inputs_metallib, sizeof(g_fsr_inputs_metallib), "ClearUintCS", format);
#else
#if defined(_WIN32)
        if (format == RenderShaderFormat::DXIL)
        {
            b.clearFloatShader = g_context->device->createShader(
                g_fsr_ClearFloatCS_dxil, sizeof(g_fsr_ClearFloatCS_dxil), "ClearFloatCS", format);
            b.clearUintShader = g_context->device->createShader(
                g_fsr_ClearUintCS_dxil, sizeof(g_fsr_ClearUintCS_dxil), "ClearUintCS", format);
        }
#endif
#if !defined(PLUME_D3D12_COREWINDOW)
        if (format == RenderShaderFormat::SPIRV)
        {
            const std::vector<uint32_t> clearFloat = VulkanSpirv(g_fsr_ClearFloatCS_spirv);
            const std::vector<uint32_t> clearUint = VulkanSpirv(g_fsr_ClearUintCS_spirv);
            b.clearFloatShader =
                g_context->device->createShader(clearFloat.data(), clearFloat.size() * 4, "ClearFloatCS", format);
            b.clearUintShader =
                g_context->device->createShader(clearUint.data(), clearUint.size() * 4, "ClearUintCS", format);
        }
#endif
#endif
        if (b.clearFloatShader == nullptr || b.clearUintShader == nullptr)
            return false;
        b.clearSetBuilder.begin();
        b.clearSetBuilder.addReadWriteTexture(0);
        b.clearSetBuilder.end();
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(b.clearSetBuilder);
        layoutBuilder.addPushConstant(0, 1, 96, RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        b.clearLayout = layoutBuilder.create(g_context->device.get());
        b.clearFloat = g_context->device->createComputePipeline(
            RenderComputePipelineDesc(b.clearLayout.get(), b.clearFloatShader.get(), 8, 8, 1));
        b.clearUint = g_context->device->createComputePipeline(
            RenderComputePipelineDesc(b.clearLayout.get(), b.clearUintShader.get(), 8, 8, 1));
        return b.clearFloat != nullptr && b.clearUint != nullptr;
    }
}

void gpu::fsr::MakeInterface(FfxInterface& backend)
{
    backend = {};
    if (!CreateClears())
        Fail("the clear shaders could not be made");
    backend.device = g_context->device.get(); // FFX only checks it is set
    backend.fpGetSDKVersion = GetSdkVersion;
    backend.fpGetEffectGpuMemoryUsage = GetEffectGpuMemoryUsage;
    backend.fpCreateBackendContext = CreateBackendContext;
    backend.fpGetDeviceCapabilities = GetDeviceCapabilities;
    backend.fpDestroyBackendContext = DestroyBackendContext;
    backend.fpCreateResource = CreateResource;
    backend.fpRegisterResource = RegisterResource;
    backend.fpGetResource = GetResource;
    backend.fpUnregisterResources = UnregisterResources;
    backend.fpGetResourceDescription = GetResourceDescription;
    backend.fpDestroyResource = DestroyResource;
    backend.fpStageConstantBufferDataFunc = StageConstantBufferData;
    backend.fpCreatePipeline = CreatePipeline;
    backend.fpDestroyPipeline = DestroyPipeline;
    backend.fpScheduleGpuJob = ScheduleGpuJob;
    backend.fpExecuteGpuJobs = ExecuteGpuJobs;
}

FfxResource gpu::fsr::External(gpu::Texture* texture, bool writable)
{
    FfxResource resource{};
    resource.resource = texture;
    resource.description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = FromRender(texture->desc.format);
    resource.description.width = texture->desc.width;
    resource.description.height = texture->desc.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.usage = writable ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
    resource.state = writable ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ;
    return resource;
}

gpu::Texture* gpu::fsr::CreateShared(const FfxCreateResourceDescription& create)
{
    FfxResourceInternal internal{};
    if (CreateResource(nullptr, &create, 0, &internal) != FFX_OK)
        return nullptr;
    const Resource* resource = Find(internal);
    return resource != nullptr ? resource->texture : nullptr;
}

FfxSurfaceFormat gpu::fsr::SurfaceFormat(plume::RenderFormat format) { return FromRender(format); }

void gpu::fsr::DestroyShared(gpu::Texture* texture)
{
    if (texture == nullptr)
        return;
    for (auto& resource : g_backend.resources)
        if (resource != nullptr && resource->owned && resource->texture == texture)
            Free(*resource);
}

bool gpu::fsr::ShadersAvailable()
{
    const RenderShaderFormat format = ShaderFormat();
#if defined(ARKEYAN_METAL)
    return format == RenderShaderFormat::METAL;
#elif defined(_WIN32)
    return format == RenderShaderFormat::DXIL || format == RenderShaderFormat::SPIRV;
#else
    return format == RenderShaderFormat::SPIRV;
#endif
}

void gpu::fsr::ReleaseDevice()
{
    ReleaseAll();
    Backend& b = g_backend;
    for (uint32_t i = 0; i < Pipeline::Frames; i++)
        b.clearSets[i].clear(), b.clearUsed[i] = 0, b.clearFrame[i] = 0;
    b.clearFloat.reset();
    b.clearUint.reset();
    b.clearLayout.reset();
    b.clearFloatShader.reset();
    b.clearUintShader.reset();
}

void gpu::fsr::ReleaseAll()
{
    Backend& b = g_backend;
    for (auto& resource : b.resources)
        if (resource != nullptr)
            Free(*resource);
    b.resources.clear();
    b.registered.clear();
    b.pipelines.clear();
    b.jobs.clear();
    b.constantUsed = 0;
    b.failed = false;
}
