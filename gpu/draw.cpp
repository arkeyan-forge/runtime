#include "gpu/draw.h"
#include "gpu/buffer_cache.h"
#include "gpu/context.h"
#include "host/collection/datadir.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../host/env.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#if defined(SSA_SHADER_CACHE)
#include "gpu/shader_cache.h"
#include <zstd.h>
#if !defined(SSA_METAL)
#include <smolv.h>
#endif
#endif
#if defined(_WIN32)
#include "shaders/point_sprite_PointSpriteGS_dxil.h"
#endif
#if defined(SSA_PIPELINE_BASELINE)
#include "shaders/pipeline_baseline.h" // g_pipelineBaseline: games/<game>/pipelines/baseline.zst
#endif

using namespace plume;
using namespace gpu::detail;

struct gpu::XenosShader
{
    // One shader; on D3D12, which has no specialization constants, one per combination of the bits of
    // specConstantsMask, in increasing order (VariantOf).
    std::vector<std::unique_ptr<RenderShader>> variants;
    uint32_t specConstantsMask;
    uint64_t hash; // of the guest shader container
    bool pointSize; // D3D12: a vertex shader that outputs a point size (points are drawn through PointSpriteGS)
};

namespace
{
    // Bindless heaps, laid out as XenosRecomp's shaders expect: descriptor set 0-2 = 2D, 3D and cube textures, set 3 =
    // samplers ([[buffer(0..3)]] on Metal, register spaces 0-3 on D3D12); push constants = three device addresses
    // ([[buffer(8)]]). D3D12 has no device addresses: the constants are root CBVs b0-b2 in space 4 and the pulled
    // vertices a root SRV t0 in space 5 (RootDescriptor), as XenosRecomp's HLSL declares them.
    constexpr uint32_t Texture2DHeap = 0;
    constexpr uint32_t Texture3DHeap = 1;
    constexpr uint32_t TextureCubeHeap = 2;
    constexpr uint32_t SamplerHeap = 3;
    constexpr uint32_t TextureHeapSize = 16384;
    constexpr uint32_t SamplerHeapSize = 1024;
    constexpr uint64_t ConstantAlignment = 256;
    // Draws recorded before the frame so far is sent to the GPU (gpu::Draw).
    constexpr uint32_t FlushDraws = 128;
    constexpr uint32_t MaxFlushDraws = 1024;

    namespace RootDescriptor
    {
        enum : uint32_t
        {
            VertexConstants,
            PixelConstants,
            SharedConstants,
            PulledVertices
        };
    }

    struct PushConstants
    {
        uint64_t vertexShaderConstants;
        uint64_t pixelShaderConstants;
        uint64_t sharedConstants;
    };

    // XenosRecomp's per-draw shared constants (shader_common.h).
    struct SharedConstants
    {
        uint32_t texture2D[gpu::MaxSamplers];
        uint32_t texture3D[gpu::MaxSamplers];
        uint32_t textureCube[gpu::MaxSamplers];
        uint32_t sampler[gpu::MaxSamplers];
        uint32_t booleans;
        uint32_t swappedTexcoords;
        float halfPixelOffset[2];
        float alphaThreshold;
        uint32_t paramGen;
        float positionScale[2];
        uint64_t pulledVertices; // address of the float4s
        uint32_t pulledCount;
        float resolutionScale;   // point sizes and pixel positions (vPos) are in guest pixels: scaled by it
        float pointScale[2];     // D3D12 point sprites (shaders/point_sprite.hlsl): 1 / the viewport's size
        uint32_t
            scaledTextures; // DrawDesc::scaledTextures: unnormalized fetches of these registers scale by resolutionScale
        float padding; // the HLSL constant buffer reads whole float4s
        uint32_t vertexTexture2D
            [gpu::MaxVertexSamplers]; // a vertex shader's descriptor indices (XenosRecomp VERTEX_*_OFFSET)
        uint32_t vertexSampler[gpu::MaxVertexSamplers];
    };
    static_assert(offsetof(SharedConstants, booleans) == 256 && offsetof(SharedConstants, alphaThreshold) == 272 &&
                  offsetof(SharedConstants, positionScale) == 280 && offsetof(SharedConstants, pulledVertices) == 288 &&
                  offsetof(SharedConstants, pulledCount) == 296 && offsetof(SharedConstants, resolutionScale) == 300 &&
                  offsetof(SharedConstants, pointScale) == 304 && offsetof(SharedConstants, scaledTextures) == 312 &&
                  offsetof(SharedConstants, vertexTexture2D) == 320 &&
                  offsetof(SharedConstants, vertexSampler) == 336 && sizeof(SharedConstants) == 352);

    struct HeapSlots
    {
        uint32_t next = 1; // slot 0 holds a placeholder
        std::vector<uint32_t> free;

        uint32_t Allocate()
        {
            if (!free.empty())
            {
                uint32_t slot = free.back();
                free.pop_back();
                return slot;
            }
            return next++;
        }
    };

    // A pipeline is listed before it is built, so that a draw needing one a precompile thread is building waits for it.
    struct PipelineEntry
    {
        std::unique_ptr<RenderPipeline> pipeline; // nullptr if it could not be built
        bool ready = false;
    };

    struct DrawContext
    {
        std::unique_ptr<RenderDescriptorSet> heaps[4];
        HeapSlots textureSlots[3];
        std::unique_ptr<RenderPipelineLayout> layout;
        bool dxil = false; // D3D12: root descriptors instead of push constants, shader variants, point sprites
        std::unique_ptr<RenderShader> pointSpriteGS;
        gpu::Texture* placeholders[3] = {}; // 1x1 black 2D, 3D and cube textures for unused slots

        std::vector<std::unique_ptr<RenderSampler>> samplers;
        std::vector<gpu::SamplerState> samplerStates; // by slot
        std::unordered_map<uint64_t, uint32_t> samplerSlots; // by hash of the SamplerState bytes

        std::vector<uint8_t> shaderCode; // decompressed shader cache of the running backend
        // Shaders and pipelines are also built by the precompile threads (PrecompilePipelines), which never take g_mutex.
        std::mutex shaderMutex;
        std::unordered_map<uint64_t, std::unique_ptr<gpu::XenosShader>> shaders;
        std::mutex pipelineMutex;
        std::condition_variable pipelineReady;
        std::unordered_map<uint64_t, PipelineEntry> pipelines; // by XXH3 of the PipelineKey
        std::unordered_map<uint64_t, std::unique_ptr<RenderFramebuffer>> framebuffers;
        // Depth resolves: one descriptor set per source depth target (the GPU reads them when the frame runs).
        std::unordered_map<const gpu::Texture*, std::unique_ptr<RenderDescriptorSet>> depthSets;
    };

    std::unique_ptr<DrawContext> g_draw;

