#include "host/glyphs.h"
#include "game.h"
#include "d3d/xenos.h"
#include "gpu/gpu.h"
#include "host/embedded_fonts.h"
#include "host/input.h"
#include "host/settings.h"
#include "memory.h"
#include <SDL3/SDL.h>
#include <stb_truetype.h>
#include <stb_image_write.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include "env.h"

namespace glyphs
{
    namespace
    {
        // The game's glyph textures (games/<id>/adapter.cpp).
        const Texture* Find(uint64_t hash)
        {
            for (const Texture& known : game::Current().glyphTextures)
                if (known.hash == hash)
                    return &known;
            return nullptr;
        }

        // What the renderer stores as the content hash of a replacement: the guest hash salted by the style.
        uint64_t Salted(uint64_t hash, Style style) { return hash ^ (0x9E3779B97F4A7C15ull * (uint64_t(style) + 1)); }

        const char* StyleName(Style style)
        {
            static const char* const Names[] = { "xbox360", "xboxone", "playstation", "nintendo", "keyboard",
                "playstation3" };
            return Names[size_t(style)];
        }

        bool IsPlayStation(Style style) { return style == Style::PlayStation || style == Style::PlayStation3; }

        // The styles we draw (every one but the game's own Xbox 360).
        constexpr Style DrawnStyles[] = { Style::XboxOne, Style::PlayStation, Style::PlayStation3, Style::Nintendo,
            Style::Keyboard };

        std::atomic<Style> g_style{ Style::Xbox360 };
        bool g_auto = true;
        bool g_xboxOnly = false; // glyphs=xbox: auto among the two Xbox styles
        bool g_keyboard = true, g_gamepad = true; // the devices the game reads (--input)
        bool g_used = false;                      // a device was used: opening a gamepad no longer switches
        std::string g_keys[std::size(g_keyActions)]; // what each keycap shows

        stbtt_fontinfo g_font; // Bungee, built into the executable
        bool g_fontLoaded = false;

        // --- Drawing: signed distances in pixels, coverage-blended into a premultiplied float canvas. --------------------

        struct Color
        {
            float r, g, b, a = 1.0f;
        };
        Color Rgb(int r, int g, int b, float a = 1.0f) { return { r / 255.0f, g / 255.0f, b / 255.0f, a }; }
        Color Mix(Color x, Color y, float t)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            return { x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t };
        }

        struct Canvas
        {
            uint32_t width, height;
            std::vector<float> pixels; // premultiplied RGBA

            Canvas(uint32_t w, uint32_t h) : width(w), height(h), pixels(size_t(w) * h * 4, 0.0f) {}

            void Blend(uint32_t x, uint32_t y, Color c, float coverage)
            {
                float a = c.a * coverage;
                float* p = &pixels[(size_t(y) * width + x) * 4];
                p[0] = c.r * a + p[0] * (1 - a);
                p[1] = c.g * a + p[1] * (1 - a);
                p[2] = c.b * a + p[2] * (1 - a);
                p[3] = a + p[3] * (1 - a);
            }

            // `distance(x, y)`: negative inside; `paint(x, y)`: the colour there.
            template <class Distance, class Paint>
            void Fill(Distance distance, Paint paint)
            {
                for (uint32_t y = 0; y < height; y++)
                    for (uint32_t x = 0; x < width; x++)
                    {
                        float px = x + 0.5f, py = y + 0.5f;
                        float coverage = std::clamp(0.5f - distance(px, py), 0.0f, 1.0f);
                        if (coverage > 0)
                            Blend(x, y, paint(px, py), coverage);
                    }
            }
            template <class Distance>
            void Fill(Distance distance, Color color)
            {
                Fill(distance, [&](float, float) { return color; });
            }

            void FillMask(const std::vector<float>& mask, Color color)
            {
                for (size_t i = 0; i < mask.size(); i++)
                    if (mask[i] > 0)
                        Blend(uint32_t(i % width), uint32_t(i / width), color, mask[i]);
            }
        };

        // Vertical gradient between y0 and y1.
        auto Gradient(Color top, Color bottom, float y0, float y1)
        {
            return [=](float, float y) { return Mix(top, bottom, (y - y0) / (y1 - y0)); };
        }

        auto Circle(float cx, float cy, float r)
        {
            return [=](float x, float y) { return std::hypot(x - cx, y - cy) - r; };
        }
        auto Ellipse(float cx, float cy, float rx, float ry)
        {
            return [=](float x, float y) { return (std::hypot((x - cx) / rx, (y - cy) / ry) - 1) * std::min(rx, ry); };
        }
        auto RoundBox(float cx, float cy, float hw, float hh, float radius)
        {
            return [=](float x, float y) {
                float qx = std::abs(x - cx) - (hw - radius), qy = std::abs(y - cy) - (hh - radius);
                return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
            };
        }
        float SegmentDistance(float x, float y, float ax, float ay, float bx, float by)
        {
            float dx = bx - ax, dy = by - ay;
            float t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
            return std::hypot(x - ax - t * dx, y - ay - t * dy);
        }
        // A closed polyline stroked `thickness` wide (round joins).
        auto Outline(std::vector<float> points, float thickness)
        {
            return [=](float x, float y) {
                float best = 1e9f;
                size_t n = points.size() / 2;
                for (size_t i = 0; i < n; i++)
                {
                    size_t j = (i + 1) % n;
                    best = std::min(best,
                        SegmentDistance(x, y, points[i * 2], points[i * 2 + 1], points[j * 2], points[j * 2 + 1]));
                }
                return best - thickness / 2;
            };
        }
        // A filled triangle (x0, y0, x1, y1, x2, y2): the distance to its edges, negative inside.
        auto Polygon(std::vector<float> p)
        {
            return [=](float x, float y) {
                float edge = 1e9f;
                int sign = 0;
                bool inside = true;
                for (size_t i = 0; i < 3; i++)
                {
                    size_t j = (i + 1) % 3;
                    edge = std::min(edge, SegmentDistance(x, y, p[i * 2], p[i * 2 + 1], p[j * 2], p[j * 2 + 1]));
                    float cross =
                        (p[j * 2] - p[i * 2]) * (y - p[i * 2 + 1]) - (p[j * 2 + 1] - p[i * 2 + 1]) * (x - p[i * 2]);
                    int s = cross > 0 ? 1 : -1;
                    if (sign == 0)
                        sign = s;
                    else if (s != sign)
                        inside = false;
                }
                return inside ? -edge : edge;
            };
        }
        auto Stroke(float ax, float ay, float bx, float by, float thickness)
        {
            return [=](float x, float y) { return SegmentDistance(x, y, ax, ay, bx, by) - thickness / 2; };
        }
        template <class A, class B>
        auto Union(A a, B b)
        {
            return [=](float x, float y) { return std::min(a(x, y), b(x, y)); };
        }
        template <class A>
        auto Grow(A a, float by)
        {
            return [=](float x, float y) { return a(x, y) - by; };
        }

        // --- Text (Bungee, SIL OFL): a coverage mask with the ink centred on (cx, cy), fitted in maxWidth x maxHeight. ---

        std::vector<uint32_t> Codepoints(const std::string& text)
        {
            std::vector<uint32_t> out;
            for (size_t i = 0; i < text.size();)
            {
                uint8_t c = uint8_t(text[i]);
                int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                uint32_t cp = extra == 0 ? c : c & (0x3F >> extra);
                for (int k = 1; k <= extra && i + k < text.size(); k++)
                    cp = cp << 6 | (uint8_t(text[i + k]) & 0x3F);
                out.push_back(cp < 128 ? uint32_t(toupper(int(cp))) : cp);
                i += extra + 1;
            }
            return out;
        }

