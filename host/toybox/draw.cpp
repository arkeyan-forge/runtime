#include "host/toybox/draw.h"
#include "host/gamemenu.h"
#include "host/toybox/hit.h"
#include "host/collection/icons.h"
#include "host/embedded_fonts.h"
#include "host/glyphs.h"
#include "host/imgui_software.h"
#include "host/input.h"
#include "host/launcher.h"
#include "host/portal.h"
#include "host/portal_backend.h"
#include "host/profiles.h"
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
#include <ctime>
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

        // The screens' colour names, on the shared palette (look.h, docs/runtime/host/ingame_ui.md). "Gold" is the
        // accent's old name: the running game's colour, made readable (look::ReadableAccent), set by DrawInit.
        namespace Look
        {
            using namespace look::Colour;
            constexpr uint32_t ChipText = TextSoft;
            uint32_t Accent = 0x6BBAFF, AccentText = Accent, Gold = Accent, GoldText = Accent, FocusRing = Accent;
            uint32_t GoldBorder = 0x3A3A44; // an accent line on a chip: the accent toward the chip's grey
            constexpr uint32_t GoldInk = Well, AccentInk = Well;
            constexpr uint32_t Night = Scrim;
            constexpr uint32_t Panel = Surface, PanelBorder = Hairline, CardBorder = Hairline,
                               CardSelected = look::Colour::Focus;
            constexpr uint32_t Chip = Raised, KeyCap = Raised2, Button = Raised2;
            constexpr uint32_t Bar = Success, Waiting = Warning;

            void SetAccent(uint32_t glow)
            {
                Accent = AccentText = Gold = GoldText = FocusRing = look::ReadableAccent(glow);
                GoldBorder = look::Mix(Accent, Raised, 0.55f);
            }
        }

        imgui_software::Texture* g_background;
        std::map<std::string, imgui_software::Texture*> g_baked;
        std::map<uint64_t, collection::Image> g_icons; // (type, variant, kind) -> 128x128 picture
        ImFont* g_bold;    // Outfit SemiBold: running text
        ImFont* g_black;   // Outfit Bold: labels, values, buttons
        ImFont* g_display; // names at display sizes: Outfit Bold (look.h's type scale)
        ImFont* g_title; // page titles and the game's name: Bungee, as the launcher's (and the debug screen's headings)
        ImFont* g_mono; // the debug screen's values: ImGui's own monospace face (ProggyClean)
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

        // Focus, the one style of every screen: a ring 2 px wide in the game's accent on the element's own edge (look.h),
        // as the launcher's. The pointer's hover is a thin grey line just outside it.
        void FocusRing(ImVec2 min, ImVec2 max, float radius, uint32_t rgb = 0)
        {
            RoundFrame(min, max, rgb != 0 ? rgb : Look::FocusRing, radius, 2);
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

        // --- Line icons (drawn on a 24-unit grid, 2 units wide, round: the launcher's line icons) -------------------------

        enum class Icon
        {
            ToyBox,
            Trophy,
            Sliders,
            Gamepad,
            Activity,
            Play,
            Volume,
            Flag,
            Quit,
            Lock,
            Plus
        };

        void DrawIcon(Icon icon, ImVec2 c, float size, uint32_t rgb, float stroke = 2)
        {
            const float k = size / 24;
            const ImU32 ink = Rgb(rgb);
            const float w = std::max(1.5f, stroke * k);
            auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12) * k, c.y + (y - 12) * k); };
            auto line = [&](float x0, float y0, float x1, float y1) { g_draw->AddLine(P(x0, y0), P(x1, y1), ink, w); };
            auto poly = [&](std::initializer_list<ImVec2> points, bool closed) {
                std::vector<ImVec2> at;
                for (ImVec2 point : points)
                    at.push_back(P(point.x, point.y));
                g_draw->AddPolyline(at.data(), int(at.size()), ink, closed ? ImDrawFlags_Closed : ImDrawFlags_None, w);
            };
            auto arc = [&](float x, float y, float r, float from, float to) {
                g_draw->PathArcTo(P(x, y), r * k, from, to, 16);
                g_draw->PathStroke(ink, ImDrawFlags_None, w);
            };
            constexpr float Pi = 3.14159265f;
            switch (icon)
            {
            case Icon::ToyBox: // a box seen from a corner
                poly({ { 12, 2.5f }, { 20.5f, 7 }, { 20.5f, 17 }, { 12, 21.5f }, { 3.5f, 17 }, { 3.5f, 7 } }, true);
                poly({ { 3.5f, 7 }, { 12, 11.5f }, { 20.5f, 7 } }, false);
                line(12, 11.5f, 12, 21.5f);
                break;
            case Icon::Trophy:
                poly({ { 6, 3 }, { 18, 3 }, { 18, 9 } }, false);
                line(6, 3, 6, 9);
                arc(12, 9, 6, 0, Pi);
                arc(5, 6.5f, 2.5f, Pi / 2, Pi * 1.5f);
                arc(19, 6.5f, 2.5f, -Pi / 2, Pi / 2);
                line(12, 15, 12, 19);
                line(7, 21, 17, 21);
                line(9, 19, 15, 19);
                break;
            case Icon::Sliders:
                line(4, 4, 10, 4);
                line(14, 4, 21, 4);
                line(14, 2, 14, 6);
                line(3, 12, 8, 12);
                line(12, 12, 21, 12);
                line(8, 10, 8, 14);
                line(3, 20, 12, 20);
                line(16, 20, 21, 20);
                line(16, 18, 16, 22);
                break;
            case Icon::Gamepad:
                g_draw->AddRect(P(2, 6), P(22, 18), ink, 5 * k, 0, w);
                line(6, 12, 10, 12);
                line(8, 10, 8, 14);
                g_draw->AddCircleFilled(P(15.5f, 12.5f), 1.3f * k, ink);
                g_draw->AddCircleFilled(P(18, 10), 1.3f * k, ink);
                break;
            case Icon::Activity:
                poly({ { 2, 12 }, { 6, 12 }, { 9, 3 }, { 15, 21 }, { 18, 12 }, { 22, 12 } }, false);
                break;
            case Icon::Play: poly({ { 7, 4 }, { 19, 12 }, { 7, 20 } }, true); break;
            case Icon::Volume:
                poly({ { 11, 5 }, { 6, 9 }, { 2.5f, 9 }, { 2.5f, 15 }, { 6, 15 }, { 11, 19 } }, true);
                arc(12, 12, 4, -Pi / 4, Pi / 4);
                arc(12, 12, 8, -Pi / 4, Pi / 4);
                break;
            case Icon::Flag:
                line(4, 3, 4, 22);
                poly({ { 4, 4 }, { 8, 2.8f }, { 12, 4 }, { 16, 5.2f }, { 20, 4 }, { 20, 14 }, { 16, 15.2f }, { 12, 14 },
                         { 8, 12.8f }, { 4, 14 } },
                    false);
                break;
            case Icon::Quit:
                poly({ { 9, 3 }, { 4, 3 }, { 4, 21 }, { 9, 21 } }, false);
                poly({ { 16, 7 }, { 21, 12 }, { 16, 17 } }, false);
                line(21, 12, 9, 12);
                break;
            case Icon::Lock:
                g_draw->AddRect(P(4, 11), P(20, 21), ink, 2 * k, 0, w);
                arc(12, 7.5f, 4.5f, Pi, 2 * Pi);
                line(7.5f, 7.5f, 7.5f, 11);
                line(16.5f, 7.5f, 16.5f, 11);
                break;
            case Icon::Plus:
                line(5, 12, 19, 12);
                line(12, 5, 12, 19);
                break;
            }
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
                found = g_icons.emplace(key, collection::Portrait(ARKEYAN_GAME_ID, toy, kind, 128, variant)).first;
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
            std::string path = collection::BestPicturePath(ARKEYAN_GAME_ID, *toy, variant);
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
            DashedCircle(center, diameter / 2.0f - 2, look::Colour::KeyOutline, 2);
            DrawIcon(Icon::Plus, center, std::min(22.0f, diameter * 0.4f), Look::TextSoft);
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

        // The main action: the accent's fill, dark ink; focused, a white ring (the accent's would not show on it).
        void GoldButton(ImVec2 min, ImVec2 max, const std::string& label, float size, bool focused)
        {
            RoundRect(min, max, Look::Gold, look::Radius::Row);
            TextCentered(g_black, size, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), Look::GoldInk,
                Fit(g_black, size, label, max.x - min.x - 16));
            if (focused)
                FocusRing(min, max, look::Radius::Row, look::Colour::Text);
        }

        void PlainButton(ImVec2 min, ImVec2 max, const std::string& label, float size, bool focused,
            uint32_t text = Look::Text, uint32_t fill = Look::Button, uint32_t border = Look::Hairline)
        {
            Box(min, max, focused ? Look::CardSelected : fill, border == Look::Hairline ? look::Colour::Border : border,
                look::Radius::Row, 1);
            TextCentered(g_black, size, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), text,
                Fit(g_black, size, label, max.x - min.x - 16));
            if (focused)
                FocusRing(min, max, look::Radius::Row);
        }

        // A page's title: Bungee, white (the launcher's titles).
        float Title(const std::string& text, ImVec2 at, float size)
        {
            Text(g_title, size, at, Look::Text, text);
            return Measure(g_title, size, text).x;
        }

        void Panel(ImVec2 min, ImVec2 max, float alpha = 1)
        {
            Box(min, max, Look::Panel, Look::PanelBorder, look::Radius::Panel, 1, alpha);
        }

        // --- Button hints ---------------------------------------------------------------------------------------------

        bool KeyboardStyle() { return glyphs::Current() == glyphs::Style::Keyboard; }

        // A key as the launcher draws it: an outlined cap, its lower edge thicker.
        float KeyCap(ImVec2 at, float height, float size, const std::string& label)
        {
            float width = std::max(Measure(g_black, size, label).x + 16, height);
            ImVec2 max(at.x + width, at.y + height);
            RoundFrame(at, max, look::Colour::KeyOutline, 8, 1);
            Fill(ImVec2(at.x + 6, max.y - 2), ImVec2(max.x - 6, max.y), look::Colour::KeyOutline);
            TextCentered(g_black, size, ImVec2(at.x + width / 2, at.y + height / 2 - 1), Look::Text, label);
            return width;
        }

        struct HintItem
        {
            char button; // A B X Y, L (LB/RB), V (View), S (Start), F (Start / F4: Filters)
            std::string text;
        };

        bool PlayStationStyle()
        {
            return glyphs::Current() == glyphs::Style::PlayStation || glyphs::Current() == glyphs::Style::PlayStation3;
        }

        // A face button's mark inside its outlined circle: the letter, or PlayStation's shapes (cross, circle, square,
        // triangle for A, B, X, Y), drawn (the fonts have none).
        void FaceMark(char button, ImVec2 c)
        {
            if (!PlayStationStyle())
            {
                TextCentered(g_black, 12, ImVec2(c.x, c.y - 1), Look::Text, std::string(1, button));
                return;
            }
            const ImU32 ink = Rgb(Look::Text);
            switch (button)
            {
            case 'A':
                g_draw->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 1.8f);
                g_draw->AddLine(ImVec2(c.x - 5, c.y + 5), ImVec2(c.x + 5, c.y - 5), ink, 1.8f);
                break;
            case 'B': g_draw->AddCircle(c, 5.5f, ink, 16, 1.8f); break;
            case 'X': g_draw->AddRect(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), ink, 0, 0, 1.8f); break;
            default:
                g_draw->AddTriangle(
                    ImVec2(c.x, c.y - 6), ImVec2(c.x + 6, c.y + 4.5f), ImVec2(c.x - 6, c.y + 4.5f), ink, 1.8f);
                break;
            }
        }

        // The words on the pad's other buttons, by the glyph style in use.
        std::string PadWord(char button, int which)
        {
            const glyphs::Style style = glyphs::Current();
            const bool ps = PlayStationStyle(), nintendo = style == glyphs::Style::Nintendo;
            switch (button)
            {
            case 'L': return which == 0 ? (ps ? "L1" : nintendo ? "L" : "LB") : (ps ? "R1" : nintendo ? "R" : "RB");
            case 'V':
                return style == glyphs::Style::Xbox360        ? "Back"
                       : style == glyphs::Style::PlayStation3 ? "Select"
                       : ps                                   ? "Share"
                       : nintendo                             ? "-"
                                                              : "View";
            default:
                return style == glyphs::Style::Xbox360 || style == glyphs::Style::PlayStation3 ? "Start"
                       : ps                                                                    ? "Options"
                       : nintendo                                                              ? "+"
                                                                                               : "Menu";
            }
        }

        // Width of one hint's button part (the glyph of the device in use: an outlined circle for a face button, caps
        // for the rest and for keys).
        float HintButton(char button, float x, float centerY, bool draw)
        {
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
                float width = std::max(Measure(g_black, 12, key).x + 16, 26.0f);
                if (draw)
                    KeyCap(ImVec2(x, centerY - 13), 26, 12, key);
                return width;
            }
            if (button == 'L')
            {
                float first = std::max(Measure(g_black, 12, PadWord('L', 0)).x + 16, 26.0f);
                float second = std::max(Measure(g_black, 12, PadWord('L', 1)).x + 16, 26.0f);
                if (draw)
                {
                    KeyCap(ImVec2(x, centerY - 13), 26, 12, PadWord('L', 0));
                    KeyCap(ImVec2(x + first + 4, centerY - 13), 26, 12, PadWord('L', 1));
                }
                return first + 4 + second;
            }
            if (button == 'V' || button == 'S' || button == 'F')
            {
                std::string word = PadWord(button, 0);
                float width = std::max(Measure(g_black, 12, word).x + 16, 26.0f);
                if (draw)
                    KeyCap(ImVec2(x, centerY - 13), 26, 12, word);
                return width;
            }
            if (draw)
            {
                Ring(ImVec2(x + 13, centerY), 26, look::Colour::KeyOutline, 1.5f);
                FaceMark(button, ImVec2(x + 13, centerY));
            }
            return 26;
        }

        float HintsWidth(const std::vector<HintItem>& hints)
        {
            float width = 0;
            for (const HintItem& hint : hints)
                width += HintButton(hint.button, 0, 0, false) + 8 + Measure(g_bold, 14, hint.text).x + 22;
            return hints.empty() ? 0 : width - 22;
        }

        // From `left`, on `centerY`; returns the right edge.
        float HintsFrom(float left, float centerY, const std::vector<HintItem>& hints)
        {
            float x = left;
            for (const HintItem& hint : hints)
            {
                x += HintButton(hint.button, x, centerY, true) + 8;
                TextMiddle(g_bold, 14, x, centerY, Look::TextSoft, hint.text);
                x += Measure(g_bold, 14, hint.text).x + 22;
            }
            return x - 22;
        }

        // Right-aligned on `right`; returns the left edge.
        float Hints(float right, float centerY, const std::vector<HintItem>& hints)
        {
            float left = right - HintsWidth(hints);
            HintsFrom(left, centerY, hints);
            return left;
        }

        bool g_under; // drawing the screen under an overlay: no footer

        // The hints of a Toy Box screen without the rail (the Figure page, Add a figure...): the bottom right.
        void Footer(const std::vector<HintItem>& hints)
        {
            if (!g_under)
                Hints(1248, 692, hints);
        }

        // The hints of a page beside the rail: a bar along its bottom, under a line, from its left (the launcher's).
        void PageFooter(const std::vector<HintItem>& hints)
        {
            Fill(ImVec2(look::PageLeft, look::FooterTop), ImVec2(look::PageRight, look::FooterTop + 1), Look::Hairline);
            HintsFrom(look::PageLeft, look::FooterTop + look::FooterHeight / 2, hints);
        }

        // --- Shared parts -----------------------------------------------------------------------------------------------

        void Header(const std::string& title, const std::string& subtitle)
        {
            float width = Title(title, ImVec2(32, 24), 28);
            if (!subtitle.empty())
                TextMiddle(g_bold, 16, 32 + width + 16, 42, Look::TextDim, subtitle);
        }

        // --- The player's profile: SKY_PLAYER_NAME and its picture (host/profiles.h) ------------------------------------

        imgui_software::Texture* g_avatar; // the picture cut round, 64 px; null: the initials on the accent
        bool g_avatarRead;

        // The profile's picture, or its initials, in a circle of `diameter`.
        void Avatar(ImVec2 center, int diameter)
        {
            const profiles::Profile& player = profiles::Current()[0];
            if (!g_avatarRead)
            {
                g_avatarRead = true;
                size_t size = 0;
                void* data = player.picture.empty() ? nullptr : SDL_LoadFile(player.picture.c_str(), &size);
                profiles::Picture picture;
                if (data != nullptr && profiles::DecodePicture(static_cast<const uint8_t*>(data), size, picture))
                {
                    // The largest centred square, sampled down to 64 px, outside the circle transparent.
                    constexpr int Side = 64;
                    int square = std::min(picture.width, picture.height);
                    int x0 = (picture.width - square) / 2, y0 = (picture.height - square) / 2;
                    g_avatar = new imgui_software::Texture{ Side, Side, std::vector<uint32_t>(Side * Side) };
                    for (int y = 0; y < Side; y++)
                        for (int x = 0; x < Side; x++)
                        {
                            const uint8_t* px = &picture.rgba[(size_t(y0 + y * square / Side) * size_t(picture.width) +
                                                                  size_t(x0 + x * square / Side)) *
                                                              4];
                            float dx = x + 0.5f - Side / 2.0f, dy = y + 0.5f - Side / 2.0f;
                            float cover = Coverage(Side / 2.0f, std::sqrt(dx * dx + dy * dy));
                            g_avatar->pixels[size_t(y) * Side + size_t(x)] = uint32_t(px[0]) | uint32_t(px[1]) << 8 |
                                                                             uint32_t(px[2]) << 16 |
                                                                             uint32_t(px[3] * cover + 0.5f) << 24;
                        }
                }
                SDL_free(data);
            }
            if (g_avatar != nullptr)
            {
                ImVec2 min(std::round(center.x - diameter / 2.0f), std::round(center.y - diameter / 2.0f));
                g_draw->AddImage(imgui_software::Ref(g_avatar), min, ImVec2(min.x + diameter, min.y + diameter));
                return;
            }
            Disc(center, diameter, Look::Accent);
            TextCentered(g_black, diameter * 0.4f, center, Look::AccentInk, Initials(player.name));
        }

        std::string PlayerName() { return profiles::Current()[0].name; }

        // The game's name without "Skylanders", in capitals, for Bungee ("IMAGINATORS", "SPYRO'S ADVENTURE").
        std::string Upper(const std::string& text)
        {
            std::string out = text;
            for (size_t i = 0; i < out.size(); i++)
            {
                if (out[i] >= 'a' && out[i] <= 'z')
                    out[i] = char(out[i] - 'a' + 'A');
                else if (uint8_t(out[i]) == 0xC3 && i + 1 < out.size() && uint8_t(out[i + 1]) >= 0xA0 &&
                         uint8_t(out[i + 1]) <= 0xBE && uint8_t(out[i + 1]) != 0xB7)
                {
                    i++;
                    out[i] = char(uint8_t(out[i]) - 0x20); // à-þ to À-Þ (not ÷)
                }
            }
            return out;
        }

        std::string GameTitle(const Model& model)
        {
            std::string name = model.GameName();
            if (name.rfind("Skylanders ", 0) == 0)
                name = name.substr(11);
            return Upper(name);
        }

        // --- The rail: the full screens' pages, the game's name above, the profile below ---------------------------------

        void DrawRail(Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            Fill(ImVec2(0, 0), ImVec2(RailWidth, CanvasHeight), Colour::Surface);
            Fill(ImVec2(RailWidth - 1, 0), ImVec2(RailWidth, CanvasHeight), Colour::Hairline);
            const float x = RailPadX, right = RailWidth - RailPadX;
            std::string title = GameTitle(model);
            float size = 17;
            while (size > 12 && Measure(g_title, size, title).x > right - x - 24)
                size -= 1;
            Text(g_title, size, ImVec2(x + 12, RailPadTop), Look::Accent, title);
            static const Icon Icons[] = { Icon::ToyBox, Icon::Trophy, Icon::Sliders, Icon::Gamepad, Icon::Activity };
            const std::string labels[] = { "Toy Box", L("Achievements", "Succès"), L("Settings", "Réglages"),
                L("Controls", "Commandes"), L("Performance", "Performances") };
            const Page current = model.CurrentPage();
            for (int i = 0; i < int(Page::Count); i++)
            {
                const Page page = Page(i);
                ImVec2 a(x, RailRowTop(i)), b(right, RailRowTop(i) + RailRowHeight);
                bool selected = page == current;
                if (selected)
                {
                    RoundRect(a, b, Colour::Raised2, Radius::Control);
                    RoundRect(ImVec2(a.x, a.y + 10), ImVec2(a.x + 4, b.y - 10), Look::Accent, 2);
                }
                const float cy = (a.y + b.y) / 2;
                uint32_t ink = selected ? Colour::Text : Colour::TextSoft;
                DrawIcon(Icons[i], ImVec2(a.x + 12 + 9, cy), 18, ink);
                TextMiddle(selected ? g_black : g_bold, 15, a.x + 12 + 18 + 12, cy, ink,
                    Fit(g_bold, 15, labels[i], b.x - a.x - 54));
                if (page == Page::Performance && gamemenu::Current().debugScreen)
                    Disc(ImVec2(b.x - 14, cy), 8, Look::Accent);
                AddHit(a, b, [&model, page] { model.OpenPage(page); });
                g_hits.back().target = "rail=" + std::to_string(i);
            }
            // The profile at the bottom.
            const float cy = CanvasHeight - 28 - 18;
            Avatar(ImVec2(x + 12 + 14, cy), 28);
            TextMiddle(g_bold, 14, x + 12 + 28 + 10, cy, Colour::TextSoft,
                Fit(g_bold, 14, PlayerName(), right - (x + 12 + 28 + 10)));
        }

        // The toasts (look.h): small cards in the corners. Over the game an achievement rests at the top right, a figure
        // put on a seat at the bottom left (its picture, its seat and level), any other notice at the bottom right; with
        // the F3 menu's panel open, at the bottom left of the game beside it; on a full screen, above its hints at the
        // right. Over the game they slide in from their edge and fade; on the menus they simply show. A notice's lines are
        // cut once per message, so that a frame of the slide does not allocate.
        struct ToastLayout
        {
            std::string text;
            float maxWidth = 0;
            std::vector<std::string> lines;
            float width = 0;
        };
        ToastLayout g_toastLayout;
        imgui_software::Texture g_toastPicture{ 48, 48, {} };
        uint32_t g_toastPictureSerial = 0;

        enum class ToastPlace
        {
            OverGame,
            BesideMenu,
            OnPage
        };

        void DrawToast(const Model& model, ToastPlace place, bool moving)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            if (model.toast.empty())
                return;
            Motion motion = moving ? ToastMotion(SDL_GetTicksNS(), model.toastSince, model.toastUntil,
                                         gamemenu::Current().reduceMotion)
                                   : Motion{};
            if (motion.alpha <= 0)
                return;
            using Kind = Model::ToastKind;
            const Figure* figure = nullptr;
            if (model.toastKind == Kind::Figure)
                for (const Figure& f : model.figures)
                    if (f.path == model.toastFigure)
                        figure = &f;
            const Kind kind = model.toastKind == Kind::Figure && figure == nullptr ? Kind::Notice : model.toastKind;
            const bool picture = kind == Kind::Achievement && !model.toastPicture.Empty();
            // Its size.
            float width = 0, height = 0;
            float maxWidth = place == ToastPlace::BesideMenu ? MainLeft - 2 * ToastMargin : NoticeToastMaxWidth;
            ToastLayout& layout = g_toastLayout;
            if (kind == Kind::Achievement)
            {
                width = std::min(AchievementToastWidth, maxWidth);
                height = 76;
            }
            else if (kind == Kind::Figure)
            {
                width = std::min(FigureToastWidth, maxWidth);
                height = 72;
            }
            else
            {
                if (layout.text != model.toast || layout.maxWidth != maxWidth)
                {
                    layout.text = model.toast;
                    layout.maxWidth = maxWidth;
                    layout.lines = Wrap(g_black, 14, model.toast, maxWidth - 16 - 10 - 12 - 16, 2);
                    float textWidth = 0;
                    for (const std::string& line : layout.lines)
                        textWidth = std::max(textWidth, Measure(g_black, 14, line).x);
                    layout.width = std::min(16 + 10 + 12 + textWidth + 18, maxWidth);
                }
                width = layout.width;
                height = layout.lines.size() > 1 ? 62 : 44;
            }
            // Its corner.
            bool top = false, left = false;
            float edgeX = 0, edgeY = 0;
            switch (place)
            {
            case ToastPlace::OverGame:
                top = kind == Kind::Achievement;
                left = kind == Kind::Figure;
                edgeX = left ? ToastMargin : CanvasWidth - ToastMargin;
                edgeY = top ? ToastMargin : CanvasHeight - ToastMargin;
                break;
            case ToastPlace::BesideMenu:
                left = true;
                edgeX = ToastMargin;
                edgeY = CanvasHeight - ToastMargin;
                break;
            case ToastPlace::OnPage:
                edgeX = PageRight;
                edgeY = FooterTop - 12;
                break;
            }
            const float x = left ? edgeX : edgeX - width;
            const float y = top ? edgeY - motion.offset : edgeY - height + motion.offset;
            ImVec2 min(std::round(x), std::round(y)), max(std::round(x + width), std::round(y + height));
            // Baked corners are kept per alpha: steps of 1/16 keep them few.
            const float alpha = std::round(motion.alpha * 16) / 16;
            const int ink = int(alpha * 255 + 0.5f);
            const float radius = kind == Kind::Notice ? Radius::Row : Radius::Card;
            RoundRect(min, max, Colour::Border, radius, 0.94f * alpha);
            RoundRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), Colour::Page,
                std::max(radius - 1, 0.0f), 0.96f * alpha);
            if (model.open)
            {
                AddHit(min, max, {});
                g_hits.back().hover = false;
            }
            const float cy = (min.y + max.y) / 2;
            if (kind == Kind::Achievement)
            {
                ImVec2 c(min.x + 16 + 24, cy);
                if (picture)
                {
                    if (g_toastPictureSerial != model.toastSerial)
                    {
                        g_toastPictureSerial = model.toastSerial;
                        collection::Image fitted = collection::Resize(model.toastPicture, 48, 48);
                        g_toastPicture.pixels = std::move(fitted.rgba);
                    }
                    g_draw->AddImage(imgui_software::Ref(&g_toastPicture), ImVec2(c.x - 24, c.y - 24),
                        ImVec2(c.x + 24, c.y + 24), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, ink));
                }
                else
                {
                    Disc(c, 48, Look::Accent, alpha);
                    DrawIcon(Icon::Trophy, c, 24, Look::AccentInk, 2.2f);
                }
                std::string points = std::to_string(model.toastPoints) + " G";
                float pointsWidth = Measure(g_black, 15, points).x;
                float tx = c.x + 24 + 14, tw = max.x - 16 - pointsWidth - 12 - tx;
                Text(g_black, Type::Caption, ImVec2(tx, cy - 20), Look::Accent, model.toastCaption, ink);
                Text(g_black, Type::Body, ImVec2(tx, cy - 2), Colour::Text, Fit(g_black, Type::Body, model.toast, tw),
                    ink);
                TextMiddle(g_black, 15, max.x - 16 - pointsWidth, cy, Colour::Text, points);
                return;
            }
            if (kind == Kind::Figure)
            {
                ImVec2 c(min.x + 16 + 22, cy);
                CardPortrait(model, *figure, c, 44);
                float tx = c.x + 22 + 12, tw = max.x - 16 - tx;
                Text(g_black, Type::Body, ImVec2(tx, cy - 20), Colour::Text,
                    Fit(g_black, Type::Body, model.FigureName(*figure), tw), ink);
                std::string line = model.toastSeat >= 0 && model.toastSeat < int(model.seats.size())
                                       ? model.SeatLabel(model.toastSeat)
                                       : std::string();
                std::string level = model.LevelLine(*figure);
                if (!level.empty())
                    line += (line.empty() ? "" : " \xC2\xB7 ") + level;
                Text(g_bold, 13, ImVec2(tx, cy + 2), Colour::TextSoft, Fit(g_bold, 13, line, tw), ink);
                return;
            }
            Disc(ImVec2(min.x + 16 + 5, cy), 10, Look::Accent, alpha);
            float textY = cy - layout.lines.size() * 18 / 2.0f;
            for (const std::string& line : layout.lines)
            {
                Text(g_black, 14, ImVec2(min.x + 16 + 10 + 12, textY), Colour::Text, line, ink);
                textY += 18;
            }
        }

        // The game asks for a figure: a pill at the top centre over the game, while the prompt lasts.
        void DrawPrompt(const Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            if (model.prompt.empty())
                return;
            const std::string tail = L("on the portal", "sur le portail");
            float width = 14 + 34 + 14 + Measure(g_black, 17, model.prompt).x + 10 + Measure(g_bold, 14, tail).x + 22;
            ImVec2 min(std::round(CanvasWidth / 2 - width / 2), ToastMargin),
                max(min.x + std::round(width), min.y + 56);
            RoundRect(min, max, Colour::Border, 28, 0.94f);
            RoundRect(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1), Colour::Page, 27, 0.94f);
            ImVec2 c(min.x + 14 + 17, min.y + 28);
            Ring(c, 34, Look::Accent, 3);
            Disc(c, 12, Look::Accent);
            float x = c.x + 17 + 14;
            TextMiddle(g_black, 17, x, c.y, Colour::Text, model.prompt);
            TextMiddle(g_bold, 14, x + Measure(g_black, 17, model.prompt).x + 10, c.y, Colour::TextSoft, tail);
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
            Box(a, b, cursor ? Look::CardSelected : Look::Surface, Look::CardBorder, look::Radius::Card, 1);
            const ImVec2 picture(cx, a.y + 14 + 34);
            CardPortrait(model, f, picture, 68, grey);
            if (model.OutOfHealth(f))
                OutOfHealthCross(picture, 24);
            TextCentered(g_black, 14, ImVec2(cx, a.y + 102), grey ? Look::TextDimmer : Look::Text,
                Fit(g_black, 14, model.FigureName(f), width - 16));
            // The level line after a dot in the element's colour (the launcher's card).
            std::string line = Fit(g_bold, 12, model.LevelLine(f), width - 34);
            if (!line.empty())
            {
                float lineWidth = Measure(g_bold, 12, line).x + (f.toy != nullptr ? 14 : 0);
                float x = cx - lineWidth / 2;
                if (f.toy != nullptr)
                {
                    Disc(ImVec2(x + 4, a.y + 124), 8, ColourOf(f.toy));
                    x += 14;
                }
                TextMiddle(g_bold, 12, x, a.y + 124, model.CardBusy(f) ? Look::DangerText : Look::TextSoft, line);
            }
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
                    TextMiddle(g_black, 12, min.x, y + 18, section.foreign ? Look::TextDimmer : Look::TextDim,
                        Upper(model.SectionLabel(section)));
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
                    Box(a, b, cursor ? Look::CardSelected : Look::Page, look::Colour::Border, look::Radius::Card, 1);
                    EmptySeatPicture(ImVec2(x + cardWidth / 2, y + 48), 58);
                    TextCentered(
                        g_black, 14, ImVec2(x + cardWidth / 2, y + 102), Look::TextSoft, L("Add a figure", "Ajouter"));
                    if (cursor)
                    {
                        ringMin = a;
                        ringMax = b;
                        ringRadius = look::Radius::Card;
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
            const float cy = min.y + 20, rowHeight = 40;
            // The Filters button.
            std::vector<Model::Tag> tags = model.Tags();
            ImVec2 fa(max.x - 132, cy - rowHeight / 2), fb(max.x, cy + rowHeight / 2);
            bool on = !tags.empty() || model.filtersOpen;
            Box(fa, fb, Look::Button, on ? Look::Gold : look::Colour::Border, look::Radius::Control, on ? 2 : 1);
            std::string label = tags.empty()
                                    ? std::string(L("Filters", "Filtres"))
                                    : std::string(L("Filters", "Filtres")) + " · " + std::to_string(tags.size());
            TextCentered(g_black, 14, ImVec2((fa.x + fb.x) / 2, cy), on ? Look::GoldText : Look::Text,
                Fit(g_black, 14, label, fb.x - fa.x - 20));
            AddHit(fa, fb, [&model] { model.filtersOpen ? model.CloseFilters() : model.OpenFilters(); });
            g_hits.back().focus = [&model] { model.FocusBar(1); };
            // The search field.
            ImVec2 sa(fa.x - 12 - 300, fa.y), sb(fa.x - 12, fb.y);
            model.barSpans = { { sa.x, sb.x }, { fa.x, fb.x } };
            bool barFocus = model.homeRow == Model::Row::Bar && model.screen == Screen::Home && !model.filtersOpen;
            Box(sa, sb, Look::Field, model.search.empty() ? look::Colour::Border : Look::Gold, look::Radius::Control,
                model.search.empty() ? 1 : 2);
            Magnifier(ImVec2(sa.x + 22, cy), model.search.empty() ? Look::TextDim : Look::GoldText);
            float textRight = sb.x - 46;
            if (model.search.empty())
            {
                TextMiddle(g_bold, 14, sa.x + 42, cy, Look::TextDim,
                    Fit(g_bold, 14, L("Name, element, game, level…", "Nom, élément, jeu, niveau…"),
                        textRight - sa.x - 42));
                HintButton('Y', sb.x - HintButton('Y', 0, 0, false) - 8, cy, true);
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
                FocusRing(model.barColumn == 0 ? sa : fa, model.barColumn == 0 ? sb : fb, look::Radius::Control);

            float bottom = min.y + 40 + 8;
            if (!model.FiltersActive())
                return bottom;
            // The tags, Clear all and the count.
            float ty = bottom + 2, x = min.x;
            for (size_t i = 0; i < tags.size(); i++)
            {
                float w = Measure(g_black, 14, tags[i].text).x + 52;
                ImVec2 a(x, ty), b(x + w, ty + 28);
                Box(a, b, Look::Chip, Look::GoldBorder, look::Radius::Control, 1);
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
            Box(ca, cb, Look::Button, look::Colour::Border, look::Radius::Control, 1);
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

        // The seats under the page's title, the figures' bar and grid below (the launcher's look, docs/runtime/host/toybox_ux.md).
        constexpr float SeatTop = look::PageTop + 46, SeatHeight = 92, SeatWidth = 260, SeatGap = 14;

        void DrawSeats(Model& model)
        {
            constexpr float Left = look::PageLeft, Right = look::PageRight;
            // Add a figure and Manage, one above the other at the end of the row, as wide as the longer label; the seats
            // share the rest (five in Trap Team).
            std::vector<std::string> rows = model.ColumnRows();
            for (size_t r = 0; r + 1 < rows.size(); r++)
                rows[r] = "+ " + rows[r];
            float buttonWidth = 0;
            for (const std::string& row : rows)
                buttonWidth = std::max(buttonWidth, std::ceil(Measure(g_black, 14, row).x) + 28);
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
                const Figure* f = model.SeatFigure(int(i));
                const bool empty = f == nullptr && !s.physical;
                // The seat being filled is lit (the raised grey, its name in the accent); an empty seat is dashed.
                if (empty && !lit)
                {
                    RoundRect(a, b, Look::Surface, look::Radius::Card);
                    for (float d = a.x + 14; d < b.x - 14; d += 12)
                    {
                        Fill(ImVec2(d, a.y), ImVec2(std::min(d + 6, b.x - 14), a.y + 2), look::Colour::Border);
                        Fill(ImVec2(d, b.y - 2), ImVec2(std::min(d + 6, b.x - 14), b.y), look::Colour::Border);
                    }
                    for (float d = a.y + 14; d < b.y - 14; d += 12)
                    {
                        Fill(ImVec2(a.x, d), ImVec2(a.x + 2, std::min(d + 6, b.y - 14)), look::Colour::Border);
                        Fill(ImVec2(b.x - 2, d), ImVec2(b.x, std::min(d + 6, b.y - 14)), look::Colour::Border);
                    }
                }
                else
                    Box(a, b, lit ? Look::CardSelected : Look::Surface, lit ? look::Colour::Border : Look::CardBorder,
                        look::Radius::Card, 1);
                const int pictureSize = seatWidth < 200 ? 46 : 58;
                ImVec2 picture(a.x + 14 + pictureSize / 2.0f, y + SeatHeight / 2);
                std::string label = Upper(model.SeatLabel(int(i)));
                std::string name, line;
                uint32_t lineColor = Look::TextDim;
                if (s.physical)
                {
                    const Toy* toy = s.physicalKnown ? Catalogue::Get().Find(s.physicalType) : nullptr;
                    Portrait(toy,
                        s.physicalKnown ? Catalogue::Get().Describe(toy, s.physicalVariant).kind : VariantKind::Base,
                        picture, pictureSize, false, s.physicalVariant);
                    label += L(" · YOUR PORTAL", " · VOTRE PORTAIL");
                    name = s.physicalKnown ? Catalogue::Get().Describe(toy, s.physicalVariant).name
                                           : L("reading...", "lecture...");
                    if (s.physicalLevel > 0)
                        line = (French() ? "Niveau " : "Level ") + std::to_string(s.physicalLevel);
                }
                else if (f != nullptr)
                {
                    CardPortrait(model, *f, picture, pictureSize);
                    if (model.OutOfHealth(*f))
                        OutOfHealthCross(picture, pictureSize / 3.0f);
                    name = model.FigureName(*f);
                    line = model.LevelLine(*f);
                    // Imaginators: a creation crystal on a player's seat is that player's Imaginator.
                    if (f->toy != nullptr && f->toy->category == Category::CreationCrystal && line.empty())
                        line = L("Your Imaginator", "Votre Imaginator");
                    if (s.waiting)
                    {
                        line = s.late ? L("not taken yet: X to undo", "pas encore pris : X pour annuler")
                                      : L("waiting for the game...", "en attente du jeu...");
                        lineColor = Look::Waiting;
                    }
                }
                else if (!s.path.empty())
                {
                    EmptySeatPicture(picture, pictureSize);
                    name = L("Missing figure", "Figurine introuvable");
                }
                else
                {
                    EmptySeatPicture(picture, pictureSize);
                    name = model.realPortal ? L("None", "Aucun") : L("Empty", "Vide");
                }
                if (s.seat.player == 2 && s.seat.id[0] == 'p' && !s.physical && (f == nullptr || !s.waiting) &&
                    !GamepadConnected(1))
                {
                    line = seatWidth < 200 ? L("No controller", "Pas de manette")
                                           : L("needs a 2nd controller", "il faut une 2e manette");
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
                float tx = picture.x + pictureSize / 2.0f + 12, tw = b.x - tx - 10;
                const float nameSize = seatWidth < 200 ? 15 : 16;
                float top = y + SeatHeight / 2 - (line.empty() ? 20 : 29);
                Text(g_black, 12, ImVec2(tx, top), lit ? Look::GoldText : Look::TextDim, Fit(g_black, 12, label, tw));
                Text(empty ? g_bold : g_black, nameSize, ImVec2(tx, top + 18), empty ? Look::TextSoft : Look::Text,
                    Fit(g_black, nameSize, name, tw));
                if (!line.empty())
                    Text(g_bold, 13, ImVec2(tx, top + 40), lineColor == Look::TextDim ? Look::TextSoft : lineColor,
                        Fit(g_bold, 13, line, tw));
                if (focused)
                    FocusRing(a, b, look::Radius::Card);
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
            float h = (SeatHeight - 8) / 2;
            for (size_t r = 0; r < rows.size(); r++)
            {
                int row = int(model.seats.size() + r);
                float y = SeatTop + (rows.size() == 1 ? (SeatHeight - h) / 2 : r * (h + 8));
                ImVec2 a(Right - buttonWidth, y), b(Right, y + h);
                model.seatSpans.push_back({ a.x, b.x });
                bool focused = rowFocused && model.column == row;
                PlainButton(a, b, rows[r], 14, focused);
                AddHit(a, b, [&model, row] {
                    model.FocusColumnRow(row);
                    model.Run(Command::Accept);
                });
                g_hits.back().focus = [&model, row] { model.FocusColumnRow(row); };
            }
        }

        void DrawEmpty(Model& model, ImVec2 min, ImVec2 max)
        {
            float x = min.x, y = min.y + 10;
            Text(g_black, look::Type::Heading, ImVec2(x, y), Look::Text,
                L("My Skylanders is empty", "Mes Skylanders est vide"));
            y += 36;
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
            DrawRail(model);
            const std::string back = model.fromMenu ? L("Back", "Retour") : L("Close", "Fermer");
            // The portal: the page's title and what to do, the seats below.
            float titleWidth = Title(L("On the portal", "Sur le portail"), ImVec2(look::PageLeft, look::PageTop), 26);
            TextMiddle(g_bold, 14, look::PageLeft + titleWidth + 14, look::PageTop + 17, Look::TextDim,
                model.realPortal ? L("Change the toys on your portal", "Changez les jouets sur votre portail")
                                 : L("Choose a place, then a figure", "Choisissez une place, puis une figurine"));
            DrawSeats(model);
            ImVec2 min(look::PageLeft, SeatTop + SeatHeight + 20), max(look::PageRight, look::FooterTop - 8);
            if (!model.OwnsAnything())
            {
                DrawEmpty(model, min, max);
                PageFooter({ { 'A', L("Choose", "Choisir") }, { 'B', back } });
                return;
            }
            // What the grid is for: the chosen seat and how many figures this game reads for it.
            std::string choosing = model.realPortal
                                       ? std::string(L("YOUR FIGURES", "VOS FIGURINES"))
                                       : std::string(L("FOR ", "POUR ")) + Upper(model.SeatLabel(model.seat));
            choosing += " \xC2\xB7 " + Upper(FigureCount(model.shownCount, French()));
            TextMiddle(g_black, 12, min.x, min.y + 20, Look::TextDim, choosing);
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
                    hints.push_back({ 'L', L("Place", "Place") });
                hints.push_back({ 'B', model.search.empty() ? back : L("Clear", "Effacer") });
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
                    hints.push_back({ 'X', L("Details", "Détails") });
                hints.push_back({ 'Y', L("Search", "Rechercher") });
                if (model.seats.size() > 1)
                    hints.push_back({ 'L', L("Place", "Place") });
                hints.push_back({ 'B', back });
            }
            else if (model.column < int(model.seats.size()))
            {
                hints.push_back({ 'A', L("Choose", "Choisir") });
                if (!model.seats[model.column].path.empty())
                    hints.push_back({ 'X', L("Options", "Options") });
                hints.push_back({ 'B', back });
            }
            else
            {
                hints.push_back({ 'A', L("Open", "Ouvrir") });
                hints.push_back({ 'B', back });
            }
            PageFooter(hints);
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

        // --- The F3 menu (docs/runtime/host/gamemenu.md): a side panel over the game, the pages beside the rail ---------------

        // A switch: a knob on a track, the accent (a dark knob) when on.
        void Switch(ImVec2 center, bool on, float width = 44, float height = 26)
        {
            ImVec2 a(center.x - width / 2, center.y - height / 2), b(center.x + width / 2, center.y + height / 2);
            RoundRect(a, b, on ? Look::Gold : look::Colour::Control, height / 2);
            const int knob = int(height - 6);
            Disc(ImVec2(on ? b.x - 3 - knob / 2.0f : a.x + 3 + knob / 2.0f, center.y), knob,
                on ? look::Colour::Well : look::Colour::TextSoft);
        }

        // A page's title in Bungee and a short line beside it.
        void PageTitle(const std::string& title, const std::string& line)
        {
            float width = Title(title, ImVec2(look::PageLeft, look::PageTop), look::Type::Title);
            if (!line.empty())
                TextMiddle(g_bold, 14, look::PageLeft + width + 16, look::PageTop + 20, Look::TextDim, line);
        }

        // The Controls page: the actions down, a column per device (Model::CommandHeaders) across, then the mouse's
        // switch, the Toy Box's keys, Reset and Back.
        void DrawCommands(Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            DrawRail(model);
            std::vector<CommandLine> lines = model.CommandLines();
            std::vector<std::string> headers = model.CommandHeaders();
            const CommandsLayout layout = look::Commands(int(lines.size()), model.commandRow, !model.toast.empty());
            PageTitle(L("Controls", "Commandes"), L("Saved as you change them", "Enregistré à chaque changement"));
            constexpr float CellWidth = 196, Gap = Space::S;
            const float cellsLeft = PageRight - float(headers.size()) * (CellWidth + Gap) + Gap;
            auto cellLeft = [&](size_t column) { return cellsLeft + float(column) * (CellWidth + Gap); };
            const float top = layout.firstRow;
            const int total = int(lines.size());
            // Page buttons remain reachable with the mouse; keyboard navigation reveals its focused row too.
            if (total > layout.capacity)
            {
                const float cy = PageTop + 20;
                std::string pages = std::to_string(layout.first + 1) + "-" +
                                    std::to_string(layout.first + layout.count) + " / " + std::to_string(total);
                TextMiddle(g_bold, Type::Small, PageRight - 96 - Measure(g_bold, Type::Small, pages).x - 12, cy,
                    Look::TextDim, pages);
                for (int direction : { -1, 1 })
                {
                    float x = PageRight - (direction < 0 ? 84 : 40);
                    ImVec2 a(x, cy - 18), b(x + 40, cy + 18);
                    bool enabled = direction < 0 ? layout.first > 0 : layout.first + layout.count < total;
                    Box(a, b, Look::Button, Colour::Border, Radius::Control, 1);
                    Chevron(ImVec2(x + 20, cy), 12, enabled ? Look::Text : Look::TextDimmer, direction < 0 ? 2 : 0);
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
                TextCentered(g_black, Type::Caption, ImVec2(cellLeft(c) + CellWidth / 2, top - 18), Look::TextDim,
                    Upper(headers[c]));
            for (int index = layout.first; index < layout.first + layout.count; index++)
            {
                const CommandLine& line = lines[size_t(index)];
                float y = top + float(index - layout.first) * CommandsPitch;
                float cy = y + layout.rowHeight / 2;
                bool focusedRow = model.commandRow == index;
                ImVec2 rowA(PageLeft, y), rowB(PageRight, y + layout.rowHeight);
                if (line.cells.empty())
                {
                    if (focusedRow)
                    {
                        RoundRect(rowA, rowB, Look::CardSelected, Radius::Row);
                        FocusRing(rowA, rowB, Radius::Row);
                    }
                    if (!line.value.empty())
                    {
                        TextMiddle(g_black, Type::Body, rowA.x + Space::L, cy, Look::Text, line.label);
                        bool on = line.value == L("On", "Oui");
                        Switch(ImVec2(cellLeft(0) + CellWidth - 22, cy), on);
                    }
                    else
                    {
                        TextMiddle(focusedRow ? g_black : g_bold, Type::Body, rowA.x + Space::L, cy,
                            index + 1 == total ? Look::TextSoft : Look::Text, line.label);
                        if (index + 1 != total)
                            Chevron(ImVec2(rowB.x - Space::L, cy), 12, Look::TextDim, 0);
                    }
                    AddHit(rowA, rowB, [&model, index] { model.FocusCommand(index, -1); });
                    g_hits.back().target = "commands=" + std::to_string(index);
                    continue;
                }
                if (focusedRow)
                    RoundRect(rowA, rowB, Look::Surface, Radius::Row);
                TextMiddle(focusedRow ? g_black : g_bold, Type::Body, rowA.x + Space::L, cy,
                    focusedRow ? Look::Text : Look::TextSoft,
                    Fit(g_black, Type::Body, line.label, cellsLeft - rowA.x - Space::XL));
                for (size_t c = 0; c < line.cells.size(); c++)
                {
                    const CommandLine::Cell& cell = line.cells[c];
                    if (cell.text.empty())
                        continue; // a Toy Box key has no pad or mouse cell
                    ImVec2 a(cellLeft(c), y + 3), b(cellLeft(c) + CellWidth, y + layout.rowHeight - 3);
                    bool focused = focusedRow && model.commandColumn == int(c);
                    Box(a, b,
                        focused         ? Look::CardSelected
                        : cell.editable ? Look::Raised
                                        : Look::Page,
                        cell.editable ? Colour::Border : Colour::Hairline, Radius::Control, 1);
                    if (focused)
                        FocusRing(a, b, Radius::Control);
                    TextCentered(g_black, Type::Small, ImVec2((a.x + b.x) / 2, cy),
                        cell.editable ? Look::Text : Look::TextDim,
                        Fit(g_black, Type::Small, cell.text, CellWidth - Space::XL));
                    int column = int(c);
                    AddHit(a, b, [&model, index, column] { model.FocusCommand(index, column); });
                    g_hits.back().target = "commands=" + std::to_string(index) + "/" + std::to_string(column);
                }
            }
            if (model.waiting != Model::Waiting::None)
            {
                Fill(ImVec2(PageLeft, FooterTop), ImVec2(PageRight, FooterTop + 1), Look::Hairline);
                TextMiddle(g_bold, Type::Small, PageLeft, FooterTop + FooterHeight / 2, Look::TextSoft,
                    model.waiting == Model::Waiting::Key
                        ? L("Press the new key (Escape cancels)", "Appuyez sur la nouvelle touche (Échap pour annuler)")
                    : model.waiting == Model::Waiting::PadButton
                        ? L("Press the new button on the pad (Escape cancels)",
                              "Appuyez sur le nouveau bouton de la manette (Échap pour annuler)")
                        : L("Click a mouse button or turn the wheel (Escape cancels)",
                              "Cliquez avec un bouton de la souris ou tournez la molette (Échap pour annuler)"));
            }
            else
                PageFooter({ { 'A', L("Change", "Changer") }, { 'L', L("Pages", "Pages") },
                    { 'B', model.fromMenu ? L("Back", "Retour") : L("Close", "Fermer") } });
        }

        // The panel over the running game (look::Main): the game's name and the profile, the volume, the rows, the hints.
        // The focus is the accent's ring; the game stays visible on the left.
        void DrawMenu(Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            const MainLayout layout = look::Main();
            Fill(ImVec2(0, 0), ImVec2(MainLeft, CanvasHeight), Colour::Scrim, 0.28f);
            // A soft shadow on the game along the panel's edge.
            for (int i = 0; i < 8; i++)
                Fill(ImVec2(MainLeft - 24 + i * 3, 0), ImVec2(MainLeft - 21 + i * 3, CanvasHeight), 0x000000,
                    0.03f * float(i + 1));
            Fill(ImVec2(MainLeft, 0), ImVec2(CanvasWidth, CanvasHeight), Colour::Page, 0.98f);
            Fill(ImVec2(MainLeft, 0), ImVec2(MainLeft + 1, CanvasHeight), Colour::Hairline);
            AddHit(ImVec2(MainLeft, 0), ImVec2(CanvasWidth, CanvasHeight), {});
            g_hits.back().hover = false;
            const float left = MainLeft + MainPad, right = CanvasWidth - MainPad;

            // The profile's pill, then the game's name in what is left.
            const std::string player = PlayerName();
            float pillWidth = std::min(200.0f, 6 + 32 + 10 + Measure(g_black, 14, player).x + 14);
            ImVec2 pa(right - pillWidth, MainPad - 2), pb(right, MainPad + 42);
            RoundRect(pa, pb, Colour::Border, 22);
            RoundRect(ImVec2(pa.x + 1, pa.y + 1), ImVec2(pb.x - 1, pb.y - 1), Colour::Raised, 21);
            Avatar(ImVec2(pa.x + 6 + 16, (pa.y + pb.y) / 2), 32);
            TextMiddle(g_black, 14, pa.x + 6 + 32 + 10, (pa.y + pb.y) / 2, Colour::Text,
                Fit(g_black, 14, player, pb.x - 12 - (pa.x + 48)));
            std::string title = GameTitle(model);
            float size = 22;
            while (size > 14 && Measure(g_title, size, title).x > pa.x - 12 - left)
                size -= 1;
            Text(g_title, size, ImVec2(left, MainPad), Look::Accent, title);
            const std::string& version = gamemenu::Current().version;
            Text(g_bold, 13, ImVec2(left, MainPad + 26), Colour::TextDim,
                Fit(g_bold, 13, "Arkeyan" + (version.empty() ? std::string() : " \xC2\xB7 " + version),
                    pa.x - 12 - left));

            std::vector<MenuRow> rows = model.MainRows();
            for (int i = 0; i < MainRows; i++)
            {
                const MenuRow& row = rows[size_t(i)];
                ImVec2 a(left, layout.rowTop[i]), b(right, layout.rowTop[i] + layout.rowHeight[i]);
                const float cy = (a.y + b.y) / 2;
                const bool focused = model.menuRow == i;
                auto focusRow = [&model, i] {
                    model.menuRow = i;
                    model.Touch();
                };
                if (i == Model::MainVolume)
                {
                    Box(a, b, focused ? Look::CardSelected : Colour::Surface, Colour::Hairline, Radius::Card, 1);
                    if (focused)
                        FocusRing(a, b, Radius::Card);
                    const float ty = a.y + 24;
                    DrawIcon(Icon::Volume, ImVec2(a.x + 16 + 10, ty), 20, Colour::TextSoft);
                    TextMiddle(g_bold, 15, a.x + 16 + 20 + 12, ty, Colour::Text, row.label);
                    float valueWidth = Measure(g_black, 15, row.value).x;
                    TextMiddle(g_black, 15, b.x - 16 - valueWidth, ty, Look::Accent, row.value);
                    ImVec2 ta(a.x + 16, a.y + 48), tb(b.x - 16, a.y + 54);
                    RoundRect(ta, tb, Colour::Raised2, 3);
                    float at = ta.x + (tb.x - ta.x) * float(row.percent) / 100.0f;
                    if (row.percent > 0)
                        RoundRect(ta, ImVec2(std::max(at, ta.x + 6), tb.y), Look::Accent, 3);
                    Disc(ImVec2(at, (ta.y + tb.y) / 2), 16, Colour::Text);
                    AddHit(a, b, [&model] { model.FocusMenuRow(Model::MainVolume, 0); });
                    g_hits.back().target = "menu=0";
                    g_hits.back().focus = focusRow;
                    const float trackLeft = ta.x, trackWidth = tb.x - ta.x;
                    AddHit(ImVec2(ta.x - 10, ta.y - 12), ImVec2(tb.x + 10, tb.y + 12), [&model, trackLeft, trackWidth] {
                        model.SetVolumeAt(int(std::lround((g_pointer.x - trackLeft) / trackWidth * 100)));
                    });
                    g_hits.back().target = "menu=0";
                    g_hits.back().focus = focusRow;
                    continue;
                }
                if (i == MainSmallFrom)
                    Fill(ImVec2(a.x + 4, a.y - MainSeparator / 2 - 4), ImVec2(b.x - 4, a.y - MainSeparator / 2 - 3),
                        Colour::Hairline);
                if (focused)
                {
                    RoundRect(a, b, Look::CardSelected, Radius::Row);
                    FocusRing(a, b, Radius::Row);
                }
                static const Icon Icons[] = { Icon::Volume, Icon::Play, Icon::ToyBox, Icon::Trophy, Icon::Sliders,
                    Icon::Activity, Icon::Flag, Icon::Quit };
                const bool quit = i == Model::MainQuit, small = i >= MainSmallFrom;
                const uint32_t ink = quit ? Colour::DangerText : small ? Colour::TextSoft : Colour::Text;
                DrawIcon(Icons[i], ImVec2(a.x + 16 + 10, cy), 20, ink);
                TextMiddle(focused ? g_black : g_bold, small ? 15 : 16, a.x + 16 + 20 + 14, cy, ink,
                    Fit(g_black, 16, row.label, b.x - a.x - 140));
                if (row.kind == MenuRow::Kind::Toggle)
                    Switch(ImVec2(b.x - 16 - 20, cy), row.value == L("On", "Oui"), 40, 24);
                else if (row.kind == MenuRow::Kind::Choice)
                {
                    // The debug screen's level: Off, Compact or Full, between chevrons when focused.
                    const bool on = gamemenu::Current().debugScreen != 0;
                    float w = Measure(g_black, 14, row.value).x, x = b.x - 16 - (focused ? 18 : 0) - w;
                    TextMiddle(g_black, 14, x, cy, on ? Look::Accent : Colour::TextDim, row.value);
                    if (focused)
                    {
                        Chevron(ImVec2(x - 12, cy), 10, Colour::TextSoft, 2);
                        Chevron(ImVec2(b.x - 16 - 5, cy), 10, Colour::TextSoft, 0);
                    }
                }
                else if (!row.value.empty())
                {
                    float w = Measure(g_bold, 13, row.value).x;
                    TextMiddle(g_bold, 13, b.x - 16 - w, cy, Colour::TextDim, row.value);
                }
                AddHit(a, b, [&model, i] { model.FocusMenuRow(i, 0); });
                g_hits.back().target = "menu=" + std::to_string(i);
                g_hits.back().focus = focusRow;
            }

            // The hints, under a line.
            Fill(ImVec2(left, layout.footerTop), ImVec2(right, layout.footerTop + 1), Colour::Hairline);
            const float fy = layout.footerTop + 14 + 13;
            float x = HintsFrom(left, fy, { { 'A', L("Select", "Choisir") }, { 'B', L("Resume", "Reprendre") } });
            if (KeyboardStyle())
            {
                x += 22;
                x += KeyCap(ImVec2(x, fy - 13), 26, 12, "F3") + 8;
                TextMiddle(g_bold, 14, x, fy, Colour::TextSoft, L("Close", "Fermer"));
            }
        }

        // The Settings page: one list in groups (the focused row's value as a stepper), and what the setting does, its cost
        // and when it applies on the right.
        void DrawSettings(Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            DrawRail(model);
            PageTitle(L("Settings", "Réglages"), L("Saved as you change them", "Enregistré à chaque changement"));
            std::vector<MenuRow> rows = model.SettingsRows();
            if (rows.empty())
                return;
            model.settingsRow = std::clamp(model.settingsRow, 0, int(rows.size()) - 1);
            const float listLeft = PageLeft, listRight = PageRight - SettingsDetailWidth - SettingsDetailGap;
            const float listTop = SettingsListTop, listBottom = FooterTop - 8;
            // The items' places from the list's top.
            std::vector<float> rowTop(rows.size()), headingTop(rows.size(), -1);
            float y = 0;
            for (size_t i = 0; i < rows.size(); i++)
            {
                if (!rows[i].heading.empty())
                {
                    if (i > 0)
                        y += SettingsGroupGap;
                    headingTop[i] = y;
                    y += SettingsHeadingHeight;
                }
                rowTop[i] = y;
                y += SettingsRowHeight + SettingsRowGap;
            }
            const size_t focus = size_t(model.settingsRow);
            model.settingsScroll = ListScroll(headingTop[focus] >= 0 ? headingTop[focus] : rowTop[focus],
                rowTop[focus] + SettingsRowHeight, y - SettingsRowGap, listBottom - listTop, model.settingsScroll);
            const float scroll = model.settingsScroll;
            g_draw->PushClipRect(ImVec2(listLeft - 2, listTop), ImVec2(listRight + 2, listBottom), true);
            for (size_t i = 0; i < rows.size(); i++)
            {
                const MenuRow& row = rows[i];
                if (headingTop[i] >= 0)
                    TextMiddle(g_black, Type::Caption, listLeft + 16, listTop + headingTop[i] - scroll + 17,
                        Colour::TextDim, Upper(row.heading));
                ImVec2 a(listLeft, listTop + rowTop[i] - scroll), b(listRight, a.y + SettingsRowHeight);
                if (b.y < listTop || a.y > listBottom)
                    continue;
                const float cy = (a.y + b.y) / 2;
                const bool focused = int(i) == model.settingsRow;
                const int index = int(i);
                if (focused)
                {
                    RoundRect(a, b, Look::CardSelected, Radius::Row);
                    FocusRing(a, b, Radius::Row);
                }
                const bool greyed = !row.unavailable.empty();
                TextMiddle(focused ? g_black : g_bold, Type::Body, a.x + 18, cy,
                    greyed    ? Colour::TextDim
                    : focused ? Colour::Text
                              : Colour::TextSoft,
                    Fit(g_black, Type::Body, row.label, b.x - a.x - 240));
                auto focusRow = [&model, index] {
                    model.settingsRow = index;
                    model.Touch();
                };
                AddHit(a, b, [&model, index] { model.FocusSettingsRow(index, 0); });
                g_hits.back().target = "settings=" + std::to_string(index);
                g_hits.back().focus = focusRow;
                const float right = b.x - 18;
                if (greyed)
                {
                    // It cannot change now: its value in the dim colour, no switch (the reason is in the detail).
                    TextMiddle(g_black, Type::Body, right - Measure(g_black, Type::Body, row.value).x, cy,
                        Colour::TextDim, row.value);
                    continue;
                }
                if (row.kind == MenuRow::Kind::Toggle)
                {
                    Switch(ImVec2(right - 22, cy), row.value == L("On", "Oui"));
                    continue;
                }
                if (row.kind == MenuRow::Kind::Action)
                {
                    Chevron(ImVec2(right - 6, cy), 12, focused ? Colour::Text : Colour::TextDim, 0);
                    continue;
                }
                if (!focused)
                {
                    TextMiddle(g_black, Type::Body, right - Measure(g_black, Type::Body, row.value).x, cy, Colour::Text,
                        row.value);
                    continue;
                }
                // The stepper: the value between two chevrons, its place among the values under it.
                const float width = 150, x0 = right - width, mid = right - width / 2;
                Chevron(ImVec2(x0 + 6, cy), 12, Colour::TextSoft, 2);
                Chevron(ImVec2(right - 6, cy), 12, Colour::TextSoft, 0);
                TextCentered(g_black, Type::Body,
                    ImVec2(mid, cy - (row.options > 1 || row.kind == MenuRow::Kind::Slider ? 5 : 0)), Look::Accent,
                    Fit(g_black, Type::Body, row.value, width - 40));
                if (row.kind == MenuRow::Kind::Slider)
                {
                    ImVec2 ta(mid - 40, cy + 9), tb(mid + 40, cy + 12);
                    RoundRect(ta, tb, Colour::Control, 1.5f);
                    if (row.percent > 0)
                        RoundRect(
                            ta, ImVec2(ta.x + (tb.x - ta.x) * float(row.percent) / 100, tb.y), Look::Accent, 1.5f);
                }
                else if (row.options > 1)
                {
                    const float step = std::min(14.0f, (width - 40 - 4 * float(row.options - 1)) / float(row.options));
                    float sx = mid - (step * float(row.options) + 4 * float(row.options - 1)) / 2;
                    for (int o = 0; o < row.options; o++, sx += step + 4)
                        RoundRect(ImVec2(sx, cy + 9), ImVec2(sx + step, cy + 12),
                            o <= row.option ? Look::Accent : Colour::Control, 1.5f);
                }
                AddHit(ImVec2(x0 - 6, a.y), ImVec2(mid, b.y), [&model, index] { model.FocusSettingsRow(index, -1); });
                g_hits.back().target = "settings=" + std::to_string(index);
                g_hits.back().focus = focusRow;
                AddHit(ImVec2(mid, a.y), ImVec2(right + 6, b.y), [&model, index] { model.FocusSettingsRow(index, 1); });
                g_hits.back().target = "settings=" + std::to_string(index);
                g_hits.back().focus = focusRow;
            }
            g_draw->PopClipRect();

            // What the focused setting does.
            const MenuRow& row = rows[focus];
            const float dx = listRight + SettingsDetailGap, dr = PageRight;
            float dy = listTop + 6;
            Text(g_black, Type::Heading, ImVec2(dx, dy), Colour::Text, Fit(g_black, Type::Heading, row.label, dr - dx));
            dy += 34;
            for (const std::string& line : Wrap(g_bold, 15, row.help, dr - dx, 3))
            {
                Text(g_bold, 15, ImVec2(dx, dy), Colour::TextSoft, line);
                dy += 22;
            }
            dy += 14;
            if (row.cost >= 0)
            {
                ImVec2 ca(dx, dy), cb(dr, dy + 48);
                Box(ca, cb, Colour::Surface, Colour::Hairline, Radius::Row, 1);
                TextMiddle(g_bold, 14, ca.x + 16, dy + 24, Colour::TextSoft, L("Graphics card", "Carte graphique"));
                for (int step = 0; step < 5; step++)
                {
                    float sx = cb.x - 16 - (5 - step) * 22 + 4;
                    RoundRect(ImVec2(sx, dy + 20), ImVec2(sx + 18, dy + 28),
                        step < row.cost ? Look::Accent : Colour::Control, 2);
                }
                dy += 48 + 12;
            }
            if (!row.unavailable.empty())
            {
                for (const std::string& line : Wrap(g_bold, 14, row.unavailable, dr - dx, 2))
                {
                    TextMiddle(g_bold, 14, dx, dy + 10, Colour::TextDim, line);
                    dy += 22;
                }
            }
            else if (row.nextStart)
            {
                Disc(ImVec2(dx + 5, dy + 10), 8, Colour::Warning);
                TextMiddle(g_bold, 14, dx + 18, dy + 10, Colour::Warning,
                    L("Applies at the next start", "S'applique au prochain démarrage"));
            }
            PageFooter({ { 'A', L("Change", "Changer") }, { 'L', L("Pages", "Pages") },
                { 'B', model.fromMenu ? L("Back", "Retour") : L("Close", "Fermer") } });
        }

        // "Unlocked Oct 20" / "Débloqué le 20 oct." (the year too when it is not this one).
        std::string UnlockDate(int64_t when)
        {
            time_t time = time_t(when), now = std::time(nullptr);
            tm day{}, today{};
#ifdef _WIN32
            localtime_s(&day, &time);
            localtime_s(&today, &now);
#else
            localtime_r(&time, &day);
            localtime_r(&now, &today);
#endif
            static const char* English[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
                "Nov", "Dec" };
            static const char* French[] = { "janv.", "févr.", "mars", "avr.", "mai", "juin", "juil.", "août", "sept.",
                "oct.", "nov.", "déc." };
            const int month = std::clamp(day.tm_mon, 0, 11);
            std::string year = day.tm_year != today.tm_year ? " " + std::to_string(day.tm_year + 1900) : "";
            return toybox::French()
                       ? "Débloqué le " + std::to_string(day.tm_mday) + " " + French[month] + year
                       : std::string("Unlocked ") + English[month] + " " + std::to_string(day.tm_mday) + year;
        }

        // The Achievements page: the ring and the count, the tabs, the title's achievements as cards of four columns.
        void DrawAchievements(Model& model)
        {
            using namespace look;
            using look::CanvasWidth, look::CanvasHeight;
            DrawRail(model);
            const int total = int(model.achievementRows.size()), unlocked = model.AchievementsUnlocked();
            uint32_t score = 0, scoreTotal = 0;
            for (const ToyboxAchievementRow& row : model.achievementRows)
            {
                scoreTotal += row.points;
                score += row.unlocked != 0 ? row.points : 0;
            }
            // The ring.
            const ImVec2 ring(PageLeft + 48, PageTop + 48);
            Ring(ring, 84, Colour::Raised2, 8);
            const float part = total > 0 ? float(unlocked) / float(total) : 0;
            if (part > 0)
            {
                g_draw->PathArcTo(ring, 38, -1.5707963f, -1.5707963f + part * 6.2831853f, 48);
                g_draw->PathStroke(Rgb(Look::Accent), ImDrawFlags_None, 8.0f);
            }
            TextCentered(g_title, 20, ring, Colour::Text, std::to_string(int(std::lround(part * 100))) + "%");
            const float tx = PageLeft + 96 + 28;
            Title(L("Achievements", "Succès"), ImVec2(tx, PageTop + 10), Type::Title);
            std::string count = toybox::French() ? std::to_string(unlocked) + " sur " + std::to_string(total)
                                                 : std::to_string(unlocked) + " of " + std::to_string(total);
            Text(g_black, 16, ImVec2(tx, PageTop + 54), Colour::Text, count);
            Text(g_bold, 16, ImVec2(tx + Measure(g_black, 16, count).x, PageTop + 54), Colour::TextSoft,
                " \xC2\xB7 " + std::to_string(score) + " / " + std::to_string(scoreTotal) + " G");
            // The tabs.
            const std::string tabs[] = { L("All", "Tous"), L("Unlocked", "Débloqués"), L("Locked", "Verrouillés") };
            float tabsWidth = 8;
            for (const std::string& tab : tabs)
                tabsWidth += Measure(g_black, 14, tab).x + 36;
            ImVec2 ta(PageRight - tabsWidth, PageTop + 28), tb(PageRight, PageTop + 28 + 46);
            Box(ta, tb, Colour::Surface, Colour::Hairline, Radius::Row, 1);
            float x = ta.x + 4;
            for (int t = 0; t < 3; t++)
            {
                float w = Measure(g_black, 14, tabs[t]).x + 36;
                ImVec2 a(x, ta.y + 4), b(x + w, tb.y - 4);
                if (model.achievementTab == t)
                    RoundRect(a, b, Colour::Raised2, 9);
                TextCentered(model.achievementTab == t ? g_black : g_bold, 14, ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2),
                    model.achievementTab == t ? Colour::Text : Colour::TextSoft, tabs[t]);
                AddHit(a, b, [&model, t] { model.SetAchievementTab(t); });
                g_hits.back().target = "tab=" + std::to_string(t);
                x += w;
            }
            // The grid.
            const std::vector<int> shown = model.AchievementsShown();
            const float top = AchievementGridTop, bottom = FooterTop - 8;
            const float cardWidth =
                (PageRight - PageLeft - AchievementGap * float(AchievementColumns - 1)) / float(AchievementColumns);
            const int rowCount = (int(shown.size()) + AchievementColumns - 1) / AchievementColumns;
            const int visible =
                std::max(1, int((bottom - top + AchievementGap) / (AchievementCardHeight + AchievementGap)));
            if (shown.empty())
            {
                Text(g_black, Type::Heading, ImVec2(PageLeft, top + 10), Colour::Text,
                    total == 0 ? L("No achievements to show", "Aucun succès à afficher")
                               : L("Nothing here yet", "Rien pour l'instant"));
                Text(g_bold, 15, ImVec2(PageLeft, top + 42), Colour::TextDim,
                    total == 0 ? L("The game's list is read from its disc.", "La liste du jeu est lue sur son disque.")
                               : L("Keep playing.", "Continuez à jouer."));
            }
            model.achievementCursor = std::clamp(model.achievementCursor, 0, std::max(0, int(shown.size()) - 1));
            model.achievementFirstRow = GridFirstRow(
                model.achievementCursor / AchievementColumns, model.achievementFirstRow, visible, rowCount);
            g_draw->PushClipRect(ImVec2(PageLeft - 2, top - 2), ImVec2(PageRight + 2, bottom), true);
            ImVec2 ringMin, ringMax;
            bool ringShown = false;
            for (int i = model.achievementFirstRow * AchievementColumns; i < int(shown.size()); i++)
            {
                const int r = i / AchievementColumns - model.achievementFirstRow, c = i % AchievementColumns;
                ImVec2 a(PageLeft + float(c) * (cardWidth + AchievementGap),
                    top + float(r) * (AchievementCardHeight + AchievementGap));
                ImVec2 b(a.x + cardWidth, a.y + AchievementCardHeight);
                if (a.y > bottom)
                    break;
                const ToyboxAchievementRow& item = model.achievementRows[size_t(shown[size_t(i)])];
                const bool focused = i == model.achievementCursor, open = item.unlocked != 0;
                Box(a, b, focused ? Look::CardSelected : Colour::Surface, Colour::Hairline, Radius::Card, 1);
                if (focused)
                {
                    ringMin = a;
                    ringMax = b;
                    ringShown = true;
                }
                const ImVec2 medal(a.x + 16 + 24, a.y + 16 + 24);
                Disc(medal, 48, open ? Look::Accent : Colour::Raised2);
                DrawIcon(open ? Icon::Trophy : Icon::Lock, medal, open ? 26 : 22, open ? Colour::Well : Colour::TextDim,
                    open ? 2.2f : 2);
                std::string points = std::to_string(item.points) + " G";
                TextMiddle(g_black, 14, b.x - 16 - Measure(g_black, 14, points).x, medal.y,
                    open ? Look::Accent : Colour::TextDim, points);
                const float w = cardWidth - 32;
                float ty = a.y + 16 + 48 + 10;
                std::string name = item.secret ? L("Secret achievement", "Succès secret") : item.name;
                std::vector<std::string> nameLines = Wrap(g_black, 16, name, w, 2);
                for (const std::string& line : nameLines)
                {
                    Text(g_black, 16, ImVec2(a.x + 16, ty), Colour::Text, line);
                    ty += 20;
                }
                std::string description = item.secret
                                              ? L("Keep playing to find it.", "Continuez à jouer pour le découvrir.")
                                              : item.description;
                for (const std::string& line : Wrap(g_bold, 13, description, w, nameLines.size() > 1 ? 2 : 3))
                {
                    ty += 0;
                    Text(g_bold, 13, ImVec2(a.x + 16, ty + 4), Colour::TextSoft, line);
                    ty += 18;
                }
                Text(g_black, 12, ImVec2(a.x + 16, b.y - 16 - 14), open ? Colour::Success : Colour::TextDim,
                    open ? UnlockDate(item.unlocked) : L("Locked", "Verrouillé"));
                const int index = i;
                AddHit(a, b, [&model, index] { model.FocusAchievement(index); });
                g_hits.back().target = "achievement=" + std::to_string(index);
                g_hits.back().focus = [&model, index] { model.FocusAchievement(index); };
            }
            g_draw->PopClipRect();
            if (ringShown && ringMax.y <= bottom)
                FocusRing(ringMin, ringMax, Radius::Card);
            PageFooter({ { 'L', L("All · Unlocked · Locked", "Tous · Débloqués · Verrouillés") },
                { 'B', model.fromMenu ? L("Back", "Retour") : L("Close", "Fermer") } });
        }

        // The FPS counter: a small readout in the top right corner of the safe area, over everything.
        void DrawFps(const Model& model)
        {
            if (model.fps.empty())
                return;
            float width = Measure(g_black, look::Type::Small, model.fps).x + 2 * look::Space::M;
            ImVec2 min(look::SafeRight - width, look::SafeTop), max(look::SafeRight, look::SafeTop + 26);
            RoundRect(min, max, look::Colour::Page, 13, 0.85f);
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

        // The full screens' backdrop: the launcher's page grey.
        imgui_software::Texture* MakeBackground()
        {
            const uint32_t rgb = look::Colour::Page;
            const uint32_t pixel = 0xFF000000u | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF);
            return new imgui_software::Texture{ CanvasWidth, CanvasHeight,
                std::vector<uint32_t>(size_t(CanvasWidth) * CanvasHeight, pixel) };
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
        g_bold = add(g_embeddedOutfitSemiBold, g_embeddedOutfitSemiBoldSize);
        g_black = add(g_embeddedOutfitBold, g_embeddedOutfitBoldSize);
        g_display = g_black;
        g_title = add(g_embeddedBungee, g_embeddedBungeeSize);
        g_mono = io.Fonts->AddFontDefaultBitmap();
        imgui_software::Init();
        g_background = MakeBackground();
        // The one accent: the running game's colour (Skylauncher's), readable on the panels.
        const GameInfo* info = FindGame(ARKEYAN_GAME_ID);
        Look::SetAccent(info != nullptr ? info->glow : GameInfo{}.glow);
        return g_bold != nullptr && g_black != nullptr && g_display != nullptr && g_title != nullptr;
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
            gamemenu::DrawDebugScreen({ g_black, g_mono, g_title }); // Outfit Bold, as its own test draws it
        if (!model.open)
        {
            DrawPrompt(model);
            DrawToast(model, ToastPlace::OverGame, true);
        }
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
            case Screen::Settings: DrawSettings(model); break;
            case Screen::Achievements: DrawAchievements(model); break;
            case Screen::Commands: DrawCommands(model); break;
            }
            DrawDialog(model);
            DrawToast(model,
                model.screen == Screen::Menu ? ToastPlace::BesideMenu
                : overGame                   ? ToastPlace::OverGame
                                             : ToastPlace::OnPage,
                false);
        }
        // What the pointer is on: a soft outline, independent of keyboard and pad focus.
        g_hovered = model.open && model.screen != Screen::Wheel ? HoveredHit() : -1;
        if (g_hovered >= 0)
        {
            const Hit& hit = g_hits[size_t(g_hovered)];
            RoundFrame(ImVec2(hit.min.x - 3, hit.min.y - 3), ImVec2(hit.max.x + 3, hit.max.y + 3),
                look::Colour::KeyOutline,
                std::min({ 15.0f, (hit.max.x - hit.min.x) / 2 + 3, (hit.max.y - hit.min.y) / 2 + 3 }), 1);
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
