// Present pass, output readback and depth resolves for the D3D12 (DXIL) and Vulkan (SPIR-V) backends (keep in sync with
// present.metal).
// Not compiled yet: needs dxc (see runtime/gpu/CMakeLists.txt). Register = binding, space = descriptor set, as plume
// maps them; SPIR-V vertex shaders are compiled with -fvk-invert-y.

[[vk::binding(0, 0)]] Texture2D<float4> g_image : register(t0, space0);
[[vk::binding(1, 0)]] SamplerState g_imageSampler : register(s1, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer g_pixels : register(u1, space0);
[[vk::binding(0, 0)]] Texture2D<float> g_depth : register(t0, space0);

// SPIR-V allows one push constant block: ReadbackRawCS reads its step from offset.x.
struct ResolveDepthConstants
{
    int2 offset;
};
[[vk::push_constant]] ConstantBuffer<ResolveDepthConstants> g_resolveDepth : register(b0, space1);

struct Interpolants
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// One triangle covering the viewport, which the caller sets to the letterboxed rectangle.
Interpolants PresentVS(uint vertexId : SV_VertexID)
{
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Interpolants output;
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PresentPS(Interpolants input) : SV_Target
{
    return float4(g_image.Sample(g_imageSampler, input.uv).rgb, 1.0);
}

// Host UI drawn over the image (gpu::SetOverlay), blended with its alpha.
float4 OverlayPS(Interpolants input) : SV_Target
{
    return g_image.Sample(g_imageSampler, input.uv);
}

// Packs the output into RGBA8 words (R in the low byte) for the CPU.
[numthreads(8, 8, 1)]
void ReadbackCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    g_image.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    uint4 c = uint4(round(saturate(g_image.Load(int3(id.xy, 0))) * 255.0));
    g_pixels.Store((id.y * width + id.x) * 4, c.r | (c.g << 8) | (c.b << 16) | (c.a << 24));
}

// The red channel's bits, unconverted, of one texel per `step` x `step` block, packed at the reduced size: depth
// resolves written back to guest memory, at the guest's resolution.
[numthreads(8, 8, 1)]
void ReadbackRawCS(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    g_image.GetDimensions(width, height);
    uint step = uint(g_resolveDepth.offset.x);
    width /= step;
    height /= step;
    if (id.x >= width || id.y >= height)
        return;
    g_pixels.Store((id.y * width + id.x) * 4, asuint(g_image.Load(int3(id.xy * step, 0)).r));
}

// EDRAM depth resolves: the depth under each pixel of the destination, read at the pixel plus `offset`, in the red
// channel. PresentVS covers the destination; the scissor limits the copy.
float4 ResolveDepthPS(Interpolants input) : SV_Target
{
    return float4(g_depth.Load(int3(int2(input.position.xy) + g_resolveDepth.offset, 0)), 0.0, 0.0, 0.0);
}