        std::vector<float> TextMask(
            const Canvas& canvas, const std::string& text, float cx, float cy, float maxWidth, float maxHeight)
        {
            std::vector<float> mask(size_t(canvas.width) * canvas.height, 0.0f);
            std::vector<uint32_t> cps = Codepoints(text);
            if (!g_fontLoaded || cps.empty())
                return mask;

            // Ink box in font units.
            int pen = 0, x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
            std::vector<int> pens;
            for (size_t i = 0; i < cps.size(); i++)
            {
                int advance, bearing, bx0, by0, bx1, by1;
                stbtt_GetCodepointHMetrics(&g_font, int(cps[i]), &advance, &bearing);
                if (stbtt_GetCodepointBox(&g_font, int(cps[i]), &bx0, &by0, &bx1, &by1))
                {
                    x0 = std::min(x0, pen + bx0), x1 = std::max(x1, pen + bx1);
                    y0 = std::min(y0, by0), y1 = std::max(y1, by1);
                }
                pens.push_back(pen);
                pen += advance +
                       (i + 1 < cps.size() ? stbtt_GetCodepointKernAdvance(&g_font, int(cps[i]), int(cps[i + 1])) : 0);
            }
            if (x1 <= x0 || y1 <= y0)
                return mask;
            float scale = std::min(maxWidth / float(x1 - x0), maxHeight / float(y1 - y0));
            float originX = cx - scale * (x0 + x1) / 2, baseline = cy + scale * (y0 + y1) / 2;

            for (size_t i = 0; i < cps.size(); i++)
            {
                float x = originX + scale * pens[i];
                float shiftX = x - std::floor(x), shiftY = baseline - std::floor(baseline);
                int bx0, by0, bx1, by1;
                stbtt_GetCodepointBitmapBoxSubpixel(
                    &g_font, int(cps[i]), scale, scale, shiftX, shiftY, &bx0, &by0, &bx1, &by1);
                int w = bx1 - bx0, h = by1 - by0;
                if (w <= 0 || h <= 0)
                    continue;
                std::vector<uint8_t> bitmap(size_t(w) * h);
                stbtt_MakeCodepointBitmapSubpixel(
                    &g_font, bitmap.data(), w, h, w, scale, scale, shiftX, shiftY, int(cps[i]));
                int left = int(std::floor(x)) + bx0, top = int(std::floor(baseline)) + by0;
                for (int y = 0; y < h; y++)
                    for (int xx = 0; xx < w; xx++)
                    {
                        int px = left + xx, py = top + y;
                        if (px >= 0 && py >= 0 && px < int(canvas.width) && py < int(canvas.height))
                        {
                            float& m = mask[size_t(py) * canvas.width + px];
                            m = std::min(1.0f, m + bitmap[size_t(y) * w + xx] / 255.0f);
                        }
                    }
            }
            return mask;
        }

        // The mask grown by `radius` pixels (for outlines).
        std::vector<float> Dilate(const Canvas& canvas, const std::vector<float>& mask, float radius)
        {
            std::vector<float> out(mask.size(), 0.0f);
            int r = int(std::ceil(radius));
            for (int y = 0; y < int(canvas.height); y++)
                for (int x = 0; x < int(canvas.width); x++)
                {
                    float best = 0;
                    for (int dy = -r; dy <= r; dy++)
                        for (int dx = -r; dx <= r; dx++)
                        {
                            int sx = x + dx, sy = y + dy;
                            if (sx < 0 || sy < 0 || sx >= int(canvas.width) || sy >= int(canvas.height))
                                continue;
                            float weight = std::clamp(radius + 0.5f - std::hypot(float(dx), float(dy)), 0.0f, 1.0f);
                            best = std::max(best, mask[size_t(sy) * canvas.width + sx] * weight);
                        }
                    out[size_t(y) * canvas.width + x] = best;
                }
            return out;
        }

        void Text(Canvas& canvas, const std::string& text, float cx, float cy, float maxWidth, float maxHeight,
            Color color, Color outline, float outlineWidth)
        {
            std::vector<float> mask = TextMask(canvas, text, cx, cy, maxWidth, maxHeight);
            if (outlineWidth > 0)
                canvas.FillMask(Dilate(canvas, mask, outlineWidth), outline);
            canvas.FillMask(mask, color);
        }

        // --- The glyphs. Like the originals: round, a dark rim with a soft edge, a gloss on the upper half. -------------

        const Color Rim = Rgb(14, 14, 18);
        const Color Ink = Rgb(10, 10, 14, 0.85f);

        // A dark round button, `s` = its diameter, centred on (cx, cy).
        void RoundButton(Canvas& c, float cx, float cy, float s)
        {
            c.Fill(Circle(cx, cy, s * 0.5f), Rgb(0, 0, 0, 0.35f));            // soft edge, as the originals' shadow
            c.Fill(Circle(cx, cy, s * 0.475f), Rim);
            c.Fill(
                Circle(cx, cy, s * 0.43f), Gradient(Rgb(84, 86, 96), Rgb(28, 29, 34), cy - s * 0.43f, cy + s * 0.43f));
            c.Fill(Ellipse(cx, cy - s * 0.2f, s * 0.31f, s * 0.18f),
                Gradient(Rgb(255, 255, 255, 0.32f), Rgb(255, 255, 255, 0.03f), cy - s * 0.38f, cy - s * 0.02f));
        }

        // A keycap filling a box (hw, hh: half sizes) with `label`.
        void Keycap(Canvas& c, float cx, float cy, float hw, float hh, const std::string& label, float textHeight)
        {
            float s = std::min(hw, hh);
            c.Fill(RoundBox(cx, cy, hw, hh, s * 0.3f), Rgb(0, 0, 0, 0.35f));
            c.Fill(RoundBox(cx, cy, hw - s * 0.05f, hh - s * 0.05f, s * 0.26f), Rim);
            c.Fill(RoundBox(cx, cy, hw - s * 0.13f, hh - s * 0.13f, s * 0.2f),
                Gradient(Rgb(196, 198, 206), Rgb(118, 120, 130), cy - hh, cy + hh));
            float top = cy - s * 0.07f, faceW = hw - s * 0.24f, faceH = hh - s * 0.3f;
            c.Fill(RoundBox(cx, top, faceW, faceH, s * 0.15f),
                Gradient(Rgb(252, 252, 254), Rgb(212, 214, 222), top - faceH, top + faceH));
            Text(c, label, cx, top, faceW * 1.7f, std::min(textHeight, faceH * 1.5f), Rgb(34, 35, 44), {}, 0);
        }