    uint64_t HashBytes(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull)
    {
        auto bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++)
            hash = (hash ^ bytes[i]) * 0x100000001B3ull;
        return hash;
    }

    template <typename T>
    uint64_t HashValue(const T& value, uint64_t hash)
    {
        return HashBytes(&value, sizeof(value), hash);
    }

    uint32_t HeapOf(const gpu::Texture& texture)
    {
        if (texture.desc.cube)
            return TextureCubeHeap;
        return texture.desc.depth > 1 ? Texture3DHeap : Texture2DHeap;
    }

    uint32_t TextureSlot(gpu::Texture* texture, const RenderComponentMapping& swizzle)
    {
        uint32_t heap = HeapOf(*texture);
        uint32_t mapping =
            uint32_t(swizzle.r) | uint32_t(swizzle.g) << 8 | uint32_t(swizzle.b) << 16 | uint32_t(swizzle.a) << 24;
        if (mapping == 0)
        {
            if (texture->heapIndex == 0)
            {
                texture->heapIndex = g_draw->textureSlots[heap].Allocate();
                g_draw->heaps[heap]->setTexture(
                    texture->heapIndex, texture->texture.get(), RenderTextureLayout::SHADER_READ);
            }
            return texture->heapIndex;
        }

        for (const gpu::Texture::SwizzledView& view : texture->swizzledViews)
            if (view.mapping == mapping)
                return view.heapIndex;

        RenderTextureViewDesc desc;
        desc.format = texture->desc.format;
        desc.dimension = heap == TextureCubeHeap ? RenderTextureViewDimension::TEXTURE_CUBE
                         : heap == Texture3DHeap ? RenderTextureViewDimension::TEXTURE_3D
                                                 : RenderTextureViewDimension::TEXTURE_2D;
        desc.componentMapping = swizzle;
        gpu::Texture::SwizzledView view{ mapping, texture->texture->createTextureView(desc),
            g_draw->textureSlots[heap].Allocate() };
        g_draw->heaps[heap]->setTexture(
            view.heapIndex, texture->texture.get(), RenderTextureLayout::SHADER_READ, view.view.get());
        texture->swizzledViews.push_back(std::move(view));
        return texture->swizzledViews.back().heapIndex;
    }

    uint32_t SamplerSlot(const gpu::SamplerState& state)
    {
        uint64_t key = XXH3_64bits(&state, sizeof(state));
        auto it = g_draw->samplerSlots.find(key);
        if (it != g_draw->samplerSlots.end() && memcmp(&g_draw->samplerStates[it->second], &state, sizeof(state)) == 0)
            return it->second;

        RenderSamplerDesc desc;
        desc.minFilter = state.minFilter;
        desc.magFilter = state.magFilter;
        desc.mipmapMode = state.mipFilter;
        desc.addressU = state.addressU;
        desc.addressV = state.addressV;
        desc.addressW = state.addressW;
        desc.borderColor = state.borderColor;
        desc.anisotropyEnabled = state.maxAnisotropy > 1;
        desc.maxAnisotropy = std::max(state.maxAnisotropy, 1u);
        desc.mipLODBias = state.mipLodBias;
        desc.minLOD = state.minLod;
        desc.maxLOD = state.maxLod;

        uint32_t slot = uint32_t(g_draw->samplers.size());
        if (slot >= SamplerHeapSize)
            return 0;
        g_draw->samplers.push_back(g_context->device->createSampler(desc));
        g_draw->samplerStates.push_back(state);
        g_draw->heaps[SamplerHeap]->setSampler(slot, g_draw->samplers.back().get());
        g_draw->samplerSlots[key] = slot;
        return slot;
    }

    void LoadPipelineList();
    void StartPrecompiling();
    std::filesystem::path PipelineListPath();

    void CreateDrawContext()
    {
        g_draw = std::make_unique<DrawContext>();
        RenderDevice& device = *g_context->device;
        g_draw->dxil = g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::DXIL;

        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin(false, true);
        for (uint32_t heap = 0; heap < 4; heap++)
        {
            RenderDescriptorSetBuilder setBuilder;
            setBuilder.begin();
            if (heap == SamplerHeap)
                setBuilder.addSampler(0, SamplerHeapSize);
            else
                setBuilder.addTexture(0, TextureHeapSize);
            setBuilder.end(true, heap == SamplerHeap ? SamplerHeapSize : TextureHeapSize);
            g_draw->heaps[heap] = setBuilder.create(&device);
            layoutBuilder.addDescriptorSet(setBuilder);
        }
        if (g_draw->dxil)
        {
            // In RootDescriptor order.
            layoutBuilder.addRootDescriptor(0, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
            layoutBuilder.addRootDescriptor(1, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
            layoutBuilder.addRootDescriptor(2, 4, RenderRootDescriptorType::CONSTANT_BUFFER);
            layoutBuilder.addRootDescriptor(0, 5, RenderRootDescriptorType::SHADER_RESOURCE);
        }
        else
        {
            layoutBuilder.addPushConstant(
                0, 4, sizeof(PushConstants), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
        }
        layoutBuilder.end();
        g_draw->layout = layoutBuilder.create(&device);
#if defined(_WIN32)
        if (g_draw->dxil)
            g_draw->pointSpriteGS = device.createShader(g_pointSpritePointSpriteGSDxil,
                sizeof(g_pointSpritePointSpriteGSDxil), "PointSpriteGS", RenderShaderFormat::DXIL);
#endif

        // Placeholders for slots no texture is bound to, and the default sampler at sampler slot 0.
        gpu::TextureDesc desc{ .width = 1, .height = 1, .format = gpu::Format::R8G8B8A8_UNORM };
        g_draw->placeholders[Texture2DHeap] = gpu::CreateTexture(desc);
        g_draw->placeholders[TextureCubeHeap] =
            gpu::CreateTexture({ .width = 1, .height = 1, .arraySize = 6, .format = desc.format, .cube = true });
        g_draw->placeholders[Texture3DHeap] =
            gpu::CreateTexture({ .width = 1, .height = 1, .depth = 2, .format = desc.format });
        const uint32_t black = 0;
        gpu::UploadTexture(g_draw->placeholders[Texture2DHeap], &black, 4);
        for (uint32_t face = 0; face < 6; face++)
            gpu::UploadTexture(g_draw->placeholders[TextureCubeHeap], &black, 4, 0, face);
        for (uint32_t heap = 0; heap < 3; heap++)
            g_draw->heaps[heap]->setTexture(
                0, g_draw->placeholders[heap]->texture.get(), RenderTextureLayout::SHADER_READ);
        SamplerSlot(gpu::SamplerState{});

#if defined(SSA_SHADER_CACHE)
#if defined(SSA_METAL)
        const uint8_t* compressed = g_compressedAirCache;
        size_t compressedSize = g_airCacheCompressedSize, size = g_airCacheDecompressedSize;
#else
        const uint8_t* compressed = g_compressedSpirvCache;
        size_t compressedSize = g_spirvCacheCompressedSize, size = g_spirvCacheDecompressedSize;
#if defined(_WIN32)
        if (g_draw->dxil)
        {
            compressed = g_compressedDxilCache;
            compressedSize = g_dxilCacheCompressedSize;
            size = g_dxilCacheDecompressedSize;
        }
#endif
#endif
        g_draw->shaderCode.resize(size);
        if (ZSTD_isError(ZSTD_decompress(g_draw->shaderCode.data(), size, compressed, compressedSize)))
        {
            fprintf(stderr, "[gpu] the shader cache does not decompress\n");
            g_draw->shaderCode.clear();
        }
        fprintf(stderr, "[gpu] shader cache: %zu shaders\n", size_t(g_shaderCacheEntryCount));
        if (!g_draw->shaderCode.empty())
        {
#if defined(_WIN32)
            // D3D12: pipelines built by an earlier run, for this shader cache (pipelines.d3d12 next to pipelines.bin).
            std::filesystem::path list = PipelineListPath();
            Step("shader cache loaded");
            if (g_draw->dxil && !list.empty())
                D3D12OpenPipelineLibrary(*g_context->device, list.parent_path() / "pipelines.d3d12",
                    XXH3_64bits(g_draw->shaderCode.data(), g_draw->shaderCode.size()));
            Step("pipeline library opened");
#endif
            LoadPipelineList();
            StartPrecompiling();
            Step("pipeline precompiling started");
        }
#else
        fprintf(stderr,
            "[gpu] built without a shader cache (tools/shaders/build_cache.sh): the game's draws are skipped\n");
#endif
    }

    // Created once, by whichever thread comes first (a producer looking up shaders, or the GPU thread).
    std::atomic<bool> g_drawCreated;
    std::mutex g_drawCreateMutex;

    DrawContext& Draws()
    {
        if (!g_drawCreated.load(std::memory_order_acquire))
        {
            std::lock_guard lock(g_drawCreateMutex);
            if (g_draw == nullptr)
                CreateDrawContext();
            g_drawCreated.store(true, std::memory_order_release);
        }
        return *g_draw;
    }

    RenderFramebuffer* Framebuffer(gpu::Texture* const* colors, uint32_t colorCount, gpu::Texture* depth)
    {
        uint64_t key = HashBytes(colors, sizeof(*colors) * colorCount, HashValue(depth, colorCount));
        auto& framebuffer = g_draw->framebuffers[key];
        if (framebuffer == nullptr)
        {
            const RenderTexture* attachments[gpu::MaxRenderTargets];
            for (uint32_t i = 0; i < colorCount; i++)
                attachments[i] = colors[i]->texture.get();
            RenderFramebufferDesc desc(attachments, colorCount, depth != nullptr ? depth->texture.get() : nullptr);
            framebuffer = g_context->device->createFramebuffer(desc);
        }
        return framebuffer.get();
    }

    RenderInputSlot InputSlot(const gpu::DrawDesc& draw, uint32_t stream)
    {
        return RenderInputSlot(stream, draw.streams[stream].stride,
            draw.streams[stream].perInstance ? RenderInputSlotClassification::PER_INSTANCE_DATA
                                             : RenderInputSlotClassification::PER_VERTEX_DATA);
    }

    // The translated shader for a guest shader container hash, built on first use.
    const gpu::XenosShader* Shader(uint64_t hash)
    {
        std::lock_guard lock(g_draw->shaderMutex);
        auto [it, inserted] = g_draw->shaders.try_emplace(hash);
        std::unique_ptr<gpu::XenosShader>& shader = it->second;
        if (!inserted)
            return shader.get();
#if defined(SSA_SHADER_CACHE)
        auto end = g_shaderCacheEntries + g_shaderCacheEntryCount;
        auto entry = std::lower_bound(
            g_shaderCacheEntries, end, hash, [](const ShaderCacheEntry& e, uint64_t h) { return e.hash < h; });
        if (entry != end && entry->hash == hash && !g_draw->shaderCode.empty())
        {
            RenderShaderFormat format = g_context->renderInterface->getCapabilities().shaderFormat;
            auto created =
                std::make_unique<gpu::XenosShader>(gpu::XenosShader{ {}, entry->specConstantsMask, hash, false });
#if defined(SSA_METAL)
            created->variants.push_back(g_context->device->createShader(
                g_draw->shaderCode.data() + entry->airOffset, entry->airSize, "shaderMain", format));
#else
            if (g_draw->dxil)
            {
                // Flags, variant count, the variants' sizes, the variants (shader_cache.h).
                const uint8_t* data = g_draw->shaderCode.data() + entry->dxilOffset;
                uint32_t flags = 0, count = 0;
                if (entry->dxilSize >= 8)
                {
                    memcpy(&flags, data, 4);
                    memcpy(&count, data + 4, 4);
                }
                created->pointSize = flags & 1;
                size_t offset = 8 + size_t(count) * 4;
                for (uint32_t i = 0; i < count && offset <= entry->dxilSize; i++)
                {
                    uint32_t size;
                    memcpy(&size, data + 8 + i * 4, 4);
                    if (offset + size > entry->dxilSize)
                        break;
                    created->variants.push_back(
                        g_context->device->createShader(data + offset, size, "shaderMain", format));
                    offset += size;
                }
                if (created->variants.size() != count || count == 0)
                {
                    fprintf(
                        stderr, "[gpu] shader %016llX: no DXIL in the shader cache entry\n", (unsigned long long)hash);
                    return nullptr;
                }
            }
            else
            {
                // XenosRecomp stores SPIR-V as smol-v.
                const uint8_t* smolv = g_draw->shaderCode.data() + entry->spirvOffset;
                std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smolv, entry->spirvSize));
                if (spirv.empty() || !smolv::Decode(smolv, entry->spirvSize, spirv.data(), spirv.size()))
                {
                    fprintf(stderr, "[gpu] shader %016llX: the shader cache entry does not decode\n",
                        (unsigned long long)hash);
                    return nullptr;
                }
                created->variants.push_back(
                    g_context->device->createShader(spirv.data(), spirv.size(), "shaderMain", format));
            }
#endif
            if (std::all_of(created->variants.begin(), created->variants.end(),
                    [](const auto& variant) { return variant != nullptr; }))
                shader = std::move(created);
        }
#endif
        return shader.get();
    }

    // The shader for these specialization constants: on D3D12, the variant compiled for the bits of the mask they set.
    RenderShader* VariantOf(const gpu::XenosShader& shader, uint32_t specConstants)
    {
        uint32_t value = specConstants & shader.specConstantsMask;
        size_t index = 0;
        if (shader.variants.size() > 1)
            for (uint32_t lower = 0; lower < value; lower++)
                index += (lower & ~shader.specConstantsMask) == 0;
        return index < shader.variants.size() ? shader.variants[index].get() : nullptr;
    }

    // Everything a graphics pipeline is built from, with the shaders by hash: the key of the pipeline map and a record
    // of the pipeline list. KeyOf zero-fills it and copies field by field, so padding never reaches the hash.
    struct PipelineKey
    {
        uint64_t vertexShader;
        uint64_t pixelShader; // 0: depth only
        uint32_t specConstants;
        RenderPrimitiveTopology topology;
        gpu::RasterState raster;
        uint32_t colorTargetCount;
        gpu::Format colorFormats[gpu::MaxRenderTargets];
        gpu::BlendState blend[gpu::MaxRenderTargets];
        gpu::Format depthFormat; // UNKNOWN: no depth target
        gpu::DepthStencilState depthStencil;
        uint32_t inputCount;
        gpu::VertexInput inputs[gpu::MaxVertexElements];
        uint32_t streamCount;
        uint32_t strides[gpu::MaxVertexStreams];
        uint32_t perInstance; // bit per stream
    };
    static_assert(std::is_trivially_copyable_v<PipelineKey>);

    PipelineKey KeyOf(const gpu::DrawDesc& draw)
    {
        PipelineKey key;
        memset(&key, 0, sizeof(key));
        key.vertexShader = draw.vertexShader->hash;
        key.pixelShader = draw.pixelShader != nullptr ? draw.pixelShader->hash : 0;
        key.specConstants = draw.specConstants;
        key.topology = draw.topology;
        key.raster.cullMode = draw.raster.cullMode;
        key.raster.frontFace = draw.raster.frontFace;
        key.raster.depthBias = draw.raster.depthBias;
        key.raster.slopeScaledDepthBias = draw.raster.slopeScaledDepthBias;
        key.colorTargetCount = draw.colorTargetCount;
        for (uint32_t i = 0; i < draw.colorTargetCount; i++)
        {
            const gpu::BlendState& from = draw.blend[i];
            gpu::BlendState& to = key.blend[i];
            key.colorFormats[i] = draw.colorTargets[i]->desc.format;
            to.enabled = from.enabled;
            to.srcColor = from.srcColor;
            to.dstColor = from.dstColor;
            to.colorOp = from.colorOp;
            to.srcAlpha = from.srcAlpha;
            to.dstAlpha = from.dstAlpha;
            to.alphaOp = from.alphaOp;
            to.writeMask = from.writeMask;
        }
        key.depthFormat = draw.depthTarget != nullptr ? draw.depthTarget->desc.format : gpu::Format::UNKNOWN;
        if (draw.depthTarget != nullptr)
        {
            const gpu::DepthStencilState& from = draw.depthStencil;
            gpu::DepthStencilState& to = key.depthStencil;
            to.depthEnabled = from.depthEnabled;
            to.depthWrite = from.depthWrite;
            to.depthFunction = from.depthFunction;
            to.stencilEnabled = from.stencilEnabled;
            to.stencilReadMask = from.stencilReadMask;
            to.stencilWriteMask = from.stencilWriteMask;
            to.stencilReference = from.stencilReference;
            to.front = from.front;
            to.back = from.back;
        }
        key.inputCount = draw.inputCount;
        for (uint32_t i = 0; i < draw.inputCount; i++)
            key.inputs[i] = draw.inputs[i];
        key.streamCount = draw.streamCount;
        for (uint32_t i = 0; i < draw.streamCount; i++)
        {
            key.strides[i] = draw.streams[i].stride;
            key.perInstance |= uint32_t(draw.streams[i].perInstance) << i;
        }
        return key;
    }

    // Called by the render thread and the precompile threads: takes no g_mutex.
    std::unique_ptr<RenderPipeline> CreatePipeline(const PipelineKey& key)
    {
        const gpu::XenosShader* vertexShader = Shader(key.vertexShader);
        const gpu::XenosShader* pixelShader = key.pixelShader != 0 ? Shader(key.pixelShader) : nullptr;
        if (vertexShader == nullptr || (key.pixelShader != 0 && pixelShader == nullptr))
            return nullptr;

        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = g_draw->layout.get();
        desc.vertexShader = VariantOf(*vertexShader, key.specConstants);
        desc.pixelShader =
            pixelShader != nullptr ? VariantOf(*pixelShader, key.specConstants) : nullptr; // none: depth only
        desc.primitiveTopology = key.topology;
        desc.cullMode = key.raster.cullMode;
        desc.frontFace = key.raster.frontFace;
        if (g_draw->dxil && key.topology == RenderPrimitiveTopology::POINT_LIST && vertexShader->pointSize)
        {
            // Point sprites: squares the size the vertex shader gives, never culled, front facing (as points are).
            desc.geometryShader = g_draw->pointSpriteGS.get();
            desc.cullMode = RenderCullMode::NONE;
            desc.frontFace = RenderFrontFace::CLOCKWISE;
        }
        desc.depthBias = key.raster.depthBias;
        desc.slopeScaledDepthBias = key.raster.slopeScaledDepthBias;
        desc.depthClipEnabled = true;

        desc.renderTargetCount = key.colorTargetCount;
        for (uint32_t i = 0; i < key.colorTargetCount; i++)
        {
            const gpu::BlendState& blend = key.blend[i];
            desc.renderTargetFormat[i] = key.colorFormats[i];
            desc.renderTargetBlend[i].blendEnabled = blend.enabled;
            desc.renderTargetBlend[i].srcBlend = blend.srcColor;
            desc.renderTargetBlend[i].dstBlend = blend.dstColor;
            desc.renderTargetBlend[i].blendOp = blend.colorOp;
            desc.renderTargetBlend[i].srcBlendAlpha = blend.srcAlpha;
            desc.renderTargetBlend[i].dstBlendAlpha = blend.dstAlpha;
            desc.renderTargetBlend[i].blendOpAlpha = blend.alphaOp;
            desc.renderTargetBlend[i].renderTargetWriteMask = blend.writeMask;
        }

        if (key.depthFormat != gpu::Format::UNKNOWN)
        {
            const gpu::DepthStencilState& depth = key.depthStencil;
            desc.depthTargetFormat = key.depthFormat;
            desc.depthEnabled = depth.depthEnabled;
            desc.depthWriteEnabled = depth.depthWrite;
            desc.depthFunction = depth.depthEnabled ? depth.depthFunction : RenderComparisonFunction::ALWAYS;
            desc.stencilEnabled = depth.stencilEnabled;
            desc.stencilReadMask = depth.stencilReadMask;
            desc.stencilWriteMask = depth.stencilWriteMask;
            desc.stencilReference = depth.stencilReference;
            desc.stencilFrontFace = depth.front;
            desc.stencilBackFace = depth.back;
        }

        RenderInputSlot slots[gpu::MaxVertexStreams];
        for (uint32_t i = 0; i < key.streamCount; i++)
            slots[i] = RenderInputSlot(i, key.strides[i],
                (key.perInstance >> i) & 1 ? RenderInputSlotClassification::PER_INSTANCE_DATA
                                           : RenderInputSlotClassification::PER_VERTEX_DATA);
        RenderInputElement elements[gpu::MaxVertexElements];
        for (uint32_t i = 0; i < key.inputCount; i++)
        {
            const gpu::VertexInput& input = key.inputs[i];
            // The translated shaders read locations 1-3 (normal, tangent, binormal) and 9 (blend indices) as uint4 and
            // every other input as float4. An integer format on a float input (the game's UBYTE4 PSIZE, location 16) is
            // converted by value by D3D12 and Metal; Vulkan needs the scaled format (the validation layer rejects the
            // pairing and MoltenVK fails the pipeline). The key keeps the guest's format: pipelines.bin and the baseline
            // were written with it.
            const bool uintInput = (input.location >= 1 && input.location <= 3) || input.location == 9;
            const RenderFormat format = !uintInput && input.format == RenderFormat::R8G8B8A8_UINT
                                            ? RenderFormat::R8G8B8A8_USCALED
                                            : input.format;
            elements[i] =
                RenderInputElement("ATTRIBUTE", input.location, input.location, format, input.stream, input.offset);
        }
        desc.inputSlots = slots;
        desc.inputSlotsCount = key.streamCount;
        desc.inputElements = elements;
        desc.inputElementsCount = key.inputCount;

        RenderSpecConstant specConstant(0, key.specConstants);
        desc.specConstants = &specConstant;
        desc.specConstantsCount = 1;

        desc.cacheKey = XXH3_64bits(&key, sizeof(key)); // D3D12's pipeline library (d3d12_pipeline_cache.cpp)
        std::unique_ptr<RenderPipeline> pipeline = g_context->device->createGraphicsPipeline(desc);
        if (pipeline == nullptr)
            CheckDeviceLost();
        return pipeline;
    }

    // --- Pipeline list --------------------------------------------------------------------------------------------------
    // A pipeline the system's shader cache does not have takes ~20 ms to build on Metal (first run, a changed shader
    // cache, an app bundle with its own cache), more from DXIL on D3D12, and the game asks for ~120 at once when a level
    // starts. The list is the built-in baseline (games/<game>/pipelines, made by touring the game: docs/history/handoffs/handoff_pipelines.md)
    // merged with pipelines.bin, in SKY_SAVE_DIR or the app's data folder, where every pipeline the game uses that the
    // list lacks is appended. At startup, threads build the listed ones while the intro movies play; every loading screen
    // waits until they are all built (WaitForPipelineList), so only unlisted pipelines are built at draw time.

    constexpr uint32_t PipelineListMagic = 0x50415353; // "SSAP"
    constexpr uint32_t PipelineListVersion = 1; // change with PipelineKey or CreatePipeline

    struct PipelineListHeader
    {
        uint32_t magic, version, recordSize;
    };

    struct PipelineRecord
    {
        PipelineKey key;
        uint64_t check; // XXH3 of the key: a torn or foreign record is skipped
    };
    // The baseline is made on a Mac and read by every platform: the layout must not depend on the compiler or the backend.
    static_assert(
        sizeof(PipelineRecord) == 832, "PipelineKey changed: bump PipelineListVersion, regenerate the baseline");

    struct PipelineList
    {
        std::ofstream file; // open for appending
        std::unordered_set<uint64_t>
            listed; // key hashes in the baseline or the file (render thread only, after loading)
        std::vector<PipelineKey>
            keys; // to precompile: pipelines.bin's in the order the game first used them, then the baseline's
        std::atomic<size_t> next = 0; // next key a precompile thread takes
        std::atomic<size_t> built = 0; // keys done (built, failed, or taken by a draw)
        std::atomic<bool> stop = false;
        std::vector<std::thread> threads;
        std::atomic<uint32_t> running = 0;
        uint64_t start = 0;
        std::mutex waitMutex; // one loading wait at a time
        // Draw-time costs: pipelines built at draw time (not listed), and draws that waited for a listed one.
        uint32_t drawBuilds = 0, drawWaits = 0;
        double drawBuildMs = 0, drawWaitMs = 0;
        std::atomic<bool> unsavedBuilds = false; // D3D12: pipelines built at draw time, not in pipelines.d3d12 yet
    } g_pipelineList;

    std::filesystem::path PipelineListPath()
    {
        std::string directory;
        if (const char* saves = skyenv::Get("SKY_SAVE_DIR"))
        {
            directory = std::string(saves) + "/";
        }
        else if (std::filesystem::path game = datadir::Game(SSA_GAME_ID); !game.empty())
            directory = datadir::Text(game) + "/";
        if (directory.empty())
            return {};
        std::u8string path(directory.begin(), directory.end());
        return std::filesystem::path(path) / "pipelines.bin";
    }

    // Adds the valid records of a list in pipelines.bin's format; false if the header is not this version's.
    bool AddListedKeys(const uint8_t* data, size_t size)
    {
        PipelineListHeader header{};
        if (size >= sizeof(header))
            memcpy(&header, data, sizeof(header));
        if (header.magic != PipelineListMagic || header.version != PipelineListVersion ||
            header.recordSize != sizeof(PipelineRecord))
            return false;
        for (size_t offset = sizeof(header); offset + sizeof(PipelineRecord) <= size; offset += sizeof(PipelineRecord))
        {
            PipelineRecord record;
            memcpy(&record, data + offset, sizeof(record));
            uint64_t hash = XXH3_64bits(&record.key, sizeof(record.key));
            if (record.check == hash && g_pipelineList.listed.insert(hash).second)
                g_pipelineList.keys.push_back(record.key);
        }
        return true;
    }

    void LoadPipelineList()
    {
        std::filesystem::path path = PipelineListPath();
        if (!path.empty())
        {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            if (AddListedKeys(data.data(), data.size()))
            {
                g_pipelineList.file.open(path, std::ios::binary | std::ios::app);
            }
            else
            {
                // Missing, or written by another version: start a new list.
                g_pipelineList.file.open(path, std::ios::binary | std::ios::trunc);
                PipelineListHeader header = { PipelineListMagic, PipelineListVersion,
                    uint32_t(sizeof(PipelineRecord)) };
                g_pipelineList.file.write(reinterpret_cast<const char*>(&header), sizeof(header));
                g_pipelineList.file.flush();
            }
        }
        size_t own = g_pipelineList.keys.size();

        // The baseline's keys pipelines.bin lacks; they are not written to it.
        size_t baseline = 0;
#if defined(SSA_PIPELINE_BASELINE) && defined(SSA_SHADER_CACHE)
        {
            unsigned long long size = ZSTD_getFrameContentSize(g_pipelineBaseline, sizeof(g_pipelineBaseline));
            std::vector<uint8_t> data(
                size != ZSTD_CONTENTSIZE_ERROR && size != ZSTD_CONTENTSIZE_UNKNOWN ? size_t(size) : 0);
            if (data.empty() ||
                ZSTD_isError(
                    ZSTD_decompress(data.data(), data.size(), g_pipelineBaseline, sizeof(g_pipelineBaseline))) ||
                !AddListedKeys(data.data(), data.size()))
                fprintf(stderr,
                    "[gpu] pipeline list: the built-in baseline is not this version's (PipelineListVersion %u)\n",
                    PipelineListVersion);
            else
                baseline = (data.size() - sizeof(PipelineListHeader)) / sizeof(PipelineRecord);
        }
#endif
        fprintf(stderr, "[gpu] pipeline list: %zu pipelines (%zu from pipelines.bin, %zu more of the baseline's %zu)\n",
            g_pipelineList.keys.size(), own, g_pipelineList.keys.size() - own, baseline);
    }

    void AddToPipelineList(const PipelineKey& key, uint64_t hash)
    {
        if (!g_pipelineList.file.is_open() || !g_pipelineList.listed.insert(hash).second)
            return;
        PipelineRecord record{ key, hash };
        g_pipelineList.file.write(reinterpret_cast<const char*>(&record), sizeof(record));
        g_pipelineList.file.flush();
    }

    void StopPrecompiling()
    {
        g_pipelineList.stop = true;
        for (std::thread& thread : g_pipelineList.threads)
            thread.join();
        g_pipelineList.threads.clear();
    }

    // Builds listed keys until none is left; run by the precompile threads and by loading waits.
    void BuildListedPipelines()
    {
        for (;;)
        {
            size_t index = g_pipelineList.next++;
            if (g_pipelineList.stop || index >= g_pipelineList.keys.size())
                break;
            const PipelineKey& key = g_pipelineList.keys[index];
            std::unique_lock lock(g_draw->pipelineMutex);
            auto [it, inserted] = g_draw->pipelines.try_emplace(XXH3_64bits(&key, sizeof(key)));
            if (inserted)
            {
                PipelineEntry& entry = it->second; // references survive rehashing
                lock.unlock();
                std::unique_ptr<RenderPipeline> pipeline = CreatePipeline(key);
                lock.lock();
                entry.pipeline = std::move(pipeline);
                entry.ready = true;
            }
            // else: the game asked for it first and is building it.
            g_pipelineList.built++;
            g_draw->pipelineReady.notify_all();
        }
    }

    void PrecompilePipelines()
    {
        BuildListedPipelines();
        if (--g_pipelineList.running == 0)
        {
            fprintf(stderr, "[gpu] pipeline list: %zu pipelines ready after %.0f ms\n", g_pipelineList.keys.size(),
                (SDL_GetTicksNS() - g_pipelineList.start) / 1e6);
#if defined(_WIN32)
            if (g_draw->dxil && !g_pipelineList.stop)
                D3D12SavePipelineLibrary(*g_context->device, "after the pipeline list");
#endif
        }
    }

    void StartPrecompiling()
    {
        if (g_pipelineList.keys.empty())
            return;
        uint32_t count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
        g_pipelineList.start = SDL_GetTicksNS();
        g_pipelineList.running = count;
        for (uint32_t i = 0; i < count; i++)
            g_pipelineList.threads.emplace_back(PrecompilePipelines);
        // Before the device goes away at exit (the game never calls gpu::Shutdown).
        std::atexit(StopPrecompiling);
    }

    RenderPipeline* Pipeline(const gpu::DrawDesc& draw)
    {
        PipelineKey key = KeyOf(draw);
        uint64_t hash = XXH3_64bits(&key, sizeof(key));
        std::unique_lock lock(g_draw->pipelineMutex);
        auto [it, inserted] = g_draw->pipelines.try_emplace(hash);
        PipelineEntry& entry = it->second;
        uint64_t start = SDL_GetTicksNS();
        if (!inserted)
        {
            // Listed: built, or being built by a precompile thread.
            if (!entry.ready)
            {
                g_draw->pipelineReady.wait(lock, [&] { return entry.ready; });
                double ms = (SDL_GetTicksNS() - start) / 1e6;
                g_pipelineList.drawWaits++;
                g_pipelineList.drawWaitMs += ms;
                fprintf(stderr, "[gpu] draw waited %.1f ms for a listed pipeline (%u waits, %.0f ms in all)\n", ms,
                    g_pipelineList.drawWaits, g_pipelineList.drawWaitMs);
            }
            return entry.pipeline.get();
        }
        lock.unlock();
        std::unique_ptr<RenderPipeline> pipeline = CreatePipeline(key);
        double ms = (SDL_GetTicksNS() - start) / 1e6;
        g_pipelineList.drawBuilds++;
        g_pipelineList.drawBuildMs += ms;
        fprintf(stderr, "[gpu] pipeline built at draw time: %.1f ms (%u built at draw time, %.0f ms in all)\n", ms,
            g_pipelineList.drawBuilds, g_pipelineList.drawBuildMs);
        if (pipeline != nullptr)
        {
            AddToPipelineList(key, hash);
            g_pipelineList.unsavedBuilds = true;
        }
        lock.lock();
        entry.pipeline = std::move(pipeline);
        entry.ready = true;
        g_draw->pipelineReady.notify_all();
        return entry.pipeline.get();
    }

    // Copies `size` bytes into this frame's upload memory and returns where they went.
    RenderBufferReference Upload(const void* data, uint64_t size, uint64_t alignment, uint64_t* address = nullptr)
    {
        RenderBuffer* buffer;
        uint64_t offset = AllocateStaging(std::max<uint64_t>(size, 4), buffer, alignment);
        if (auto mapped = static_cast<uint8_t*>(buffer->map()))
            memcpy(mapped + offset, data, size);
        else
            CheckDeviceLost();
        buffer->unmap();
        if (address != nullptr)
            *address = buffer->getDeviceAddress() + offset;
        return buffer->at(offset);
    }

    uint32_t SwapConstant(uint32_t value)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        return _byteswap_ulong(value);
#else
        return __builtin_bswap32(value);
#endif
    }

    // Both 4 KB float constant banks, byte-swapped, deduplicated within the frame: most draws of a pass share them.
    std::unordered_map<uint64_t, RenderBufferReference> g_frameConstants;
    // The last upload and its id (DrawDesc::guestConstantsId): consecutive draws with the same id skip the hash.
    uint64_t g_lastConstantsId;
    RenderBufferReference g_lastConstants;

    RenderBufferReference UploadConstants(const void* guest, uint64_t id, uint64_t* address)
    {
        constexpr uint64_t Size = 2 * gpu::FloatConstantCount * 16;
        if (id != 0 && id == g_lastConstantsId)
        {
            *address = g_lastConstants.ref->getDeviceAddress() + g_lastConstants.offset;
            return g_lastConstants;
        }
        g_lastConstantsId = id;
        uint64_t hash = XXH3_64bits(guest, Size);
        auto it = g_frameConstants.find(hash);
        if (it != g_frameConstants.end())
        {
            *address = it->second.ref->getDeviceAddress() + it->second.offset;
            g_lastConstants = it->second;
            return it->second;
        }

        RenderBuffer* buffer;
        uint64_t offset = AllocateStaging(Size, buffer, ConstantAlignment);
        auto mapped = static_cast<uint8_t*>(buffer->map());
        if (mapped == nullptr)
        {
            CheckDeviceLost();
            *address = 0;
            g_lastConstantsId = 0;
            return buffer->at(offset);
        }
        auto* destination = static_cast<uint32_t*>(static_cast<void*>(mapped + offset));
        auto* source = static_cast<const uint32_t*>(guest);
        for (uint32_t i = 0; i < 2 * gpu::FloatConstantCount * 4; i++)
            destination[i] = SwapConstant(source[i]);
        buffer->unmap();

        RenderBufferReference reference = buffer->at(offset);
        *address = buffer->getDeviceAddress() + offset;
        g_frameConstants.emplace(hash, reference);
        g_lastConstants = reference;
        return reference;
    }

    // Removes cached objects that refer to a texture about to be destroyed.
    void ForgetTexture(gpu::Texture* texture)
    {
        // Framebuffers are keyed by texture pointers: drop them all rather than risk a stale one for a reused address.
        g_draw->framebuffers.clear();
        g_draw->depthSets.erase(texture);
        uint32_t heap = HeapOf(*texture);
        auto release = [&](uint32_t slot) {
            g_draw->heaps[heap]->setTexture(
                slot, g_draw->placeholders[heap]->texture.get(), RenderTextureLayout::SHADER_READ);
            g_draw->textureSlots[heap].free.push_back(slot);
        };
        if (texture->heapIndex != 0)
            release(texture->heapIndex);
        for (const gpu::Texture::SwizzledView& view : texture->swizzledViews)
            release(view.heapIndex);
    }

    std::vector<gpu::Texture*> g_pendingDestroy;
}

