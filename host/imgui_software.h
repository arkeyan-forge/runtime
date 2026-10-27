#pragma once

// Dear ImGui's draw data rasterized on the CPU into an RGBA image, for the Toy Box on every platform (it used SDL's
// software renderer before, which UWP does not have). It follows that renderer's rules, which the Toy Box's drawing
// is tuned for: a pair of triangles forming an axis-aligned rectangle of one colour is drawn as a rectangle fill or a
// (scaled) copy of the texture, at whole pixels; other triangles are filled pixel by pixel with the vertex colours
// interpolated and the texture sampled at the nearest texel.
#include <imgui.h>
#include <cstdint>
#include <vector>

namespace imgui_software
{
    // RGBA8, R in the low byte (ImGui's colour order), `width` pixels a row.
    struct Texture
    {
        int width = 0;
        int height = 0;
        std::vector<uint32_t> pixels;
        bool blend = true;    // false: copied over the target with its alpha (opaque pictures)
        bool nearest = false; // false: scaled with bilinear filtering
    };

    inline ImTextureRef Ref(const Texture* texture)
    {
        return ImTextureRef(ImTextureID(intptr_t(texture)));
    }

    // After ImGui::CreateContext: declares the renderer to ImGui (textures it creates, large meshes).
    void Init();

    // Creates, updates and destroys ImGui's own textures (the font atlas), then draws every list into `target`.
    void Render(ImDrawData* data, Texture& target);
}