        void PlayStationSymbol(Canvas& c, Glyph glyph, float cx, float cy, float s)
        {
            float t = s * 0.075f, k = s * 0.19f;
            auto draw = [&](auto shape, Color color) {
                c.Fill(Grow(shape, s * 0.025f), Ink);
                c.Fill(shape, color);
            };
            switch (glyph)
            {
            case Glyph::A: // cross
                draw(Union(Stroke(cx - k, cy - k, cx + k, cy + k, t), Stroke(cx - k, cy + k, cx + k, cy - k, t)),
                    Rgb(132, 182, 240));
                break;
            case Glyph::B: // circle
                draw([=](float x, float y) { return std::abs(std::hypot(x - cx, y - cy) - k * 1.05f) - t / 2; },
                    Rgb(250, 108, 110));
                break;
            case Glyph::X: // square
                draw([=](float x,
                         float y) { return std::abs(RoundBox(cx, cy, k * 0.95f, k * 0.95f, s * 0.02f)(x, y)) - t / 2; },
                    Rgb(222, 150, 214));
                break;
            default: // triangle
                draw(Outline({ cx, cy - k * 1.15f, cx + k * 1.15f, cy + k * 0.8f, cx - k * 1.15f, cy + k * 0.8f }, t),
                    Rgb(72, 222, 170));
                break;
            }
        }

        // Xbox One / Series: a dark button with the letter in its colour (A green, B red, X blue, Y yellow).
        void XboxOneLetter(Canvas& c, Glyph glyph, float cx, float cy, float s)
        {
            static const char* const Letters[] = { "A", "B", "X", "Y" };
            static const Color Colors[] = { Rgb(110, 200, 80), Rgb(240, 80, 70), Rgb(70, 140, 245), Rgb(250, 200, 50) };
            int index = int(glyph);
            Text(c, Letters[index], cx, cy, s * 0.5f, s * 0.42f, Colors[index], Ink, s * 0.03f);
        }

        // Xbox A/B/X/Y by position: A bottom, B right, X left, Y top.
        void FaceButton(Canvas& c, Glyph glyph, Style style)
        {
            float s = float(std::min(c.width, c.height)), cx = c.width / 2.0f, cy = c.height / 2.0f;
            int index = int(glyph);
            if (style == Style::Keyboard)
            {
                Keycap(c, cx, cy, s * 0.48f, s * 0.48f, g_keys[index], s * 0.36f);
                return;
            }
            RoundButton(c, cx, cy, s * 0.96f);
            if (IsPlayStation(style))
                PlayStationSymbol(c, glyph, cx, cy, s);
            else if (style == Style::XboxOne)
                XboxOneLetter(c, glyph, cx, cy, s);
            else
            {
                static const char* const Letters[] = { "B", "A", "Y",
                    "X" }; // Nintendo: south B, east A, west Y, north X
                Text(c, Letters[index], cx, cy, s * 0.5f, s * 0.4f, Rgb(244, 244, 246), Ink, s * 0.03f);
            }
        }

        // A dark pill (Start or Back on PlayStation) with a label.
        void Pill(Canvas& c, float cx, float cy, float hw, float hh, const char* label, float radius = -1)
        {
            float r = radius < 0 ? hh : radius;
            c.Fill(RoundBox(cx, cy, hw + 2, hh + 2, r + 2), Rgb(0, 0, 0, 0.35f));
            c.Fill(RoundBox(cx, cy, hw, hh, r), Rim);
            c.Fill(RoundBox(cx, cy, hw - hh * 0.12f, hh * 0.88f, r * 0.88f),
                Gradient(Rgb(84, 86, 96), Rgb(28, 29, 34), cy - hh, cy + hh));
            c.Fill(Ellipse(cx, cy - hh * 0.42f, hw * 0.75f, hh * 0.36f),
                Gradient(Rgb(255, 255, 255, 0.3f), Rgb(255, 255, 255, 0.03f), cy - hh * 0.8f, cy - hh * 0.05f));
            Text(c, label, cx, cy, hw * 1.55f, hh * 0.55f, Rgb(244, 244, 246), Ink, 1.5f);
        }

        // A shoulder button (a bumper, wide, or a trigger, tall) filling the canvas: a dark box with the style's label, or a keycap.
        void Shoulder(Canvas& c, Glyph glyph, Style style)
        {
            int index = int(glyph) - int(Glyph::LeftBumper); // LB, RB, LT, RT
            float cx = c.width * 0.5f, cy = c.height * 0.5f, hw = c.width * 0.5f - 1, hh = c.height * 0.5f - 1;
            if (style == Style::Keyboard)
            {
                Keycap(c, cx, cy, hw, hh, g_keys[6 + index], std::min(hw, hh) * 1.1f);
                return;
            }
            static const char* const Labels[4][4] = { { "LB", "RB", "LT", "RT" }, { "L1", "R1", "L2", "R2" },
                { "L", "R", "ZL", "ZR" }, { "LB", "RB", "LT", "RT" } };
            size_t row = IsPlayStation(style) ? 1 : style == Style::Nintendo ? 2 : 0;
            Pill(c, cx, cy, hw, hh, Labels[row][index], index < 2 ? hh : std::min(hw, hh) * 0.45f);
        }

        // A white symbol with a dark edge, on a round button of diameter `s`.
        template <class Shape>
        void SymbolButton(Canvas& c, float cx, float cy, float s, Shape shape)
        {
            RoundButton(c, cx, cy, s);
            c.Fill(Grow(shape, s * 0.025f), Ink);
            c.Fill(shape, Rgb(244, 244, 246));
        }

        // START (or BACK) in the box (hw, hh: half sizes) centred in the canvas. SSA's START texture is a white pill
        // (x 2-125, y 23-105 of 128x128) with a play arrow; Giants' atlas cells are pills too, ▷ for START and ◁ for BACK.
        void MenuButton(Canvas& c, Style style, bool start, float hw, float hh)
        {
            float cx = c.width * 0.5f, cy = c.height * 0.5f, s = std::min(hh * 2, hw * 2);
            switch (style)
            {
            case Style::Keyboard: Keycap(c, cx, cy, hw, hh, g_keys[start ? 4 : 5], hh * 0.7f); break;
            case Style::PlayStation3: Pill(c, cx, cy, hw, hh, start ? "START" : "SELECT"); break;
            case Style::PlayStation:
                if (start)
                    Pill(c, cx, cy, hw, hh, "OPTIONS");
                else // the touchpad (BACK is the touchpad's click, or Share / Create): a wide dark pad with a soft gloss
                {
                    float pw = std::min(hw, hh * 1.9f), ph = std::min(hh, pw * 0.55f), r = ph * 0.35f;
                    c.Fill(RoundBox(cx, cy, pw + 2, ph + 2, r + 2), Rgb(0, 0, 0, 0.35f));
                    c.Fill(RoundBox(cx, cy, pw, ph, r), Rim);
                    c.Fill(RoundBox(cx, cy, pw - ph * 0.14f, ph * 0.86f, r * 0.86f),
                        Gradient(Rgb(84, 86, 96), Rgb(28, 29, 34), cy - ph, cy + ph));
                    c.Fill(Ellipse(cx, cy - ph * 0.4f, pw * 0.8f, ph * 0.3f),
                        Gradient(
                            Rgb(255, 255, 255, 0.28f), Rgb(255, 255, 255, 0.03f), cy - ph * 0.75f, cy - ph * 0.1f));
                }
                break;
            case Style::
                Xbox360: // a silver round button with a play arrow (START) or a back arrow (BACK), as on the pad
            {
                float k = s * 0.17f;
                c.Fill(Circle(cx, cy, s * 0.5f), Rgb(0, 0, 0, 0.35f));
                c.Fill(Circle(cx, cy, s * 0.475f), Rgb(70, 72, 78));
                c.Fill(Circle(cx, cy, s * 0.43f),
                    Gradient(Rgb(236, 238, 242), Rgb(160, 164, 172), cy - s * 0.43f, cy + s * 0.43f));
                float d = start ? 1.0f : -1.0f;
                c.Fill(Polygon({ cx - d * k * 0.8f, cy - k, cx + d * k * 1.1f, cy, cx - d * k * 0.8f, cy + k }),
                    Rgb(30, 31, 36));
                break;
            }
            case Style::XboxOne:
            {
                float k = s * 0.2f, t = s * 0.075f, gap = s * 0.14f;
                if (start) // Menu: three lines
                    SymbolButton(c, cx, cy, s,
                        Union(Stroke(cx - k, cy - gap, cx + k, cy - gap, t),
                            Union(Stroke(cx - k, cy, cx + k, cy, t), Stroke(cx - k, cy + gap, cx + k, cy + gap, t))));
                else // View: two overlapping windows
                {
                    float w = s * 0.13f, o = s * 0.07f;
                    SymbolButton(c, cx, cy, s,
                        Union(
                            [=](float x, float y) {
                                return std::abs(RoundBox(cx - o, cy - o, w, w * 0.8f, s * 0.02f)(x, y)) - t / 2;
                            },
                            RoundBox(cx + o, cy + o, w, w * 0.8f, s * 0.02f)));
                }
                break;
            }
            default: // Nintendo: plus and minus
            {
                float k = s * 0.24f, t = s * 0.13f;
                if (start)
                    SymbolButton(
                        c, cx, cy, s, Union(Stroke(cx - k, cy, cx + k, cy, t), Stroke(cx, cy - k, cx, cy + k, t)));
                else
                    SymbolButton(c, cx, cy, s, Stroke(cx - k, cy, cx + k, cy, t));
                break;
            }
            }
        }

