#include "host/imgui_software.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace imgui_software
{
    namespace
    {
        struct Rect
        {
            int x0, y0, x1, y1; // [x0, x1) x [y0, y1)
        };

        struct Color
        {
            int r, g, b, a;
        };

        Color Unpack(uint32_t c) { return { int(c & 0xFF), int(c >> 8 & 0xFF), int(c >> 16 & 0xFF), int(c >> 24) }; }
        uint32_t Pack(Color c)
        {
            return uint32_t(c.r) | uint32_t(c.g) << 8 | uint32_t(c.b) << 16 | uint32_t(c.a) << 24;
        }

        // x * y / 255, rounded.
        int Mul(int x, int y)
        {
            int t = x * y + 128;
            return (t + (t >> 8)) >> 8;
        }

        Color Modulate(Color a, Color b) { return { Mul(a.r, b.r), Mul(a.g, b.g), Mul(a.b, b.b), Mul(a.a, b.a) }; }

        // Source over (straight alpha), or a plain write.
        void Put(uint32_t& pixel, Color s, bool blend)
        {
            if (!blend)
            {
                pixel = Pack(s);
                return;
            }
            if (s.a == 0)
                return;
            Color d = Unpack(pixel);
            int keep = 255 - s.a;
            pixel = Pack({ Mul(s.r, s.a) + Mul(d.r, keep), Mul(s.g, s.a) + Mul(d.g, keep),
                Mul(s.b, s.a) + Mul(d.b, keep), s.a + Mul(d.a, keep) });
        }

        Rect Intersect(Rect a, Rect b)
        {
            return { std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1) };
        }

        Color Texel(const Texture& texture, int x, int y)
        {
            return Unpack(texture.pixels[size_t(y) * size_t(texture.width) + size_t(x)]);
        }

        // Bilinear sample at texel coordinates (texel centres at whole numbers), clamped to `area`.
        Color Bilinear(const Texture& texture, const Rect& area, float x, float y)
        {
            x = std::clamp(x, float(area.x0), float(area.x1 - 1));
            y = std::clamp(y, float(area.y0), float(area.y1 - 1));
            int x0 = int(x), y0 = int(y);
            int x1 = std::min(x0 + 1, area.x1 - 1), y1 = std::min(y0 + 1, area.y1 - 1);
            int fx = int((x - x0) * 256), fy = int((y - y0) * 256);
            Color c[4] = { Texel(texture, x0, y0), Texel(texture, x1, y0), Texel(texture, x0, y1),
                Texel(texture, x1, y1) };
            auto mix = [&](int Color::* channel) {
                int top = (c[0].*channel) * (256 - fx) + (c[1].*channel) * fx;
                int bottom = (c[2].*channel) * (256 - fx) + (c[3].*channel) * fx;
                return (top * (256 - fy) + bottom * fy + 32768) >> 16;
            };
            return { mix(&Color::r), mix(&Color::g), mix(&Color::b), mix(&Color::a) };
        }

        void FillRect(Texture& target, const Rect& clip, Rect rect, Color color)
        {
            rect = Intersect(rect, clip);
            for (int y = rect.y0; y < rect.y1; y++)
            {
                uint32_t* row = target.pixels.data() + size_t(y) * size_t(target.width);
                for (int x = rect.x0; x < rect.x1; x++)
                    Put(row[x], color, true);
            }
        }

        // The texels `source` of `texture` onto the pixels `destination` of the target, modulated by `color`: one to
        // one when the sizes match, else scaled (texel centres mapped onto pixel centres).
        void Copy(Texture& target, const Rect& clip, const Texture& texture, Rect source, Rect destination, Color color)
        {
            source = Intersect(source, { 0, 0, texture.width, texture.height });
            int sw = source.x1 - source.x0, sh = source.y1 - source.y0;
            int dw = destination.x1 - destination.x0, dh = destination.y1 - destination.y0;
            if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
                return;
            Rect area = Intersect(destination, clip);
            bool scaled = sw != dw || sh != dh;
            float stepX = float(sw) / float(dw), stepY = float(sh) / float(dh);
            for (int y = area.y0; y < area.y1; y++)
            {
                uint32_t* row = target.pixels.data() + size_t(y) * size_t(target.width);
                float sy = source.y0 + (float(y - destination.y0) + 0.5f) * stepY;
                for (int x = area.x0; x < area.x1; x++)
                {
                    Color texel;
                    if (!scaled)
                        texel = Texel(texture, source.x0 + x - destination.x0, source.y0 + y - destination.y0);
                    else
                    {
                        float sx = source.x0 + (float(x - destination.x0) + 0.5f) * stepX;
                        if (texture.nearest)
                            texel = Texel(texture, std::min(int(sx), source.x1 - 1), std::min(int(sy), source.y1 - 1));
                        else
                            texel = Bilinear(texture, source, sx - 0.5f, sy - 0.5f);
                    }
                    Put(row[x], Modulate(texel, color), texture.blend);
                }
            }
        }

        float Edge(const ImVec2& a, const ImVec2& b, float x, float y)
        {
            return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        }

        // Pixels whose centre is inside, or on a top or left edge (shared edges are drawn once). As SDL does, the corners
        // are moved to whole pixels and their texture coordinates to whole texels first (truncated).
        void Triangle(Texture& target, const Rect& clip, const Texture* texture, const ImDrawVert* a,
            const ImDrawVert* b, const ImDrawVert* c)
        {
            ImVec2 p[3], t[3];
            Color colors[3];
            const ImDrawVert* corners[3] = { a, b, c };
            for (int i = 0; i < 3; i++)
            {
                p[i] = ImVec2(float(int(corners[i]->pos.x)), float(int(corners[i]->pos.y)));
                if (texture != nullptr)
                    t[i] = ImVec2(
                        float(int(corners[i]->uv.x * texture->width)), float(int(corners[i]->uv.y * texture->height)));
                colors[i] = Unpack(corners[i]->col);
            }
            float area = Edge(p[0], p[1], p[2].x, p[2].y);
            if (area == 0)
                return;
            if (area < 0)
            {
                std::swap(p[1], p[2]);
                std::swap(t[1], t[2]);
                std::swap(colors[1], colors[2]);
                area = -area;
            }
            Rect box = { int(std::min({ p[0].x, p[1].x, p[2].x })), int(std::min({ p[0].y, p[1].y, p[2].y })),
                int(std::max({ p[0].x, p[1].x, p[2].x })), int(std::max({ p[0].y, p[1].y, p[2].y })) };
            box = Intersect(box, clip);
            bool topLeft[3];
            for (int i = 0; i < 3; i++)
            {
                // Edge i is the one opposite corner i.
                const ImVec2& from = p[(i + 1) % 3];
                const ImVec2& to = p[(i + 2) % 3];
                topLeft[i] = (to.y == from.y && to.x > from.x) || to.y < from.y;
            }
            for (int y = box.y0; y < box.y1; y++)
            {
                uint32_t* row = target.pixels.data() + size_t(y) * size_t(target.width);
                float py = y + 0.5f;
                for (int x = box.x0; x < box.x1; x++)
                {
                    float px = x + 0.5f;
                    float w[3] = { Edge(p[1], p[2], px, py), Edge(p[2], p[0], px, py), Edge(p[0], p[1], px, py) };
                    if ((w[0] < 0 || (w[0] == 0 && !topLeft[0])) || (w[1] < 0 || (w[1] == 0 && !topLeft[1])) ||
                        (w[2] < 0 || (w[2] == 0 && !topLeft[2])))
                        continue;
                    // Relative to the last corner: exact when the corners agree (ImGui's white texel, plain colours).
                    float k0 = w[0] / area, k1 = w[1] / area;
                    auto lerp = [&](int Color::* channel) {
                        int last = colors[2].*channel;
                        return std::clamp(
                            int(last + (colors[0].*channel - last) * k0 + (colors[1].*channel - last) * k1 + 0.5f), 0,
                            255);
                    };
                    Color color = { lerp(&Color::r), lerp(&Color::g), lerp(&Color::b), lerp(&Color::a) };
                    if (texture != nullptr)
                    {
                        int tx = std::clamp(
                            int(t[2].x + (t[0].x - t[2].x) * k0 + (t[1].x - t[2].x) * k1), 0, texture->width - 1);
                        int ty = std::clamp(
                            int(t[2].y + (t[0].y - t[2].y) * k0 + (t[1].y - t[2].y) * k1), 0, texture->height - 1);
                        color = Modulate(Texel(*texture, tx, ty), color);
                    }
                    Put(row[x], color, texture == nullptr || texture->blend);
                }
            }
        }

        // Two triangles sharing two vertices, of one colour, that are right triangles with axis-aligned sides on the
        // diagonal from the second's top left to its bottom right corner: drawn as that rectangle, at whole pixels
        // (truncated position and size). The test is SDL's.
        bool Quad(Texture& target, const Rect& clip, const Texture* texture, const ImDrawVert* vertices,
            const ImDrawIdx* first, const ImDrawIdx* second)
        {
            int shared = 0;
            for (int i = 0; i < 3; i++)
                shared += int(second[i] == first[0] || second[i] == first[1] || second[i] == first[2]);
            if (shared != 2)
                return false;
            const ImDrawVert* v[3] = { &vertices[second[0]], &vertices[second[1]], &vertices[second[2]] };
            // The first corner at or above and left of both others (else the last), and likewise for the bottom right.
            auto pick = [&](auto before) {
                if (before(v[0], v[1]))
                    return before(v[0], v[2]) ? 0 : 2;
                return before(v[1], v[2]) ? 1 : 2;
            };
            int a = pick(
                [](const ImDrawVert* p, const ImDrawVert* q) { return p->pos.x <= q->pos.x && p->pos.y <= q->pos.y; });
            int b = pick(
                [](const ImDrawVert* p, const ImDrawVert* q) { return p->pos.x >= q->pos.x && p->pos.y >= q->pos.y; });
            int c = a != 0 && b != 0 ? 0 : a != 1 && b != 1 ? 1 : 2;
            const ImDrawVert* topLeft = v[a];
            const ImDrawVert* bottomRight = v[b];
            // The first triangle's first corner off that diagonal.
            int o = first[0] != second[a] && first[0] != second[b]   ? 0
                    : first[1] != second[a] && first[1] != second[b] ? 1
                                                                     : 2;
            const ImDrawVert* other = &vertices[first[o]];
            for (const ImDrawVert* third : { v[c], other })
            {
                if (!((topLeft->pos.x == third->pos.x && bottomRight->pos.y == third->pos.y) ||
                        (topLeft->pos.y == third->pos.y && bottomRight->pos.x == third->pos.x)))
                    return false;
            }
            if (v[0]->col != v[1]->col || v[0]->col != v[2]->col || v[0]->col != other->col)
                return false;
            if (texture != nullptr)
            {
                for (const ImDrawVert* corner : { v[0], v[1], v[2], other })
                {
                    if (corner->uv.x < 0 || corner->uv.x > 1)
                        return false;
                }
            }
            ImVec2 min = topLeft->pos, max = bottomRight->pos;
            Rect destination = { int(min.x), int(min.y), 0, 0 };
            destination.x1 = destination.x0 + int(max.x - min.x);
            destination.y1 = destination.y0 + int(max.y - min.y);
            Color color = Unpack(v[0]->col);
            if (texture != nullptr)
            {
                float sx = topLeft->uv.x * texture->width, sy = topLeft->uv.y * texture->height;
                float sw = bottomRight->uv.x * texture->width - sx, sh = bottomRight->uv.y * texture->height - sy;
                if (sw < 0 || sh < 0)
                    return false; // mirrored: as triangles
                if (sw != 0 && sh != 0)
                {
                    Rect source = { int(sx), int(sy), 0, 0 };
                    source.x1 = source.x0 + int(sw);
                    source.y1 = source.y0 + int(sh);
                    Copy(target, clip, *texture, source, destination, color);
                    return true;
                }
            }
            FillRect(target, clip, destination, color);
            return true;
        }

        void UpdateTexture(ImTextureData* data)
        {
            if (data->Status == ImTextureStatus_WantCreate)
            {
                auto* texture = new Texture{ data->Width, data->Height,
                    std::vector<uint32_t>(size_t(data->Width) * size_t(data->Height)) };
                memcpy(texture->pixels.data(), data->GetPixels(), texture->pixels.size() * 4);
                data->SetTexID(ImTextureID(intptr_t(texture)));
                data->SetStatus(ImTextureStatus_OK);
            }
            else if (data->Status == ImTextureStatus_WantUpdates)
            {
                auto* texture = reinterpret_cast<Texture*>(intptr_t(data->TexID));
                for (const ImTextureRect& r : data->Updates)
                {
                    for (int y = r.y; y < r.y + r.h; y++)
                        memcpy(&texture->pixels[size_t(y) * size_t(texture->width) + r.x], data->GetPixelsAt(r.x, y),
                            size_t(r.w) * 4);
                }
                data->SetStatus(ImTextureStatus_OK);
            }
            else if (data->Status == ImTextureStatus_WantDestroy)
            {
                delete reinterpret_cast<Texture*>(intptr_t(data->TexID));
                data->SetTexID(ImTextureID_Invalid);
                data->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    void Init()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "imgui_software";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    }

    void Render(ImDrawData* data, Texture& target)
    {
        if (data->Textures != nullptr)
        {
            for (ImTextureData* texture : *data->Textures)
            {
                if (texture->Status != ImTextureStatus_OK)
                    UpdateTexture(texture);
            }
        }
        const Rect bounds = { 0, 0, target.width, target.height };
        for (const ImDrawList* list : data->CmdLists)
        {
            for (const ImDrawCmd& command : list->CmdBuffer)
            {
                if (command.UserCallback != nullptr || command.ElemCount == 0)
                    continue;
                Rect clip = { int(command.ClipRect.x), int(command.ClipRect.y), 0, 0 };
                clip.x1 = clip.x0 + int(command.ClipRect.z - command.ClipRect.x);
                clip.y1 = clip.y0 + int(command.ClipRect.w - command.ClipRect.y);
                clip = Intersect(clip, bounds);
                const auto* texture = reinterpret_cast<const Texture*>(intptr_t(command.GetTexID()));
                const ImDrawVert* vertices = list->VtxBuffer.Data + command.VtxOffset;
                const ImDrawIdx* indices = list->IdxBuffer.Data + command.IdxOffset;
                // Triangles are paired as they come: a pair that makes a rectangle is drawn as one, otherwise the
                // first is drawn alone and the second waits for the next.
                const ImDrawIdx* pending = nullptr;
                for (unsigned i = 0; i < command.ElemCount; i += 3)
                {
                    const ImDrawIdx* triangle = indices + i;
                    if (pending == nullptr)
                    {
                        pending = triangle;
                        continue;
                    }
                    if (Quad(target, clip, texture, vertices, pending, triangle))
                    {
                        pending = nullptr;
                        continue;
                    }
                    Triangle(
                        target, clip, texture, &vertices[pending[0]], &vertices[pending[1]], &vertices[pending[2]]);
                    pending = triangle;
                }
                if (pending != nullptr)
                    Triangle(
                        target, clip, texture, &vertices[pending[0]], &vertices[pending[1]], &vertices[pending[2]]);
            }
        }
    }
}
