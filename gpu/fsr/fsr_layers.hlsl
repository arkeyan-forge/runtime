// InputsLayersCS for D3D12 (DXIL) and Vulkan (SPIR-V); keep in sync with fsr_inputs.metal. A file of its own: SPIR-V
// allows one push constant block per file, and this pass's (224 bytes) is larger than fsr_inputs.hlsl's.
[[vk::binding(0, 0)]] Texture2D<float> g_sceneDepth : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> g_motion : register(u1, space0);
[[vk::binding(2, 0)]] RWTexture2D<float> g_depth : register(u2, space0);

// InputsLayersCS (gpu::UpscaleDesc::motionLayers): as InputsCS, but each pixel gets, among the camera's motion and the
// scene's other motions (others: two more reprojections, otherCount of them set), the one that finds its colour in the
// previous frame's (five taps, luminance; summed over 8x8 tiles), the camera's unless another is clearly better; a depth outside the scene's
// range (the HUD's portrait and its mask) gets no motion and the nearest depth, so that nothing near it pulls it away.
// Set 0 = { depth, motion, depth out, colour, previous colour }.
struct LayersConstants
{
    float4 reprojection[4];
    float2 size;
    float2 jitter;
    float depthMin, depthScale;
    float invertDepth;
    float otherCount;
    float4 others[8]; // two matrices, rows
};
[[vk::push_constant]] ConstantBuffer<LayersConstants> g_layers : register(b0, space1);
[[vk::binding(3, 0)]] Texture2D<float4> g_colour : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> g_previousColour : register(t4, space0);

float Luma(float4 c)
{
    return dot(c.rgb, float3(0.299, 0.587, 0.114));
}

// previous - current, in clip units; false when the point goes behind the previous camera.
bool LayerMotion(float4 r0, float4 r1, float4 r2, float4 r3, float4 current, out float2 delta)
{
    float4 previous = float4(dot(r0, current), dot(r1, current), dot(r2, current), dot(r3, current));
    delta = previous.w > 1e-6 ? previous.xy / previous.w - current.xy : float2(0.0, 0.0);
    return previous.w > 1e-6 && all(isfinite(delta));
}

// How far the pixel's neighbourhood is from where `delta` takes it in the previous frame.
float LayerCost(uint2 id, float2 delta)
{
    const float2 size = g_layers.size;
    const float2 previous = float2(id) + 0.5 + delta * float2(0.5, -0.5) * size;
    if (any(previous < 0.0) || any(previous >= size))
        return 1e9;
    const int2 taps[5] = { int2(0, 0), int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2) };
    const int2 last = int2(size) - 1;
    float cost = 0.0;
    for (int i = 0; i < 5; i++)
    {
        const int2 here = clamp(int2(id) + taps[i], int2(0, 0), last);
        const int2 there = clamp(int2(previous) + taps[i], int2(0, 0), last);
        cost += abs(Luma(g_colour.Load(int3(here, 0))) - Luma(g_previousColour.Load(int3(there, 0))));
    }
    return cost;
}

// Chosen per 8x8 tile (the costs summed over it), not per pixel: a pixel alone may find its colour better with the
// wrong motion, and the interpolation turned such pixels into scattered shards.
groupshared float g_tileCost[64][3];
groupshared uint g_tileChoice;

[numthreads(8, 8, 1)]
void InputsLayersCS(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    const bool inside = id.x < uint(g_layers.size.x) && id.y < uint(g_layers.size.y);
    const uint2 pixel = min(id.xy, uint2(g_layers.size) - 1);
    float2 ndc = float2((float(pixel.x) + 0.5) / g_layers.size.x * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / g_layers.size.y * 2.0);
    ndc -= g_layers.jitter;
    const float unclamped = (g_sceneDepth.Load(int3(pixel, 0)) - g_layers.depthMin) * g_layers.depthScale;
    const bool layer = unclamped < -1e-4 || unclamped > 1.0 + 1e-4 || !isfinite(unclamped);
    const float z = saturate(isfinite(unclamped) ? unclamped : 1.0);
    const float4 current = float4(ndc, z, 1.0);
    float2 motions[3];
    bool valid[3];
    valid[0] = LayerMotion(g_layers.reprojection[0], g_layers.reprojection[1], g_layers.reprojection[2],
        g_layers.reprojection[3], current, motions[0]);
    if (!valid[0])
        motions[0] = float2(0.0, 0.0);
    motions[0] = clamp(motions[0], -2.0, 2.0);
    for (int k = 0; k < 2; k++)
    {
        valid[k + 1] = k < int(g_layers.otherCount) &&
                       LayerMotion(g_layers.others[k * 4], g_layers.others[k * 4 + 1], g_layers.others[k * 4 + 2],
                           g_layers.others[k * 4 + 3], current, motions[k + 1]);
        motions[k + 1] = valid[k + 1] ? clamp(motions[k + 1], -2.0, 2.0) : float2(0.0, 0.0);
    }
    // Pixels outside the picture or the scene (the HUD's layer) count for nothing, nor the sky (the far plane: an
    // animated texture no draw's motion describes, which made the tiles' edges show as steps).
    const bool sky = z > 0.9995;
    for (int c = 0; c < 3; c++)
        g_tileCost[index][c] =
            inside && !layer && !sky ? (valid[c] || c == 0 ? min(LayerCost(pixel, motions[c]), 15.0) : 1e6) : 0.0;
    GroupMemoryBarrierWithGroupSync();
    if (index == 0)
    {
        float sums[3] = { 0.0, 0.0, 0.0 };
        for (int t = 0; t < 64; t++)
            for (int c = 0; c < 3; c++)
                sums[c] += g_tileCost[t][c];
        uint choice = 0;
        for (int c = 1; c < 3; c++)
            if (sums[c] < sums[choice] * 0.75 - 0.5)
                choice = uint(c);
        g_tileChoice = choice;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!inside)
        return;
    if (layer)
    {
        g_motion[id.xy] = float2(0.0, 0.0);
        g_depth[id.xy] = 0.0;
        return;
    }
    // A pixel at a tile's edge that the camera's motion explains much better (the world beside a ship's mast) keeps it;
    // never another motion than the tile's: those made the shards.
    uint choice = sky ? 0 : g_tileChoice;
    if (choice != 0 && g_tileCost[index][0] < 0.8 * g_tileCost[index][choice])
        choice = 0;
    g_motion[id.xy] = motions[choice] * float2(0.5, -0.5);
    g_depth[id.xy] = g_layers.invertDepth > 0.5 ? 1.0 - z : z;
}