        // The four keys of a stick (up, down, left, right in the bindings) as one label: "WASD" when each is one character.
        std::string StickKeys(size_t first, const char* otherwise)
        {
            const std::string &up = g_keys[first], &down = g_keys[first + 1], &left = g_keys[first + 2],
                              &right = g_keys[first + 3];
            if (up.size() == 1 && down.size() == 1 && left.size() == 1 && right.size() == 1)
                return up + left + down + right;
            return otherwise;
        }

        // A small stick picture of a font atlas (Giants: a dark stick top with "LS" / "RS"): a round button with the label.
        void StickIcon(Canvas& c, Style style, bool left)
        {
            float s = float(std::min(c.width, c.height)), cx = c.width / 2.0f, cy = c.height / 2.0f;
            if (style == Style::Keyboard)
            {
                Keycap(
                    c, cx, cy, c.width * 0.48f, s * 0.36f, StickKeys(left ? 10 : 14, left ? "MOVE" : "CAM"), s * 0.3f);
                return;
            }
            RoundButton(c, cx, cy, s * 0.96f);
            const char* label = IsPlayStation(style) ? (left ? "L3" : "R3") : (left ? "LS" : "RS");
            Text(c, label, cx, cy, s * 0.6f, s * 0.36f, Rgb(244, 244, 246), Ink, s * 0.03f);
        }

        // The right stick on a 3D prompt: 256x256, opaque, the stick's top seen from above centred near (106, 104).
        void RightStick(Canvas& c, Style style)
        {
            float k = c.width / 256.0f, cx = 106 * k, cy = 104 * k, r = 94 * k;
            c.Fill([](float, float) { return -1.0f; }, Rgb(44, 44, 46));
            c.Fill(Circle(cx, cy, r), Rgb(24, 24, 26));
            c.Fill(Circle(cx, cy, r * 0.94f), Gradient(Rgb(96, 97, 100), Rgb(50, 50, 54), cy - r, cy + r));
            c.Fill([=](float x, float y) { return std::abs(std::hypot(x - cx, y - cy) - r * 0.8f) - r * 0.03f; },
                Rgb(0, 0, 0, 0.25f));
            if (style == Style::Keyboard)
            {
                // The camera keys as an inverted T: up above left, down, right.
                float key = r * 0.42f, gap = r * 0.04f;
                float row = cy + key * 0.5f + gap / 2;
                Keycap(c, cx, row - key - gap, key / 2, key / 2, g_keys[14], key * 0.4f);
                Keycap(c, cx - key - gap, row, key / 2, key / 2, g_keys[16], key * 0.4f);
                Keycap(c, cx, row, key / 2, key / 2, g_keys[15], key * 0.4f);
                Keycap(c, cx + key + gap, row, key / 2, key / 2, g_keys[17], key * 0.4f);
                return;
            }
            const char* label = IsPlayStation(style) ? "R3" : "RS";
            Text(c, label, cx, cy, r * 1.1f, r * 0.62f, Rgb(250, 248, 250), {}, 0);
        }

        // RGBA8 levels (straight alpha) of `glyph` drawn for `style`.
        // A small key of an arrow cluster: its bound key when that is one character, else an arrow (dx, dy) on the cap.
        void ArrowKey(Canvas& c, float cx, float cy, float h, const std::string& key, float dx, float dy)
        {
            if (key.size() == 1)
            {
                Keycap(c, cx, cy, h, h, key, h * 0.8f);
                return;
            }
            Keycap(c, cx, cy, h, h, "", h);
            float k = h * 0.32f, top = cy - h * 0.07f, px = -dy, py = dx; // the arrow's tip along (dx, dy)
            c.Fill(Polygon({ cx + dx * k, top + dy * k, cx - dx * k + px * k, top - dy * k + py * k,
                       cx - dx * k - px * k, top - dy * k - py * k }),
                Rgb(34, 35, 44));
        }

        // Four keys as an inverted T (up above left, down, right), centred on (cx, cy); `first`: the up key's binding.
        void ArrowCluster(Canvas& c, float cx, float cy, float key, size_t first)
        {
            float h = key / 2, gap = key * 0.08f, row = cy + h + gap / 2;
            ArrowKey(c, cx, row - key - gap, h, g_keys[first], 0, -1);
            ArrowKey(c, cx - key - gap, row, h, g_keys[first + 2], -1, 0);
            ArrowKey(c, cx, row, h, g_keys[first + 1], 0, 1);
            ArrowKey(c, cx + key + gap, row, h, g_keys[first + 3], 1, 0);
        }

        // SSA's Controls page pad (360_controller, 256x256) on the keyboard: a plain pad shape with a keycap of the bound key
        // where each button is, so that the page's leader lines (another texture) point at keycaps. The places are the
        // original's (centres measured on the disc's texture, in 256ths).
        void ControllerKeys(Canvas& c)
        {
            float k = c.width / 256.0f;
            auto at = [&](float v) { return v * k; };
            Color body = Rgb(70, 72, 80, 0.92f);
            c.Fill(Union(RoundBox(at(128), at(112), at(108), at(56), at(44)),
                       Union(Circle(at(66), at(160), at(44)), Circle(at(190), at(160), at(44)))),
                body);
            auto cap = [&](float x, float y, float hw, float hh, const std::string& label) {
                Keycap(c, at(x), at(y), at(hw), at(hh), label, at(hh) * 1.1f);
            };
            cap(62.5f, 57, 15, 8, g_keys[6]); // LB
            cap(193.5f, 57, 15, 8, g_keys[7]); // RB
            cap(62.5f, 91.5f, 24, 13, StickKeys(10, "MOVE"));
            cap(159.5f, 131.5f, 24, 13, StickKeys(14, "CAM"));
            cap(105.5f, 94, 11, 9, g_keys[5]); // Back
            cap(151, 94, 11, 9, g_keys[4]); // Start
            cap(192.5f, 108.5f, 10, 10, g_keys[0]); // A
            cap(209.5f, 91.5f, 10, 10, g_keys[1]); // B
            cap(175, 92, 10, 10, g_keys[2]); // X
            cap(192.5f, 75.5f, 10, 10, g_keys[3]); // Y
            ArrowCluster(c, at(94), at(130.5f), at(13), 18); // the d-pad
        }

