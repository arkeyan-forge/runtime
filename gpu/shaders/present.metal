// Present pass, output readback and depth resolves for the Metal backend (keep in sync with present.hlsl).
// plume binds descriptor set N as an argument buffer at [[buffer(N)]], each binding at [[id(binding)]].
#include <metal_stdlib>
using namespace metal;

struct PresentSet
{
    texture2d<float> image [[id(0)]];
    sampler imageSampler [[id(1)]];
};

struct ReadbackSet
{
    texture2d<float> image [[id(0)]];
    device uint* pixels [[id(1)]];
};

struct ResolveDepthSet
{
    depth2d<float> depth [[id(0)]];
};

struct Interpolants
{
    float4 position [[position]];
    float2 uv;
};

// One triangle covering the viewport, which the caller sets to the letterboxed rectangle.
vertex Interpolants PresentVS(uint vertexId [[vertex_id]])
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Interpolants out;
    out.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    out.uv = uv;
    return out;
}

fragment float4 PresentPS(Interpolants in [[stage_in]], constant PresentSet& set [[buffer(0)]])
{
    return float4(set.image.sample(set.imageSampler, in.uv).rgb, 1.0);
}

// Host UI drawn over the image (gpu::SetOverlay), blended with its alpha.
fragment float4 OverlayPS(Interpolants in [[stage_in]], constant PresentSet& set [[buffer(0)]])
{
    return set.image.sample(set.imageSampler, in.uv);
}

// Packs the output into RGBA8 words (R in the low byte) for the CPU: plume cannot copy textures to buffers on Metal.
kernel void ReadbackCS(uint2 id [[thread_position_in_grid]], constant ReadbackSet& set [[buffer(0)]])
{
    uint width = set.image.get_width();
    if (id.x >= width || id.y >= set.image.get_height())
        return;
    set.pixels[id.y * width + id.x] = pack_float_to_unorm4x8(set.image.read(id));
}

// The red channel's bits, unconverted, of one texel per `step` x `step` block (push constants, buffer 8), packed at
// the reduced size: depth resolves written back to guest memory, at the guest's resolution.
kernel void ReadbackRawCS(uint2 id [[thread_position_in_grid]], constant ReadbackSet& set [[buffer(0)]],
    constant uint& step [[buffer(8)]])
{
    uint width = set.image.get_width() / step;
    if (id.x >= width || id.y >= set.image.get_height() / step)
        return;
    set.pixels[id.y * width + id.x] = as_type<uint>(set.image.read(id * step).r);
}

// EDRAM depth resolves: the depth under each pixel of the destination, read at the pixel plus `offset` (push constants,
// buffer 8), in the red channel. PresentVS covers the destination; the scissor limits the copy.
fragment float4 ResolveDepthPS(Interpolants in [[stage_in]], constant ResolveDepthSet& set [[buffer(0)]],
    constant int2& offset [[buffer(8)]])
{
    return float4(set.depth.read(uint2(int2(in.position.xy) + offset)), 0.0, 0.0, 0.0);
}
