// FSR's inputs from the game's scene, and the clears of FSR's own resources, for Metal (keep in sync with
// fsr_inputs.hlsl). plume binds descriptor set N as an argument buffer at [[buffer(N)]], each binding at [[id(binding)]],
// and the push constants at [[buffer(8)]].
#include <metal_stdlib>
using namespace metal;

struct InputsSet
{
    depth2d<float> sceneDepth [[id(0)]];
    texture2d<float, access::write> motion [[id(1)]];
    texture2d<float, access::write> depth [[id(2)]];
};

// Shared by the three kernels: the clears read their value from reprojection[0] and the mip's size from size.
struct Constants
{
    float4 reprojection[4]; // rows: previous clip = reprojection x current clip
    float2 size;
    float2 jitter; // in clip space
    float depthMin, depthScale;
    float invertDepth; // 1: the depth written is 1 - ndc z (a reversed projection), so that 0 is near for FSR
    float padding;
};

// The camera's motion of each pixel (previous - current, texture coordinates) and its normalised device depth.
kernel void InputsCS(uint2 id [[thread_position_in_grid]], constant InputsSet& set [[buffer(0)]],
    constant Constants& p [[buffer(8)]])
{
    if (id.x >= uint(p.size.x) || id.y >= uint(p.size.y))
        return;
    float2 ndc = float2((float(id.x) + 0.5) / p.size.x * 2.0 - 1.0, 1.0 - (float(id.y) + 0.5) / p.size.y * 2.0);
    ndc -= p.jitter;
    // A depth outside the scene's range is another layer's (drawn in its own slice of the range, a HUD's portrait
    // [Alchemy]): it stays where it is, as the HUD does.
    const float unclamped = (set.sceneDepth.read(id) - p.depthMin) * p.depthScale;
    const bool layer = unclamped < -1e-4 || unclamped > 1.0 + 1e-4;
    float z = saturate(unclamped);
    z = isfinite(z) ? z : 1.0;
    float4 current = float4(ndc, z, 1.0);
    float4 previous = float4(dot(p.reprojection[0], current), dot(p.reprojection[1], current),
        dot(p.reprojection[2], current), dot(p.reprojection[3], current));
    // Only finite motion within the picture reaches FSR (a NaN or an infinity would spread through its history: a
    // point near the camera plane, w close to 0, gives one).
    float2 delta = previous.w > 1e-6 ? previous.xy / previous.w - ndc : float2(0.0);
    delta = all(isfinite(delta)) && !layer ? clamp(delta, -2.0, 2.0) : float2(0.0);
    set.motion.write(float4(delta * float2(0.5, -0.5), 0.0, 0.0), id);
    set.depth.write(float4(p.invertDepth > 0.5 ? 1.0 - z : z, 0.0, 0.0, 0.0), id);
}

struct ClearFloatSet
{
    texture2d<float, access::write> target [[id(0)]];
};

struct ClearUintSet
{
    texture2d<uint, access::write> target [[id(0)]];
};

// One value in every texel of a mip (FSR's clear jobs); a uint target takes the value's bits, as Vulkan's clears do.
kernel void ClearFloatCS(uint2 id [[thread_position_in_grid]], constant ClearFloatSet& set [[buffer(0)]],
    constant Constants& p [[buffer(8)]])
{
    if (id.x < uint(p.size.x) && id.y < uint(p.size.y))
        set.target.write(p.reprojection[0], id);
}

kernel void ClearUintCS(uint2 id [[thread_position_in_grid]], constant ClearUintSet& set [[buffer(0)]],
    constant Constants& p [[buffer(8)]])
{
    if (id.x < uint(p.size.x) && id.y < uint(p.size.y))
        set.target.write(uint4(as_type<uint>(p.reprojection[0].x)), id);
}

// InputsLayersCS: see fsr_inputs.hlsl (keep in sync).
struct LayersSet
{
    depth2d<float> sceneDepth [[id(0)]];
    texture2d<float, access::write> motion [[id(1)]];
    texture2d<float, access::write> depth [[id(2)]];
    texture2d<float> colour [[id(3)]];
    texture2d<float> previousColour [[id(4)]];
};

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

static float Luma(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }

// previous - current, in clip units; false when the point goes behind the previous camera.
static bool LayerMotion(float4 r0, float4 r1, float4 r2, float4 r3, float4 current, thread float2& delta)
{
    const float4 previous = float4(dot(r0, current), dot(r1, current), dot(r2, current), dot(r3, current));
    delta = previous.w > 1e-6 ? previous.xy / previous.w - current.xy : float2(0.0);
    return previous.w > 1e-6 && all(isfinite(delta));
}

