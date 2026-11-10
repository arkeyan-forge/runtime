// FSR's inputs from the game's scene, and the clears of FSR's own resources, for D3D12 (DXIL) and Vulkan (SPIR-V); keep
// in sync with fsr_inputs.metal. Register = binding, space = descriptor set, as plume maps them (gpu/fsr/backend.cpp).

// InputsCS: the camera's motion of each pixel and its depth, from the scene's depth target. The motion is the pixel's
// position without this frame's jitter, at its depth, taken back to the previous frame's clip space by the camera's
// reprojection: previous - current, in texture coordinates. The depth is the normalised device depth (0 near, 1 far),
// whatever the viewport's depth range.
[[vk::binding(0, 0)]] Texture2D<float> g_sceneDepth : register(t0, space0);
// Vulkan: the views' formats (fsr.cpp), not the HLSL types' (DXC would declare rg32f and rgba32f).
[[vk::binding(1, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> g_motion : register(u1, space0);
[[vk::binding(2, 0)]] RWTexture2D<float> g_depth : register(u2, space0);

// One push constant block for all three (SPIR-V allows one): the clears read their value from reprojection[0] and the
// mip's size from size.
struct Constants
{
    float4 reprojection[4]; // rows: previous clip = reprojection x current clip
    float2 size;
    float2 jitter;          // in clip space
    float depthMin, depthScale;
    float invertDepth; // 1: the depth written is 1 - ndc z (a reversed projection), so that 0 is near for FSR
    float padding;
};
[[vk::push_constant]] ConstantBuffer<Constants> g_inputs : register(b0, space1);

[numthreads(8, 8, 1)]
void InputsCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(g_inputs.size.x) || id.y >= uint(g_inputs.size.y))
        return;
    float2 ndc = float2((float(id.x) + 0.5) / g_inputs.size.x * 2.0 - 1.0, 1.0 - (float(id.y) + 0.5) / g_inputs.size.y * 2.0);
    ndc -= g_inputs.jitter;
    // A depth outside the scene's range is another layer's (drawn in its own slice of the range, a HUD's portrait
    // [Alchemy]): it stays where it is, as the HUD does.
    const float unclamped = (g_sceneDepth.Load(int3(id.xy, 0)) - g_inputs.depthMin) * g_inputs.depthScale;
    const bool layer = unclamped < -1e-4 || unclamped > 1.0 + 1e-4;
    float z = saturate(unclamped);
    z = isfinite(z) ? z : 1.0;
    float4 current = float4(ndc, z, 1.0);
    float4 previous = float4(dot(g_inputs.reprojection[0], current), dot(g_inputs.reprojection[1], current),
        dot(g_inputs.reprojection[2], current), dot(g_inputs.reprojection[3], current));
    // Only finite motion within the picture reaches FSR (a NaN or an infinity would spread through its history: a
    // point near the camera plane, w close to 0, gives one).
    float2 delta = previous.w > 1e-6 ? previous.xy / previous.w - ndc : float2(0.0, 0.0);
    delta = all(isfinite(delta)) && !layer ? clamp(delta, -2.0, 2.0) : float2(0.0, 0.0);
    g_motion[id.xy] = delta * float2(0.5, -0.5);
    g_depth[id.xy] = g_inputs.invertDepth > 0.5 ? 1.0 - z : z;
}

// ClearFloatCS, ClearUintCS: one value in every texel of a mip (FSR's clear jobs). A uint target takes the value's bits,
// as Vulkan's clears do.
[[vk::binding(0, 0)]] RWTexture2D<float4> g_clearFloat : register(u0, space0);
[[vk::binding(0, 0)]] RWTexture2D<uint> g_clearUint : register(u0, space0);

[numthreads(8, 8, 1)]
void ClearFloatCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < uint(g_inputs.size.x) && id.y < uint(g_inputs.size.y))
        g_clearFloat[id.xy] = g_inputs.reprojection[0];
}

[numthreads(8, 8, 1)]
void ClearUintCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x < uint(g_inputs.size.x) && id.y < uint(g_inputs.size.y))
        g_clearUint[id.xy] = asuint(g_inputs.reprojection[0].x);
}

// SanitizeCS: a float scene's colour for FSR, without NaN or infinite values (FSR's history would spread them): NaN
// is 0, the rest clamped to half precision's range. Set 0 = { colour, its copy }.
[[vk::binding(0, 0)]] Texture2D<float4> g_sceneColor : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> g_cleanColor : register(u1, space0);

[numthreads(8, 8, 1)]
void SanitizeCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(g_inputs.size.x) || id.y >= uint(g_inputs.size.y))
        return;
    float4 c = g_sceneColor.Load(int3(id.xy, 0));
    g_cleanColor[id.xy] = clamp(any(isnan(c)) ? float4(0.0, 0.0, 0.0, 1.0) : c, 0.0, 65504.0);
}
