#include "host/toybox/draw.h"
#include "host/gamemenu.h"
#include "host/toybox/hit.h"
#include "host/collection/icons.h"
#include "host/embedded_fonts.h"
#include "host/glyphs.h"
#include "host/imgui_software.h"
#include "host/input.h"
#include "host/portal.h"
#include "host/portal_backend.h"
#include "host/toybox.h"
#include "host/toybox/look.h"
#include "host/toybox/trap_villain.h"
#include "language.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace collection;

namespace toybox
{
    namespace
    {
        // A swapper half's name without the catalogue's " (top)" / " (bottom)" (the sheet says Top: and Bottom: already).
        std::string WithoutHalf(std::string name)
        {
            for (const char* suffix : { " (top)", " (bottom)" })
            {
                size_t length = strlen(suffix);
                if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
                    name.erase(name.size() - length);
            }
            return name;
        }

        // The screens' colour names, on the shared palette (look.h, docs/runtime/host/ingame_ui.md).
        namespace Look
        {
            using namespace look::Colour;
            constexpr uint32_t ChipText = TextSoft;
            constexpr uint32_t Gold = Accent, GoldInk = AccentInk, GoldText = AccentText;
            constexpr uint32_t GoldBorder = 0x8A6A1A;
            constexpr uint32_t Night = 0x0A0C1A;
            constexpr uint32_t Panel = Surface, PanelBorder = Hairline, CardBorder = Hairline,
                               CardSelected = look::Colour::Focus;
            constexpr uint32_t Chip = Raised, KeyCap = Raised2, Button = Raised2;
            constexpr uint32_t Field = Well, Bar = Success, Waiting = Warning;
        }

        imgui_software::Texture* g_background;
        std::map<std::string, imgui_software::Texture*> g_baked;
        std::map<uint64_t, collection::Image> g_icons; // (type, variant, kind) -> 128x128 picture
        ImFont* g_bold;    // Outfit Bold: text
        ImFont* g_black;   // Outfit Black: labels, buttons
        ImFont* g_display; // titles and names: Outfit Black at display sizes (look.h's type scale)
        ImDrawList* g_draw;
        uint64_t g_frameTime;

        struct Hit
        {
            ImVec2 min, max;
            std::function<void()> left, right, twice;
            bool hover = true; // outlined under the pointer (not a backdrop that only catches clicks)
            std::string target; // script proof: card, F3 row or Commands cell, independent of keyboard focus
            std::function<void()> focus; // the pointer moving onto it moves the focus there (Pointer)
        };
        std::vector<Hit> g_hits;
        ImVec2 g_pointer; // the mouse in canvas pixels (Pointer)
        bool g_pointerIn;
        int g_hovered = -1; // the hit under it, as last computed

        // The topmost hit under the pointer that shows a hover outline, -1: none.
        int HoveredHit()
        {
            if (!g_pointerIn)
                return -1;
            int i = HitAt(std::span<const Hit>(g_hits), g_pointer.x, g_pointer.y);
            return i >= 0 && g_hits[size_t(i)].hover ? i : -1;
        }

        ImU32 Rgb(uint32_t rgb, int alpha = 255)
        {
            return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
        }

        ImVec4 Color(uint32_t rgb, float alpha = 1)
        {
            return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha);
        }

        // --- Primitives (tuned for the software rasteriser: baked round shapes, opaque rectangles as copies) ----------

        ImVec2 Measure(ImFont* font, float size, const std::string& text)
        {
            return font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
        }

        void Text(ImFont* font, float size, ImVec2 at, uint32_t rgb, const std::string& text, int alpha = 255)
        {
            g_draw->AddText(font, size, ImVec2(std::round(at.x), std::round(at.y)), Rgb(rgb, alpha), text.c_str());
        }

        void TextCentered(ImFont* font, float size, ImVec2 center, uint32_t rgb, const std::string& text)
        {
            ImVec2 extent = Measure(font, size, text);
            Text(font, size, ImVec2(center.x - extent.x / 2, center.y - extent.y / 2), rgb, text);
        }

        // Left-aligned, vertically centred on `centerY`.
        void TextMiddle(ImFont* font, float size, float x, float centerY, uint32_t rgb, const std::string& text)
        {
            Text(font, size, ImVec2(x, centerY - Measure(font, size, "Ag").y / 2), rgb, text);
        }

        std::string Fit(ImFont* font, float size, const std::string& text, float width)
        {
            if (Measure(font, size, text).x <= width)
                return text;
            std::string out = text;
            while (!out.empty() && Measure(font, size, out + "...").x > width)
            {
                out.pop_back();
                while (!out.empty() && (uint8_t(out.back()) & 0xC0) == 0x80)
                    out.pop_back();
            }
            return out + "...";
        }

        // Lines cut at spaces to fit `width`.
        std::vector<std::string> Wrap(
            ImFont* font, float size, const std::string& text, float width, size_t maxLines = 8)
        {
            std::vector<std::string> lines;
            std::string line;
            size_t at = 0;
            while (at < text.size())
            {
                size_t space = text.find(' ', at);
                std::string word = text.substr(at, space == std::string::npos ? std::string::npos : space - at);
                std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && Measure(font, size, candidate).x > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = candidate;
                at = space == std::string::npos ? text.size() : space + 1;
            }
            if (!line.empty())
                lines.push_back(line);
            if (lines.size() > maxLines)
            {
                lines.resize(maxLines);
                lines.back() = Fit(font, size, lines.back() + " ...", width);
            }
            return lines;
        }

