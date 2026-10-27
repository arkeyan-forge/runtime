// Point sprites on D3D12 (DXIL only): D3D12 draws points one pixel wide whatever their size, where Metal and Vulkan
// take the vertex shader's point size. This geometry shader turns each point into a square of that size, in host
// pixels, and writes the point coordinate the pixel shaders read with param_gen (0,0 top left, 1,1 bottom right, as
// Metal's point_coord). Its input is the output of a translated vertex shader that exports a point size and its output
// what the translated pixel shaders take (XenosRecomp, patches/xenosrecomp.patch): keep the three in the same order.

// The shared constants of the draw (gpu/draw.cpp SharedConstants): 1 / the viewport's size in host pixels.
cbuffer SharedConstants : register(b2, space4)
{
    float2 g_PointScale : packoffset(c19.x);
};

#define INTERPOLATORS(X) \
    X(texCoord0, TEXCOORD0) X(texCoord1, TEXCOORD1) X(texCoord2, TEXCOORD2) X(texCoord3, TEXCOORD3) \
    X(texCoord4, TEXCOORD4) X(texCoord5, TEXCOORD5) X(texCoord6, TEXCOORD6) X(texCoord7, TEXCOORD7) \
    X(texCoord8, TEXCOORD8) X(texCoord9, TEXCOORD9) X(texCoord10, TEXCOORD10) X(texCoord11, TEXCOORD11) \
    X(texCoord12, TEXCOORD12) X(texCoord13, TEXCOORD13) X(texCoord14, TEXCOORD14) X(texCoord15, TEXCOORD15) \
    X(color0, COLOR0) X(color1, COLOR1) X(normal0, NORMAL0)
#define DECLARE(name, semantic) float4 name : semantic;
#define COPY(name, semantic) vertex.name = input[0].name;

struct Point
{
    float4 position : SV_Position;
    INTERPOLATORS(DECLARE)
    float4 pointCoord : POINTCOORD;
    float pointSize : PSIZE;
};

struct Vertex
{
    float4 position : SV_Position;
    INTERPOLATORS(DECLARE)
    float4 pointCoord : POINTCOORD;
};

[maxvertexcount(4)]
void PointSpriteGS(point Point input[1], inout TriangleStream<Vertex> output)
{
    // The vertex shaders move killed points to (-2, -2, 0, 1) (XenosRecomp's kill vertex).
    if (input[0].pointSize <= 0.0 || all(input[0].position == float4(-2.0, -2.0, 0.0, 1.0)))
        return;

    Vertex vertex;
    INTERPOLATORS(COPY)
    float2 extent = input[0].pointSize * g_PointScale * input[0].position.w;
    // Top left, top right, bottom left, bottom right: clockwise on screen (the pipeline makes clockwise the front).
    const float2 corners[4] = { float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0) };
    for (uint i = 0; i < 4; i++)
    {
        vertex.position = float4(input[0].position.xy + corners[i] * extent, input[0].position.zw);
        vertex.pointCoord = float4(corners[i] * float2(0.5, -0.5) + 0.5, 0.0, 0.0);
        output.Append(vertex);
    }
}