// How far the pixel's neighbourhood is from where `delta` takes it in the previous frame.
static float LayerCost(uint2 id, float2 delta, constant LayersSet& set, constant LayersConstants& p)
{
    const float2 previous = float2(id) + 0.5 + delta * float2(0.5, -0.5) * p.size;
    if (any(previous < 0.0) || any(previous >= p.size))
        return 1e9;
    const int2 taps[5] = { int2(0, 0), int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2) };
    const int2 last = int2(p.size) - 1;
    float cost = 0.0;
    for (int i = 0; i < 5; i++)
    {
        const uint2 here = uint2(clamp(int2(id) + taps[i], int2(0), last));
        const uint2 there = uint2(clamp(int2(previous) + taps[i], int2(0), last));
        cost += abs(Luma(set.colour.read(here)) - Luma(set.previousColour.read(there)));
    }
    return cost;
}

// Chosen per 8x8 tile (the costs summed over it), not per pixel: a pixel alone may find its colour better with the
// wrong motion, and the interpolation turned such pixels into scattered shards.
kernel void InputsLayersCS(uint2 id [[thread_position_in_grid]], uint index [[thread_index_in_threadgroup]],
    constant LayersSet& set [[buffer(0)]], constant LayersConstants& p [[buffer(8)]])
{
    threadgroup float tileCost[64][3];
    threadgroup uint tileChoice;
    const bool inside = id.x < uint(p.size.x) && id.y < uint(p.size.y);
    const uint2 pixel = min(id, uint2(p.size) - 1);
    float2 ndc = float2((float(pixel.x) + 0.5) / p.size.x * 2.0 - 1.0, 1.0 - (float(pixel.y) + 0.5) / p.size.y * 2.0);
    ndc -= p.jitter;
    const float unclamped = (set.sceneDepth.read(pixel) - p.depthMin) * p.depthScale;
    const bool layer = unclamped < -1e-4 || unclamped > 1.0 + 1e-4 || !isfinite(unclamped);
    const float z = saturate(isfinite(unclamped) ? unclamped : 1.0);
    const float4 current = float4(ndc, z, 1.0);
    float2 motions[3];
    bool valid[3];
    valid[0] = LayerMotion(p.reprojection[0], p.reprojection[1], p.reprojection[2], p.reprojection[3], current, motions[0]);
    if (!valid[0])
        motions[0] = float2(0.0);
    motions[0] = clamp(motions[0], -2.0, 2.0);
    for (int k = 0; k < 2; k++)
    {
        valid[k + 1] = k < int(p.otherCount) &&
                       LayerMotion(p.others[k * 4], p.others[k * 4 + 1], p.others[k * 4 + 2], p.others[k * 4 + 3], current,
                           motions[k + 1]);
        motions[k + 1] = valid[k + 1] ? clamp(motions[k + 1], -2.0, 2.0) : float2(0.0);
    }
    // Pixels outside the picture or the scene (the HUD's layer) count for nothing, nor the sky (the far plane: an
    // animated texture no draw's motion describes, which made the tiles' edges show as steps).
    const bool sky = z > 0.9995;
    for (int c = 0; c < 3; c++)
        tileCost[index][c] =
            inside && !layer && !sky ? (valid[c] || c == 0 ? min(LayerCost(pixel, motions[c], set, p), 15.0) : 1e6) : 0.0;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (index == 0)
    {
        float sums[3] = { 0.0, 0.0, 0.0 };
        for (int t = 0; t < 64; t++)
            for (int c = 0; c < 3; c++)
                sums[c] += tileCost[t][c];
        uint choice = 0;
        for (int c = 1; c < 3; c++)
            if (sums[c] < sums[choice] * 0.75 - 0.5)
                choice = uint(c);
        tileChoice = choice;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (!inside)
        return;
    if (layer)
    {
        set.motion.write(float4(0.0), id);
        set.depth.write(float4(0.0), id);
        return;
    }
    // A pixel at a tile's edge that the camera's motion explains much better (the world beside a ship's mast) keeps it;
    // never another motion than the tile's: those made the shards.
    uint choice = sky ? 0 : tileChoice;
    if (choice != 0 && tileCost[index][0] < 0.8 * tileCost[index][choice])
        choice = 0;
    set.motion.write(float4(motions[choice] * float2(0.5, -0.5), 0.0, 0.0), id);
    set.depth.write(float4(p.invertDepth > 0.5 ? 1.0 - z : z, 0.0, 0.0, 0.0), id);
}

struct SanitizeSet
{
    texture2d<float> sceneColor [[id(0)]];
    texture2d<float, access::write> cleanColor [[id(1)]];
};

// A float scene's colour for FSR, without NaN or infinite values (FSR's history would spread them): NaN is 0, the rest
// clamped to half precision's range.
kernel void SanitizeCS(uint2 id [[thread_position_in_grid]], constant SanitizeSet& set [[buffer(0)]],
    constant Constants& p [[buffer(8)]])
{
    if (id.x >= uint(p.size.x) || id.y >= uint(p.size.y))
        return;
    float4 c = set.sceneColor.read(id);
    set.cleanColor.write(clamp(any(isnan(c)) ? float4(0.0, 0.0, 0.0, 1.0) : c, 0.0, 65504.0), id);
}
