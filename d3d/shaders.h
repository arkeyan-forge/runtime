#pragma once

#include <cstdint>

namespace d3d
{
    // A Direct3D shader object the game created, and the shader container (microcode + constant table) it came from.
    struct GuestShader
    {
        uint32_t container; // guest address of the container passed to Create*Shader
        uint32_t size;      // container size (virtual + physical parts)
        uint64_t hash;      // XXH3-64 of the container bytes: the key of the translated-shader cache (as XenosRecomp)
        bool vertex;
        uint32_t inputs;    // vertex shaders: the (usage, index) inputs it declares, one bit per InputLocation()
        uint32_t pulled;    // the inputs among them it fetches at a computed index instead of the vertex index
        uint32_t samplers;  // vertex shaders: the vertex texture samplers (registers 0-3, fetch constants 16-19) its constant table lists, one bit each
        uint32_t paramGen;  // pixel shaders: gpu::DrawDesc::paramGen, from the header (bit 31 = param_gen enabled, bits 0-7 = register)
    };

    // XenosRecomp's vertex input location for a declaration usage and index (its USAGE_LOCATIONS table, plus PSIZE),
    // or ~0u for usages it has no location for.
    uint32_t InputLocation(uint32_t usage, uint32_t usageIndex);

    // The container behind shader object `object`, or nullptr if the game did not create it through the hooked calls.
    const GuestShader* FindGuestShader(uint32_t object);

    uint64_t HashBytes(const void* data, uint64_t size);
}