void gpu::detail::DrawInit() { Draws(); }

void gpu::detail::DrawShutdown()
{
    StopPrecompiling();
    // Textures destroyed since the last frame are still queued: free them while the device exists. Left queued, the
    // next Init's first frame deleted them against a destroyed device (a texture destructor locks the device's mutex:
    // std::terminate, about 1 test run in 6).
    for (Texture* texture : g_pendingDestroy)
    {
        if (g_draw != nullptr)
            ForgetTexture(texture);
        delete texture;
    }
    g_pendingDestroy.clear();
}

bool gpu::detail::DrawHasPendingDestroys() { return !g_pendingDestroy.empty(); }

void gpu::detail::DrawFrameDone()
{
    g_frameConstants.clear();
    g_lastConstantsId = 0;
#if defined(_WIN32)
    // D3D12: keep pipelines built at draw time for the next run, at most every 30 s, off the render thread.
    static uint64_t lastSave = 0;
    uint64_t now = SDL_GetTicksNS();
    if (g_draw != nullptr && g_draw->dxil && g_pipelineList.unsavedBuilds && g_pipelineList.running == 0 &&
        now - lastSave > 30'000'000'000ull)
    {
        g_pipelineList.unsavedBuilds = false;
        lastSave = now;
        std::thread([] { D3D12SavePipelineLibrary(*g_context->device, "after draw-time builds"); }).detach();
    }
#endif
    for (Texture* texture : g_pendingDestroy)
    {
        if (g_draw != nullptr)
            ForgetTexture(texture);
        delete texture;
    }
    g_pendingDestroy.clear();
}

namespace gpu::detail
{
    // DestroyTexture defers here: the frame being recorded may still use the texture.
    void DeferDestroy(gpu::Texture* texture) { g_pendingDestroy.push_back(texture); }
}

const gpu::XenosShader* gpu::FindXenosShader(uint64_t hash)
{
    // Not under g_mutex: producers call it for every draw while the GPU thread records (Shader has its own lock).
    if (g_context == nullptr)
        return nullptr;
    Draws();
    return Shader(hash);
}

void gpu::WaitForPipelineList(const char* reason)
{
    {
        std::lock_guard lock(g_mutex);
        if (g_context == nullptr)
            return;
        Draws(); // loads the list and starts the precompile threads
    }
    std::lock_guard wait(g_pipelineList.waitMutex);
    size_t count = g_pipelineList.keys.size();
    if (g_pipelineList.built == count)
    {
        // One line per level load: the draw-time builds so far, so a tour counts them per level.
        fprintf(stderr, "[gpu] loading (%s): the %zu listed pipelines are built; %u built at draw time so far\n",
            reason, count, g_pipelineList.drawBuilds);
        return;
    }

    // Build on this thread too and, while loading, on more threads than in the background.
    uint64_t start = SDL_GetTicksNS();
    size_t before = g_pipelineList.built;
    uint32_t helpers = std::clamp(std::thread::hardware_concurrency(), 2u, 16u) - 1;
    helpers -= std::min(helpers, g_pipelineList.running.load());
    std::vector<std::thread> threads;
    for (uint32_t i = 0; i < helpers; i++)
        threads.emplace_back(BuildListedPipelines);
    BuildListedPipelines();
    for (std::thread& thread : threads)
        thread.join();
    // The last ones may still be on the background threads.
    std::unique_lock lock(g_draw->pipelineMutex);
    g_draw->pipelineReady.wait(lock, [&] { return g_pipelineList.built == count || g_pipelineList.stop; });
    fprintf(stderr, "[gpu] loading (%s) waited %.0f ms for %zu of the %zu listed pipelines (%u extra threads)\n",
        reason, (SDL_GetTicksNS() - start) / 1e6, count - before, count, helpers);
}

void gpu::Draw(const DrawDesc& draw)
{
    ProducerLock producer;
    if (!WorkerRunning())
    {
        DrawNow(draw);
        return;
    }
    // A copy for the GPU thread, with the data the draw points at (valid only until the caller's next draw). Guest
    // constants go to the GPU thread's own copy, as the groups of 16 registers that changed since the last draw.
    static std::array<uint32_t, 2 * FloatConstantCount * 4> mirror; // the GPU thread's copy
    static std::array<uint32_t, 2 * FloatConstantCount / 4> sentVersions;
    static bool versionsSent;
    static uint64_t constantsId;
    auto copy = static_cast<DrawDesc*>(FrameAllocate(sizeof(DrawDesc), alignof(DrawDesc)));
    *copy = draw;
    for (uint32_t i = 0; i < draw.streamCount; i++)
        if (draw.streams[i].converted.buffer == nullptr)
            copy->streams[i].data = FrameCopy(draw.streams[i].data, draw.streams[i].size);
    if (draw.indicesConverted.buffer == nullptr)
        copy->indices = FrameCopy(draw.indices, draw.indexBytes);
    copy->vertexConstants = static_cast<const float*>(FrameCopy(draw.vertexConstants, FloatConstantCount * 16));
    copy->pixelConstants = static_cast<const float*>(FrameCopy(draw.pixelConstants, FloatConstantCount * 16));
    copy->pulledVertices = static_cast<const float*>(FrameCopy(draw.pulledVertices, uint64_t(draw.pulledCount) * 16));
    copy->guestConstantGroups = nullptr;
    if (draw.guestConstants != nullptr)
    {
        constexpr uint32_t Groups = 2 * FloatConstantCount / 4, GroupWords = 16;
        uint8_t changed[Groups];
        uint32_t count = 0;
        for (uint32_t group = 0; group < Groups; group++)
            if (draw.guestConstantGroups == nullptr || !versionsSent ||
                sentVersions[group] != draw.guestConstantGroups[group])
                changed[count++] = uint8_t(group);
        if (count != 0)
        {
            auto source = static_cast<const uint32_t*>(draw.guestConstants);
            auto words = static_cast<uint32_t*>(FrameAllocate(count * GroupWords * 4, 16));
            for (uint32_t i = 0; i < count; i++)
                memcpy(words + i * GroupWords, source + changed[i] * GroupWords, GroupWords * 4);
            auto groups = static_cast<uint8_t*>(FrameAllocate(count, 1));
            memcpy(groups, changed, count);
            Enqueue([words, groups, count] {
                for (uint32_t i = 0; i < count; i++)
                    memcpy(mirror.data() + groups[i] * GroupWords, words + i * GroupWords, GroupWords * 4);
            });
            versionsSent = draw.guestConstantGroups != nullptr;
            if (versionsSent)
                memcpy(sentVersions.data(), draw.guestConstantGroups, sizeof(sentVersions));
            constantsId++;
        }
        copy->guestConstants = mirror.data();
        copy->guestConstantsId = constantsId;
    }
    Enqueue([copy] { DrawNow(*copy); });
}

void gpu::detail::DrawNow(const DrawDesc& draw)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
        return;
    // The perf log's draw time (gpu.cpp LogPerf).
    struct Timer
    {
        uint64_t start = SDL_GetTicksNS();
        ~Timer()
        {
            g_context->perf.drawNs += SDL_GetTicksNS() - start;
            g_context->perf.draws++;
        }
    } timer;
    DrawContext& d = Draws();
    if (draw.vertexShader == nullptr || draw.colorTargetCount + (draw.depthTarget != nullptr) == 0)
        return;

    RenderPipeline* pipeline = Pipeline(draw);
    if (pipeline == nullptr)
        return;

    // Send the frame so far to the GPU where a render pass ends anyway (or, in a long pass, every MaxFlushDraws
    // draws), so the GPU works while the rest of the frame is recorded.
    RenderFramebuffer* framebuffer = Framebuffer(draw.colorTargets, draw.colorTargetCount, draw.depthTarget);
    if (g_context->recording &&
        ((framebuffer != g_context->boundFramebuffer && g_context->drawsSinceFlush >= FlushDraws) ||
            g_context->drawsSinceFlush >= MaxFlushDraws))
        Flush();
    RenderCommandList& list = Record();
    g_context->drawsSinceFlush++;

    SharedConstants shared{};
    for (uint32_t i = 0; i < MaxSamplers; i++)
    {
        // Slots without a texture sample the black placeholder, whatever the sampler: they keep sampler 0.
        Texture* texture = draw.textures[i];
        if (texture == nullptr)
            continue;
        shared.sampler[i] = SamplerSlot(draw.samplers[i]);
        Transition(list, *texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
        uint32_t slot = TextureSlot(texture, draw.swizzles[i]);
        switch (HeapOf(*texture))
        {
        case Texture2DHeap: shared.texture2D[i] = slot; break;
        case Texture3DHeap: shared.texture3D[i] = slot; break;
        default: shared.textureCube[i] = slot; break;
        }
    }
    for (uint32_t i = 0; i < MaxVertexSamplers; i++)
    {
        Texture* texture = draw.vertexTextures[i];
        if (texture == nullptr)
            continue;
        shared.vertexSampler[i] = SamplerSlot(draw.vertexSamplers[i]);
        Transition(list, *texture, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
        shared.vertexTexture2D[i] = TextureSlot(texture, draw.vertexSwizzles[i]);
    }
    shared.booleans = draw.booleans;
    shared.swappedTexcoords = draw.swappedTexcoords;
    shared.halfPixelOffset[0] = draw.halfPixelOffset[0];
    shared.halfPixelOffset[1] = draw.halfPixelOffset[1];
    shared.alphaThreshold = draw.alphaThreshold;
    shared.paramGen = draw.paramGen;
    shared.positionScale[0] = draw.positionScale[0];
    shared.positionScale[1] = draw.positionScale[1];
    shared.resolutionScale = float(gpu::g_settings.resolutionScale);
    shared.scaledTextures = draw.scaledTextures;
    shared.pointScale[0] = draw.viewport.width != 0.0f ? 1.0f / std::fabs(draw.viewport.width) : 0.0f;
    shared.pointScale[1] = draw.viewport.height != 0.0f ? 1.0f / std::fabs(draw.viewport.height) : 0.0f;

    for (uint32_t i = 0; i < draw.colorTargetCount; i++)
        Transition(list, *draw.colorTargets[i], RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
    if (draw.depthTarget != nullptr)
        Transition(list, *draw.depthTarget, RenderTextureLayout::DEPTH_WRITE, RenderBarrierStage::GRAPHICS);

    PushConstants push;
    RenderBufferReference vertexConstants, pixelConstants, pulledVertices;
    if (draw.guestConstants != nullptr)
    {
        vertexConstants = pixelConstants =
            UploadConstants(draw.guestConstants, draw.guestConstantsId, &push.vertexShaderConstants);
        pixelConstants.offset += FloatConstantCount * 16;
        push.pixelShaderConstants = push.vertexShaderConstants + FloatConstantCount * 16;
    }
    else
    {
        vertexConstants =
            Upload(draw.vertexConstants, FloatConstantCount * 16, ConstantAlignment, &push.vertexShaderConstants);
        pixelConstants =
            Upload(draw.pixelConstants, FloatConstantCount * 16, ConstantAlignment, &push.pixelShaderConstants);
    }
    if (draw.pulledCount != 0)
    {
        pulledVertices = Upload(draw.pulledVertices, uint64_t(draw.pulledCount) * 16, 16, &shared.pulledVertices);
        shared.pulledCount = draw.pulledCount;
    }
    RenderBufferReference sharedConstants = Upload(&shared, sizeof(shared), ConstantAlignment, &push.sharedConstants);

    RenderVertexBufferView views[MaxVertexStreams];
    RenderInputSlot slots[MaxVertexStreams];
    for (uint32_t i = 0; i < draw.streamCount; i++)
    {
        const VertexStream& stream = draw.streams[i];
        RenderBufferReference buffer = stream.converted.buffer != nullptr
                                           ? stream.converted.buffer->at(stream.converted.offset)
                                           : Upload(stream.data, stream.size, 16);
        views[i] = RenderVertexBufferView(buffer, stream.size);
        slots[i] = InputSlot(draw, i);
    }

    SetFramebuffer(list, framebuffer);
    if (d.dxil)
    {
        // D3D12: only what changed since the last draw of this command list. Setting the root signature unbinds every
        // root argument, so it is set once per command list with the descriptor heaps.
        Context::BoundGraphics& bound = g_context->bound;
        if (!bound.valid)
        {
            list.setGraphicsPipelineLayout(d.layout.get());
            for (uint32_t heap = 0; heap < 4; heap++)
                list.setGraphicsDescriptorSet(d.heaps[heap].get(), heap);
            bound = {};
            bound.valid = true;
        }
        if (bound.pipeline != pipeline)
        {
            list.setPipeline(pipeline);
            bound.pipeline = pipeline;
        }
        if (!bound.hasViewport || memcmp(&bound.viewport, &draw.viewport, sizeof(draw.viewport)) != 0)
            list.setViewports(draw.viewport);
        if (!bound.hasViewport || memcmp(&bound.scissor, &draw.scissor, sizeof(draw.scissor)) != 0)
            list.setScissors(draw.scissor);
        if (!bound.hasViewport || memcmp(bound.blendFactor, draw.blendFactor, sizeof(draw.blendFactor)) != 0)
            list.setBlendFactor(draw.blendFactor);
        bound.hasViewport = true;
        bound.viewport = draw.viewport;
        bound.scissor = draw.scissor;
        memcpy(bound.blendFactor, draw.blendFactor, sizeof(bound.blendFactor));
        // Without pulled vertices the shaders read none: any valid address does.
        const RenderBufferReference roots[4] = { vertexConstants, pixelConstants, sharedConstants,
            draw.pulledCount != 0 ? pulledVertices : sharedConstants };
        for (uint32_t i = 0; i < 4; i++)
        {
            if (bound.roots[i].ref == roots[i].ref && bound.roots[i].offset == roots[i].offset)
                continue;
            list.setGraphicsRootDescriptor(roots[i], i);
            bound.roots[i] = roots[i];
        }
    }
    else
    {
        list.setViewports(draw.viewport);
        list.setScissors(draw.scissor);
        list.setBlendFactor(draw.blendFactor);
        list.setGraphicsPipelineLayout(d.layout.get());
        list.setPipeline(pipeline);
        for (uint32_t heap = 0; heap < 4; heap++)
            list.setGraphicsDescriptorSet(d.heaps[heap].get(), heap);
        list.setGraphicsPushConstants(0, &push);
    }
    if (draw.streamCount != 0)
        list.setVertexBuffers(0, views, draw.streamCount, slots);

    if (draw.indices != nullptr || draw.indicesConverted.buffer != nullptr)
    {
        RenderBufferReference buffer = draw.indicesConverted.buffer != nullptr
                                           ? draw.indicesConverted.buffer->at(draw.indicesConverted.offset)
                                           : Upload(draw.indices, draw.indexBytes, 16);
        RenderIndexBufferView view(
            buffer, draw.indexBytes, draw.indices32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
        list.setIndexBuffer(&view);
        list.drawIndexedInstanced(draw.count, 1, draw.first, draw.baseVertex, 0);
    }
    else
    {
        list.drawInstanced(draw.count, 1, draw.first, 0);
    }
}

void gpu::ClearColor(Texture* target, const float rgba[4], const plume::RenderRect* rect)
{
    ProducerLock producer;
    std::array<float, 4> color{ rgba[0], rgba[1], rgba[2], rgba[3] };
    bool hasRect = rect != nullptr;
    plume::RenderRect area = hasRect ? *rect : plume::RenderRect();
    Enqueue([=] { ClearColorNow(target, color.data(), hasRect ? &area : nullptr); });
}

void gpu::detail::ClearColorNow(Texture* target, const float rgba[4], const plume::RenderRect* rect)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
        return;
    Draws();
    RenderCommandList& list = Record();
    Transition(list, *target, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
    SetFramebuffer(list, Framebuffer(&target, 1, nullptr));
    list.clearColor(0, RenderColor(rgba[0], rgba[1], rgba[2], rgba[3]), rect, rect != nullptr ? 1 : 0);
}

void gpu::ClearDepthStencil(
    Texture* target, bool depth, bool stencil, float z, uint8_t stencilValue, const plume::RenderRect* rect)
{
    ProducerLock producer;
    bool hasRect = rect != nullptr;
    plume::RenderRect area = hasRect ? *rect : plume::RenderRect();
    Enqueue([=] { ClearDepthStencilNow(target, depth, stencil, z, stencilValue, hasRect ? &area : nullptr); });
}

void gpu::detail::ClearDepthStencilNow(
    Texture* target, bool depth, bool stencil, float z, uint8_t stencilValue, const plume::RenderRect* rect)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
        return;
    Draws();
    RenderCommandList& list = Record();
    Transition(list, *target, RenderTextureLayout::DEPTH_WRITE, RenderBarrierStage::GRAPHICS);
    SetFramebuffer(list, Framebuffer(nullptr, 0, target));
    list.clearDepthStencil(depth, stencil, z, stencilValue, rect, rect != nullptr ? 1 : 0);
}

void gpu::CopyTexture(
    Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region, uint32_t slice)
{
    ProducerLock producer;
    Enqueue([=] { CopyTextureNow(destination, x, y, source, region, slice); });
}

void gpu::detail::CopyTextureNow(
    Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region, uint32_t slice)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
        return;
    RenderCommandList& list = Record();
    Transition(list, *source, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
    // A second copy into an image already in COPY_DEST (the game resolves several times into one texture) is a
    // write-after-write hazard Vulkan wants a barrier for; the layout is unchanged, so Transition adds none.
    if (destination->layout == RenderTextureLayout::COPY_DEST &&
        g_context->renderInterface->getCapabilities().shaderFormat == RenderShaderFormat::SPIRV)
        list.barriers(
            RenderBarrierStage::COPY, RenderTextureBarrier(destination->texture.get(), RenderTextureLayout::COPY_DEST));
    Transition(list, *destination, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
    RenderBox box(region.left, region.top, region.right, region.bottom);
    list.copyTextureRegion(RenderTextureCopyLocation::Subresource(destination->texture.get(), 0, slice),
        RenderTextureCopyLocation::Subresource(source->texture.get(), 0, 0), x, y, 0, &box);
}

void gpu::ResolveDepth(Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region)
{
    ProducerLock producer;
    Enqueue([=] { ResolveDepthNow(destination, x, y, source, region); });
}

void gpu::detail::ResolveDepthNow(
    Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region)
{
    std::lock_guard lock(g_mutex);
    if (DeviceLost())
        return;
    DrawContext& d = Draws();
    Context& c = *g_context;
    std::unique_ptr<RenderDescriptorSet>& set = d.depthSets[source];
    if (set == nullptr)
    {
        RenderDescriptorSetBuilder builder;
        builder.begin();
        builder.addTexture(0);
        builder.end();
        set = builder.create(c.device.get());
        set->setTexture(0, source->texture.get(), RenderTextureLayout::SHADER_READ);
    }

    RenderCommandList& list = Record();
    Transition(list, *source, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
    Transition(list, *destination, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
    const int32_t offset[2] = { region.left - int32_t(x), region.top - int32_t(y) };
    SetFramebuffer(list, Framebuffer(&destination, 1, nullptr));
    list.setViewports(RenderViewport(0.0f, 0.0f, float(destination->desc.width), float(destination->desc.height)));
    list.setScissors(RenderRect(
        int32_t(x), int32_t(y), int32_t(x) + region.right - region.left, int32_t(y) + region.bottom - region.top));
    c.bound = {};
    list.setGraphicsPipelineLayout(c.resolveDepthLayout.get());
    list.setPipeline(c.resolveDepthPipeline.get());
    list.setGraphicsDescriptorSet(set.get(), 0);
    list.setGraphicsPushConstants(0, offset);
    list.drawInstanced(3, 1, 0, 0);
}