        // SSA's blue arrows (arrow_rl 64x32, arrow_ud 32x32) on the keyboard: the left stick's keys, side by side or stacked.
        void ArrowKeys(Canvas& c, bool leftRight)
        {
            float w = float(c.width), h = float(c.height);
            if (leftRight)
            {
                float half = std::min(w / 4, h / 2) - 1;
                ArrowKey(c, w * 0.25f, h / 2, half, g_keys[12], -1, 0);
                ArrowKey(c, w * 0.75f, h / 2, half, g_keys[13], 1, 0);
            }
            else
            {
                float half = std::min(w / 2, h / 4) - 1;
                ArrowKey(c, w / 2, h * 0.25f, half, g_keys[10], 0, -1);
                ArrowKey(c, w / 2, h * 0.75f, half, g_keys[11], 0, 1);
            }
        }

        // Paints `glyph` for `style` into the canvas: a whole texture (SSA's layouts) or an atlas cell (its bounding box).
        void Paint(Canvas& canvas, Glyph glyph, Style style, bool cell)
        {
            if (glyph == Glyph::Start || glyph == Glyph::Back)
            {
                if (cell)
                    MenuButton(canvas, style, glyph == Glyph::Start, canvas.width * 0.5f - 1, canvas.height * 0.5f - 1);
                else
                    MenuButton(canvas, style, glyph == Glyph::Start, canvas.width * 0.48f, canvas.height * 0.32f);
            }
            else if (glyph == Glyph::Controller)
                ControllerKeys(canvas);
            else if (glyph == Glyph::ArrowsLeftRight || glyph == Glyph::ArrowsUpDown)
                ArrowKeys(canvas, glyph == Glyph::ArrowsLeftRight);
            else if (glyph == Glyph::RightStick)
                RightStick(canvas, style);
            else if (glyph == Glyph::LeftStickIcon || glyph == Glyph::RightStickIcon)
                StickIcon(canvas, style, glyph == Glyph::LeftStickIcon);
            else if (glyph >= Glyph::LeftBumper && glyph <= Glyph::RightTrigger)
                Shoulder(canvas, glyph, style);
            else
                FaceButton(canvas, glyph, style);
        }

