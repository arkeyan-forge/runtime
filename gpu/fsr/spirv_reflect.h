#pragma once

// What a compute shader binds, read from its SPIR-V: each resource variable's name, kind, set and binding, in the
// module's order, and the work group size. FSR's backend (gpu/fsr/backend.cpp) builds its descriptor sets from it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gpu::spirv
{
    enum class Kind : uint8_t
    {
        ConstantBuffer, // cbuffer
        Sampler,
        Texture,        // Texture2D (an SRV)
        StorageTexture, // RWTexture2D (a UAV)
        Buffer,         // StructuredBuffer and the like (an SRV)
        StorageBuffer,  // RWStructuredBuffer and the like (a UAV)
    };

    struct Binding
    {
        std::string name;
        Kind kind;
        uint32_t set, binding;
    };

    struct Module
    {
        std::vector<Binding> bindings;
        uint32_t localSize[3] = { 1, 1, 1 };
    };

    // False when the words are not SPIR-V.
    bool Reflect(const uint32_t* words, size_t count, Module& module);

    // Vulkan: the module with its storage images' formats made Unknown, and the two capabilities that allows. DXC gives
    // a storage image the format of its HLSL element type (float2: Rg32f), which is not the view's (FSR's motion is
    // RG16F): the validation layer then calls every load and store undefined. Unknown takes the view's format, as
    // Metal and D3D12 do; it needs shaderStorageImageReadWithoutFormat and WriteWithoutFormat. An image used by an
    // atomic keeps its format (Vulkan requires one there). Empty when the words are not SPIR-V.
    std::vector<uint32_t> WithoutStorageFormats(const uint32_t* words, size_t count);
}