        imgui_software::Texture* Bake(
            const std::string& key, int width, int height, const std::function<ImVec4(float, float)>& shade)
        {
            if (auto found = g_baked.find(key); found != g_baked.end())
                return found->second;
            auto* texture =
                new imgui_software::Texture{ width, height, std::vector<uint32_t>(size_t(width) * size_t(height)) };
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    ImVec4 color = shade(x + 0.5f - width / 2.0f, y + 0.5f - height / 2.0f);
                    const float channels[] = { color.x, color.y, color.z, color.w };
                    uint32_t pixel = 0;
                    for (int c = 0; c < 4; c++)
                        pixel |= uint32_t(uint8_t(std::clamp(channels[c], 0.0f, 1.0f) * 255 + 0.5f)) << (c * 8);
                    texture->pixels[size_t(y) * size_t(width) + size_t(x)] = pixel;
                }
            }
            g_baked[key] = texture;
            return texture;
        }

        imgui_software::Texture* Bake(
            const std::string& key, int size, const std::function<ImVec4(float, float)>& shade)
        {
            return Bake(key, size, size, shade);
        }

        float Coverage(float radius, float distance) { return std::clamp(radius - distance + 0.5f, 0.0f, 1.0f); }

        void Blit(imgui_software::Texture* texture, ImVec2 center, float size)
        {
            ImVec2 min(std::round(center.x - size / 2), std::round(center.y - size / 2));
            g_draw->AddImage(imgui_software::Ref(texture), min, ImVec2(min.x + size, min.y + size));
        }

        void Corners(imgui_software::Texture* disc, ImVec2 min, ImVec2 max, float r)
        {
            ImTextureRef texture = imgui_software::Ref(disc);
            g_draw->AddImage(texture, min, ImVec2(min.x + r, min.y + r), ImVec2(0, 0), ImVec2(0.5f, 0.5f));
            g_draw->AddImage(
                texture, ImVec2(max.x - r, min.y), ImVec2(max.x, min.y + r), ImVec2(0.5f, 0), ImVec2(1, 0.5f));
            g_draw->AddImage(
                texture, ImVec2(min.x, max.y - r), ImVec2(min.x + r, max.y), ImVec2(0, 0.5f), ImVec2(0.5f, 1));
            g_draw->AddImage(texture, ImVec2(max.x - r, max.y - r), max, ImVec2(0.5f, 0.5f), ImVec2(1, 1));
        }

        void Fill(ImVec2 min, ImVec2 max, uint32_t rgb, float alpha = 1)
        {
            if (alpha < 1)
            {
                g_draw->AddRectFilled(min, max, Rgb(rgb, int(alpha * 255 + 0.5f)));
                return;
            }
            char key[24];
            snprintf(key, sizeof(key), "solid %06X", rgb);
            imgui_software::Texture* solid = g_baked.count(key) != 0 ? g_baked[key] : nullptr;
            if (solid == nullptr)
            {
                solid = Bake(key, 4, [&](float, float) { return Color(rgb); });
                solid->blend = false;
                solid->nearest = true;
            }
            g_draw->AddImage(imgui_software::Ref(solid), min, max);
        }

        void RoundRect(ImVec2 min, ImVec2 max, uint32_t rgb, float radius, float alpha = 1)
        {
            min = ImVec2(std::round(min.x), std::round(min.y));
            max = ImVec2(std::round(max.x), std::round(max.y));
            int r =
                int(std::min({ std::round(radius), std::floor((max.x - min.x) / 2), std::floor((max.y - min.y) / 2) }));
            if (r <= 0)
            {
                Fill(min, max, rgb, alpha);
                return;
            }
            Fill(ImVec2(min.x, min.y + r), ImVec2(max.x, max.y - r), rgb, alpha);
            Fill(ImVec2(min.x + r, min.y), ImVec2(max.x - r, min.y + r), rgb, alpha);
            Fill(ImVec2(min.x + r, max.y - r), ImVec2(max.x - r, max.y), rgb, alpha);
            char key[40];
            snprintf(key, sizeof(key), "corner %d %06X %.2f", r, rgb, alpha);
            Corners(Bake(key, 2 * r,
                        [&](float dx, float dy) {
                            return Color(rgb, alpha * Coverage(float(r), std::sqrt(dx * dx + dy * dy)));
                        }),
                min, max, float(r));
        }

        void RoundFrame(ImVec2 min, ImVec2 max, uint32_t rgb, float radius, int thickness)
        {
            min = ImVec2(std::round(min.x), std::round(min.y));
            max = ImVec2(std::round(max.x), std::round(max.y));
            int r = int(std::round(radius));
            float t = float(thickness), fr = float(r);
            Fill(ImVec2(min.x + fr, min.y), ImVec2(max.x - fr, min.y + t), rgb);
            Fill(ImVec2(min.x + fr, max.y - t), ImVec2(max.x - fr, max.y), rgb);
            Fill(ImVec2(min.x, min.y + fr), ImVec2(min.x + t, max.y - fr), rgb);
            Fill(ImVec2(max.x - t, min.y + fr), ImVec2(max.x, max.y - fr), rgb);
            char key[40];
            snprintf(key, sizeof(key), "ring %d %d %06X", r, thickness, rgb);
            Corners(Bake(key, 2 * r,
                        [&](float dx, float dy) {
                            float distance = std::sqrt(dx * dx + dy * dy);
                            return Color(rgb, Coverage(fr, distance) * (1 - Coverage(fr - t, distance)));
                        }),
                min, max, fr);
        }

        // A box: fill, then a border of `thickness` inside its edge.
        void Box(ImVec2 min, ImVec2 max, uint32_t fill, uint32_t border, float radius, float thickness, float alpha = 1)
        {
            if (thickness > 0 && alpha >= 1)
                RoundRect(min, max, border, radius);
            else if (thickness > 0)
                RoundFrame(min, max, border, radius, int(thickness));
            float inset = std::max(thickness, 0.0f);
            RoundRect(ImVec2(min.x + inset, min.y + inset), ImVec2(max.x - inset, max.y - inset), fill,
                std::max(radius - inset, 0.0f), alpha);
        }

        // Focus, the one style of every screen: a white ring 3 px wide, outside the element (look.h). The selected seat is
        // gold, the pointer's hover a thin grey line on the element's edge.
        void FocusRing(ImVec2 min, ImVec2 max, float radius, uint32_t rgb = Look::FocusRing)
        {
            RoundFrame(ImVec2(min.x - 5, min.y - 5), ImVec2(max.x + 5, max.y + 5), rgb, radius + 5, 3);
        }

        // The same ring for rows 4 px apart (the F3 menu, Commands): 2 px wide, inside the gap.
        void DenseFocusRing(ImVec2 min, ImVec2 max, float radius)
        {
            RoundFrame(ImVec2(min.x - 3, min.y - 3), ImVec2(max.x + 3, max.y + 3), Look::FocusRing, radius + 3, 2);
        }

        // A row or card that takes the focus: lit and ringed when focused.
        void FocusBox(ImVec2 min, ImVec2 max, bool focused, float radius, uint32_t fill)
        {
            Box(min, max, focused ? Look::CardSelected : fill, Look::CardBorder, radius, 1);
            if (focused)
                FocusRing(min, max, radius);
        }

        void DashedCircle(ImVec2 center, float radius, uint32_t rgb, float thickness)
        {
            constexpr int Dashes = 12;
            for (int i = 0; i < Dashes; i++)
            {
                float start = 6.2831853f * i / Dashes;
                g_draw->PathArcTo(center, radius, start, start + 6.2831853f / Dashes * 0.55f, 4);
                g_draw->PathStroke(Rgb(rgb), ImDrawFlags_None, thickness);
            }
        }

        void Disc(ImVec2 center, int diameter, uint32_t rgb, float alpha = 1)
        {
            char key[48];
            snprintf(key, sizeof(key), "disc %d %06X %.2f", diameter, rgb, alpha);
            Blit(Bake(key, diameter,
                     [&](float dx, float dy) {
                         ImVec4 color = Color(rgb, alpha);
                         color.w *= Coverage(diameter / 2.0f, std::sqrt(dx * dx + dy * dy));
                         return color;
                     }),
                center, float(diameter));
        }

        void Ring(ImVec2 center, int diameter, uint32_t rgb, float thickness)
        {
            char key[48];
            snprintf(key, sizeof(key), "dring %d %06X %.1f", diameter, rgb, thickness);
            Blit(Bake(key, diameter + 4,
                     [&](float dx, float dy) {
                         float d = std::sqrt(dx * dx + dy * dy), r = diameter / 2.0f;
                         return Color(rgb, Coverage(r, d) * (1 - Coverage(r - thickness, d)));
                     }),
                center, float(diameter + 4));
        }

        // A five-pointed star (the fonts have no ★).
        void Star(ImVec2 center, float radius, uint32_t rgb)
        {
            ImVec2 points[10];
            for (int i = 0; i < 10; i++)
            {
                float angle = -1.5707963f + i * 3.14159265f / 5, r = i % 2 == 0 ? radius : radius * 0.45f;
                points[i] = ImVec2(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
            }
            for (int i = 0; i < 10; i += 2)
                g_draw->AddTriangleFilled(center, points[i], points[(i + 1) % 10], Rgb(rgb));
            for (int i = 1; i < 10; i += 2)
                g_draw->AddTriangleFilled(center, points[i], points[(i + 1) % 10], Rgb(rgb));
        }

        void AddHit(ImVec2 min, ImVec2 max, std::function<void()> left, std::function<void()> right = {},
            std::function<void()> twice = {})
        {
            // Scrolled cards are clipped when drawn; their pointer areas must stop at the same edge.
            if (g_draw != nullptr)
            {
                ImVec2 clipMin = g_draw->GetClipRectMin(), clipMax = g_draw->GetClipRectMax();
                min = ImVec2(std::max(min.x, clipMin.x), std::max(min.y, clipMin.y));
                max = ImVec2(std::max(min.x, std::min(max.x, clipMax.x)), std::max(min.y, std::min(max.y, clipMax.y)));
            }
            g_hits.push_back({ min, max, std::move(left), std::move(right), std::move(twice), true, {}, {} });
        }

        // --- Pictures -------------------------------------------------------------------------------------------------

        uint32_t ColourOf(const Toy* toy)
        {
            if (toy == nullptr)
                return 0x5E5A86;
            if (toy->category == Category::MagicItem || toy->category == Category::Trophy)
                return 0x1E8F82;
            if (toy->category == Category::Adventure)
                return 0xA8406F;
            return ElementRgb(toy->element);
        }

        std::map<std::string, collection::Image> g_artworks; // the figure sheet's artworks, by path

        const collection::Image& IconOf(const Toy* toy, VariantKind kind, uint16_t variant)
        {
            uint64_t key =
                uint64_t(toy != nullptr ? toy->type : 0xFFFF) << 24 | uint64_t(variant) << 8 | uint64_t(kind);
            auto found = g_icons.find(key);
            if (found == g_icons.end())
                found = g_icons.emplace(key, collection::Portrait(SSA_GAME_ID, toy, kind, 128, variant)).first;
            return found->second;
        }

        std::string Initials(const std::string& name)
        {
            std::string initials;
            bool next = true;
            for (char c : name)
            {
                if (initials.size() >= 2)
                    break;
                if (next && isalnum(uint8_t(c)))
                    initials += char(toupper(uint8_t(c)));
                next = c == ' ' || c == '-';
            }
            return initials;
        }

        // The round picture: the element colour, the disc's picture (or the drawn icon) over it, bilinear.
        void Portrait(
            const Toy* toy, VariantKind kind, ImVec2 center, int diameter, bool grey = false, uint16_t variant = 0)
        {
            char key[80];
            snprintf(key, sizeof(key), "portrait %u %u %d %d %d", toy != nullptr ? toy->type : 0xFFFF,
                unsigned(variant), int(kind), diameter, grey ? 1 : 0);
            const collection::Image& image = IconOf(toy, kind, variant);
            Blit(Bake(key, diameter,
                     [&](float dx, float dy) {
                         float radius = diameter / 2.0f;
                         ImVec4 color = Color(ColourOf(toy));
                         if (dx * dx + (dy + diameter * 0.07f) * (dy + diameter * 0.07f) > radius * radius)
                             color = ImVec4(color.x * 0.75f, color.y * 0.75f, color.z * 0.75f, 1);
                         if (!image.Empty())
                         {
                             float u =
                                 std::clamp((dx + radius) / diameter * image.width - 0.5f, 0.0f, image.width - 1.001f);
                             float v = std::clamp(
                                 (dy + radius) / diameter * image.height - 0.5f, 0.0f, image.height - 1.001f);
                             int x0 = int(u), y0 = int(v);
                             float fx = u - x0, fy = v - y0, texel[4] = {};
                             for (int c = 0; c < 4; c++)
                             {
                                 auto at = [&](int x, int y) {
                                     return float(image.rgba[size_t(y) * image.width + x] >> (c * 8) & 0xFF) / 255.0f;
                                 };
                                 texel[c] = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
                                            (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
                             }
                             float alpha = texel[3];
                             color.x = color.x * (1 - alpha) + texel[0] * alpha;
                             color.y = color.y * (1 - alpha) + texel[1] * alpha;
                             color.z = color.z * (1 - alpha) + texel[2] * alpha;
                         }
                         if (grey)
                         {
                             float l = (color.x + color.y + color.z) / 3 * 0.6f;
                             color = ImVec4(l, l, l, 1);
                         }
                         color.w = Coverage(radius, std::sqrt(dx * dx + dy * dy));
                         return color;
                     }),
                center, float(diameter));
            if (image.Empty() && toy != nullptr)
                TextCentered(g_display, diameter * 0.37f, center, 0xFFFFFF, Initials(toy->name));
        }

        void FigurePortrait(const Figure& f, ImVec2 center, int diameter, bool grey = false)
        {
            Portrait(f.toy, f.Kind(), center, diameter, grey, f.variant);
        }

        // A swapper's bottom half, small at the lower right of its top's picture (one card, two halves).
        void BottomBadge(const Model& model, const Figure& f, ImVec2 center, int diameter, bool grey = false)
        {
            collection::Card card = model.CardFor(f);
            if (!card.Swapper())
                return;
            int size = diameter * 4 / 10;
            ImVec2 at(center.x + diameter * 0.36f, center.y + diameter * 0.36f);
            g_draw->AddCircleFilled(at, size * 0.5f + 3, Rgb(Look::Night), 24);
            FigurePortrait(*card.second, at, size, grey);
        }

        bool PictureAt(const std::string& path, ImVec2 center, int box, bool backing);

        // The figure's best picture when it is an artwork (the game's large, not square picture of the variant): fitted into
        // a square of `box` pixels on a rounded backing. False when there is none (the round portrait is drawn instead).
        bool ArtworkPicture(const Toy* toy, uint16_t variant, ImVec2 center, int box, bool backing = true)
        {
            if (toy == nullptr)
                return false;
            std::string path = collection::BestPicturePath(SSA_GAME_ID, *toy, variant);
            if (path.find("/artworks/") == std::string::npos)
                return false;
            return PictureAt(path, center, box, backing);
        }

        // A picture file fitted into a square of `box` pixels at `center`, on a rounded backing. False when it is missing.
        bool PictureAt(const std::string& path, ImVec2 center, int box, bool backing)
        {
            if (path.empty())
                return false;
            auto found = g_artworks.find(path);
            if (found == g_artworks.end())
                found = g_artworks.emplace(path, collection::LoadPng(path)).first;
            const collection::Image& source = found->second;
            if (source.Empty())
                return false;
            float scale = std::min(float(box) / source.width, float(box) / source.height);
            int w = std::max(1, int(source.width * scale)), h = std::max(1, int(source.height * scale));
            if (backing)
                RoundRect(ImVec2(center.x - box / 2.0f, center.y - box / 2.0f),
                    ImVec2(center.x + box / 2.0f, center.y + box / 2.0f), Look::Field, 18);
            std::string key = path + " " + std::to_string(box);
            collection::Image fitted;
            if (!g_baked.count(key))
                fitted = collection::Resize(source, w, h);
            imgui_software::Texture* texture = Bake(key, w, h, [&](float dx, float dy) {
                uint32_t pixel = fitted.rgba[size_t(std::clamp(int(dy + h / 2.0f), 0, h - 1)) * w +
                                             size_t(std::clamp(int(dx + w / 2.0f), 0, w - 1))];
                return ImVec4((pixel & 0xFF) / 255.0f, (pixel >> 8 & 0xFF) / 255.0f, (pixel >> 16 & 0xFF) / 255.0f,
                    (pixel >> 24 & 0xFF) / 255.0f);
            });
            ImVec2 min(std::round(center.x - w / 2.0f), std::round(center.y - h / 2.0f));
            g_draw->AddImage(imgui_software::Ref(texture), min, ImVec2(min.x + w, min.y + h));
            return true;
        }

        std::map<std::string, collection::Image>
            g_stacks; // swappers' joined pictures (an empty one: the halves have none)

        // A swapper's two halves joined at the waist, as the game's swap screen (collection::SwapperPicture, the same
        // picture as Skylauncher's), fitted into a square of `box` pixels at `center`. False when a half has no such picture
        // from a disc: the caller draws the round portrait with the bottom small in a corner.
        bool SwapperStack(const Toy* top, uint16_t topVariant, const Toy* bottom, uint16_t bottomVariant, ImVec2 center,
            int box, bool grey = false)
        {
            if (top == nullptr || bottom == nullptr)
                return false;
            char key[96];
            snprintf(key, sizeof(key), "swapper %u %u %u %u", top->type, topVariant, bottom->type, bottomVariant);
            auto found = g_stacks.find(key);
            if (found == g_stacks.end())
            {
                found =
                    g_stacks.emplace(key, collection::SwapperPicture(*top, topVariant, *bottom, bottomVariant)).first;
                fprintf(stderr, "[toybox] swapper picture %s + %s: %s\n", top->name.c_str(), bottom->name.c_str(),
                    found->second.Empty() ? "no halves from the disc (round portrait)" : "joined");
            }
            const collection::Image& source = found->second;
            if (source.Empty())
                return false;
            float scale = std::min(float(box) / source.width, float(box) / source.height);
            int w = std::max(1, int(source.width * scale)), h = std::max(1, int(source.height * scale));
            std::string baked = std::string(key) + " " + std::to_string(box) + (grey ? " grey" : "");
            collection::Image fitted;
            if (!g_baked.count(baked))
                fitted = collection::Resize(source, w, h);
            imgui_software::Texture* texture = Bake(baked, w, h, [&](float dx, float dy) {
                uint32_t pixel = fitted.rgba[size_t(std::clamp(int(dy + h / 2.0f), 0, h - 1)) * w +
                                             size_t(std::clamp(int(dx + w / 2.0f), 0, w - 1))];
                ImVec4 color((pixel & 0xFF) / 255.0f, (pixel >> 8 & 0xFF) / 255.0f, (pixel >> 16 & 0xFF) / 255.0f,
                    (pixel >> 24 & 0xFF) / 255.0f);
                if (grey)
                {
                    float l = (color.x + color.y + color.z) / 3 * 0.6f;
                    color = ImVec4(l, l, l, color.w);
                }
                return color;
            });
            ImVec2 min(std::round(center.x - w / 2.0f), std::round(center.y - h / 2.0f));
            g_draw->AddImage(imgui_software::Ref(texture), min, ImVec2(min.x + w, min.y + h));
            return true;
        }

        // The bottom that goes with a swapper's top toy (the same version's when it has one), for the Add a figure screen.
        bool SwapperStackOfToy(const Toy* top, uint16_t variant, ImVec2 center, int box)
        {
            if (top == nullptr || top->category != Category::SwapTop)
                return false;
            const Toy* bottom = Catalogue::Get().Find(collection::MatchingHalf(top->type));
            uint16_t bottomVariant = 0;
            if (bottom != nullptr && variant != 0)
                for (const collection::Variant& candidate : bottom->variants)
                    if (candidate.id == variant)
                        bottomVariant = candidate.id;
            return SwapperStack(top, variant, bottom, bottomVariant, center, box);
        }

        // A figure's round portrait, a swapper's two halves joined instead (else its top with the bottom small in a corner).
        bool CardStack(const Model& model, const Figure& f, ImVec2 center, int box, bool grey = false)
        {
            collection::Card card = model.CardFor(f);
            return card.Swapper() && SwapperStack(card.first->toy, card.first->variant, card.second->toy,
                                         card.second->variant, center, box, grey);
        }

        void CardPortrait(const Model& model, const Figure& f, ImVec2 center, int diameter, bool grey = false)
        {
            if (CardStack(model, f, center, diameter + 14, grey))
                return;
            FigurePortrait(f, center, diameter, grey);
            BottomBadge(model, f, center, diameter, grey);
        }

        // The red cross of a figure that ran out of health, over its picture (the card's and the seat's).
        void OutOfHealthCross(ImVec2 center, float half)
        {
            const float width = std::max(3.0f, half / 3.0f);
            g_draw->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                IM_COL32(0, 0, 0, 120), width + 3);
            g_draw->AddLine(ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half),
                IM_COL32(0, 0, 0, 120), width + 3);
            g_draw->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
                Rgb(Look::Danger), width);
            g_draw->AddLine(ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half),
                Rgb(Look::Danger), width);
        }

        void HeroPortrait(const Model& model, const Figure& f, ImVec2 center, int diameter)
        {
            if (collection::Card card = model.CardFor(f);
                card.Swapper() && card.first->toy != nullptr && card.second->toy != nullptr &&
                !collection::HalfPicturePath(*card.first->toy, card.first->variant).empty() &&
                !collection::HalfPicturePath(*card.second->toy, card.second->variant).empty())
            {
                RoundRect(ImVec2(center.x - diameter / 2.0f - 12, center.y - diameter / 2.0f - 12),
                    ImVec2(center.x + diameter / 2.0f + 12, center.y + diameter / 2.0f + 12), Look::Field, 18);
                if (CardStack(model, f, center, diameter + 8))
                    return;
            }
            if (ArtworkPicture(f.toy, f.variant, center, diameter + 24))
            {
                BottomBadge(model, f, center, diameter);
                return;
            }
            int size = diameter + 2 * 40;
            Blit(Bake("halo " + std::to_string(diameter), size,
                     [&](float dx, float dy) {
                         float distance = std::sqrt(dx * dx + dy * dy), ring = diameter / 2.0f + 5;
                         float glow = 0.43f * std::clamp(1 - (distance - ring) / 32, 0.0f, 1.0f);
                         return Color(Look::Gold, std::max(Coverage(ring, distance), glow * glow / 0.43f));
                     }),
                center, float(size));
            FigurePortrait(f, center, diameter);
            BottomBadge(model, f, center, diameter);
        }

        void EmptySeatPicture(ImVec2 center, int diameter)
        {
            DashedCircle(center, diameter / 2.0f - 2, Look::Control, 2.5f);
            TextCentered(g_display, diameter * 0.45f, center, Look::TextDim, "+");
        }

        void Pill(ImVec2 center, const std::string& text, uint32_t fill, uint32_t ink = Look::Night, float size = 13)
        {
            float width = Measure(g_black, size, text).x + 20, height = size + 11;
            RoundRect(ImVec2(center.x - width / 2, center.y - height / 2),
                ImVec2(center.x + width / 2, center.y + height / 2), fill, height / 2);
            TextCentered(g_black, size, center, ink, text);
        }

        // A chevron drawn with two lines (no font glyph): dir 1 down, -1 up, 0 right, 2 left; `size` is its width.
        void Chevron(ImVec2 center, float size, uint32_t rgb, int dir)
        {
            float h = size / 2, r = size / 4;
            ImVec2 points[3];
            if (dir == 0 || dir == 2)
            {
                float side = dir == 0 ? r : -r;
                points[0] = ImVec2(center.x - side, center.y - h);
                points[1] = ImVec2(center.x + side, center.y);
                points[2] = ImVec2(center.x - side, center.y + h);
            }
            else
            {
                float rise = dir > 0 ? r : -r;
                points[0] = ImVec2(center.x - h, center.y - rise);
                points[1] = ImVec2(center.x, center.y + rise);
                points[2] = ImVec2(center.x + h, center.y - rise);
            }
            g_draw->AddPolyline(points, 3, Rgb(rgb), ImDrawFlags_None, std::max(2.0f, size / 5));
        }

        void GoldButton(ImVec2 min, ImVec2 max, const std::string& label, float size, bool focused)
        {
            RoundRect(min, max, Look::Gold, look::Radius::Control);
            TextCentered(g_display, size, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), Look::GoldInk,
                Fit(g_display, size, label, max.x - min.x - 16));
            if (focused)
                FocusRing(min, max, look::Radius::Control);
        }

        void PlainButton(ImVec2 min, ImVec2 max, const std::string& label, float size, bool focused,
            uint32_t text = Look::Text, uint32_t fill = Look::Button, uint32_t border = Look::Hairline)
        {
            Box(min, max, fill, border, look::Radius::Control, 1);
            TextCentered(g_black, size, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), text,
                Fit(g_black, size, label, max.x - min.x - 16));
            if (focused)
                FocusRing(min, max, look::Radius::Control);
        }

        // A screen's title: white on a short gold mark (the type scale's Title at 28).
        float Title(const std::string& text, ImVec2 at, float size)
        {
            float height = Measure(g_display, size, "Ag").y;
            RoundRect(ImVec2(at.x, at.y + height * 0.2f), ImVec2(at.x + 5, at.y + height * 0.85f), Look::Gold, 2);
            Text(g_display, size, ImVec2(at.x + 15, at.y), Look::Text, text);
            return Measure(g_display, size, text).x + 15;
        }

        void Panel(ImVec2 min, ImVec2 max, float alpha = 1)
        {
            Box(min, max, Look::Panel, Look::PanelBorder, look::Radius::Panel, 1, alpha);
        }

        // --- Button hints ---------------------------------------------------------------------------------------------

        bool KeyboardStyle() { return glyphs::Current() == glyphs::Style::Keyboard; }

        float KeyCap(ImVec2 at, float height, float size, const std::string& label)
        {
            float width = std::max(Measure(g_black, size, label).x + 14, height + 4);
            Box(at, ImVec2(at.x + width, at.y + height), Look::KeyCap, Look::Control, look::Radius::Key, 1);
            TextCentered(g_black, size, ImVec2(at.x + width / 2, at.y + height / 2), Look::Text, label);
            return width;
        }

        struct HintItem
        {
            char button; // A B X Y, L (LB/RB), V (View), S (Start), F (Start / F4: Filters)
            std::string text;
        };

        // Width of one hint's button part.
        float HintButton(char button, float x, float centerY, bool draw)
        {
            struct Face
            {
                char letter;
                uint32_t fill, ink;
            };
            static const Face Faces[] = { { 'A', 0x3FB950, 0x06240F }, { 'X', 0x2F81F7, 0x04163A },
                { 'Y', 0xE3B341, 0x2E2000 }, { 'B', 0xF85149, 0x3A0603 } };
            if (KeyboardStyle())
            {
                const std::string key = button == 'A'   ? "Enter"
                                        : button == 'B' ? "Esc"
                                        : button == 'X' ? "F2"
                                        : button == 'Y' ? "F3"
                                        : button == 'L' ? "PgUp PgDn"
                                        : button == 'V' ? ToyboxWheelKeyName()
                                        : button == 'F' ? "F4"
                                                        : "Enter";
                float width = std::max(Measure(g_black, 12, key).x + 14, 28.0f);
                if (draw)
                    KeyCap(ImVec2(x, centerY - 13), 26, 12, key);
                return width;
            }
            if (button == 'L' || button == 'V' || button == 'S' || button == 'F')
            {
                const char* label = button == 'L'   ? "LB  RB"
                                    : button == 'V' ? (glyphs::Current() == glyphs::Style::Xbox360 ? "Back" : "View")
                                                    : (glyphs::Current() == glyphs::Style::Xbox360 ? "Start" : "Menu");
                float width = Measure(g_black, 12, label).x + 16;
                if (draw)
                {
                    RoundRect(ImVec2(x, centerY - 12), ImVec2(x + width, centerY + 12), Look::KeyCap, 12);
                    TextCentered(g_black, 12, ImVec2(x + width / 2, centerY), Look::Text, label);
                }
                return width;
            }
            if (draw)
            {
                for (const Face& face : Faces)
                {
                    if (face.letter == button)
                    {
                        Disc(ImVec2(x + 13, centerY), 26, face.fill);
                        TextCentered(g_black, 14, ImVec2(x + 13, centerY), face.ink, std::string(1, face.letter));
                    }
                }
            }
            return 26;
        }

        // Right-aligned on `right`; returns the left edge.
        float Hints(float right, float centerY, const std::vector<HintItem>& hints)
        {
            float width = 0;
            for (const HintItem& hint : hints)
                width += HintButton(hint.button, 0, 0, false) + 6 + Measure(g_bold, 14, hint.text).x + 20;
            float x = right - width + 20;
            float left = x;
            for (const HintItem& hint : hints)
            {
                x += HintButton(hint.button, x, centerY, true) + 6;
                TextMiddle(g_bold, 14, x, centerY, Look::TextSoft, hint.text);
                x += Measure(g_bold, 14, hint.text).x + 20;
            }
            return left;
        }

        bool g_under; // drawing the screen under an overlay: no footer

        void Footer(const std::vector<HintItem>& hints)
        {
            if (!g_under)
                Hints(1248, 692, hints);
        }

        // --- Shared parts -----------------------------------------------------------------------------------------------

        void Header(const std::string& title, const std::string& subtitle)
        {
            float width = Title(title, ImVec2(32, 22), 34);
            if (!subtitle.empty())
                TextMiddle(g_bold, 16, 32 + width + 16, 42, Look::TextDim, subtitle);
        }

        // The toast (look.h): a card at `centerX`, at most `maxWidth` wide, resting on `edge` (its top when `fromTop`, else
        // its bottom). Over the game it slides in from that edge and fades; inside the Toy Box's screens it simply shows.
        // A notice is a gold dot and up to two lines; an achievement has its picture and a caption line over its name. The
        // lines are cut once per message, so that a frame of the slide does not allocate.
        struct ToastLayout
        {
            std::string text, caption;
            float maxWidth = 0;
            std::vector<std::string> lines;
            float width = 0, height = 0;
        };
        ToastLayout g_toastLayout;
        imgui_software::Texture g_toastPicture{ 64, 64, {} };
        uint32_t g_toastPictureSerial = 0;

        void DrawToast(const Model& model, float centerX, float maxWidth, float edge, bool fromTop, bool moving)
        {
            using namespace look;
            if (model.toast.empty())
                return;
            Motion motion = moving ? ToastMotion(SDL_GetTicksNS(), model.toastSince, model.toastUntil,
                                         gamemenu::Current().reduceMotion)
                                   : Motion{};
            if (motion.alpha <= 0)
                return;
            const bool achievement = !model.toastCaption.empty();
            const bool picture = achievement && !model.toastPicture.Empty();
            const float textSize = achievement ? Type::Label : Type::Body, lineHeight = textSize + 6;
            const float left = Space::L + (picture ? 64 + Space::M : achievement ? 0 : 10 + Space::M);
            ToastLayout& layout = g_toastLayout;
            if (layout.text != model.toast || layout.caption != model.toastCaption || layout.maxWidth != maxWidth)
            {
                layout.text = model.toast;
                layout.caption = model.toastCaption;
                layout.maxWidth = maxWidth;
                layout.lines = Wrap(g_black, textSize, model.toast, maxWidth - left - Space::L, 2);
                float textWidth = achievement ? Measure(g_black, Type::Caption, model.toastCaption).x : 0;
                for (const std::string& line : layout.lines)
                    textWidth = std::max(textWidth, Measure(g_black, textSize, line).x);
                layout.width = std::clamp(left + textWidth + Space::L, 220.0f, maxWidth);
                float block = layout.lines.size() * lineHeight + (achievement ? Type::Caption + Space::S : 0);
                layout.height = std::max(picture ? 64 + 2 * Space::M : 48, block + 2 * Space::M);
            }
            if (picture && g_toastPictureSerial != model.toastSerial)
            {
                g_toastPictureSerial = model.toastSerial;
                collection::Image fitted = collection::Resize(model.toastPicture, 64, 64);
                g_toastPicture.pixels = std::move(fitted.rgba);
            }
            // Baked corners are kept per alpha: steps of 1/16 keep them few.
            const float alpha = std::round(motion.alpha * 16) / 16;
            const float y = fromTop ? edge - motion.offset : edge - layout.height + motion.offset;
            ImVec2 min(std::round(centerX - layout.width / 2), std::round(y)),
                max(min.x + std::round(layout.width), min.y + std::round(layout.height));
            RoundRect(min, max, Look::Surface, Radius::Card, 0.96f * alpha);
            if (model.open)
            {
                AddHit(min, max, {});
                g_hits.back().hover = false;
            }
            const int ink = int(alpha * 255 + 0.5f);
            float textY = (min.y + max.y) / 2 - (layout.lines.size() * lineHeight) / 2;
            if (picture)
            {
                ImVec2 at(min.x + Space::L, (min.y + max.y) / 2 - 32);
                g_draw->AddImage(imgui_software::Ref(&g_toastPicture), at, ImVec2(at.x + 64, at.y + 64), ImVec2(0, 0),
                    ImVec2(1, 1), IM_COL32(255, 255, 255, ink));
            }
            if (achievement)
            {
                float top = (min.y + max.y) / 2 - (Type::Caption + Space::S + layout.lines.size() * lineHeight) / 2;
                Text(g_black, Type::Caption, ImVec2(min.x + left, top), Look::AccentText, model.toastCaption, ink);
                textY = top + Type::Caption + Space::S;
            }
            else
                Disc(ImVec2(min.x + Space::L + 5, (min.y + max.y) / 2), 10, Look::Accent, alpha);
            for (const std::string& line : layout.lines)
            {
                Text(g_black, textSize, ImVec2(min.x + left, textY + 3), Look::Text, line, ink);
                textY += lineHeight;
            }
        }

        // The section names as a strip, the current one lit; scrolled so that it shows.
        std::string StripLabel(Model& model, Grid& grid, const ListSection& section)
        {
            if (section.kind == Section::Kind::Foreign)
                return L("Not for this game", "Pas pour ce jeu");
            if (&grid == &model.add && section.kind == Section::Kind::Category)
                return CategoryLabel(Category(section.value));
            return model.SectionLabel(section);
        }

        void SectionStrip(Model& model, Grid& grid, ImVec2 at, float width)
        {
            if (grid.sections.empty())
                return;
            int current = grid.cells.empty() ? 0 : grid.cells[grid.cursor].section;
            std::vector<float> widths;
            for (const ListSection& section : grid.sections)
                widths.push_back(Measure(g_black, 13, StripLabel(model, grid, section)).x + 22);
            float offset = 0, before = 0;
            for (int i = 0; i < current; i++)
                before += widths[i] + 6;
            if (before + widths[current] > width - 40)
                offset = before + widths[current] - (width - 40);
            float x = at.x - offset;
            for (size_t i = 0; i < grid.sections.size(); i++)
            {
                std::string label = StripLabel(model, grid, grid.sections[i]);
                if (x + widths[i] > at.x && x < at.x + width)
                {
                    bool lit = int(i) == current;
                    ImVec2 min(x, at.y), max(x + widths[i], at.y + 26);
                    if (min.x >= at.x && max.x <= at.x + width)
                    {
                        RoundRect(min, max, lit ? Look::Gold : Look::Chip, 13);
                        TextCentered(g_black, 13, ImVec2((min.x + max.x) / 2, at.y + 13),
                            lit ? Look::GoldInk : Look::ChipText, label);
                        int first = -1;
                        for (size_t c = 0; c < grid.cells.size() && first < 0; c++)
                        {
                            if (grid.cells[c].section == int(i))
                                first = int(c);
                        }
                        Grid* target = &grid;
                        AddHit(min, max, [&model, target, first] {
                            if (first >= 0)
                            {
                                if (target == &model.add)
                                {
                                    if (model.add.cursor != first)
                                        model.FocusAddCell(first); // the versions of the new toy
                                    return;
                                }
                                target->cursor = first;
                                model.Touch();
                                if (target == &model.list)
                                    model.FocusCell(first);
                            }
                        });
                    }
                }
                x += widths[i] + 6;
            }
        }

        // One figure's card, the same as Skylauncher's: picture, name, level for this game, star, seat tag, lock.
        void Card(Model& model, const Figure& f, ImVec2 a, ImVec2 b, bool cursor, bool grey)
        {
            float width = b.x - a.x, cx = (a.x + b.x) / 2;
            Box(a, b, cursor ? Look::CardSelected : Look::Raised, cursor ? Look::CardSelected : Look::CardBorder,
                look::Radius::Card, 1);
            CardPortrait(model, f, ImVec2(cx, a.y + 54), 80, grey);
            if (model.OutOfHealth(f))
                OutOfHealthCross(ImVec2(cx, a.y + 54), 26);
            TextCentered(g_black, 15, ImVec2(cx, a.y + 110), grey ? Look::TextDimmer : Look::Text,
                Fit(g_black, 15, model.FigureName(f), width - 16));
            std::string line = model.LevelLine(f);
            if (!line.empty())
                TextCentered(g_bold, 12, ImVec2(cx, a.y + 128), model.CardBusy(f) ? Look::DangerText : Look::TextDim,
                    Fit(g_bold, 12, line, width - 16));
            if (f.favourite)
                Star(ImVec2(a.x + 18, a.y + 18), 9, Look::Gold);
            if (int on = model.SeatOf(f.path); on >= 0)
            {
                // P1, P2; "On" for the shared seats (their names are too long for a tag).
                std::string tag = model.seats[on].seat.player > 0 ? model.SeatTag(on) : L("On", "Posé");
                float w = Measure(g_black, 11, tag).x + 20;
                Pill(ImVec2(b.x - 8 - w / 2, a.y + 17), tag, Look::Bar, Look::Night, 11);
            }
            else if (model.CardBusy(f))
            {
                // A padlock.
                ImVec2 lock(b.x - 26, a.y + 10);
                g_draw->AddRect(ImVec2(lock.x + 3, lock.y), ImVec2(lock.x + 13, lock.y + 10), Rgb(Look::DangerText),
                    4.0f, ImDrawFlags_None, 2.0f);
                Fill(ImVec2(lock.x, lock.y + 7), ImVec2(lock.x + 16, lock.y + 18), Look::DangerText);
            }
        }

        // Your figures for the seat: the shelf (no header: the line above the grid says what it is for), then the
        // unknown toys and the folded "Not for this game"; `focused` draws the cursor.
        void DrawList(Model& model, ImVec2 min, ImVec2 max, bool focused, bool wheelList)
        {
            Grid& grid = model.list;
            grid.visibleHeight = int(max.y - min.y);
            float width = max.x - min.x, gap = 12;
            float cardWidth = (width - gap * (Model::ListColumns - 1)) / Model::ListColumns,
                  cardHeight = float(grid.rowHeight - 12);
            if (!wheelList)
            {
                model.gridSpans.clear();
                for (int c = 0; c < Model::ListColumns; c++)
                    model.gridSpans.push_back(
                        { min.x + c * (cardWidth + gap), min.x + c * (cardWidth + gap) + cardWidth });
            }
            // The cursor's ring is drawn after the clip, whole: FixGrid keeps its card in view.
            ImVec2 ringMin, ringMax;
            float ringRadius = -1;
            g_draw->PushClipRect(ImVec2(min.x - 8, min.y), ImVec2(max.x + 8, max.y), true);
            for (size_t s = 0; s < grid.sections.size(); s++)
            {
                const ListSection& section = grid.sections[s];
                float y = min.y + grid.HeaderY(int(s)) - grid.scroll;
                if (section.titled && y + grid.headerHeight >= min.y - 40 && y < max.y &&
                    section.kind != Section::Kind::Foreign)
                    TextMiddle(g_black, 17, min.x, y + 18, section.foreign ? Look::TextDimmer : Look::Gold,
                        model.SectionLabel(section));
            }
            const SeatState& seat = model.seats[wheelList ? model.wheel.seat : model.seat];
            for (size_t i = 0; i < grid.cells.size(); i++)
            {
                const Cell& cell = grid.cells[i];
                float x = min.x + cell.column * (cardWidth + gap);
                float y = min.y + grid.RowY(cell.row) - grid.scroll;
                if (y + cardHeight < min.y || y > max.y)
                    continue;
                ImVec2 a(x, y), b(x + cardWidth, y + cardHeight);
                bool cursor = focused && int(i) == grid.cursor;
                const ListSection& section = grid.sections[cell.section];
                if (cell.kind == Cell::Kind::Fold)
                {
                    ImVec2 fb(min.x + width, y + 40);
                    Box(a, fb, Look::Chip, Look::CardBorder, 12, 1);
                    std::string label = model.SectionLabel(section) + "   " +
                                        (section.folded ? L("A: open", "A : ouvrir") : L("A: fold", "A : replier"));
                    TextMiddle(g_black, 15, a.x + 16, y + 20, Look::TextDim, label);
                    if (cursor)
                    {
                        ringMin = a;
                        ringMax = fb;
                        ringRadius = 12;
                    }
                    AddHit(a, fb, [&model, i] { model.ActivateCell(int(i)); });
                    g_hits.back().focus = [&model, i] { model.FocusCell(int(i), false); };
                    continue;
                }
                if (cell.kind == Cell::Kind::Add)
                {
                    Box(a, b, Look::Night, Look::CardBorder, 16, 1, 0.6f);
                    EmptySeatPicture(ImVec2(x + cardWidth / 2, y + 54), 64);
                    TextCentered(
                        g_black, 15, ImVec2(x + cardWidth / 2, y + 110), Look::TextDim, L("Add a figure", "Ajouter"));
                    if (cursor)
                    {
                        ringMin = a;
                        ringMax = b;
                        ringRadius = 16;
                    }
                    AddHit(
                        a, b, [&model, i] { model.FocusCell(int(i)); }, {},
                        [&model, i] { model.ActivateCell(int(i)); });
                    g_hits.back().focus = [&model, i] { model.FocusCell(int(i), false); };
                    continue;
                }
                const Figure& f = model.figures[cell.figure];
                Card(model, f, a, b, cursor, section.foreign || model.CardBusy(f));
                if (cursor)
                {
                    ringMin = a;
                    ringMax = b;
                    ringRadius = look::Radius::Card;
                }
                AddHit(
                    a, b, [&model, i] { model.FocusCell(int(i)); }, [&model, i] { model.OptionsCell(int(i)); },
                    [&model, i] { model.ActivateCell(int(i)); });
                g_hits.back().target = "card=" + std::to_string(i);
                g_hits.back().focus = [&model, i] { model.FocusCell(int(i), false); };
            }
            g_draw->PopClipRect();
            if (ringRadius >= 0)
                FocusRing(ringMin, ringMax, ringRadius);
            // "Not owned yet" beside the + card, for a seat without figures of its family.
            if (!wheelList && !grid.sections.empty() && grid.sections[0].count == 0 &&
                grid.sections[0].kind == Section::Kind::Shelf && !model.FiltersActive())
            {
                int total = 0;
                for (const Toy* toy : Catalogue::Get().Accepted(model.game))
                {
                    if (std::find(seat.seat.accepts.begin(), seat.seat.accepts.end(), toy->category) !=
                        seat.seat.accepts.end())
                        total++;
                }
                char text[200];
                snprintf(text, sizeof(text),
                    L("None yet: %d exist for this game.", "Aucune pour l'instant : %d existent pour ce jeu."), total);
                float y = min.y + grid.RowY(0) - grid.scroll + 40;
                for (const std::string& line : Wrap(g_bold, 15, text, width - cardWidth - 40))
                {
                    Text(g_bold, 15, ImVec2(min.x + cardWidth + 24, y), Look::TextDim, line);
                    y += 22;
                }
            }
        }

        // --- The shelf's bar: the search field, the Filters button, the tags and the count ---------------------------------

        // A magnifying glass.
        void Magnifier(ImVec2 center, uint32_t rgb)
        {
            Ring(ImVec2(center.x - 2, center.y - 2), 14, rgb, 2);
            g_draw->AddLine(ImVec2(center.x + 4, center.y + 4), ImVec2(center.x + 9, center.y + 9), Rgb(rgb), 2.5f);
        }

        // A small x.
        void Cross(ImVec2 center, float half, uint32_t rgb)
        {
            g_draw->AddLine(
                ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), Rgb(rgb), 2);
            g_draw->AddLine(
                ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half), Rgb(rgb), 2);
        }

        // The bar sits on the line of "Choose for ...": the search field (typing on a keyboard goes straight into it, Y opens
        // the pad's), the Filters button; under them, when anything is on, its tags with an x, Clear all and the count.
        // Returns where the grid starts.
        float DrawShelfBar(Model& model, ImVec2 min, ImVec2 max)
        {
            const float cy = min.y + 10, rowHeight = 36;
            // The Filters button.
            std::vector<Model::Tag> tags = model.Tags();
            ImVec2 fa(max.x - 150, cy - rowHeight / 2), fb(max.x, cy + rowHeight / 2);
            bool on = !tags.empty() || model.filtersOpen;
            Box(fa, fb, on ? Look::Chip : Look::Button, on ? Look::Gold : Look::Control, 18, on ? 2 : 1);
            std::string label = tags.empty()
                                    ? std::string(L("Filters", "Filtres"))
                                    : std::string(L("Filters", "Filtres")) + " · " + std::to_string(tags.size());
            TextCentered(g_black, 15, ImVec2((fa.x + fb.x) / 2, cy), on ? Look::GoldText : Look::Text,
                Fit(g_black, 15, label, fb.x - fa.x - 20));
            AddHit(fa, fb, [&model] { model.filtersOpen ? model.CloseFilters() : model.OpenFilters(); });
            g_hits.back().focus = [&model] { model.FocusBar(1); };
            // The search field.
            ImVec2 sa(fa.x - 10 - 360, fa.y), sb(fa.x - 10, fb.y);
            model.barSpans = { { sa.x, sb.x }, { fa.x, fb.x } };
            bool barFocus = model.homeRow == Model::Row::Bar && model.screen == Screen::Home && !model.filtersOpen;
            Box(sa, sb, Look::Field, model.search.empty() ? Look::Control : Look::Gold, 18, 2);
            Magnifier(ImVec2(sa.x + 24, cy), model.search.empty() ? Look::TextDimmer : Look::GoldText);
            float textRight = sb.x - 46;
            if (model.search.empty())
            {
                TextMiddle(g_bold, 15, sa.x + 46, cy, Look::TextDimmer,
                    Fit(g_bold, 15, L("Name, element, game, level…", "Nom, élément, jeu, niveau…"),
                        textRight - sa.x - 46));
                HintButton('Y', sb.x - 40, cy, true);
            }
            else
            {
                std::string shown = Fit(g_black, 16, model.search, textRight - sa.x - 52);
                TextMiddle(g_black, 16, sa.x + 46, cy, Look::Text, shown);
                float caret = sa.x + 48 + Measure(g_black, 16, shown).x;
                g_draw->AddRectFilled(ImVec2(caret, cy - 10), ImVec2(caret + 2, cy + 10), Rgb(Look::GoldText));
                Disc(ImVec2(sb.x - 24, cy), 24, Look::Chip);
                Cross(ImVec2(sb.x - 24, cy), 5, Look::TextSoft);
                AddHit(ImVec2(sb.x - 40, sa.y), sb, [&model] { model.ClearSearch(); });
            }
            AddHit(sa, ImVec2(sb.x - (model.search.empty() ? 0 : 40), sb.y), [&model] { model.Run(Command::View); });
            g_hits.back().focus = [&model] { model.FocusBar(0); };
            if (barFocus)
                FocusRing(model.barColumn == 0 ? sa : fa, model.barColumn == 0 ? sb : fb, 18);

            float bottom = min.y + 36;
            if (!model.FiltersActive())
                return bottom;
            // The tags, Clear all and the count.
            float ty = bottom + 2, x = min.x;
            for (size_t i = 0; i < tags.size(); i++)
            {
                float w = Measure(g_black, 14, tags[i].text).x + 52;
                ImVec2 a(x, ty), b(x + w, ty + 28);
                Box(a, b, Look::Chip, Look::GoldBorder, 14, 2);
                TextMiddle(g_black, 14, a.x + 14, ty + 14, Look::GoldText, tags[i].text);
                Cross(ImVec2(b.x - 18, ty + 14), 4, Look::GoldText);
                int index = int(i);
                AddHit(a, b, [&model, index] { model.RemoveTag(index); });
                x += w + 8;
            }
            std::string count = FigureCount(model.shownCount, French());
            float countW = Measure(g_bold, 14, count).x;
            TextMiddle(g_bold, 14, max.x - countW, ty + 14, Look::TextSoft, count);
            std::string clear = L("Clear all", "Tout effacer");
            float clearW = Measure(g_black, 14, clear).x + 28;
            ImVec2 ca(max.x - countW - 16 - clearW, ty), cb(max.x - countW - 16, ty + 28);
            Box(ca, cb, Look::Button, Look::Control, 14, 1);
            TextCentered(g_black, 14, ImVec2((ca.x + cb.x) / 2, ty + 14), Look::Text, clear);
            AddHit(ca, cb, [&model] { model.ClearFilters(); });
            return bottom + 36;
        }

        // The Filters panel, over the shelf: Element, Game, Type, Sort by (a value each: Left and Right, or a click, change
        // it) and Clear all.
        void DrawFiltersPanel(Model& model, float top, ImVec2 max)
        {
            const float rowH = 46, width = 460;
            float height = 14 + (Model::FilterRows + 1) * rowH + 6;
            ImVec2 pa(max.x - width, top + 4), pb(max.x, top + 4 + height);
            // A click outside closes it (the hits after this one are on the panel).
            AddHit(ImVec2(0, 0), ImVec2(1280, 720), [&model] { model.CloseFilters(); });
            g_hits.back().hover = false;
            Panel(pa, pb);
            AddHit(pa, pb, [] {});
            g_hits.back().hover = false;
            for (int row = 0; row <= Model::FilterRows; row++)
            {
                ImVec2 a(pa.x + 12, pa.y + 14 + row * rowH), b(pb.x - 12, a.y + rowH - 6);
                bool focused = model.filterRow == row;
                float cy = (a.y + b.y) / 2;
                if (row == Model::FilterRows)
                {
                    Box(a, b, Look::Button, Look::Control, 12, 2);
                    if (focused)
                        FocusRing(a, b, 12);
                    TextCentered(g_black, 15, ImVec2((a.x + b.x) / 2, cy), Look::Text, model.FilterRowLabel(row));
                    AddHit(a, b, [&model, row] {
                        model.filterRow = row;
                        model.Run(Command::Accept);
                    });
                    g_hits.back().focus = [&model, row] {
                        model.filterRow = row;
                        model.Touch();
                    };
                    continue;
                }
                Box(a, b, focused ? Look::CardSelected : Look::Field, Look::Control, 12, 2);
                if (focused)
                    FocusRing(a, b, 12);
                TextMiddle(g_bold, 14, a.x + 14, cy, focused ? Look::Text : Look::TextDim, model.FilterRowLabel(row));
                float vx = a.x + 130;
                float valueRight = b.x - 34;
                std::string value = model.FilterValueLabel(row);
                bool set = row == 3 ? model.sort != Sort::Recent
                                    : (row == 0      ? model.filterElement >= 0
                                          : row == 1 ? !model.filterGame.empty()
                                                     : model.filterCategory >= 0);
                g_draw->AddTriangleFilled(ImVec2(vx, cy), ImVec2(vx + 7, cy - 6), ImVec2(vx + 7, cy + 6),
                    Rgb(focused ? Look::Text : Look::TextDimmer));
                g_draw->AddTriangleFilled(ImVec2(b.x - 14, cy), ImVec2(b.x - 21, cy - 6), ImVec2(b.x - 21, cy + 6),
                    Rgb(focused ? Look::Text : Look::TextDimmer));
                TextCentered(g_black, 15, ImVec2((vx + 14 + valueRight - 8) / 2, cy), set ? Look::GoldText : Look::Text,
                    Fit(g_black, 15, value, valueRight - vx - 30));
                float split = (vx + b.x) / 2;
                AddHit(a, b, [&model, row] {
                    model.filterRow = row;
                    model.CycleFilter(row, 1);
                });
                AddHit(ImVec2(vx - 8, a.y), ImVec2(split - 40, b.y), [&model, row] {
                    model.filterRow = row;
                    model.CycleFilter(row, -1);
                });
                g_hits.back().focus = g_hits[g_hits.size() - 2].focus = [&model, row] {
                    model.filterRow = row;
                    model.Touch();
                };
            }
        }

        // --- Home: the seats along the top, your figures for the chosen seat below -----------------------------------

        constexpr float SeatTop = 84, SeatHeight = 100, SeatWidth = 218, SeatGap = 12;

        void DrawSeats(Model& model)
        {
            constexpr float Left = 32, Right = 1248;
            // Add a figure and Manage, one above the other at the end of the row, as wide as the longer label; the seats
            // share the rest (five in Trap Team).
            std::vector<std::string> rows = model.ColumnRows();
            for (size_t r = 0; r + 1 < rows.size(); r++)
                rows[r] = "+ " + rows[r];
            float buttonWidth = 0;
            for (const std::string& row : rows)
                buttonWidth = std::max(buttonWidth, std::ceil(Measure(g_black, 15, row).x) + 36);
            const size_t seatCount = model.seats.size();
            const float seatWidth =
                std::min(SeatWidth, std::floor((Right - Left - buttonWidth - SeatGap * float(seatCount)) /
                                               float(std::max<size_t>(seatCount, 1))));
            bool rowFocused = model.homeRow == Model::Row::Seats && model.screen == Screen::Home;
            model.seatSpans.clear();
            for (size_t i = 0; i < model.seats.size(); i++)
            {
                const SeatState& s = model.seats[i];
                float x = Left + i * (seatWidth + SeatGap), y = SeatTop;
                ImVec2 a(x, y), b(x + seatWidth, y + SeatHeight);
                model.seatSpans.push_back({ a.x, b.x });
                bool focused = rowFocused && model.column == int(i);
                bool lit = model.seat == int(i);
                Box(a, b, lit ? Look::CardSelected : Look::Panel, lit ? Look::Gold : Look::CardBorder, 16, lit ? 2 : 1);
                ImVec2 picture(a.x + 44, y + SeatHeight / 2);
                const Figure* f = model.SeatFigure(int(i));
                std::string label = model.SeatLabel(int(i));
                std::string name, line;
                uint32_t lineColor = Look::TextDim;
                if (s.physical)
                {
                    const Toy* toy = s.physicalKnown ? Catalogue::Get().Find(s.physicalType) : nullptr;
                    Portrait(toy,
                        s.physicalKnown ? Catalogue::Get().Describe(toy, s.physicalVariant).kind : VariantKind::Base,
                        picture, 64, false, s.physicalVariant);
                    label += L(" · on your portal", " · sur votre portail");
                    name = s.physicalKnown ? Catalogue::Get().Describe(toy, s.physicalVariant).name
                                           : L("reading...", "lecture...");
                    if (s.physicalLevel > 0)
                        line = (French() ? "Niveau " : "Level ") + std::to_string(s.physicalLevel);
                }
                else if (f != nullptr)
                {
                    CardPortrait(model, *f, picture, 64);
                    if (model.OutOfHealth(*f))
                        OutOfHealthCross(picture, 21);
                    name = model.FigureName(*f);
                    line = model.LevelLine(*f);
                    if (s.waiting)
                    {
                        line = s.late ? L("not taken yet: X to undo", "pas encore pris : X pour annuler")
                                      : L("waiting for the game...", "en attente du jeu...");
                        lineColor = Look::Waiting;
                    }
                }
                else if (!s.path.empty())
                {
                    EmptySeatPicture(picture, 64);
                    name = L("Missing figure", "Figurine introuvable");
                }
                else
                {
                    EmptySeatPicture(picture, 64);
                    name = model.realPortal ? L("None", "Aucun") : L("Empty", "Vide");
                    if (!model.realPortal)
                        line = L("A: choose", "A : choisir");
                }
                if (s.seat.player == 2 && s.seat.id[0] == 'p' && !s.physical && (f == nullptr || !s.waiting) &&
                    !GamepadConnected(1))
                {
                    line = L("needs a 2nd controller", "il faut une 2e manette");
                    lineColor = Look::TextDimmer;
                }
                // A LightCore toy lights up with the portal's light, where the game drives it.
                bool lightCore =
                    s.physical
                        ? s.physicalKnown && Catalogue::Get()
                                                     .Describe(Catalogue::Get().Find(s.physicalType), s.physicalVariant)
                                                     .kind == VariantKind::LightCore
                        : f != nullptr && f->Kind() == VariantKind::LightCore;
                if (lightCore && model.lightRgb != 0)
                    RoundRect(ImVec2(a.x + 10, y + 10), ImVec2(a.x + 20, y + 20), model.lightRgb, 5);
                float tx = a.x + 86, tw = b.x - tx - 10;
                Text(g_bold, 12, ImVec2(tx, y + 18), lit ? Look::GoldText : Look::TextDimmer,
                    Fit(g_bold, 12, label, tw));
                Text(g_black, 16, ImVec2(tx, y + 36), Look::Text, Fit(g_black, 16, name, tw));
                if (!line.empty())
                    Text(g_bold, 12, ImVec2(tx, y + 62), lineColor, Fit(g_bold, 12, line, tw));
                if (focused)
                    FocusRing(a, b, 16);
                int index = int(i);
                AddHit(
                    a, b, [&model, index] { model.FocusColumnRow(index); },
                    [&model, index] {
                        model.FocusColumnRow(index);
                        model.Run(Command::Options);
                    },
                    [&model, index] {
                        model.FocusColumnRow(index);
                        model.Run(Command::Accept);
                    });
            }
            float h = (SeatHeight - 10) / 2;
            for (size_t r = 0; r < rows.size(); r++)
            {
                int row = int(model.seats.size() + r);
                float y = SeatTop + (rows.size() == 1 ? (SeatHeight - h) / 2 : r * (h + 10));
                ImVec2 a(Right - buttonWidth, y), b(Right, y + h);
                model.seatSpans.push_back({ a.x, b.x });
                bool focused = rowFocused && model.column == row;
                PlainButton(a, b, rows[r], 15, focused);
                AddHit(a, b, [&model, row] {
                    model.FocusColumnRow(row);
                    model.Run(Command::Accept);
                });
                g_hits.back().focus = [&model, row] { model.FocusColumnRow(row); };
            }
            if (model.realPortal)
                TextMiddle(g_bold, 13, Left, SeatTop + SeatHeight + 16, Look::TextDim,
                    L("Change the toys on your portal.", "Changez les jouets sur votre portail."));
        }

        void DrawEmpty(Model& model, ImVec2 min, ImVec2 max)
        {
            float x = min.x, y = min.y + 10;
            Text(g_display, 28, ImVec2(x, y), Look::Text, L("My Skylanders is empty", "Mes Skylanders est vide"));
            y += 50;
            Text(g_bold, 16, ImVec2(x, y), Look::TextDim,
                L("Add the figures you want to play with.", "Ajoutez les figurines avec lesquelles jouer."));
            y += 44;
            const std::string choices[] = { L("Add a figure", "Ajouter une figurine"),
                L("Import figure files", "Importer des fichiers de figurines"),
                L("Start with Spyro, Gill Grunt, Trigger Happy", "Commencer avec Spyro, Gill Grunt, Trigger Happy") };
            for (int i = 0; i < 3; i++)
            {
                ImVec2 a(x, y), b(x + 520, y + 54);
                bool focused = model.homeRow != Model::Row::Seats && model.list.cursor == i;
                if (i == 2)
                    GoldButton(a, b, choices[i], 15, focused);
                else
                    PlainButton(a, b, choices[i], 16, focused);
                if (i == 2)
                    Text(g_bold, 13, ImVec2(b.x + 16, y + 20), Look::TextDimmer,
                        L("the starter pack", "le pack de démarrage"));
                AddHit(a, b, [&model, i] {
                    model.homeRow = Model::Row::Grid;
                    model.list.cursor = i;
                    model.Run(Command::Accept);
                });
                y += 70;
            }
            (void)max;
        }

        void DrawHome(Model& model)
        {
            Header(L("TOY BOX", "TOY BOX"),
                model.GameName() + (model.realPortal ? L(" · your portal", " · votre portail") : ""));
            std::vector<HintItem> top;
            top.push_back({ 'B', L("Close", "Fermer") });
            Hints(1248, 44, top);
            DrawSeats(model);
            ImVec2 min(32, SeatTop + SeatHeight + 22), max(1248, 664);
            if (!model.OwnsAnything())
            {
                DrawEmpty(model, min, max);
                Footer({ { 'A', L("Choose", "Choisir") }, { 'B', L("Close", "Fermer") } });
                return;
            }
            // What the grid is for: the chosen seat, and that only what this game reads is shown.
            std::string choosing = model.realPortal
                                       ? std::string(L("Your figures", "Vos figurines"))
                                       : std::string(L("Choose for ", "Choisir pour ")) + model.SeatLabel(model.seat);
            TextMiddle(g_black, 18, min.x, min.y + 10, Look::Text, choosing);
            std::string only = model.realPortal ? L("Browse only", "Consultation seule")
                                                : std::string(L("for ", "pour ")) + model.GameName();
            TextMiddle(g_bold, 13, min.x + Measure(g_black, 18, choosing).x + 16, min.y + 11, Look::TextDimmer, only);
            float listTop = DrawShelfBar(model, min, max);
            DrawList(model, ImVec2(min.x, listTop), max,
                model.homeRow == Model::Row::Grid && model.screen == Screen::Home && !model.filtersOpen, false);
            if (model.shownCount == 0 && model.FiltersActive())
            {
                Text(g_black, 20, ImVec2(min.x, listTop + 24), Look::Text,
                    L("No figure matches", "Aucune figurine ne correspond"));
                Text(g_bold, 14, ImVec2(min.x, listTop + 54), Look::TextDim,
                    L("Change the search or Clear all.", "Modifiez la recherche ou choisissez Tout effacer."));
            }
            if (model.filtersOpen)
                DrawFiltersPanel(model, listTop, max);

            std::vector<HintItem> hints;
            if (model.filtersOpen)
            {
                hints.push_back({ 'A', L("Change", "Changer") });
                hints.push_back({ 'B', L("Close", "Fermer") });
            }
            else if (model.homeRow == Model::Row::Bar)
            {
                hints.push_back({ 'A', model.barColumn == 0 ? L("Search", "Rechercher") : L("Filters", "Filtres") });
                if (model.seats.size() > 1)
                    hints.push_back({ 'L', L("Other seat", "Autre place") });
                hints.push_back({ 'B', model.search.empty() ? L("Close", "Fermer") : L("Clear", "Effacer") });
            }
            else if (model.homeRow == Model::Row::Grid)
            {
                const Cell* cell = model.list.cells.empty() ? nullptr : &model.list.cells[model.list.cursor];
                if (cell != nullptr && cell->kind == Cell::Kind::Figure && !model.realPortal)
                    hints.push_back({ 'A', std::string(L("Put on ", "Poser en ")) + model.SeatLabel(model.seat) });
                else if (cell != nullptr && cell->kind == Cell::Kind::Figure)
                    hints.push_back({ 'A',
                        L("Use the virtual portal next time", "Utiliser le portail virtuel la prochaine fois") });
                else if (cell != nullptr)
                    hints.push_back({ 'A', cell->kind == Cell::Kind::Add ? L("Add a figure", "Ajouter une figurine")
                                                                         : L("Open / fold", "Ouvrir / replier") });
                if (cell != nullptr && cell->kind == Cell::Kind::Figure)
                    hints.push_back({ 'X', L("Figure", "Figurine") });
                if (model.seats.size() > 1)
                    hints.push_back({ 'L', L("Other seat", "Autre place") });
                if (!model.realPortal)
                    hints.push_back({ 'V', L("hold: wheel", "maintenir : roue") });
                hints.push_back({ 'F', L("Filters", "Filtres") });
            }
            else if (model.column < int(model.seats.size()))
            {
                hints.push_back({ 'A', L("Choose", "Choisir") });
                if (!model.seats[model.column].path.empty())
                    hints.push_back({ 'X', L("Take off / options", "Retirer / options") });
                hints.push_back({ 'B', L("Close", "Fermer") });
            }
            else
            {
                hints.push_back({ 'A', L("Open", "Ouvrir") });
                hints.push_back({ 'B', L("Close", "Fermer") });
            }
            Footer(hints);
        }

        // --- Figure page ----------------------------------------------------------------------------------------------

        // An Imaginators toy made here, without the factory signature: the port plays it, a real game would not.
        std::string VirtualCrystalText() { return collection::VirtualToyText(French() ? "fr" : "en"); }

        // A part id's name as the game shows it (parts.json, from the disc), else "#<id>".
        std::string PartLabel(const std::string& game, int id)
        {
            std::string name = collection::PartName(game, id, TextLanguage());
            return name.empty() ? "#" + std::to_string(id) : name;
        }

        // A crystal's parts (their names, "-" for none): body, gear and powers.
        std::string CrystalSummary(const std::string& game, const std::string& path)
        {
            figures::CrystalStats c;
            if (collection::ReadCrystal(path, c) != figures::AreaState::Valid || !c.hasRecipe)
                return {};
            std::string text;
            for (const char* name : { "head", "torso", "arms", "legs", "tail", "weaponR", "weaponL", "Hat",
                     "PrimaryPower", "SecondaryPower", "TertiaryPower" })
            {
                int i = figures::CrystalParamIndex(name);
                const figures::CrystalParam& p = figures::CrystalParams()[size_t(i)];
                uint16_t v = c.creation[size_t(i)];
                text += (text.empty() ? "" : ", ") + std::string(name) + " " +
                        (v == (1u << p.bits) - 1 ? std::string("-") : PartLabel(game, p.min + v));
            }
            return text;
        }

        std::string Duration(uint32_t seconds)
        {
            char text[32];
            snprintf(text, sizeof(text), "%u h %02u", seconds / 3600, seconds / 60 % 60);
            return text;
        }

        // Trap Team: the villains a trap of this element can hold, the game's own table (Game::villains), with the
        // pictures of its villain vault. Which one a trap holds is read from the tag once its place is verified
        // (figures::TrapVillainVerified), with the evolved flag. The Villain row's choice is ringed in gold; a click on a
        // picture puts that villain in the trap.
        void DrawTrapVillains(Model& model, const Figure& f, float x, float y, float width)
        {
            std::vector<const collection::Villain*> villains = model.TrapVillainsOf(f);
            if (villains.empty())
                return;
            uint8_t pick = model.TrapPick();
            const collection::Villain* held = nullptr;
            for (const collection::Villain* villain : villains)
                if (figures::TrapVillainVerified && f.stats.villain + figures::VillainResourceBase == villain->id)
                    held = villain;
            Text(g_bold, 14, ImVec2(x, y), Look::TextSoft,
                held != nullptr ? std::string(L("Holds ", "Contient ")) + held->name +
                                      (f.stats.villainEvolved ? L(" · Evolved", " · Évolué") : "")
                                : std::string(L("Can hold ", "Peut contenir ")) + std::to_string(villains.size()) +
                                      L(" villains", " méchants"));
            y += 28;
            const float box = 64, gap = 8;
            float left = x + (width - (villains.size() * box + (villains.size() - 1) * gap)) / 2;
            for (const collection::Villain* villain : villains)
            {
                ImVec2 center(left + box / 2, y + box / 2);
                bool escaped = held != nullptr && villain != held;
                PictureAt(collection::VillainPicturePath(model.game, *villain, !escaped), center, int(box), true);
                uint8_t id = VillainId(*villain);
                if (id == pick)
                    Ring(center, int(box) + 6, Look::Gold, 3);
                AddHit(ImVec2(center.x - box / 2, center.y - box / 2), ImVec2(center.x + box / 2, center.y + box / 2),
                    [&model, id] { model.PickTrapVillain(id); });
                left += box + gap;
            }
            y += box + 8;
            std::string names;
            for (const collection::Villain* villain : villains)
                names += (names.empty() ? "" : ", ") + villain->name;
            for (const std::string& l : Wrap(g_bold, 13, names, width, 3))
            {
                Text(g_bold, 13, ImVec2(x, y), Look::TextDimmer, l);
                y += 18;
            }
        }

        void DrawFigureInfo(Model& model, const Figure& f)
        {
            ImVec2 min(32, 88), max(520, 664);
            Panel(min, max);
            float cx = (min.x + max.x) / 2, y = min.y + 24;
            HeroPortrait(model, f, ImVec2(cx, y + 74), 148);
            y += 170;
            std::string name = model.FigureName(f);
            float size = 30;
            while (size > 18 && Measure(g_display, size, name).x > max.x - min.x - 40)
                size -= 2;
            TextCentered(g_display, size, ImVec2(cx, y + 16), 0xFFFFFF, Fit(g_display, size, name, max.x - min.x - 32));
            y += 42;
            std::string toy = f.toy != nullptr ? f.toy->name : model.FigureName(f);
            // The plain toy says no version: its name is the toy's; any other reads as the game sees it.
            VariantKind seen = collection::KindIn(f, model.game);
            std::string version = seen != VariantKind::Base ? VariantLabel(seen) : std::string();
            if (!f.edition.empty())
                version = f.edition;
            std::string from;
            if (f.toy != nullptr)
            {
                const Game* g = Catalogue::Get().FindGame(f.toy->game);
                from = std::string(L("from ", "de ")) +
                       (g != nullptr ? g->name.substr(g->name.rfind("Skylanders: ", 0) == 0 ? 12 : 0) : f.toy->game);
            }
            std::string line;
            for (const std::string& part : { toy == name ? std::string() : toy, version,
                     f.toy != nullptr && f.toy->element < Element::None ? ElementLabel(f.toy->element) : std::string(),
                     from })
                line += part.empty() ? std::string() : (line.empty() ? "" : " · ") + part;
            for (const std::string& l : Wrap(g_bold, 14, line, max.x - min.x - 48, 2))
            {
                TextCentered(g_bold, 14, ImVec2(cx, y + 8), Look::TextDim, l);
                y += 20;
            }
            y += 12;
            float x = min.x + 28, w = max.x - min.x - 56;
            bool progress =
                f.toy == nullptr || f.toy->category <= Category::Mini || f.toy->category == Category::CreationCrystal;
            if (progress && f.state == figures::AreaState::Valid)
            {
                // "Level 10 (15)": the game's cap, and the figure's own level when a later game took it higher.
                int cap = model.LevelCap(), level = std::min(f.level, cap);
                Text(g_black, 20, ImVec2(x, y), Look::Text, LevelText(f, model.game, French()));
                std::string gold = std::to_string(f.stats.gold) + L(" gold", " or");
                Text(g_black, 20, ImVec2(x + w - Measure(g_black, 20, gold).x, y), Look::GoldText, gold);
                y += 32;
                if (f.toy != nullptr && f.toy->category == Category::CreationCrystal)
                {
                    // A creation: its class and level above, the bar on the game's own 80-level table.
                    int lv = std::min(f.level, figures::MaxCrystalLevel);
                    uint32_t from = figures::CrystalLevelExperience(lv), to = figures::CrystalLevelExperience(lv + 1);
                    float t =
                        lv >= figures::MaxCrystalLevel
                            ? 1.0f
                            : std::clamp(float(f.stats.experience - from) / float(std::max(to - from, 1u)), 0.0f, 1.0f);
                    RoundRect(ImVec2(x, y), ImVec2(x + w, y + 10), Look::Field, 5);
                    if (t > 0)
                        RoundRect(ImVec2(x, y), ImVec2(x + std::max(w * t, 10.0f), y + 10), Look::Bar, 5);
                    y += 24;
                }
                else
                {
                    uint32_t low = level > 1 ? figures::LevelExperience[level - 1] : 0;
                    uint32_t high = level < cap ? figures::LevelExperience[level] : low + 1;
                    float t = level >= cap
                                  ? 1.0f
                                  : std::clamp(
                                        float(figures::TotalExperience(f.stats) - low) / float(high - low), 0.0f, 1.0f);
                    RoundRect(ImVec2(x, y), ImVec2(x + w, y + 10), Look::Field, 5);
                    if (t > 0)
                        RoundRect(ImVec2(x, y), ImVec2(x + std::max(w * t, 10.0f), y + 10), Look::Bar, 5);
                    y += 24;
                }
            }
            std::vector<std::string> facts;
            if (collection::Card card = model.CardFor(f); card.Swapper())
                facts.push_back(std::string(L("Top: ", "Haut : ")) + WithoutHalf(card.first->DisplayName()) +
                                L(" · Bottom: ", " · Bas : ") + WithoutHalf(card.second->DisplayName()));
            else if (f.IsSwapTop() || f.IsSwapBottom())
                facts.push_back(f.IsSwapTop() ? L("A top with no bottom yet.", "Un haut sans bas pour l'instant.")
                                              : L("A bottom with no top yet.", "Un bas sans haut pour l'instant."));
            std::string nickname = figures::NicknameUtf8(f.stats);
            bool crystal = f.toy != nullptr && f.toy->category == Category::CreationCrystal;
            if (f.missingSignature)
                facts.push_back(VirtualCrystalText());
            if (figures::IsVehicleType(f.type))
                facts.push_back(f.state == figures::AreaState::Valid
                                    ? std::string(L("Gearbits: ", "Gearbits : ")) +
                                          std::to_string(figures::Gearbits(f.stats)) + L(" · shield ", " · bouclier ") +
                                          std::to_string(figures::VehicleShieldLevel(f.stats)) + "/5" +
                                          L(" · weapon ", " · arme ") +
                                          std::to_string(figures::VehicleWeaponLevel(f.stats)) + "/5" +
                                          L(" · driven ", " · conduit ") + Duration(f.stats.playSeconds)
                                    : std::string(L("New: never driven", "Neuf : jamais conduit")));
            if (crystal && f.state == figures::AreaState::Blank)
                facts.push_back(L("Empty: place it in the game to create an Imaginator.",
                    "Vide : posez-le dans le jeu pour créer un Imaginator."));
            else if (progress && f.state == figures::AreaState::Blank)
                facts.push_back(L("New: never played", "Neuve : jamais jouée"));
            else if (progress && f.state != figures::AreaState::Valid)
                facts.push_back(L("Unreadable. Restore a backup or reset it.",
                    "Illisible. Restaurez une sauvegarde ou remettez-la à neuf."));
            if (crystal && !nickname.empty())
                facts.push_back(std::string(L("Name: ", "Nom : ")) + nickname);
            if (crystal && f.state == figures::AreaState::Valid && model.figureDetails)
                facts.push_back(std::string(L("Creation: ", "Création : ")) + CrystalSummary(model.game, f.path));
            else if (progress && !nickname.empty())
                facts.push_back(std::string(L("Nickname in the game: ", "Surnom dans le jeu : ")) + nickname);
            if (progress && f.state == figures::AreaState::Valid)
                facts.push_back(
                    std::string(L("Played ", "Joué ")) + Duration(f.stats.playSeconds) +
                    (crystal ? std::string()
                             : L(" · hero points ", " · points de héros ") + std::to_string(f.stats.heroPoints)));
            if (!f.lastUsed.empty() &&
                f.state != figures::AreaState::Blank) // a figure the game never wrote was not played
            {
                const Game* g = Catalogue::Get().FindGame(f.lastGame);
                std::string game =
                    g != nullptr ? g->name.substr(g->name.rfind("Skylanders: ", 0) == 0 ? 12 : 0) : f.lastGame;
                std::string when = f.lastUsed.substr(0, 10);
                facts.push_back(std::string(L("Last played: ", "Dernière partie : ")) + game + ", " + when);
            }
            // Details (the line of the same name): what only a curious owner or a bug report needs.
            if (model.figureDetails)
            {
                char toy[96];
                snprintf(toy, sizeof(toy), L("Toy %u · variant %04X", "Jouet %u · variante %04X"), f.type, f.variant);
                facts.push_back(toy);
                size_t backups = Backups(f.path).size();
                facts.push_back(std::string(L("File: ", "Fichier : ")) + f.file + " · " + std::to_string(backups) +
                                L(" backups", " sauvegardes"));
                if (!f.origin.empty())
                    facts.push_back(std::string(L("Made by: ", "Origine : ")) + f.origin);
            }
            for (const std::string& fact : facts)
            {
                for (const std::string& l : Wrap(g_bold, 14, fact, w, 2))
                {
                    Text(g_bold, 14, ImVec2(x, y), Look::TextSoft, l);
                    y += 20;
                }
                y += 4;
            }
            if (f.toy != nullptr && f.toy->category == Category::Trap)
                DrawTrapVillains(model, f, x, y, w);
        }

        void DrawFigure(Model& model)
        {
            const Figure& f = model.figures[model.figure];
            Header(
                L("TOY BOX", "TOY BOX"), std::string(L("My Skylanders › ", "Mes Skylanders › ")) + model.FigureName(f));
            Hints(1248, 44, { { 'L', L("previous / next figure", "figurine précédente / suivante") } });
            DrawFigureInfo(model, f);
            ImVec2 min(540, 88), max(1248, 664);
            Panel(min, max);
            std::vector<std::string> actions = model.FigureActions();
            float y = min.y + 20;
            if (model.figureMore)
            {
                TextMiddle(g_black, 18, min.x + 24, y + 14, Look::GoldText, L("More", "Plus"));
                y += 40;
            }
            float rowHeight = std::min(62.0f, (max.y - y - 20) / std::max(int(actions.size()), 1));
            for (size_t i = 0; i < actions.size(); i++)
            {
                size_t tab = actions[i].find('\t');
                std::string label = actions[i].substr(0, tab),
                            hint = tab == std::string::npos ? "" : actions[i].substr(tab + 1);
                ImVec2 a(min.x + 20, y), b(max.x - 20, y + rowHeight - 8);
                bool focused = model.figureAction == int(i) && model.screen == Screen::Figure;
                bool danger = label == L("Delete", "Supprimer") || label == L("Reset to new", "Remettre à neuf");
                FocusBox(a, b, focused, 12, Look::Raised);
                TextMiddle(g_black, 16, a.x + 18, (a.y + b.y) / 2, danger ? Look::DangerText : Look::Text, label);
                // More opens a list (a chevron to the right), Details unfolds lines (down, up once open).
                int fold = label == L("Details", "Détails")                        ? 1
                           : label == L("Hide the details", "Masquer les détails") ? -1
                           : label == L("More…", "Plus…")                          ? 0
                                                                                   : 2;
                float right = b.x - 18;
                if (fold != 2)
                {
                    Chevron(ImVec2(right - 6, (a.y + b.y) / 2), 14, focused ? Look::GoldText : Look::TextDim, fold);
                    right -= 28;
                }
                // A trap's Villain row steps with Left / Right: its two arrows, as Edit stats' fields.
                bool stepper = focused && label == L("Villain", "Méchant");
                float cy = (a.y + b.y) / 2, arrows = right - 20;
                if (stepper)
                {
                    g_draw->AddTriangleFilled(
                        ImVec2(arrows, cy), ImVec2(arrows + 7, cy - 6), ImVec2(arrows + 7, cy + 6), Rgb(Look::Gold));
                    g_draw->AddTriangleFilled(ImVec2(arrows + 20, cy), ImVec2(arrows + 13, cy - 6),
                        ImVec2(arrows + 13, cy + 6), Rgb(Look::Gold));
                    right -= 36;
                }
                if (!hint.empty())
                    TextMiddle(
                        g_bold, 13, right - Measure(g_bold, 13, hint).x, (a.y + b.y) / 2, Look::TextDimmer, hint);
                int index = int(i);
                AddHit(
                    a, b,
                    [&model, index] {
                        bool again = model.figureAction == index;
                        model.FocusAction(index);
                        if (again)
                            model.Run(Command::Accept);
                    },
                    {},
                    [&model, index] {
                        model.FocusAction(index);
                        model.Run(Command::Accept);
                    });
                g_hits.back().focus = [&model, index] { model.FocusAction(index); };
                if (stepper)
                {
                    AddHit(ImVec2(arrows - 6, a.y), ImVec2(arrows + 10, b.y), [&model] { model.Run(Command::Left); });
                    AddHit(ImVec2(arrows + 10, a.y), ImVec2(arrows + 26, b.y), [&model] { model.Run(Command::Right); });
                }
                y += rowHeight;
            }
            Footer({ { 'A', L("Do it", "Valider") }, { 'B', L("Back", "Retour") } });
        }

        // Change the bottom / the top: every dump of the other half, one card each; A takes it for this swapper.
        void DrawHalf(Model& model)
        {
            const Figure& f = model.figures[model.figure];
            bool bottom = f.IsSwapTop();
            Header(
                L("TOY BOX", "TOY BOX"), std::string(L("My Skylanders › ", "Mes Skylanders › ")) + model.FigureName(f));
            ImVec2 min(32, 88), max(1248, 664);
            Panel(min, max, 0.95f);
            Title(bottom ? L("Change the bottom", "Changer le bas") : L("Change the top", "Changer le haut"),
                ImVec2(min.x + 28, min.y + 18), 28);
            if (model.list.cells.empty())
                Text(g_bold, 16, ImVec2(min.x + 28, min.y + 90), Look::TextDim,
                    bottom ? L("No other bottom to choose: add one from Add a figure.",
                                 "Pas d'autre bas à choisir : ajoutez-en un avec Ajouter.")
                           : L("No other top to choose: add one from Add a figure.",
                                 "Pas d'autre haut à choisir : ajoutez-en un avec Ajouter."));
            DrawList(model, ImVec2(min.x + 28, min.y + 80), ImVec2(max.x - 28, max.y - 56), true, true);
            Footer({ { 'A', L("Use this one", "Prendre celle-ci") }, { 'B', L("Back", "Retour") } });
        }

        void DrawPin(Model& model)
        {
            g_under = true;
            DrawFigure(model);
            g_under = false;
            Fill(ImVec2(0, 0), ImVec2(CanvasWidth, CanvasHeight), Look::Night, 0.55f);
            ImVec2 min(380, 120), max(900, 620);
            Panel(min, max);
            Title(L("PIN TO THE WHEEL", "ÉPINGLER SUR LA ROUE"), ImVec2(min.x + 24, min.y + 18), 26);
            int playerSeat =
                model.seats[model.seat].seat.player > 0 && model.seats[model.seat].seat.id[0] == 'p' ? model.seat : 0;
            Text(g_bold, 14, ImVec2(min.x + 24, min.y + 60), Look::TextDim,
                std::string(L("Pick a place for the wheel of ", "Choisissez une place sur la roue du ")) +
                    model.SeatLabel(playerSeat) + ":");
            std::vector<WheelPlace> places = GetWheel(model.game, model.seats[playerSeat].seat.id);
            places.resize(Model::WheelPlaces);
            ImVec2 center((min.x + max.x) / 2, min.y + 290);
            for (int i = 0; i < Model::WheelPlaces; i++)
            {
                float angle = -1.5707963f + i * 6.2831853f / Model::WheelPlaces;
                ImVec2 at(center.x + std::cos(angle) * 160, center.y + std::sin(angle) * 160);
                int index = places[i].file.empty() ? -1 : -1;
                for (size_t k = 0; k < model.figures.size() && !places[i].file.empty(); k++)
                {
                    if (model.figures[k].file == places[i].file)
                        index = int(k);
                }
                if (index >= 0)
                    CardPortrait(model, model.figures[index], at, 72);
                else
                    EmptySeatPicture(at, 72);
                if (places[i].pinned)
                    Star(ImVec2(at.x + 30, at.y - 30), 8, Look::Gold);
                if (model.pinPlace == i)
                    Ring(at, 86, Look::Gold, 4);
                TextCentered(g_black, 13, ImVec2(at.x, at.y + 46), Look::TextDim, std::to_string(i + 1));
                AddHit(ImVec2(at.x - 40, at.y - 40), ImVec2(at.x + 40, at.y + 40), [&model, i] {
                    model.pinPlace = i;
                    model.Run(Command::Accept);
                });
            }
            FigurePortrait(model.figures[model.figure], center, 90);
            Footer({ { 'A', L("Pin here", "Épingler ici") }, { 'B', L("Back", "Retour") } });
        }

        void DrawBackups(Model& model)
        {
            g_under = true;
            DrawFigure(model);
            g_under = false;
            Fill(ImVec2(0, 0), ImVec2(CanvasWidth, CanvasHeight), Look::Night, 0.55f);
            ImVec2 min(330, 110), max(950, 640);
            Panel(min, max);
            Title(L("RESTORE A BACKUP", "RESTAURER UNE SAUVEGARDE"), ImVec2(min.x + 24, min.y + 18), 26);
            float y = min.y + 70;
            int first = std::max(0, model.backup - 7);
            for (int i = first; i < int(model.backups.size()) && y < max.y - 50; i++)
            {
                std::string file = model.backups[i].substr(model.backups[i].find_last_of('/') + 1);
                // 2026-10-24T101500Z.sky → 2026-10-24 10:15 UTC
                std::string date = file.size() >= 17 ? file.substr(0, 10) + " " + file.substr(11, 2) + ":" +
                                                           file.substr(13, 2) + " UTC"
                                                     : file;
                FigureData tag;
                figures::FigureStats st;
                std::string detail;
                if (LoadFigure(model.backups[i], tag) && figures::ReadStats(tag, st) == figures::AreaState::Valid)
                    detail = (French() ? "Niv " : "Lv ") +
                             std::to_string(figures::LevelForExperience(
                                 figures::ExperienceFor(st, model.LevelCap()), model.LevelCap())) +
                             " · " + std::to_string(st.gold) + L(" gold", " or");
                else
                    detail = L("never played", "jamais jouée");
                ImVec2 a(min.x + 20, y), b(max.x - 20, y + 44);
                bool focused = model.backup == i;
                FocusBox(a, b, focused, 12, Look::Raised);
                TextMiddle(g_black, 15, a.x + 16, y + 22, Look::Text, date);
                TextMiddle(g_bold, 14, b.x - 16 - Measure(g_bold, 14, detail).x, y + 22, Look::TextDim, detail);
                AddHit(a, b, [&model, i] {
                    bool again = model.backup == i;
                    model.backup = i;
                    model.Touch();
                    if (again)
                        model.Run(Command::Accept);
                });
                g_hits.back().focus = [&model, i] {
                    model.backup = i;
                    model.Touch();
                };
                y += 52;
            }
            Footer({ { 'A', L("Restore", "Restaurer") }, { 'B', L("Back", "Retour") } });
        }

        // --- Edit stats (the first Toy Box's sheet) ----------------------------------------------------------------

        int SetBits(uint32_t value)
        {
            int count = 0;
            for (; value != 0; value &= value - 1)
                count++;
            return count;
        }

        std::string StatLabel(int row, int cap = figures::MaxLevel)
        {
            switch (row)
            {
            case 0: return L("Nickname", "Surnom");
            case 1: return L("Hat", "Chapeau");
            case 2: return (French() ? "Niveau (1 à " : "Level (1 to ") + std::to_string(cap) + ")";
            case 3: return L("Experience", "Expérience");
            case 4: return L("Gold", "Or");
            case 5: return L("Play Time", "Temps de jeu");
            case 6: return L("Hero Points", "Points de héros");
            case 7: return L("Heroic Challenges", "Défis héroïques");
            case 8: return L("Upgrades", "Améliorations");
            }
            return "";
        }

        std::string StatValue(int row, const Model& model)
        {
            const figures::FigureStats& s = model.stats.stats;
            const int cap = model.LevelCap();
            char text[64] = "";
            switch (row)
            {
            case 0: return figures::NicknameUtf8(s);
            case 1:
                // Giants: the hat picker's name; Spyro's Adventure: its own hat's number (the first area's).
                if (!model.Hats().empty())
                    return model.HatName(s.hat);
                snprintf(text, sizeof(text), "%u", cap > figures::MaxLevel ? s.hat : s.ssaHat);
                break;
            case 2:
                snprintf(text, sizeof(text), "%d", figures::LevelForExperience(figures::ExperienceFor(s, cap), cap));
                break;
            case 3: snprintf(text, sizeof(text), "%u", figures::ExperienceFor(s, cap)); break;
            case 4: snprintf(text, sizeof(text), "%u", s.gold); break;
            case 5: return Duration(s.playSeconds);
            case 6: snprintf(text, sizeof(text), "%u", s.heroPoints); break;
            case 7: snprintf(text, sizeof(text), "%d / 32", SetBits(s.heroicChallenges)); break;
            case 8:
            {
                const Figure& f = model.figures[model.stats.figure];
                if (f.toy == nullptr)
                    return "";
                collection::UpgradeTable table = collection::Upgrades(*f.toy, model.game, French() ? "fr" : "en");
                snprintf(text, sizeof(text), L("%d of %d", "%d sur %d"),
                    collection::BoughtCount(collection::EditorBits(s.upgrades, s.wowPow), table),
                    int(table.upgrades.size()));
                break;
            }
            }
            return text;
        }

        float FieldHeight(float height) { return Measure(g_bold, 13, "Ag").y + 6 + height; }

        void StatField(Model& model, int row, ImVec2 at, float width, float height, float size, bool gold = false)
        {
            bool focused = model.stats.row == row;
            std::string label = StatLabel(row, model.LevelCap());
            Text(g_bold, 13, at, gold ? Look::Gold : Look::TextDim, label);
            ImVec2 min(at.x, at.y + Measure(g_bold, 13, label).y + 6), max(at.x + width, min.y + height);
            Box(min, max, Look::Field, gold ? Look::GoldBorder : Look::Control, 12, 2);
            if (focused)
                FocusRing(min, max, 12);
            std::string value = StatValue(row, model);
            uint32_t color = gold ? Look::GoldText : Look::Text;
            if (value.empty())
            {
                value = row == 0 ? L("(none)", "(aucun)") : "";
                color = Look::TextDimmer;
            }
            bool arrows = focused && row != 0 && row != 8;
            Text(g_black, size, ImVec2(min.x + 12, (min.y + max.y) / 2 - Measure(g_black, size, "0").y / 2), color,
                Fit(g_black, size, value, width - 24 - (arrows ? 36 : 0)));
            if (arrows)
            {
                float cy = (min.y + max.y) / 2, x = max.x - 30;
                g_draw->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + 7, cy - 6), ImVec2(x + 7, cy + 6), Rgb(Look::Gold));
                g_draw->AddTriangleFilled(
                    ImVec2(x + 20, cy), ImVec2(x + 13, cy - 6), ImVec2(x + 13, cy + 6), Rgb(Look::Gold));
            }
            if (row == 8) // opens the Upgrades screen
                Chevron(ImVec2(max.x - 26, (min.y + max.y) / 2), 16, focused ? Look::GoldText : Look::TextDim, 0);
            AddHit(at, ImVec2(at.x + width, max.y), [&model, row] { model.FocusStatsRow(row); });
            g_hits.back().focus = [&model, row] {
                model.stats.row = row;
                model.Touch();
            };
        }

        // The creation editor's rows: label and value as the game stores them (its ids; "none" for a missing part).
        std::string CreationLabel(int row)
        {
            switch (row)
            {
            case 0: return L("Name", "Nom");
            case 1: return L("Class", "Classe");
            case 2: return L("Element", "Élément");
            case 3: return L("Level", "Niveau");
            default: break;
            }
            int i = row - CreationState::First;
            return i >= 0 && i < int(figures::CrystalParamCount) ? figures::CrystalParams()[size_t(i)].name : "";
        }

        std::string CreationValue(const Model& model, int row)
        {
            const figures::CrystalStats& c = model.creation.crystal;
            switch (row)
            {
            case 0: return c.name.empty() ? L("(none: the game asks)", "(aucun : le jeu le demande)") : c.name;
            case 1: return collection::BattleClassText(c.battleClass, French());
            case 2:
            {
                const Toy* toy = Catalogue::Get().Find(model.creation.type);
                std::string element = toy != nullptr ? ElementLabel(toy->element) : std::to_string(model.creation.type);
                return model.creation.virtualCrystal ? element : element + L(" (the toy's own)", " (celui du jouet)");
            }
            case 3:
                return std::to_string(figures::CrystalLevel(c.experience)) + " · " + std::to_string(c.experience) +
                       L(" XP", " XP");
            default: break;
            }
            int i = row - CreationState::First;
            if (i < 0 || i >= int(figures::CrystalParamCount))
                return {};
            const figures::CrystalParam& p = figures::CrystalParams()[size_t(i)];
            uint16_t v = c.creation[size_t(i)];
            if (v == (1u << p.bits) - 1 && p.kind != figures::CrystalParamKind::Colour &&
                p.kind != figures::CrystalParamKind::Shape)
                return L("none", "aucune");
            switch (p.kind)
            {
            case figures::CrystalParamKind::Colour: return L("colour ", "couleur ") + std::to_string(v);
            case figures::CrystalParamKind::Shape: return std::to_string(v) + " / 8";
            default: return PartLabel(model.game, p.min + v);
            }
        }

        void DrawCreation(Model& model)
        {
            CreationState& cs = model.creation;
            const Figure& f = model.figures[cs.figure];
            Header(L("CREATION", "CRÉATION"), model.FigureName(f));
            constexpr float Top = 88, Height = 530;
            {
                constexpr float Left = 32, Width = 300, Center = Left + Width / 2;
                Panel(ImVec2(Left, Top), ImVec2(Left + Width, Top + Height));
                float y = Top + 24;
                HeroPortrait(model, f, ImVec2(Center, y + 74), 148);
                y += 176;
                std::string name = cs.crystal.name.empty() ? model.FigureName(f) : cs.crystal.name;
                TextCentered(g_display, 24, ImVec2(Center, y + 12), 0xFFFFFF, Fit(g_display, 24, name, Width - 32));
                y += 40;
                std::string line = collection::BattleClassText(cs.crystal.battleClass, French()) +
                                   L(" · Level ", " · Niveau ") +
                                   std::to_string(figures::CrystalLevel(cs.crystal.experience));
                TextCentered(g_bold, 16, ImVec2(Center, y + 8), Look::TextSoft, line);
                y += 34;
                if (cs.virtualCrystal)
                    for (const std::string& l : Wrap(g_bold, 13, VirtualCrystalText(), Width - 48, 4))
                    {
                        TextCentered(g_bold, 13, ImVec2(Center, y + 6), Look::GoldText, l);
                        y += 18;
                    }
            }
            {
                constexpr float Left = 352, Width = 896, RowHeight = 38;
                Panel(ImVec2(Left, Top), ImVec2(Left + Width, Top + Height));
                const int shown = int((Height - 32) / RowHeight), last = CreationState::Rows() - 2;
                int focus = std::min(cs.row, last - 1);
                if (focus < cs.top)
                    cs.top = focus;
                if (focus >= cs.top + shown)
                    cs.top = focus - shown + 1;
                float y = Top + 16;
                for (int row = cs.top; row < std::min(last, cs.top + shown); row++, y += RowHeight)
                {
                    ImVec2 a(Left + 20, y), b(Left + Width - 20, y + RowHeight - 6);
                    bool focused = cs.row == row && model.screen == Screen::Creation;
                    FocusBox(a, b, focused, 10, Look::Raised);
                    TextMiddle(g_bold, 15, a.x + 16, (a.y + b.y) / 2, Look::TextSoft, CreationLabel(row));
                    std::string value = CreationValue(model, row);
                    float right = b.x - (focused && row != 0 ? 52 : 16);
                    TextMiddle(g_black, 16, right - Measure(g_black, 16, value).x, (a.y + b.y) / 2, Look::Text, value);
                    float cy = (a.y + b.y) / 2;
                    if (focused && row != 0)
                    {
                        float x = b.x - 40;
                        g_draw->AddTriangleFilled(
                            ImVec2(x, cy), ImVec2(x + 7, cy - 6), ImVec2(x + 7, cy + 6), Rgb(Look::Gold));
                        g_draw->AddTriangleFilled(
                            ImVec2(x + 20, cy), ImVec2(x + 13, cy - 6), ImVec2(x + 13, cy + 6), Rgb(Look::Gold));
                        AddHit(ImVec2(x - 6, a.y), ImVec2(x + 10, b.y), [&model] { model.Run(Command::Left); });
                        AddHit(ImVec2(x + 10, a.y), ImVec2(x + 26, b.y), [&model] { model.Run(Command::Right); });
                    }
                    AddHit(a, b, [&model, row] { model.FocusCreationRow(row); });
                    g_hits.back().focus = [&model, row] {
                        model.creation.row = row;
                        model.Touch();
                    };
                }
            }
            const int cancel = CreationState::Rows() - 2, save = CreationState::Rows() - 1;
            ImVec2 cancelMin(820, 636), cancelMax(1010, 680), saveMin(1030, 636), saveMax(1248, 680);
            PlainButton(cancelMin, cancelMax, L("Cancel", "Annuler"), 16, cs.row == cancel);
            GoldButton(saveMin, saveMax, L("Save", "Enregistrer"), 16, cs.row == save);
            AddHit(cancelMin, cancelMax, [&model, cancel] {
                model.FocusCreationRow(cancel);
                model.FocusCreationRow(cancel);
            });
            AddHit(saveMin, saveMax, [&model, save] {
                model.FocusCreationRow(save);
                model.FocusCreationRow(save);
            });
            TextMiddle(g_bold, 14, 32, 658, Look::TextSoft,
                cs.dirty      ? L("Changed: B or Save to keep it", "Modifiée : B ou Enregistrer pour garder")
                : cs.row == 0 ? L("A: type the name", "A : saisir le nom")
                              : L("Left / right: change · LB / RB or Shift: by 10 · in the game's order",
                                    "Gauche / droite : changer · LB / RB ou Maj : par 10 · dans l'ordre du jeu"));
        }

        void DrawStats(Model& model)
        {
            const Figure& f = model.figures[model.stats.figure];
            Header(L("SKYLANDER STATS", "FICHE DU SKYLANDER"), model.FigureName(f));
            std::string file = std::string(L("File: ", "Fichier : ")) + f.file;
            TextMiddle(g_bold, 14, 1248 - Measure(g_bold, 14, file).x, 42, Look::TextDim, Fit(g_bold, 14, file, 420));
            constexpr float Top = 88, Height = 530;
            {
                constexpr float Left = 32, Width = 300, Center = Left + Width / 2;
                Panel(ImVec2(Left, Top), ImVec2(Left + Width, Top + Height));
                float y = Top + 24;
                HeroPortrait(model, f, ImVec2(Center, y + 74), 148);
                y += 176;
                std::string name = model.FigureName(f);
                float size = 28;
                while (size > 18 && Measure(g_display, size, name).x > Width - 48)
                    size -= 2;
                TextCentered(g_display, size, ImVec2(Center, y + 14), 0xFFFFFF, Fit(g_display, size, name, Width - 32));
                y += 40;
                if (f.toy != nullptr && f.toy->element < Element::None)
                    Pill(ImVec2(Center, y + 13), ElementLabel(f.toy->element), ColourOf(f.toy), 0xFFFFFF);
                y += 40;
                for (int row : { 0, 1 })
                {
                    StatField(model, row, ImVec2(Left + 24, y), Width - 48, 42, 16);
                    y += FieldHeight(42) + 14;
                }
            }
            {
                constexpr float Left = 352, Width = 896, Inner = Width - 48;
                Panel(ImVec2(Left, Top), ImVec2(Left + Width, Top + Height));
                float x = Left + 24, y = Top + 24;
                float width = (Inner - 28) / 3;
                const int grid[2][3] = { { 2, 3, 4 }, { 5, 6, 7 } };
                for (const auto& line : grid)
                {
                    for (int i = 0; i < 3; i++)
                        StatField(model, line[i], ImVec2(x + i * (width + 14), y), width, 46, 20, line[i] == 4);
                    y += FieldHeight(46) + 18;
                }
                StatField(model, 8, ImVec2(x, y), Inner, 46, 20);
                y += FieldHeight(46) + 18;
                int seatIndex = model.SeatOf(f.path);
                std::string where = seatIndex >= 0 ? std::string(L("On the portal as ", "Sur le portail en ")) +
                                                         model.SeatLabel(seatIndex)
                                                   : std::string(L("Not on the portal.", "Pas sur le portail."));
                for (const std::string& line : Wrap(g_bold, 14, where, Inner))
                {
                    Text(g_bold, 14, ImVec2(x, y), Look::TextDim, line);
                    y += 20;
                }
            }
            // Footer: hint or the buttons.
            ImVec2 cancelMin(820, 636), cancelMax(1010, 680), saveMin(1030, 636), saveMax(1248, 680);
            PlainButton(cancelMin, cancelMax, L("Cancel", "Annuler"), 16, model.stats.row == 9);
            GoldButton(saveMin, saveMax, L("Save", "Enregistrer"), 16, model.stats.row == 10);
            AddHit(cancelMin, cancelMax, [&model] {
                model.FocusStatsRow(9);
                model.FocusStatsRow(9);
            });
            AddHit(saveMin, saveMax, [&model] {
                model.FocusStatsRow(10);
                model.FocusStatsRow(10);
            });
            TextMiddle(g_bold, 14, 32, 658, Look::TextSoft,
                model.stats.dirty      ? L("Changed: B or Save to keep it", "Modifiée : B ou Enregistrer pour garder")
                : model.stats.row == 8 ? L("A: choose the upgrades", "A : choisir les améliorations")
                                       : L("Left / right: change · LB / RB or Shift: by 10",
                                             "Gauche / droite : changer · LB / RB ou Maj : par 10"));
        }

        // --- Upgrades: by name, the basic ones, the two paths side by side, the Soul Gem ---------------------------------------

        void DrawUpgrades(Model& model)
        {
            const Figure& f = model.figures[model.stats.figure];
            UpgradesState& u = model.upgrades;
            uint16_t bits = collection::EditorBits(model.stats.stats.upgrades, model.stats.stats.wowPow);
            std::string language = French() ? "fr" : "en";
            Header(L("UPGRADES", "AMÉLIORATIONS"), model.FigureName(f));
            constexpr float Left = 32, Gap = 16, Width = (1216 - 2 * Gap) / 3, Top = 88, Card = 58, Step = 66;
            auto columnX = [&](int column) { return Left + column * (Width + Gap); };
            // Column titles: the basic ones, then the two paths (the chosen one lit).
            bool chosen = (bits >> collection::UpgradePathChosenBit & 1) != 0,
                 second = (bits >> collection::UpgradePathSecondBit & 1) != 0;
            const std::string titles[3] = { L("Basic", "De base"), u.table.pathNames[0], u.table.pathNames[1] };
            for (int column = 0; column < 3; column++)
            {
                bool lit = column == 0 || (chosen && second == (column == 2));
                TextMiddle(g_black, 17, columnX(column), Top + 12, lit ? Look::Gold : Look::TextDimmer,
                    Fit(g_black, 17, titles[column], Width));
            }
            std::string detail;
            for (size_t i = 0; i < u.items.size(); i++)
            {
                const UpgradeItem& item = u.items[i];
                bool focused = int(i) == u.cursor;
                if (item.kind != UpgradeItem::Kind::Upgrade)
                {
                    float x = columnX(item.column);
                    ImVec2 a(x, 600), b(x + (item.column == 2 ? 200 : 180), 644);
                    if (item.column == 2)
                    {
                        a.x = 1248 - 200;
                        b.x = 1248;
                    }
                    std::string label = item.kind == UpgradeItem::Kind::All    ? L("All", "Tout")
                                        : item.kind == UpgradeItem::Kind::None ? L("None", "Aucune")
                                                                               : L("Done", "OK");
                    if (item.kind == UpgradeItem::Kind::Done)
                        GoldButton(a, b, label, 16, focused);
                    else
                        PlainButton(a, b, label, 16, focused);
                    int index = int(i);
                    AddHit(a, b, [&model, index] { model.FocusUpgrade(index); });
                    continue;
                }
                const collection::Upgrade& upgrade = u.table.upgrades[item.index];
                bool bought = collection::UpgradeBought(bits, upgrade),
                     greyed = collection::UpgradeGreyed(bits, upgrade);
                float x = columnX(item.column), y = Top + 36 + item.row * Step;
                ImVec2 a(x, y), b(x + Width, y + Card);
                FocusBox(a, b, focused, 14, Look::Raised);
                ImVec2 mark(a.x + 28, (a.y + b.y) / 2);
                Disc(mark, 26, bought ? Look::Gold : Look::Field);
                if (!bought)
                    Ring(mark, 26, greyed ? Look::CardBorder : Look::Control, 2);
                else
                    TextCentered(g_black, 16, mark, Look::GoldInk, "✓");
                uint32_t ink = greyed ? Look::TextDimmer : Look::Text;
                std::string price = std::to_string(upgrade.price) + L(" gold", " or");
                float priceW = Measure(g_bold, 12, price).x;
                TextMiddle(g_black, 15, a.x + 54, (a.y + b.y) / 2, ink,
                    Fit(g_black, 15, collection::UpgradeName(upgrade.name, language), Width - 54 - priceW - 28));
                TextMiddle(
                    g_bold, 12, b.x - 14 - priceW, (a.y + b.y) / 2, bought ? Look::GoldText : Look::TextDimmer, price);
                if (focused)
                    detail = collection::UpgradeLine(upgrade.description, language);
                int index = int(i);
                AddHit(a, b, [&model, index] { model.FocusUpgrade(index); });
            }
            if (!detail.empty())
                TextMiddle(g_bold, 15, Left, 566, Look::TextSoft, Fit(g_bold, 15, detail, 1216));
            Footer({ { 'A', L("Buy / give back", "Acheter / rendre") }, { 'B', L("Back", "Retour") } });
        }

        // --- Add a figure ---------------------------------------------------------------------------------------------

        void DrawAdd(Model& model)
        {
            char subtitle[128];
            snprintf(subtitle, sizeof(subtitle), L("toys %s reads · %d", "jouets que %s lit · %d"),
                model.GameName().c_str(), int(model.addToys.size()));
            Header(L("ADD A FIGURE", "AJOUTER UNE FIGURINE"), subtitle);
            Hints(1248, 44, { { 'B', L("Back", "Retour") } });
            Grid& grid = model.add;
            ImVec2 min(32, 88), max(824, 664);
            SectionStrip(model, grid, min, max.x - min.x);
            ImVec2 listMin(min.x, min.y + 40);
            grid.visibleHeight = int(max.y - listMin.y);
            float gap = 10, cardWidth = (max.x - min.x - gap * (Model::AddColumns - 1)) / Model::AddColumns,
                  cardHeight = float(grid.rowHeight - 8);
            ImVec2 ringMin, ringMax;
            bool ring = false;
            g_draw->PushClipRect(ImVec2(listMin.x - 8, listMin.y), ImVec2(max.x + 8, max.y), true);
            for (const ListSection& section : grid.sections)
            {
                float y = listMin.y + grid.RowY(section.firstRow) - grid.headerHeight - grid.scroll;
                if (y > listMin.y - 40 && y < max.y)
                    TextMiddle(g_black, 17, listMin.x, y + 18, Look::Gold, StripLabel(model, grid, section));
            }
            for (size_t i = 0; i < grid.cells.size(); i++)
            {
                const Cell& cell = grid.cells[i];
                float x = listMin.x + cell.column * (cardWidth + gap),
                      y = listMin.y + grid.RowY(cell.row) - grid.scroll;
                if (y + cardHeight < listMin.y || y > max.y)
                    continue;
                const Toy* toy = model.addToys[cell.figure];
                ImVec2 a(x, y), b(x + cardWidth, y + cardHeight);
                bool cursor = int(i) == grid.cursor;
                // The toy whose versions show stays lit while the focus is in them.
                Box(a, b, cursor ? Look::CardSelected : Look::Raised, Look::CardBorder, 14, 1);
                if (!SwapperStackOfToy(toy, 0, ImVec2(x + 34, y + cardHeight / 2), 52))
                    Portrait(toy, toy->edition, ImVec2(x + 34, y + cardHeight / 2), 48);
                const size_t versionCount = Catalogue::Get().VersionsFor(model.game, *toy).size();
                Text(g_black, 14, ImVec2(x + 66, y + 12), Look::Text,
                    Fit(g_black, 14, Model::AddName(*toy), cardWidth - 74));
                int owned = 0;
                for (const Figure& f : model.figures)
                    owned += f.type == toy->type ? 1 : 0;
                std::string line = owned == 0   ? L("not owned", "pas à vous")
                                   : owned == 1 ? L("owned", "à vous")
                                                : std::string(L("owned ×", "à vous ×")) + std::to_string(owned);
                Text(g_bold, 12, ImVec2(x + 66, y + 34), owned > 0 ? Look::Bar : Look::TextDimmer,
                    Fit(g_bold, 12, line, cardWidth - 74 - (versionCount == 0 ? 0 : 44)));
                // It has versions this game tells apart: a small badge with their count, on the second line so that the
                // name keeps the card's width.
                if (versionCount != 0)
                    Pill(ImVec2(b.x - 28, y + 41), "+" + std::to_string(versionCount), Look::Gold, Look::GoldInk, 12);
                if (cursor && !model.addVersions)
                {
                    ring = true;
                    ringMin = a;
                    ringMax = b;
                }
                int index = int(i);
                AddHit(a, b, [&model, index] { model.FocusAddCell(index); });
                g_hits.back().focus = [&model, index] { model.HoverAddCell(index); };
            }
            g_draw->PopClipRect();
            if (ring)
                FocusRing(ringMin, ringMax, 14);

            // Versions.
            ImVec2 pmin(844, 88), pmax(1248, 664);
            Panel(pmin, pmax);
            if (!grid.cells.empty())
            {
                const Toy* toy = model.addToys[grid.cells[grid.cursor].figure];
                float y = pmin.y + 20;
                if (!SwapperStackOfToy(toy, 0, ImVec2(pmin.x + 52, y + 32), 68))
                    Portrait(toy, toy->edition, ImVec2(pmin.x + 52, y + 32), 64);
                Text(g_display, 20, ImVec2(pmin.x + 96, y + 8), 0xFFFFFF,
                    Fit(g_display, 20, Model::AddName(*toy), pmax.x - pmin.x - 110));
                Text(g_bold, 14, ImVec2(pmin.x + 96, y + 38), Look::TextDim, L("Versions", "Versions"));
                y += 84;
                float rowHeight = std::min(46.0f, (pmax.y - y - 110) / std::max(int(model.versions.size()), 1));
                for (size_t v = 0; v < model.versions.size(); v++)
                {
                    const Version& version = model.versions[v];
                    ImVec2 a(pmin.x + 16, y), b(pmax.x - 16, y + rowHeight - 6);
                    bool focused = model.addVersions && model.addVersion == int(v);
                    FocusBox(a, b, focused, 10, Look::Night);
                    // The version's picture (the toy's own when it has none), its name and its kind.
                    float middle = (a.y + b.y) / 2, picture = std::min(36.0f, b.y - a.y - 6);
                    if (!SwapperStackOfToy(toy, version.variant, ImVec2(a.x + 8 + picture / 2, middle), int(picture)) &&
                        !ArtworkPicture(
                            toy, version.variant, ImVec2(a.x + 8 + picture / 2, middle), int(picture), false))
                        Portrait(toy, version.kind, ImVec2(a.x + 8 + picture / 2, middle), int(picture), false,
                            version.variant);
                    float textX = a.x + 16 + picture;
                    Text(g_black, 14, ImVec2(textX, middle - (version.kind == VariantKind::Base ? 9 : 17)), Look::Text,
                        Fit(g_black, 14, version.name, b.x - textX - 100));
                    if (version.kind != VariantKind::Base)
                        Text(g_bold, 12, ImVec2(textX, middle + 2), Look::TextDim, VariantLabel(version.kind));
                    std::string mark = version.owned == 0 ? (version.edition.empty() ? "" : L("same tag", "même puce"))
                                       : version.owned == 1
                                           ? L("owned", "à vous")
                                           : std::string(L("owned ×", "à vous ×")) + std::to_string(version.owned);
                    TextMiddle(g_bold, 12, b.x - 12 - Measure(g_bold, 12, mark).x, (a.y + b.y) / 2,
                        version.owned > 0 ? Look::Bar : Look::TextDimmer, mark);
                    int index = int(v);
                    AddHit(a, b, [&model, index] { model.FocusVersion(index); });
                    g_hits.back().focus = [&model, index] {
                        model.addVersions = true;
                        model.addVersion = index;
                        model.Touch();
                    };
                    y += rowHeight;
                }
            }
            Footer({ { 'A', model.addVersions ? L("Create", "Créer") : L("Choose", "Choisir") },
                { 'L', L("Section", "Section") }, { 'B', L("Back", "Retour") } });
        }

        // --- Names -----------------------------------------------------------------------------------------------------

        std::string KeyLabel(const std::string& key, const NameEntry& name)
        {
            if (key == "\x01"
                       "shift")
                return L("Shift", "Maj");
            if (key == "\x01"
                       "symbols")
                return name.symbols ? "ABC" : "123";
            if (key == "\x01"
                       "space")
                return L("space", "espace");
            if (key == "\x01"
                       "delete")
                return L("Delete", "Effacer");
            if (key == "\x01"
                       "done")
                return L("Done", "OK");
            return key;
        }

        void DrawName(Model& model)
        {
            NameEntry& name = model.name;
            Header(name.title, {});
            float y = 96;
            // The field.
            ImVec2 fmin(160, y), fmax(1120, y + 64);
            Box(fmin, fmax, Look::Field, Look::Gold, 14, 3);
            std::string before = name.text.substr(0, name.cursor);
            float tx = fmin.x + 20, ty = y + 32 - Measure(g_black, 28, "Ag").y / 2;
            Text(g_black, 28, ImVec2(tx, ty), Look::Text, name.text);
            float caret = tx + Measure(g_black, 28, before).x + 1;
            Fill(ImVec2(caret, y + 14), ImVec2(caret + 3, y + 50), Look::Gold);
            char limit[64];
            snprintf(limit, sizeof(limit), L("up to %d characters", "jusqu'à %d caractères"), int(name.limit));
            TextMiddle(g_bold, 13, fmax.x - 16 - Measure(g_bold, 13, limit).x, y + 32, Look::TextDimmer, limit);
            y += 86;
            if (!name.suggestions.empty())
            {
                Text(g_bold, 14, ImVec2(160, y), Look::TextDim, L("Suggestions:", "Suggestions :"));
                y += 24;
                float x = 160;
                for (size_t i = 0; i < name.suggestions.size(); i++)
                {
                    float w = Measure(g_black, 15, name.suggestions[i]).x + 32;
                    if (x + w > 1120)
                    {
                        x = 160;
                        y += 52;
                    }
                    ImVec2 a(x, y), b(x + w, y + 40);
                    bool focused = !name.onKeys && name.focus == int(i);
                    FocusBox(a, b, focused, 20, Look::Raised);
                    TextCentered(g_black, 15, ImVec2(x + w / 2, y + 20), Look::Text, name.suggestions[i]);
                    int index = int(i);
                    AddHit(a, b, [&model, index] { model.FocusSuggestion(index); });
                    g_hits.back().focus = [&model, index] {
                        model.name.onKeys = false;
                        model.name.focus = index;
                        model.Touch();
                    };
                    x += w + 12;
                }
                y += 64;
            }
            const auto& keys = Model::Keys(French(), name.symbols, name.shift);
            float keyWidth = 84, keyHeight = 58, gap = 10;
            for (size_t r = 0; r < keys.size(); r++)
            {
                bool special = r + 1 == keys.size();
                float rowWidth = 0;
                std::vector<float> widths;
                for (const std::string& key : keys[r])
                {
                    float w = special ? (key == "\x01"
                                                "space"
                                                ? keyWidth * 3
                                                : keyWidth * 1.5f)
                                      : keyWidth;
                    widths.push_back(w);
                    rowWidth += w + gap;
                }
                float x = 640 - (rowWidth - gap) / 2;
                for (size_t c = 0; c < keys[r].size(); c++)
                {
                    ImVec2 a(x, y), b(x + widths[c], y + keyHeight);
                    bool focused = name.onKeys && name.keyRow == int(r) && name.keyColumn == int(c);
                    bool lit = (keys[r][c] == "\x01"
                                              "shift" &&
                                   name.shift) ||
                               (keys[r][c] == "\x01"
                                              "done");
                    Box(a, b, focused ? Look::CardSelected : lit ? Look::Button : Look::KeyCap, Look::Control, 10, 1);
                    std::string label = KeyLabel(keys[r][c], name);
                    TextCentered(
                        g_black, special ? 16 : 24, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), Look::Text, label);
                    if (focused)
                        FocusRing(a, b, 10);
                    int row = int(r), column = int(c);
                    AddHit(a, b, [&model, row, column] { model.FocusKey(row, column); });
                    g_hits.back().focus = [&model, row, column] {
                        model.name.onKeys = true;
                        model.name.keyRow = row;
                        model.name.keyColumn = column;
                        model.Touch();
                    };
                    x += widths[c] + gap;
                }
                y += keyHeight + gap;
            }
            if (name.purpose == NameEntry::Purpose::Nickname)
                Text(g_bold, 13, ImVec2(160, y + 6), Look::TextDimmer,
                    L("15 characters at most.", "15 caractères au plus."));
            else if (name.purpose == NameEntry::Purpose::Rename || name.purpose == NameEntry::Purpose::NewFigure)
                Text(g_bold, 13, ImVec2(160, y + 6), Look::TextDimmer,
                    L("Shown in the menus.", "Affiché dans les menus."));
            Footer({ { 'A', L("Type", "Taper") }, { 'X', L("Delete", "Effacer") }, { 'L', L("Cursor", "Curseur") },
                { 'Y', L("Done", "OK") }, { 'B', L("Cancel", "Annuler") } });
        }

        // --- Manage ---------------------------------------------------------------------------------------------------

        void DrawManage(Model& model)
        {
            ManageState& m = model.manage;
            char subtitle[96];
            snprintf(subtitle, sizeof(subtitle), L("collection · %d figures", "collection · %d figurines"),
                int(model.figures.size()));
            Header(L("MANAGE", "GÉRER"), subtitle);
            Hints(1248, 44, { { 'B', L("Back", "Retour") } });
            const std::string names[] = { L("Trash", "Corbeille"), L("Backups", "Sauvegardes"), L("Import", "Importer"),
                L("Export", "Exporter"), L("Unknown toys", "Jouets inconnus"), L("Tidy", "Ranger") };
            const int counts[] = { int(m.trash.size()), int(m.withBackups.size()), -1, -1, int(m.unknown.size()),
                int(m.starters.size()) };
            ImVec2 min(32, 88), max(330, 664);
            Panel(min, max);
            float y = min.y + 20;
            for (int i = 0; i < 6; i++)
            {
                ImVec2 a(min.x + 16, y), b(max.x - 16, y + 52);
                bool lit = m.section == i;
                bool focused = lit && !m.inPanel;
                Box(a, b, lit ? Look::CardSelected : Look::Raised, Look::CardBorder, 12, 1);
                if (focused)
                    FocusRing(a, b, 12);
                TextMiddle(g_black, 17, a.x + 16, y + 26, Look::Text, names[i]);
                if (counts[i] >= 0)
                    TextMiddle(g_black, 16, b.x - 16 - Measure(g_black, 16, std::to_string(counts[i])).x, y + 26,
                        Look::TextDim, std::to_string(counts[i]));
                AddHit(a, b, [&model, i] { model.FocusManage(i, 0, false); });
                y += 62;
            }
            ImVec2 pmin(350, 88), pmax(1248, 664);
            Panel(pmin, pmax);
            float x = pmin.x + 24;
            y = pmin.y + 20;
            auto heading = [&](const std::string& text) {
                Text(g_display, 22, ImVec2(x, y), Look::Gold, text);
                y += 40;
            };
            auto note = [&](const std::string& text) {
                for (const std::string& line : Wrap(g_bold, 14, text, pmax.x - pmin.x - 48))
                {
                    Text(g_bold, 14, ImVec2(x, y), Look::TextDim, line);
                    y += 20;
                }
                y += 8;
            };
            auto row = [&](int item, const std::string& left, const std::string& right, const Figure* figure,
                           const Toy* toy = nullptr) {
                ImVec2 a(x, y), b(pmax.x - 24, y + 50);
                bool focused = m.inPanel && m.item == item;
                FocusBox(a, b, focused, 12, Look::Raised);
                if (figure != nullptr)
                    FigurePortrait(*figure, ImVec2(a.x + 28, y + 25), 38);
                else if (toy != nullptr)
                    Portrait(toy, VariantKind::Base, ImVec2(a.x + 28, y + 25), 38);
                TextMiddle(g_black, 15, a.x + 56, y + 25, Look::Text, Fit(g_black, 15, left, b.x - a.x - 300));
                TextMiddle(g_bold, 13, b.x - 16 - Measure(g_bold, 13, right).x, y + 25, Look::TextDim, right);
                int section = m.section;
                AddHit(a, b, [&model, section, item] { model.FocusManage(section, item, true); });
                g_hits.back().focus = [&model, item] {
                    model.manage.inPanel = true;
                    model.manage.item = item;
                    model.Touch();
                };
                y += 58;
            };
            auto button = [&](const std::string& label) {
                ImVec2 a(x, y), b(x + 360, y + 50);
                GoldButton(a, b, label, 15, m.inPanel);
                int section = m.section;
                AddHit(a, b, [&model, section] {
                    model.FocusManage(section, 0, true);
                    model.FocusManage(section, 0, true);
                });
                y += 66;
            };
            switch (m.section)
            {
            case 0:
                heading(L("Trash (kept 30 days)", "Corbeille (gardée 30 jours)"));
                if (m.trash.empty())
                    note(L("Empty.", "Vide."));
                for (int i = std::max(0, m.item - 6); i < int(m.trash.size()) && y < pmax.y - 60; i++)
                {
                    const TrashEntry& e = m.trash[i];
                    std::string when = e.deleted.substr(0, 10);
                    std::string why = e.reason == "tidy" ? L("never played · tidy, ", "jamais jouée · rangement, ")
                                                         : L("deleted ", "supprimée le ");
                    std::string stem = e.file.substr(0, e.file.find_last_of('.'));
                    bool numbered = stem.size() > 4 && isdigit(uint8_t(stem[0])) && stem[3] == ' ';
                    uint16_t type = uint16_t(atoi(stem.c_str()));
                    row(i, numbered ? stem.substr(4) : stem, why + when + L(" · A: put back", " · A : remettre"),
                        nullptr, numbered ? Catalogue::Get().Find(type) : nullptr);
                }
                break;
            case 1:
                heading(L("Backups", "Sauvegardes"));
                note(L("The last 10 per figure are kept.", "Les 10 dernières par figurine sont gardées."));
                for (int i = std::max(0, m.item - 6); i < int(m.withBackups.size()) && y < pmax.y - 60; i++)
                {
                    const Figure& f = model.figures[m.withBackups[i]];
                    row(i, model.FigureName(f),
                        std::to_string(Backups(f.path).size()) +
                            L(" backups · A: restore one", " sauvegardes · A : en restaurer une"),
                        &f);
                }
                break;
            case 2:
                heading(L("Import", "Importer"));
                note(L("Put figure files in this folder, then import.",
                    "Mettez des fichiers de figurines dans ce dossier, puis importez."));
                note(m.importFolder);
                button(L("Import now", "Importer maintenant"));
                note(L("Not available on Xbox yet.", "Pas encore disponible sur Xbox."));
                break;
            case 3:
                heading(L("Export", "Exporter"));
                note(L("Copies everything into a dated folder:", "Copie tout dans un dossier daté :"));
                note(m.exportFolder);
                button(L("Export all figures", "Exporter toutes les figurines"));
                break;
            case 4:
                heading(L("Unknown toys", "Jouets inconnus"));
                if (m.unknown.empty())
                    note(L("None.", "Aucun."));
                else
                    note(L("Toys missing from the catalogue. Name one to show it.",
                        "Jouets absents du catalogue. Nommes-en un pour l'afficher."));
                for (int i = 0; i < int(m.unknown.size()) && y < pmax.y - 60; i++)
                {
                    const Figure& f = model.figures[m.unknown[i]];
                    row(i, model.FigureName(f), L("A: name this toy", "A : nommer ce jouet"), &f);
                }
                break;
            case 5:
            {
                heading(L("Tidy", "Ranger"));
                char text[200];
                snprintf(text, sizeof(text),
                    L("%d blank figures made by old game versions. Tidy moves them to the trash.",
                        "%d figurines vierges créées par d'anciennes versions du jeu. Ranger les met à la corbeille."),
                    int(m.starters.size()));
                note(text);
                button(L("Tidy now", "Ranger maintenant"));
                break;
            }
            }
            std::vector<HintItem> hints = { { 'A', m.inPanel ? L("Do it", "Valider") : L("Open", "Ouvrir") } };
            if (m.section == 0 && m.inPanel)
                hints.push_back({ 'X', L("Delete for good (hold)", "Supprimer pour de bon (maintenir)") });
            hints.push_back({ 'B', L("Back", "Retour") });
            Footer(hints);
        }

        // --- Dialog ------------------------------------------------------------------------------------------------------

        void DrawDialog(Model& model)
        {
            if (model.dialogs.empty())
                return;
            Dialog& dialog = model.dialogs.back();
            AddHit(ImVec2(0, 0), ImVec2(CanvasWidth, CanvasHeight), {});
            g_hits.back().hover = false;
            Fill(ImVec2(0, 0), ImVec2(CanvasWidth, CanvasHeight), Look::Night, 0.6f);
            float width = 600;
            std::vector<std::string> lines =
                dialog.text.empty() ? std::vector<std::string>() : Wrap(g_bold, 16, dialog.text, width - 56);
            float height = 80 + lines.size() * 22 + (lines.empty() ? 0 : 16) + dialog.buttons.size() * 58 + 10;
            ImVec2 min(640 - width / 2, 360 - height / 2), max(640 + width / 2, 360 + height / 2);
            Panel(min, max);
            float y = min.y + 22;
            Text(g_display, 22, ImVec2(min.x + 28, y), Look::Gold, Fit(g_display, 22, dialog.title, width - 56));
            y += 46;
            for (const std::string& line : lines)
            {
                Text(g_bold, 16, ImVec2(min.x + 28, y), Look::TextSoft, line);
                y += 22;
            }
            if (!lines.empty())
                y += 16;
            for (size_t i = 0; i < dialog.buttons.size(); i++)
            {
                ImVec2 a(min.x + 28, y), b(max.x - 28, y + 48);
                bool focused = dialog.focused == int(i);
                if (dialog.hold && i == 0)
                {
                    Box(a, b, Look::Chip, Look::Danger, 12, 2);
                    if (model.holdProgress > 0)
                        RoundRect(a, ImVec2(a.x + (b.x - a.x) * model.holdProgress, b.y), Look::Danger, 12);
                    TextCentered(
                        g_black, 16, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), Look::DangerText, dialog.buttons[i]);
                    if (focused)
                        FocusRing(a, b, 12);
                }
                else
                    PlainButton(a, b, dialog.buttons[i], 16, focused);
                int index = int(i);
                AddHit(a, b, [&model, index] { model.FocusDialogButton(index); });
                g_hits.back().focus = [&model, index] {
                    model.dialogs.back().focused = index;
                    model.Touch();
                };
                y += 58;
            }
        }

        // --- The F3 menu, over the game (docs/runtime/host/gamemenu.md) ----------------------------------------------------

        // A switch: a knob on a track, gold (with a dark knob) when on.
        void Switch(ImVec2 center, bool on)
        {
            ImVec2 a(center.x - 22, center.y - 12), b(center.x + 22, center.y + 12);
            RoundRect(a, b, on ? Look::Gold : Look::Control, 12);
            Disc(ImVec2(on ? b.x - 12 : a.x + 12, center.y), 18, on ? Look::GoldInk : Look::Text);
        }

        // The Commands page: the actions down, a column per device (Model::CommandHeaders) across, then the mouse's
        // switch, the Toy Box's keys, Reset and Back.
        void DrawCommands(Model& model)
        {
            using namespace look;
            std::vector<CommandLine> lines = model.CommandLines();
            std::vector<std::string> headers = model.CommandHeaders();
            const CommandsLayout layout = look::Commands(int(lines.size()), model.commandRow, !model.toast.empty());
            ImVec2 min(SafeLeft, SafeTop), max(SafeRight, SafeBottom);
            Panel(min, max, 0.97f);
            Title(L("Commands", "Commandes"), ImVec2(min.x + Space::XL, min.y + Space::L), Type::Title);
            constexpr float CellWidth = 220, Gap = Space::S;
            const float cellsLeft = max.x - Space::XL - float(headers.size()) * (CellWidth + Gap) + Gap;
            auto cellLeft = [&](size_t column) { return cellsLeft + float(column) * (CellWidth + Gap); };
            const float top = layout.firstRow;
            const int total = int(lines.size());
            // Page buttons remain reachable with the mouse; keyboard navigation reveals its focused row too.
            if (total > layout.capacity)
            {
                const float cy = min.y + 30;
                TextMiddle(g_bold, Type::Small, max.x - 200, cy, Look::TextDim,
                    std::to_string(layout.first + 1) + "-" + std::to_string(layout.first + layout.count) + " / " +
                        std::to_string(total));
                for (int direction : { -1, 1 })
                {
                    float x = max.x - (direction < 0 ? 96 : 56);
                    ImVec2 a(x, cy - 14), b(x + 32, cy + 14);
                    bool enabled = direction < 0 ? layout.first > 0 : layout.first + layout.count < total;
                    Box(a, b, Look::Raised2, Look::Hairline, Radius::Key, 1);
                    Chevron(ImVec2(x + 16, cy), 12, enabled ? Look::Text : Look::TextDimmer, direction < 0 ? 2 : 0);
                    if (enabled)
                    {
                        int row = std::clamp(layout.first + direction * layout.capacity, 0, total - 1);
                        AddHit(a, b, [&model, row] {
                            if (model.waiting == Model::Waiting::None)
                            {
                                model.commandRow = row;
                                model.Touch();
                            }
                        });
                        g_hits.back().target = direction < 0 ? "commands=previous" : "commands=next";
                    }
                }
            }
            for (size_t c = 0; c < headers.size(); c++)
                TextCentered(
                    g_bold, Type::Small, ImVec2(cellLeft(c) + CellWidth / 2, top - 16), Look::TextDim, headers[c]);
            for (int index = layout.first; index < layout.first + layout.count; index++)
            {
                const CommandLine& line = lines[size_t(index)];
                float y = top + float(index - layout.first) * CommandsPitch;
                float cy = y + layout.rowHeight / 2;
                bool focusedRow = model.commandRow == index;
                ImVec2 rowA(min.x + Space::M, y), rowB(max.x - Space::M, y + layout.rowHeight);
                if (line.cells.empty())
                {
                    if (focusedRow)
                    {
                        RoundRect(rowA, rowB, Look::CardSelected, Radius::Control);
                        DenseFocusRing(rowA, rowB, Radius::Control);
                    }
                    if (!line.value.empty())
                    {
                        TextMiddle(g_black, Type::Body, rowA.x + Space::L, cy, Look::Text, line.label);
                        bool on = line.value == L("On", "Oui");
                        float right = cellLeft(0) + CellWidth;
                        Switch(ImVec2(right - 22, cy), on);
                        TextMiddle(g_bold, Type::Small,
                            right - 44 - Space::S - Measure(g_bold, Type::Small, line.value).x, cy,
                            on ? Look::Text : Look::TextDim, line.value);
                    }
                    else
                    {
                        TextMiddle(g_black, Type::Body, rowA.x + Space::L, cy,
                            index + 1 == total ? Look::TextSoft : Look::Text, line.label);
                        if (index + 1 != total)
                            Chevron(ImVec2(rowB.x - Space::L, cy), 12, Look::TextDim, 0);
                    }
                    AddHit(rowA, rowB, [&model, index] { model.FocusCommand(index, -1); });
                    g_hits.back().target = "commands=" + std::to_string(index);
                    continue;
                }
                TextMiddle(g_black, Type::Body, rowA.x + Space::L, cy, Look::Text,
                    Fit(g_black, Type::Body, line.label, cellsLeft - rowA.x - Space::XL));
                for (size_t c = 0; c < line.cells.size(); c++)
                {
                    const CommandLine::Cell& cell = line.cells[c];
                    if (cell.text.empty())
                        continue; // a Toy Box key has no pad or mouse cell
                    ImVec2 a(cellLeft(c), y), b(cellLeft(c) + CellWidth, y + layout.rowHeight);
                    bool focused = focusedRow && model.commandColumn == int(c);
                    Box(a, b,
                        focused         ? Look::CardSelected
                        : cell.editable ? Look::Raised2
                                        : Look::Surface,
                        Look::Hairline, Radius::Control, 1);
                    if (focused)
                        DenseFocusRing(a, b, Radius::Control);
                    TextCentered(g_black, Type::Small, ImVec2((a.x + b.x) / 2, cy),
                        cell.editable ? Look::Text : Look::TextDim,
                        Fit(g_black, Type::Small, cell.text, CellWidth - Space::XL));
                    int column = int(c);
                    AddHit(a, b, [&model, index, column] { model.FocusCommand(index, column); });
                    g_hits.back().target = "commands=" + std::to_string(index) + "/" + std::to_string(column);
                }
            }
            if (model.waiting != Model::Waiting::None)
                TextMiddle(g_bold, Type::Small, min.x + Space::XL, max.y - MenuFooter / 2, Look::TextDim,
                    model.waiting == Model::Waiting::Key
                        ? L("Press the new key (Escape cancels)", "Appuyez sur la nouvelle touche (Échap pour annuler)")
                    : model.waiting == Model::Waiting::PadButton
                        ? L("Press the new button on the pad (Escape cancels)",
                              "Appuyez sur le nouveau bouton de la manette (Échap pour annuler)")
                        : L("Click a mouse button or turn the wheel (Escape cancels)",
                              "Cliquez avec un bouton de la souris ou tournez la molette (Échap pour annuler)"));
            else
                Hints(max.x - Space::XL, max.y - MenuFooter / 2,
                    { { 'A', L("Change", "Changer") }, { 'B', L("Back", "Retour") } });
        }

        // A sheet on the left of the safe area (look::Menu): the title, the rows as tall as fit (all 14 rows clear the
        // hints), the note and the hints at the bottom. A row is its label on the left and its value on the right: a switch,
        // or a value between two chevrons that the mouse steps (the left half -1, the right half +1). The focus is the
        // white ring; gold marks what is on.
        void DrawMenu(Model& model)
        {
            using namespace look;
            Fill(ImVec2(0, 0), ImVec2(look::CanvasWidth, look::CanvasHeight), Look::Scrim, 0.45f);
            if (model.commandsOpen)
            {
                DrawCommands(model);
                return;
            }
            std::vector<MenuRow> rows = model.MenuRows();
            const MenuLayout layout = look::Menu(int(rows.size()));
            ImVec2 min(layout.left, layout.top), max(layout.right, layout.bottom);
            Panel(min, max, 0.97f);
            float titleWidth = Title(L("Game menu", "Menu du jeu"), ImVec2(min.x + Space::XL, min.y + 22), Type::Title);
            float nameLeft = min.x + Space::XL + titleWidth + Space::L;
            std::string game = Fit(g_bold, Type::Small, model.GameName(), max.x - Space::XL - nameLeft);
            Text(g_bold, Type::Small, ImVec2(max.x - Space::XL - Measure(g_bold, Type::Small, game).x, min.y + 34),
                Look::TextDim, game);
            const std::string& version = gamemenu::Current().version;
            if (!version.empty())
                Text(g_bold, Type::Caption, ImVec2(min.x + Space::XL, min.y + 54), Look::TextDim,
                    Fit(g_bold, Type::Caption, "Version " + version, MenuWidth - 2 * Space::XL));
            bool anyNextStart = false;
            for (size_t i = 0; i < rows.size(); i++)
            {
                const MenuRow& row = rows[i];
                const float y = layout.firstRow + float(i) * layout.pitch, cy = y + layout.rowHeight / 2;
                ImVec2 a(min.x + Space::M, y), b(max.x - Space::M, y + layout.rowHeight);
                const bool focused = model.menuRow == int(i), last = i + 1 == rows.size();
                const int index = int(i);
                if (focused)
                {
                    RoundRect(a, b, Look::CardSelected, Radius::Control);
                    DenseFocusRing(a, b, Radius::Control);
                }
                AddHit(a, b, [&model, index] { model.FocusMenuRow(index, 0); });
                g_hits.back().target = "menu=" + std::to_string(index);
                auto focusRow = [&model, index] {
                    model.menuRow = index;
                    model.Touch();
                };
                g_hits.back().focus = focusRow;
                anyNextStart = anyNextStart || row.nextStart;
                const float right = b.x - Space::L;
                const float valueWidth = row.kind == MenuRow::Kind::Action   ? 20
                                         : row.kind == MenuRow::Kind::Toggle ? 44 + Space::S + 44
                                                                             : 172;
                std::string label = row.label + (row.nextStart ? " *" : "");
                TextMiddle(g_black, Type::Body, a.x + Space::L, cy, last ? Look::TextSoft : Look::Text,
                    Fit(g_black, Type::Body, label, right - valueWidth - Space::M - (a.x + Space::L)));
                switch (row.kind)
                {
                case MenuRow::Kind::Action:
                    if (!last)
                        Chevron(ImVec2(right - 4, cy), 12, focused ? Look::Text : Look::TextDim, 0);
                    break;
                case MenuRow::Kind::Toggle:
                {
                    bool on = row.value == L("On", "Oui");
                    Switch(ImVec2(right - 22, cy), on);
                    float w = Measure(g_bold, Type::Small, row.value).x;
                    TextMiddle(
                        g_bold, Type::Small, right - 44 - Space::S - w, cy, on ? Look::Text : Look::TextDim, row.value);
                    break;
                }
                case MenuRow::Kind::Choice:
                case MenuRow::Kind::Slider:
                {
                    const float x0 = right - valueWidth;
                    uint32_t mark = focused ? Look::Text : Look::TextDim;
                    Chevron(ImVec2(x0 + 6, cy), 12, mark, 2);
                    Chevron(ImVec2(right - 6, cy), 12, mark, 0);
                    float mid = (x0 + right) / 2;
                    AddHit(ImVec2(x0 - 6, a.y), ImVec2(mid, b.y), [&model, index] { model.FocusMenuRow(index, -1); });
                    g_hits.back().target = "menu=" + std::to_string(index);
                    g_hits.back().focus = focusRow;
                    AddHit(ImVec2(mid, a.y), ImVec2(right + 6, b.y), [&model, index] { model.FocusMenuRow(index, 1); });
                    g_hits.back().target = "menu=" + std::to_string(index);
                    g_hits.back().focus = focusRow;
                    if (row.kind == MenuRow::Kind::Slider)
                    {
                        ImVec2 track(x0 + 22, cy - 3), trackEnd(x0 + 110, cy + 3);
                        RoundRect(track, trackEnd, Look::Well, 3);
                        RoundRect(track,
                            ImVec2(track.x + (trackEnd.x - track.x) * float(row.percent) / 100.0f, trackEnd.y),
                            Look::Gold, 3);
                        TextCentered(
                            g_black, Type::Small, ImVec2((trackEnd.x + right - 12) / 2, cy), Look::Text, row.value);
                    }
                    else
                        TextCentered(g_black, Type::Body, ImVec2(mid, cy), focused ? Look::GoldText : Look::TextSoft,
                            Fit(g_black, Type::Body, row.value, valueWidth - 40));
                    break;
                }
                }
            }
            const float footer = max.y - MenuFooter / 2;
            float hintsLeft =
                Hints(max.x - Space::XL, footer, { { 'A', L("Change", "Changer") }, { 'B', L("Close", "Fermer") } });
            if (anyNextStart)
                TextMiddle(g_bold, Type::Caption, min.x + Space::XL, footer, Look::TextDim,
                    Fit(g_bold, Type::Caption, L("* applies at the next start", "* au prochain démarrage"),
                        hintsLeft - Space::L - (min.x + Space::XL)));
        }

        // The FPS counter: a small readout in the top right corner of the safe area, over everything.
        void DrawFps(const Model& model)
        {
            if (model.fps.empty())
                return;
            float width = Measure(g_black, look::Type::Small, model.fps).x + 2 * look::Space::M;
            ImVec2 min(look::SafeRight - width, look::SafeTop), max(look::SafeRight, look::SafeTop + 26);
            RoundRect(min, max, Look::Surface, 13, 0.8f);
            TextCentered(
                g_black, look::Type::Small, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), Look::Text, model.fps);
        }

        // --- Over the game: the wheel, its list, the toast -------------------------------------------------------------

        constexpr float WheelRadius = 220;
        const ImVec2 WheelCenter(640, 340);

        void DrawWheel(Model& model)
        {
            WheelState& w = model.wheel;
            Disc(WheelCenter, int(WheelRadius * 2 + 150), Look::Night, 0.72f);
            const SeatState& s = model.seats[w.seat];
            const Figure* current = model.SeatFigure(w.seat);
            for (int i = 0; i < Model::WheelPlaces; i++)
            {
                float angle = -1.5707963f + i * 6.2831853f / Model::WheelPlaces;
                ImVec2 at(WheelCenter.x + std::cos(angle) * WheelRadius, WheelCenter.y + std::sin(angle) * WheelRadius);
                bool aimed = w.aimed == i;
                int index = w.figures[i];
                if (aimed)
                    Disc(at, 118, Look::Gold, 0.35f);
                if (index >= 0)
                {
                    const Figure& f = model.figures[index];
                    CardPortrait(model, f, at, 92);
                    std::string name = model.FigureName(f);
                    TextCentered(g_black, 15, ImVec2(at.x, at.y + 60), aimed ? Look::GoldText : Look::Text,
                        Fit(g_black, 15, name, 170));
                    // The card's words: the level for this game, unless the place says more.
                    std::string tag = model.LevelLine(f);
                    int on = model.SeatOf(f.path);
                    if (on == w.seat)
                        tag = L("playing now", "en jeu");
                    else if (on >= 0)
                        tag = model.SeatTag(on) + L(": swap players", " : échanger");
                    TextCentered(g_bold, 12, ImVec2(at.x, at.y + 78), Look::TextDimmer, tag);
                    if (w.places[i].pinned)
                        Star(ImVec2(at.x + 38, at.y - 38), 10, Look::Gold);
                }
                else
                {
                    // An empty place fills from this game's recent figures; it needs no words.
                    DashedCircle(at, 30, Look::Control, 2);
                }
                if (aimed)
                    Ring(at, 104, Look::Gold, 5);
                if (aimed && w.pinHoldStart != 0)
                {
                    float t = std::min(float(g_frameTime - w.pinHoldStart) / 6e8f, 1.0f);
                    g_draw->PathArcTo(at, 58, -1.5707963f, -1.5707963f + t * 6.2831853f, 32);
                    g_draw->PathStroke(Rgb(Look::Bar), ImDrawFlags_None, 5.0f);
                }
            }
            // Centre: the seat's figure; releasing here cancels.
            if (current != nullptr)
                CardPortrait(model, *current, WheelCenter, 110);
            else
                EmptySeatPicture(WheelCenter, 110);
            if (w.aimed < 0)
                Ring(WheelCenter, 122, Look::Text, 3);
            TextCentered(g_display, 18, ImVec2(WheelCenter.x, WheelCenter.y + 74), Look::Gold, model.SeatLabel(w.seat));
            TextCentered(g_bold, 12, ImVec2(WheelCenter.x, WheelCenter.y + 96), Look::TextDim,
                L("release here: cancel", "relâcher ici : annuler"));
            (void)s;
            // Hints under the wheel.
            float y = 676;
            std::vector<HintItem> hints = { { 'A', L("Swap", "Changer") }, { 'Y', L("All", "Tout") },
                { 'L', L("Other seat", "Autre place") }, { 'X', L("hold: pin here", "maintenir : épingler ici") },
                { 'B', L("Cancel", "Annuler") } };
            float width = 0;
            for (const HintItem& hint : hints)
                width += HintButton(hint.button, 0, 0, false) + 6 + Measure(g_bold, 14, hint.text).x + 20;
            RoundRect(
                ImVec2(640 - width / 2 - 16, y - 22), ImVec2(640 + width / 2 + 4, y + 22), Look::Night, 22, 0.85f);
            Hints(640 + width / 2, y, hints);
            if (KeyboardStyle())
                TextCentered(g_bold, 13, ImVec2(640, 30), Look::TextSoft,
                    std::string(L("Arrows or 1-8: aim · release ", "Flèches ou 1-8 : viser · relâcher ")) +
                        ToyboxWheelKeyName() + L(": swap · Esc: cancel", " : changer · Échap : annuler"));
            for (int i = 0; i < Model::WheelPlaces; i++)
            {
                float angle = -1.5707963f + i * 6.2831853f / Model::WheelPlaces;
                ImVec2 at(WheelCenter.x + std::cos(angle) * WheelRadius, WheelCenter.y + std::sin(angle) * WheelRadius);
                AddHit(ImVec2(at.x - 55, at.y - 55), ImVec2(at.x + 55, at.y + 55), [&model, i] {
                    model.AimWheel(i);
                    model.ConfirmWheel();
                });
            }
        }

        void DrawWheelList(Model& model)
        {
            ImVec2 min(110, 40), max(1170, 680);
            Panel(min, max, 0.95f);
            std::string title = model.SeatLabel(model.wheel.seat) + L(": choose", " : choisissez");
            float w = Title(title, ImVec2(min.x + 28, min.y + 18), 28);
            const Figure* now = model.SeatFigure(model.wheel.seat);
            if (now != nullptr)
                TextMiddle(g_bold, 16, min.x + 28 + w + 20, min.y + 36, Look::TextDim,
                    std::string(L("Now: ", "Maintenant : ")) + model.FigureName(*now));
            SectionStrip(model, model.list, ImVec2(min.x + 28, min.y + 70), max.x - min.x - 56);
            DrawList(model, ImVec2(min.x + 28, min.y + 110), ImVec2(max.x - 28, max.y - 56), true, true);
            Hints(max.x - 28, max.y - 28,
                { { 'A', L("Swap", "Changer") }, { 'L', L("Section", "Section") },
                    { 'B', L("Back to the game", "Retour au jeu") } });
        }

        imgui_software::Texture* MakeBackground()
        {
            auto* texture = new imgui_software::Texture{ CanvasWidth, CanvasHeight,
                std::vector<uint32_t>(size_t(CanvasWidth) * CanvasHeight) };
            struct Stop
            {
                float at;
                float rgb[3];
            };
            static const Stop Stops[] = { { 0, { 0x24, 0x28, 0x58 } }, { 0.55f, { 0x11, 0x14, 0x30 } },
                { 1, { 0x08, 0x0A, 0x1A } } };
            for (int y = 0; y < CanvasHeight; y++)
            {
                auto* row = reinterpret_cast<uint8_t*>(texture->pixels.data() + size_t(y) * CanvasWidth);
                for (int x = 0; x < CanvasWidth; x++)
                {
                    float dx = (x + 0.5f - CanvasWidth * 0.5f) / (CanvasWidth * 0.6f),
                          dy = (y + 0.5f - CanvasHeight * 1.08f) / (CanvasHeight * 0.45f);
                    float t = std::min(std::sqrt(dx * dx + dy * dy), 1.0f);
                    const Stop& a = t < Stops[1].at ? Stops[0] : Stops[1];
                    const Stop& b = t < Stops[1].at ? Stops[1] : Stops[2];
                    float f = (t - a.at) / (b.at - a.at);
                    for (int c = 0; c < 3; c++)
                        row[x * 4 + c] = uint8_t(a.rgb[c] + (b.rgb[c] - a.rgb[c]) * f + 0.5f);
                    row[x * 4 + 3] = 255;
                }
            }
            return texture;
        }
    }

    bool DrawInit()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(CanvasWidth, CanvasHeight);
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        auto add = [&](const unsigned char* data, size_t size) {
            return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), 16, &config);
        };
        g_bold = add(g_embeddedOutfitBold, g_embeddedOutfitBoldSize);
        g_black = add(g_embeddedOutfitBlack, g_embeddedOutfitBlackSize);
        g_display = g_black; // one family on every screen (look.h's type scale)
        imgui_software::Init();
        g_background = MakeBackground();
        return g_bold != nullptr && g_black != nullptr && g_display != nullptr;
    }

    void Draw(Model& model, std::vector<uint32_t>& pixels)
    {
        ImGuiIO& io = ImGui::GetIO();
        uint64_t now = SDL_GetTicksNS();
        io.DeltaTime =
            g_frameTime != 0 && now > g_frameTime ? std::max(float(now - g_frameTime) / 1e9f, 1e-4f) : 1.0f / 30;
        g_frameTime = now;
        ImGui::NewFrame();
        g_draw = ImGui::GetBackgroundDrawList();
        g_hits.clear();
        bool overGame = !model.open || model.screen == Screen::Wheel || model.screen == Screen::WheelList ||
                        model.screen == Screen::Menu;
        if (overGame)
            gamemenu::DrawDebugScreen(g_bold);
        if (!model.open)
            DrawToast(model, 640, 640, look::SafeBottom, false, true);
        else
        {
            switch (model.screen)
            {
            case Screen::Home: DrawHome(model); break;
            case Screen::Figure: DrawFigure(model); break;
            case Screen::Pin: DrawPin(model); break;
            case Screen::Half: DrawHalf(model); break;
            case Screen::Backups: DrawBackups(model); break;
            case Screen::Stats: DrawStats(model); break;
            case Screen::Creation: DrawCreation(model); break;
            case Screen::Upgrades: DrawUpgrades(model); break;
            case Screen::Add: DrawAdd(model); break;
            case Screen::Name: DrawName(model); break;
            case Screen::Manage: DrawManage(model); break;
            case Screen::Wheel: DrawWheel(model); break;
            case Screen::WheelList: DrawWheelList(model); break;
            case Screen::Menu: DrawMenu(model); break;
            }
            DrawDialog(model);
            if (model.screen == Screen::Menu && model.commandsOpen)
                DrawToast(model, 640, 760, look::SafeBottom - look::MenuFooter, false, false);
            else if (model.screen == Screen::Menu)
            {
                // Beside the menu's sheet.
                float left = look::SafeLeft + look::MenuWidth + look::Space::XL;
                DrawToast(model, (left + look::SafeRight) / 2, look::SafeRight - left, look::SafeBottom, false, false);
            }
            else if (overGame)
                DrawToast(model, 640, 760, look::SafeTop, true, false);
            else
                DrawToast(model, 640, 760, 672, false, false);
        }
        // What the pointer is on: a soft outline, independent of keyboard and pad focus.
        g_hovered = model.open && model.screen != Screen::Wheel ? HoveredHit() : -1;
        if (g_hovered >= 0)
        {
            const Hit& hit = g_hits[size_t(g_hovered)];
            RoundFrame(ImVec2(hit.min.x - 2, hit.min.y - 2), ImVec2(hit.max.x + 2, hit.max.y + 2), Look::TextSoft,
                std::min({ 12.0f, (hit.max.x - hit.min.x) / 2, (hit.max.y - hit.min.y) / 2 }), 2);
        }
        if (!model.open) // it would sit on the corner buttons of the full-screen pages
            DrawFps(model);
        ImGui::Render();
        // Into the caller's buffer: once it has its size, a frame (a toast sliding in) allocates nothing.
        imgui_software::Texture canvas{ CanvasWidth, CanvasHeight };
        canvas.pixels.swap(pixels);
        if (overGame)
            canvas.pixels.assign(size_t(CanvasWidth) * CanvasHeight, 0);
        else
            canvas.pixels.assign(g_background->pixels.begin(), g_background->pixels.end());
        imgui_software::Render(ImGui::GetDrawData(), canvas);
        canvas.pixels.swap(pixels);
    }

    void Click(Model& model, float x, float y, int button, bool twice)
    {
        if (int i = HitAt(std::span<const Hit>(g_hits), x, y); i >= 0)
        {
            const Hit& hit = g_hits[size_t(i)];
            std::function<void()> action = button == 3 ? hit.right : twice && hit.twice ? hit.twice : hit.left;
            if (action)
                action();
            model.Touch();
            return;
        }
        if (button == 3)
            model.Run(Command::Back);
    }

    bool Pointer(float x, float y, bool inside)
    {
        g_pointer = ImVec2(x, y);
        g_pointerIn = inside;
        // The pointer moving onto something focusable moves the focus there; a still pointer never takes it back.
        int hovered = HoveredHit();
        if (hovered != g_hovered && hovered >= 0 && g_hits[size_t(hovered)].focus)
            g_hits[size_t(hovered)].focus();
        return hovered != g_hovered;
    }

    std::string HoverTarget()
    {
        int i = HoveredHit();
        return i >= 0 && !g_hits[size_t(i)].target.empty() ? g_hits[size_t(i)].target : "none";
    }

    int WheelPlaceAt(float x, float y)
    {
        float dx = x - WheelCenter.x, dy = y - WheelCenter.y, distance = std::sqrt(dx * dx + dy * dy);
        if (distance < 70)
            return -1;
        if (distance > WheelRadius + 90)
            return -2;
        float angle = std::atan2(dy, dx) + 1.5707963f;
        int place = int(std::lround(angle / (6.2831853f / Model::WheelPlaces)));
        return (place % Model::WheelPlaces + Model::WheelPlaces) % Model::WheelPlaces;
    }
}