        // RGBA8 levels (straight alpha) of the canvas, each a box filter of the one above.
        std::vector<std::vector<uint32_t>> Levels(Canvas canvas, uint32_t levels)
        {
            std::vector<std::vector<uint32_t>> out;
            for (uint32_t level = 0; level < levels; level++)
            {
                if (level > 0)
                {
                    // Box filter of the premultiplied level above.
                    Canvas smaller(std::max(canvas.width / 2, 1u), std::max(canvas.height / 2, 1u));
                    for (uint32_t y = 0; y < smaller.height; y++)
                        for (uint32_t x = 0; x < smaller.width; x++)
                            for (int i = 0; i < 4; i++)
                            {
                                float sum = 0;
                                for (uint32_t dy = 0; dy < 2; dy++)
                                    for (uint32_t dx = 0; dx < 2; dx++)
                                    {
                                        uint32_t sx = std::min(x * 2 + dx, canvas.width - 1),
                                                 sy = std::min(y * 2 + dy, canvas.height - 1);
                                        sum += canvas.pixels[(size_t(sy) * canvas.width + sx) * 4 + i];
                                    }
                                smaller.pixels[(size_t(y) * smaller.width + x) * 4 + i] = sum / 4;
                            }
                    canvas = std::move(smaller);
                }
                std::vector<uint32_t> rgba(size_t(canvas.width) * canvas.height);
                for (size_t i = 0; i < rgba.size(); i++)
                {
                    const float* p = &canvas.pixels[i * 4];
                    float a = p[3];
                    auto channel = [&](float v) {
                        return uint32_t(std::lround(std::clamp(a > 0 ? v / a : 0.0f, 0.0f, 1.0f) * 255));
                    };
                    rgba[i] = channel(p[0]) | channel(p[1]) << 8 | channel(p[2]) << 16 |
                              uint32_t(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)) << 24;
                }
                out.push_back(std::move(rgba));
            }
            return out;
        }

        // RGBA8 levels of a whole-texture glyph.
        std::vector<std::vector<uint32_t>> Draw(
            Glyph glyph, Style style, uint32_t width, uint32_t height, uint32_t levels, bool flipped)
        {
            Canvas canvas(width, height);
            Paint(canvas, glyph, style, false);
            if (flipped)
                for (uint32_t y = 0; y < height / 2; y++)
                    std::swap_ranges(&canvas.pixels[size_t(y) * width * 4], &canvas.pixels[size_t(y + 1) * width * 4],
                        &canvas.pixels[size_t(height - 1 - y) * width * 4]);
            return Levels(std::move(canvas), levels);
        }

        // RGBA8 levels of an atlas: the guest's base level with each cell (and a pixel around it, the original's soft edge)
        // cleared and redrawn.
        std::vector<std::vector<uint32_t>> DrawAtlas(const Texture& atlas, Style style,
            const std::vector<uint32_t>& base, uint32_t width, uint32_t height, uint32_t levels)
        {
            Canvas canvas(width, height);
            for (size_t i = 0; i < base.size(); i++)
            {
                float a = float(base[i] >> 24) / 255.0f;
                for (int c = 0; c < 3; c++)
                    canvas.pixels[i * 4 + c] = float((base[i] >> (c * 8)) & 0xFF) / 255.0f * a;
                canvas.pixels[i * 4 + 3] = a;
            }
            for (const Cell& cell : atlas.cells)
            {
                if (cell.x1 >= width || cell.y1 >= height)
                    continue;
                for (uint32_t y = cell.y0 > 0 ? cell.y0 - 1u : 0u; y <= std::min<uint32_t>(cell.y1 + 1u, height - 1);
                    y++)
                    for (uint32_t x = cell.x0 > 0 ? cell.x0 - 1u : 0u; x <= std::min<uint32_t>(cell.x1 + 1u, width - 1);
                        x++)
                        std::fill_n(&canvas.pixels[(size_t(y) * width + x) * 4], 4, 0.0f);
                Canvas glyph(cell.x1 - cell.x0 + 1u, cell.y1 - cell.y0 + 1u);
                Paint(glyph, cell.glyph, style, true);
                for (uint32_t y = 0; y < glyph.height; y++)
                    for (uint32_t x = 0; x < glyph.width; x++)
                        std::copy_n(&glyph.pixels[(size_t(y) * glyph.width + x) * 4], 4,
                            &canvas.pixels[(size_t(cell.y0 + y) * width + cell.x0 + x) * 4]);
            }
            std::vector<std::vector<uint32_t>> out = Levels(std::move(canvas), levels);
            // Outside the cells every level stays the game's, its colour under alpha 0 too: Trap Team's hints take their
            // white from this atlas, and the canvas is premultiplied.
            std::vector<uint32_t> plain = base;
            for (uint32_t level = 0; level < levels; level++)
            {
                uint32_t w = std::max(width >> level, 1u), h = std::max(height >> level, 1u);
                if (level > 0)
                {
                    uint32_t aboveWidth = std::max(width >> (level - 1), 1u),
                             aboveHeight = std::max(height >> (level - 1), 1u);
                    std::vector<uint32_t> smaller(size_t(w) * h);
                    for (uint32_t y = 0; y < h; y++)
                        for (uint32_t x = 0; x < w; x++)
                        {
                            uint32_t sum[4] = {};
                            for (uint32_t dy = 0; dy < 2; dy++)
                                for (uint32_t dx = 0; dx < 2; dx++)
                                {
                                    uint32_t texel = plain[size_t(std::min(y * 2 + dy, aboveHeight - 1)) * aboveWidth +
                                                           std::min(x * 2 + dx, aboveWidth - 1)];
                                    for (int c = 0; c < 4; c++)
                                        sum[c] += (texel >> (c * 8)) & 0xFF;
                                }
                            for (int c = 0; c < 4; c++)
                                smaller[size_t(y) * w + x] |= (sum[c] / 4) << (c * 8);
                        }
                    plain = std::move(smaller);
                }
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++)
                    {
                        bool touched = false;
                        for (const Cell& cell : atlas.cells)
                            touched = touched || (((x + 1) << level) > cell.x0 && (x << level) <= cell.x1 + 1u &&
                                                     ((y + 1) << level) > cell.y0 && (y << level) <= cell.y1 + 1u);
                        if (!touched)
                            out[level][size_t(y) * w + x] = plain[size_t(y) * w + x];
                    }
            }
            return out;
        }

        // What a keycap shows for a bound key: its name on the current keyboard layout, shortened.
        std::string KeyLabel(const std::string& settingName)
        {
            SDL_Scancode scancode = SDL_GetScancodeFromName(settingName.c_str());
            const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false));
            std::string label = name != nullptr && *name != '\0' ? name : settingName;
            static const std::pair<const char*, const char*> Short[] = {
                { "Return", "Enter" },
                { "Backspace", "Bksp" },
                { "Escape", "Esc" },
                { "Left Shift", "Shift" },
                { "Right Shift", "Shift" },
                { "Left Ctrl", "Ctrl" },
                { "Right Ctrl", "Ctrl" },
                { "Left Alt", "Alt" },
                { "Right Alt", "Alt" },
                { "Left GUI", "Cmd" },
                { "Right GUI", "Cmd" },
                { "CapsLock", "Caps" },
            };
            for (const auto& [from, to] : Short)
                if (label == from)
                    return to;
            if (label.starts_with("Keypad "))
                return "Num" + label.substr(7);
            return label;
        }

        void Select(Style style)
        {
            if (g_style.exchange(style) != style)
                fprintf(stderr, "[glyphs] %s\n", StyleName(style));
        }

        Style GamepadStyle(SDL_Gamepad* gamepad)
        {
            SDL_GamepadType type = SDL_GetGamepadType(gamepad);
            if (type == SDL_GAMEPAD_TYPE_XBOXONE)
                return Style::XboxOne;
            if (g_xboxOnly)
                return Style::Xbox360;
            switch (type)
            {
            case SDL_GAMEPAD_TYPE_PS3: return Style::PlayStation3;
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS5: return Style::PlayStation;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Style::Nintendo;
            default: // the Xbox 360 pad and anything else: the game's own glyphs
                return Style::Xbox360;
            }
        }

        // --- Pictures the game copies on the CPU (MemoryImage): found in guest memory, redrawn in place. -------------------

        constexpr uint32_t PrefixBytes = 4096;

        // 32-bit tiled ARGB (big-endian bytes A, R, G, B) <-> RGBA8 with R in the low byte.
        std::vector<uint32_t> Untile32(const uint8_t* source, uint32_t width, uint32_t height)
        {
            std::vector<uint32_t> rgba(size_t(width) * height);
            uint32_t pitch = (width + 31) & ~31u;
            for (uint32_t y = 0; y < height; y++)
                for (uint32_t x = 0; x < width; x++)
                {
                    const uint8_t* p = source + d3d::xenos::Tiled2D(x, y, pitch, 2);
                    rgba[size_t(y) * width + x] = p[1] | p[2] << 8 | p[3] << 16 | uint32_t(p[0]) << 24;
                }
            return rgba;
        }

        void Tile32(uint8_t* destination, uint32_t width, uint32_t x, uint32_t y, uint32_t rgba)
        {
            uint8_t* p = destination + d3d::xenos::Tiled2D(x, y, (width + 31) & ~31u, 2);
            p[0] = uint8_t(rgba >> 24), p[1] = uint8_t(rgba), p[2] = uint8_t(rgba >> 8), p[3] = uint8_t(rgba >> 16);
        }

        struct Placed
        {
            const MemoryImage* image;
            uint32_t address = 0;
            std::vector<uint8_t> original; // the game's bytes
            std::vector<uint32_t> originalRgba;
            uint64_t seen = 0; // the prefix hash of what is in memory (ours or the game's)
            Style applied = Style::Xbox360;
            std::vector<Cell> added; // cells we add where the game's image is empty (drawn in every style)
        };

        uint32_t FindImage(const MemoryImage& image)
        {
            uint32_t found = 0;
            for (GuestHeap* heap : { &g_virtualHeap4K, &g_virtualHeap64K, &g_physicalHeap })
                heap->ForEachAllocation([&](uint32_t address, uint32_t size) {
                    uint32_t base =
                        heap == &g_physicalHeap ? PhysicalToGuest(address) : address; // physical: through its window
                    for (uint32_t offset = 0; found == 0 && base != 0 && offset + image.bytes <= size; offset += 0x1000)
                        if (XXH3_64bits(g_memory.Translate(base + offset), PrefixBytes) == image.prefixHash)
                            found = base + offset;
                });
            return found;
        }

        void ApplyImage(Placed& placed, Style style)
        {
            const MemoryImage& image = *placed.image;
            auto* memory = static_cast<uint8_t*>(g_memory.Translate(placed.address));
            memcpy(memory, placed.original.data(), image.bytes);
            // The Xbox 360 style keeps the game's bytes but for the cells we add (their 360 look is ours too).
            std::span<const Cell> cells = style == Style::Xbox360 ? std::span<const Cell>(placed.added) : image.cells;
            if (g_fontLoaded && !cells.empty())
            {
                Texture atlas = { 0, image.name, Glyph::A, cells };
                std::vector<uint32_t> drawn =
                    DrawAtlas(atlas, style, placed.originalRgba, image.width, image.height, 1)[0];
                for (const Cell& cell : cells)
                    for (uint32_t y = cell.y0 > 0 ? cell.y0 - 1u : 0u;
                        y <= std::min<uint32_t>(cell.y1 + 1u, image.height - 1u); y++)
                        for (uint32_t x = cell.x0 > 0 ? cell.x0 - 1u : 0u;
                            x <= std::min<uint32_t>(cell.x1 + 1u, image.width - 1u); x++)
                            Tile32(memory, image.width, x, y, drawn[size_t(y) * image.width + x]);
            }
            placed.seen = XXH3_64bits(memory, PrefixBytes);
            placed.applied = style;
            fprintf(stderr, "[glyphs] %s (guest memory) drawn as %s\n", image.name, StyleName(style));
        }

        // Until every image is found (the font loads at the game's start-up, or with the language), then on each style change.
        void WatchImages()
        {
            std::vector<Placed> placed;
            for (const MemoryImage& image : game::Current().glyphImages)
                placed.push_back({ &image });
            for (;;)
            {
                bool missing = false;
                for (Placed& one : placed)
                {
                    const MemoryImage& image = *one.image;
                    if (one.address != 0 && XXH3_64bits(g_memory.Translate(one.address), PrefixBytes) != one.seen)
                    {
                        fprintf(stderr, "[glyphs] %s: its memory changed, looking for it again\n", image.name);
                        one.address = 0;
                    }
                    if (one.address == 0)
                    {
                        one.address = FindImage(image);
                        if (one.address == 0)
                        {
                            missing = true;
                            continue;
                        }
                        auto* memory = static_cast<const uint8_t*>(g_memory.Translate(one.address));
                        one.original.assign(memory, memory + image.bytes);
                        one.originalRgba = Untile32(memory, image.width, image.height);
                        one.added.clear();
                        for (const Cell& cell : image.cells)
                        {
                            bool empty = true;
                            for (uint32_t y = cell.y0; y <= cell.y1 && empty; y++)
                                for (uint32_t x = cell.x0; x <= cell.x1 && empty; x++)
                                    empty = (one.originalRgba[size_t(y) * image.width + x] >> 24) == 0;
                            if (empty)
                                one.added.push_back(cell);
                        }
                        one.seen = XXH3_64bits(memory, PrefixBytes);
                        one.applied = one.added.empty() ? Style::Xbox360 : Style(0xFF);
                        fprintf(stderr, "[glyphs] %s found in guest memory at %08X\n", image.name, one.address);
                    }
                    if (one.applied != Current())
                        ApplyImage(one, Current());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(missing ? 2000 : 250));
            }
        }
    }

    namespace
    {
        // Test aid (SKY_GLYPHS_SHEET=<file.png>): every glyph in every style, one row per style (Xbox 360: only what we draw
        // for it, the added Start), 96x96 cells, written once at start-up with the key bindings in use.
        void WriteSheet(const char* path)
        {
            constexpr uint32_t Cell = 96;
            constexpr Glyph Glyphs[] = { Glyph::A, Glyph::B, Glyph::X, Glyph::Y, Glyph::Start, Glyph::Back,
                Glyph::LeftBumper, Glyph::RightBumper, Glyph::LeftTrigger, Glyph::RightTrigger, Glyph::LeftStickIcon,
                Glyph::RightStickIcon, Glyph::RightStick, Glyph::ArrowsLeftRight, Glyph::ArrowsUpDown,
                Glyph::Controller };
            constexpr Style Rows[] = { Style::Xbox360, Style::XboxOne, Style::PlayStation, Style::PlayStation3,
                Style::Nintendo, Style::Keyboard };
            uint32_t width = Cell * uint32_t(std::size(Glyphs)), height = Cell * uint32_t(std::size(Rows));
            std::vector<uint32_t> sheet(size_t(width) * height, 0xFF303030);
            for (size_t row = 0; row < std::size(Rows); row++)
                for (size_t column = 0; column < std::size(Glyphs); column++)
                {
                    Glyph glyph = Glyphs[column];
                    Style style = Rows[row];
                    bool keyboardOnly = glyph >= Glyph::Controller;
                    if ((style == Style::Xbox360 && glyph != Glyph::Start && glyph != Glyph::Back) ||
                        (keyboardOnly && style != Style::Keyboard))
                        continue;
                    bool cell = glyph == Glyph::Start || glyph == Glyph::Back; // the square cell of a font image
                    uint32_t h = glyph == Glyph::ArrowsLeftRight ? Cell / 2 : Cell;
                    Canvas canvas(Cell, h);
                    Paint(canvas, glyph, style, cell);
                    std::vector<uint32_t> rgba = Levels(std::move(canvas), 1)[0];
                    for (uint32_t y = 0; y < h; y++)
                        for (uint32_t x = 0; x < Cell; x++)
                        {
                            uint32_t c = rgba[size_t(y) * Cell + x], a = c >> 24;
                            uint32_t& d = sheet[(row * Cell + y) * width + column * Cell + x];
                            uint32_t out = 0xFF000000;
                            for (int k = 0; k < 3; k++)
                                out |= (((c >> (k * 8)) & 0xFF) * a + ((d >> (k * 8)) & 0xFF) * (255 - a)) / 255
                                       << (k * 8);
                            d = out;
                        }
                }
            if (stbi_write_png(path, int(width), int(height), 4, sheet.data(), int(width * 4)))
                fprintf(stderr, "[glyphs] sheet written: %s\n", path);
        }
    }

    void Init(const RuntimeSettings& settings)
    {
        g_fontLoaded = stbtt_InitFont(&g_font, g_embeddedBungee, stbtt_GetFontOffsetForIndex(g_embeddedBungee, 0)) != 0;
        if (!g_fontLoaded)
            fprintf(stderr, "[glyphs] the embedded Bungee font does not load: the Xbox 360 glyphs stay\n");
        for (size_t i = 0; i < std::size(g_keyActions); i++)
            g_keys[i] = KeyLabel(settings.keys[i]);
        fprintf(stderr, "[glyphs] keycaps: a=%s b=%s x=%s y=%s start=%s back=%s\n", g_keys[0].c_str(),
            g_keys[1].c_str(), g_keys[2].c_str(), g_keys[3].c_str(), g_keys[4].c_str(), g_keys[5].c_str());
        g_keyboard = settings.inputDevice != "gamepad";
        g_gamepad = settings.inputDevice != "keyboard";
        g_xboxOnly = settings.glyphs == "xbox";
        g_auto = settings.glyphs == "auto" || g_xboxOnly;
        // auto: until a gamepad is opened or used
        Style style = g_keyboard && !g_xboxOnly ? Style::Keyboard : Style::Xbox360;
        for (Style named : { Style::Xbox360, Style::XboxOne, Style::PlayStation, Style::Nintendo, Style::Keyboard,
                 Style::PlayStation3 })
            if (settings.glyphs == StyleName(named))
                style = named;
        Select(style);
        if (const char* sheet = skyenv::Get("SKY_GLYPHS_SHEET"))
            WriteSheet(sheet);
        if (!game::Current().glyphImages.empty())
            std::thread(WatchImages).detach();
    }

    void OnEvent(const SDL_Event& event, SDL_Gamepad* gamepad)
    {
        if (!g_auto)
            return;
        bool ours = gamepad != nullptr && g_gamepad;
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
            if (g_keyboard && !g_xboxOnly && !event.key.repeat)
            {
                g_used = true;
                Select(Style::Keyboard);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (ours && !g_used && SDL_GetGamepadID(gamepad) == event.gdevice.which)
                Select(GamepadStyle(gamepad));
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (ours && SDL_GetGamepadID(gamepad) == event.gbutton.which)
            {
                g_used = true;
                Select(GamepadStyle(gamepad));
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (ours && SDL_GetGamepadID(gamepad) == event.gaxis.which && std::abs(int(event.gaxis.value)) > 16000)
            {
                g_used = true;
                Select(GamepadStyle(gamepad));
            }
            break;
        default: break;
        }
    }

    void RunTestScript()
    {
        struct Step
        {
            double at;
            std::string action;
        };
        static std::vector<Step> steps;
        static size_t next = 0;
        static SDL_JoystickID virtualId = 0;
        static SDL_Joystick* virtualPad = nullptr;
        static double pressAt = -1, releaseAt = -1;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            if (const char* script = skyenv::Get("SKY_GLYPHS_SCRIPT"))
                for (std::string text = script; !text.empty();)
                {
                    size_t comma = std::min(text.find(','), text.size());
                    std::string item = text.substr(0, comma);
                    size_t colon = item.rfind(':');
                    if (colon != std::string::npos)
                        steps.push_back({ atof(item.c_str() + colon + 1), item.substr(0, colon) });
                    text.erase(0, std::min(comma + 1, text.size()));
                }
        }
        if (steps.empty())
            return;
        double now = SDL_GetTicksNS() / 1e9;
        if (virtualPad != nullptr && pressAt >= 0 && now >= pressAt)
        {
            SDL_SetJoystickVirtualButton(virtualPad, SDL_GAMEPAD_BUTTON_GUIDE, true);
            pressAt = -1;
            releaseAt = now + 0.15;
        }
        if (virtualPad != nullptr && releaseAt >= 0 && now >= releaseAt)
        {
            SDL_SetJoystickVirtualButton(virtualPad, SDL_GAMEPAD_BUTTON_GUIDE, false);
            releaseAt = -1;
        }
        while (next < steps.size() && steps[next].at <= now)
        {
            const std::string& action = steps[next++].action;
            fprintf(stderr, "[glyphs] script: %s\n", action.c_str());
            // btn:<name> / btnup:<name>: hold / release a button of the virtual pad (a b x y lb rb lt rt start back ls rs dpad_*),
            // for the pad remapping test (docs/runtime/xbox360/input.md). vibrate / vibrate0: the title asks Player 1's pad to rumble.
            if (action == "vibrate" || action == "vibrate0")
            {
                SetHostVibration(0, action == "vibrate" ? 40000 : 0, action == "vibrate" ? 20000 : 0);
                continue;
            }
            if (action.starts_with("btn:") || action.starts_with("btnup:"))
            {
                const bool down = action.starts_with("btn:");
                const std::string name = action.substr(action.find(':') + 1);
                static const std::pair<const char*, SDL_GamepadButton> Buttons[] = {
                    { "a", SDL_GAMEPAD_BUTTON_SOUTH },
                    { "b", SDL_GAMEPAD_BUTTON_EAST },
                    { "x", SDL_GAMEPAD_BUTTON_WEST },
                    { "y", SDL_GAMEPAD_BUTTON_NORTH },
                    { "lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
                    { "rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
                    { "start", SDL_GAMEPAD_BUTTON_START },
                    { "back", SDL_GAMEPAD_BUTTON_BACK },
                    { "ls", SDL_GAMEPAD_BUTTON_LEFT_STICK },
                    { "rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK },
                    { "dpad_up", SDL_GAMEPAD_BUTTON_DPAD_UP },
                    { "dpad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
                    { "dpad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT },
                    { "dpad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
                };
                if (virtualPad != nullptr)
                {
                    if (name == "lt" || name == "rt")
                        SDL_SetJoystickVirtualAxis(virtualPad,
                            name == "lt" ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                            down ? 32767 : -32768); // the triggers' rest is the axis minimum
                    for (const auto& [text, button] : Buttons)
                        if (name == text)
                            SDL_SetJoystickVirtualButton(virtualPad, button, down);
                }
                continue;
            }
            if (action == "key")
            {
                for (bool down : { true, false })
                {
                    SDL_Event event{};
                    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    event.key.key = SDLK_F12;
                    event.key.scancode = SDL_SCANCODE_F12;
                    event.key.down = down;
                    SDL_PushEvent(&event);
                }
                continue;
            }
            static const struct
            {
                const char* name;
                uint16_t vendor, product;
            } Pads[] = {
                { "ps5", 0x054C, 0x0CE6 },
                { "ps4", 0x054C, 0x09CC },
                { "ps3", 0x054C, 0x0268 },
                { "switch", 0x057E, 0x2009 },
                { "xboxone", 0x045E, 0x0B12 },
                { "xbox360", 0x045E, 0x028E },
            };
            for (const auto& pad : Pads)
            {
                if (action != pad.name)
                    continue;
                if (virtualPad != nullptr)
                {
                    SDL_CloseJoystick(virtualPad);
                    SDL_DetachVirtualJoystick(virtualId);
                    virtualPad = nullptr;
                }
                SDL_VirtualJoystickDesc desc;
                SDL_INIT_INTERFACE(&desc);
                desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                desc.vendor_id = pad.vendor;
                desc.product_id = pad.product;
                desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
                desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
                desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
                desc.name = pad.name;
                desc.Rumble = [](void*, Uint16 low, Uint16 high) -> bool {
                    fprintf(stderr, "[input] virtual pad rumbles: %u/%u\n", unsigned(low), unsigned(high));
                    return true;
                };
                virtualId = SDL_AttachVirtualJoystick(&desc);
                virtualPad = virtualId != 0 ? SDL_OpenJoystick(virtualId) : nullptr;
                if (virtualPad == nullptr)
                    fprintf(stderr, "[glyphs] script: no virtual gamepad (%s)\n", SDL_GetError());
                pressAt = now + 0.5;
            }
        }
    }

    Style Current() { return g_style.load(std::memory_order_relaxed); }

    bool Replace(uint64_t guestHash, uint32_t width, uint32_t height, uint32_t levels,
        const std::function<std::vector<uint32_t>()>& guestBase, gpu::Texture*& texture, uint64_t& hostHash)
    {
        const Texture* known = Find(guestHash);
        Style style = Current();
        bool keyboardOnly = known != nullptr && known->glyph >= Glyph::Controller;
        bool draw =
            known != nullptr && style != Style::Xbox360 && g_fontLoaded && (!keyboardOnly || style == Style::Keyboard);
        uint64_t wanted = draw ? Salted(guestHash, style) : guestHash;
        if (texture != nullptr && hostHash == wanted)
            return draw;

        // Our RGBA8 texture is not the guest's format: drop it before anything else is uploaded in its place.
        if (texture != nullptr)
            for (Style other : DrawnStyles)
                if (Find(Salted(hostHash, other)) != nullptr)
                {
                    gpu::DestroyTexture(texture);
                    texture = nullptr;
                    hostHash = 0;
                    break;
                }
        if (!draw)
            return false;

        std::vector<std::vector<uint32_t>> image;
        if (known->cells.empty())
            image = Draw(known->glyph, style, width, height, levels, known->flipped);
        else
        {
            std::vector<uint32_t> base = guestBase();
            if (base.size() != size_t(width) * height)
            {
                static std::atomic<bool> warned;
                if (!warned.exchange(true))
                    fprintf(
                        stderr, "[glyphs] %s: its format cannot be read here: the game's glyphs stay\n", known->name);
                return false;
            }
            image = DrawAtlas(*known, style, base, width, height, levels);
        }
        static std::atomic<uint32_t> logged;
        if (logged.fetch_add(1) < 64)
            fprintf(stderr, "[glyphs] %s drawn as %s\n", known->name, StyleName(style));
        if (texture != nullptr)
            gpu::DestroyTexture(texture);
        texture = gpu::CreateTexture(
            { .width = width, .height = height, .mipLevels = levels, .format = gpu::Format::R8G8B8A8_UNORM });
        for (uint32_t level = 0; level < levels; level++)
            gpu::UploadTexture(texture, image[level].data(), std::max(width >> level, 1u) * 4, level);
        hostHash = wanted;
        return true;
    }
}
