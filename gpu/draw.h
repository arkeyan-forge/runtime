#pragma once

// Host side of the Direct3D replacement: draws with the game's shaders as XenosRecomp translated them (looked up in
// ppc/shader_cache.cpp), bindless textures and samplers, and render targets standing in for EDRAM surfaces.
#include "gpu/buffer_cache.h"
#include "gpu/gpu.h"

namespace gpu
{
    struct XenosShader;

    // The translated shader for the guest shader container with this XXH3-64 hash, or nullptr when the cache lacks it
    // (built without a shader cache, or a shader XenosRecomp could not translate).
    const XenosShader* FindXenosShader(uint64_t hash);

    // XenosRecomp specialization constants (shader_common.h).
    namespace SpecConstant
    {
        enum : uint32_t
        {
            R11G11B10Normal = 1u << 0,
            AlphaTest = 1u << 1,
            BlendIndicesFloat = 1u << 2, // SPEC_CONSTANT_BLEND_INDICES_FLOAT
        };
    }

    constexpr uint32_t MaxSamplers = 16; // sampler registers a translated shader can use
    constexpr uint32_t MaxVertexSamplers = 4; // vertex texture fetch: fetch constants 16-19, sampler registers 0-3 of a vertex shader

    struct VertexInput
    {
        uint32_t location; // the shader's [[attribute(n)]], from XenosRecomp's usage table
        uint32_t stream;
        uint32_t offset;
        Format format;
    };

    // Vertex data: already on the GPU (`converted`), or in host byte order and copied into this frame's upload memory.
    struct VertexStream
    {
        const void* data; // host byte order, copied into this frame's upload memory (ignored with `converted`)
        uint32_t size;
        uint32_t stride;
        bool perInstance; // one element for the whole draw (a constant input)
        BufferRange converted; // from gpu/buffer_cache.h
    };

    struct DrawDesc
    {
        const XenosShader* vertexShader;
        const XenosShader* pixelShader; // nullptr: depth-only draw
        uint32_t specConstants;

        Texture* colorTargets[MaxRenderTargets];
        uint32_t colorTargetCount;
        Texture* depthTarget;

        BlendState blend[MaxRenderTargets];
        float blendFactor[4]; // the blend constant (BLEND_FACTOR)
        DepthStencilState depthStencil;
        RasterState raster;
        plume::RenderPrimitiveTopology topology;
        plume::RenderViewport viewport;
        plume::RenderRect scissor;

        VertexInput inputs[MaxVertexElements];
        uint32_t inputCount;
        VertexStream streams[MaxVertexStreams];
        uint32_t streamCount;

        const void* indices; // host byte order; nullptr for a non-indexed draw (and with `indicesConverted`)
        uint32_t indexBytes;
        bool indices32;
        BufferRange indicesConverted; // from gpu/buffer_cache.h

        uint32_t count;      // vertices, or indices for an indexed draw
        uint32_t first;      // first vertex, or first index
        int32_t baseVertex;

        const float* vertexConstants; // 256 float4 each, host byte order
        const float* pixelConstants;
        // When non-null: 512 float4 of big-endian guest constants (vertex bank, then pixel bank). Identical banks are
        // uploaded once per frame.
        const void* guestConstants;
        // Nonzero: consecutive draws with the same id have the same guestConstants (they are uploaded and copied once).
        uint64_t guestConstantsId;
        // Optional, with guestConstants: a version per group of 16 registers (128), changing with the group's content;
        // gpu::Draw then queues only the groups that changed.
        const uint32_t* guestConstantGroups;

        // Per sampler register: the texture (nullptr = nothing bound), its component swizzle and how it is sampled.
        Texture* textures[MaxSamplers];
        plume::RenderComponentMapping swizzles[MaxSamplers];
        SamplerState samplers[MaxSamplers];
        // The same for a vertex shader's texture fetches (SSA's grass reads a wind texture).
        Texture* vertexTextures[MaxVertexSamplers];
        plume::RenderComponentMapping vertexSwizzles[MaxVertexSamplers];
        SamplerState vertexSamplers[MaxVertexSamplers];

        uint32_t booleans;         // vertex shader booleans 0-15 in bits 0-15, pixel shader booleans in bits 16-31
        uint32_t swappedTexcoords; // TEXCOORDn inputs whose 16-bit component pairs arrive swapped (bit n)
        float halfPixelOffset[2];   // added to the output position, times w
        float alphaThreshold;
        float positionScale[2];     // multiplies the output position: (1, 1), or pixels to clip space (2/W, -2/H)
        uint32_t paramGen;          // point sprites: bit 31 = the point coordinate goes to pixel shader register (bits 0-3)
        // Sampler registers whose texture is resolution-scale times its guest size (resolves): fetches with unnormalized
        // coordinates (in guest texels) scale them (bit n = register n).
        uint32_t scaledTextures;
        // The vertex input the vertex shader fetches at a computed index, as `pulledCount` float4s (0: none).
        const float* pulledVertices;
        uint32_t pulledCount;
    };

    void Draw(const DrawDesc& draw);
    // Clears the whole target, or only `rect` when given.
    void ClearColor(Texture* target, const float rgba[4], const plume::RenderRect* rect = nullptr);
    void ClearDepthStencil(Texture* target, bool depth, bool stencil, float z, uint8_t stencilValue, const plume::RenderRect* rect = nullptr);

    // Copies `region` of `source` to (x, y) in array slice (cube face) `slice` of `destination`; both have the same format
    // (EDRAM resolves).
    void CopyTexture(Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region, uint32_t slice = 0);

    // Copies the depth of `region` of the depth target `source` to (x, y) in the red channel of `destination`, an R32_FLOAT
    // render target (EDRAM depth resolves).
    void ResolveDepth(Texture* destination, uint32_t x, uint32_t y, Texture* source, const plume::RenderRect& region);

    // Blocks until every pipeline of the pipeline list (the built-in baseline and pipelines.bin) is built, building them
    // on this thread and extra threads too. Called while the game loads (kernel/files.cpp, at each mission.str open), so
    // that no listed pipeline is built at draw time. Returns at once when they are all built.
    void WaitForPipelineList(const char* reason);
}
