#pragma once

// Which textures the bindless heaps (gpu/draw.cpp) bind through a view of their own. plume's default view follows the
// resource: a cube map (six slices) or a stacked 2D array is seen as a 2D array on D3D12 and Vulkan, but the translated
// shaders declare the cube heap TextureCube and the 2D heap Texture2D. A read through a view of another type is
// undefined: NVIDIA (D3D12 and Vulkan) returned 0 from every cube map where AMD and Apple read its faces (Trap Team's
// title lost the logo's reflections, then its glow). A component swizzle needs its own view too.
#include <cstdint>

namespace gpu
{
    constexpr bool NeedsOwnView(bool cube, uint32_t arraySize, uint32_t swizzle)
    {
        return swizzle != 0 || cube || arraySize > 1;
    }
}
