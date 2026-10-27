#include "launcher_ui_draw.h"
#include "launcher_theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stb_image.h>
#include <stb_truetype.h>

namespace
{
    constexpr int CircleTextureSize = 256;

    float Smoothstep(float edge0, float edge1, float x)
    {
        float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    }

    void SetTint(SDL_Texture* texture, SDL_Color color)
    {
        SDL_SetTextureColorMod(texture, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(texture, color.a);
    }

    bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out)
    {
        size_t size = 0;
        void* data = SDL_LoadFile(path.c_str(), &size);
        if (data == nullptr)
            return false;
        out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        SDL_free(data);
        return true;
    }
}

bool LauncherPainter::Init(SDL_Renderer* renderer)
{
    m_renderer = renderer;
    SDL_GetRenderOutputSize(renderer, &m_width, &m_height);
    if (m_width <= 0)
        m_width = 1280;
    if (m_height <= 0)
        m_height = 800;

    m_circle = CreateCircle(CircleTextureSize);
    m_ring = CreateRing(CircleTextureSize, 0.055f);

    // Procedural background, rendered once at the launcher's design resolution.
    const int width = 1280;
    const int height = 800;
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    const SDL_Color top = BackgroundTop;
    const SDL_Color middle = BackgroundMiddle;
    const SDL_Color bottom = BackgroundBottom;

    struct Blob { float x, y, radius; SDL_Color color; float strength; };
    const Blob blobs[] = {
        { 0.10f, 0.12f, 0.52f, { 32, 152, 232, 255 }, 0.34f },
        { 0.90f, 0.18f, 0.50f, { 214, 52, 150, 255 }, 0.30f },
        { 0.68f, 0.98f, 0.52f, { 238, 142, 46, 255 }, 0.22f },
        { 0.32f, 0.92f, 0.46f, { 32, 190, 152, 255 }, 0.18f },
        { 0.55f, 0.46f, 0.70f, { 82, 64, 190, 255 }, 0.18f },
    };

    uint32_t noise = 0x1234567u;
    for (int y = 0; y < height; y++)
    {
        float v = float(y) / float(height - 1);
        SDL_Color base = v < 0.55f ? Mix(top, middle, v / 0.55f) : Mix(middle, bottom, (v - 0.55f) / 0.45f);
        for (int x = 0; x < width; x++)
        {
            float r = float(base.r), g = float(base.g), b = float(base.b);
            float nx = float(x) / width;
            for (const Blob& blob : blobs)
            {
                float dx = (nx - blob.x);
                float dy = (float(y) / height - blob.y);
                float distance = std::sqrt(dx * dx + dy * dy) / blob.radius;
                float amount = Smoothstep(1.0f, 0.0f, distance) * blob.strength;
                r += blob.color.r * amount;
                g += blob.color.g * amount;
                b += blob.color.b * amount;
            }

            // Vignette and a fine dithering grain keep large flat areas from banding.
            float dx = nx - 0.5f;
            float dy = float(y) / height - 0.5f;
            float vignette = 1.0f - Smoothstep(0.45f, 1.05f, std::sqrt(dx * dx + dy * dy) * 1.6f) * 0.55f;
            noise = noise * 1664525u + 1013904223u;
            float grain = (float((noise >> 24) & 0xFF) / 255.0f - 0.5f) * 5.0f;

            size_t offset = (size_t(y) * width + x) * 4;
            pixels[offset + 0] = uint8_t(std::clamp(r * vignette + grain, 0.0f, 255.0f));
            pixels[offset + 1] = uint8_t(std::clamp(g * vignette + grain, 0.0f, 255.0f));
            pixels[offset + 2] = uint8_t(std::clamp(b * vignette + grain, 0.0f, 255.0f));
            pixels[offset + 3] = 255;
        }
    }

    // A faint star field, baked into the background so it never flickers.
    uint32_t stars = 0xC0FFEEu;
    for (int i = 0; i < 210; i++)
    {
        stars = stars * 1103515245u + 12345u;
        int sx = int((stars >> 8) % width);
        stars = stars * 1103515245u + 12345u;
        int sy = int((stars >> 8) % height);
        stars = stars * 1103515245u + 12345u;
        int brightness = 40 + int((stars >> 16) % 120);
        int radius = (stars >> 28) % 3 == 0 ? 2 : 1;
        for (int oy = -radius; oy <= radius; oy++)
        {
            for (int ox = -radius; ox <= radius; ox++)
            {
                int px = std::clamp(sx + ox, 0, width - 1);
                int py = std::clamp(sy + oy, 0, height - 1);
                float falloff = std::max(0.0f, 1.0f - std::sqrt(float(ox * ox + oy * oy)) / (radius + 0.5f));
                size_t offset = (size_t(py) * width + px) * 4;
                for (int channel = 0; channel < 3; channel++)
                    pixels[offset + channel] = uint8_t(std::min(255, pixels[offset + channel] + int(brightness * falloff)));
            }
        }
    }

    m_background = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (m_background == nullptr)
        return false;
    SDL_UpdateTexture(m_background, nullptr, pixels.data(), width * 4);
    SDL_SetTextureBlendMode(m_background, SDL_BLENDMODE_NONE);

    LoadFonts();
    return m_circle != nullptr && m_ring != nullptr;
}

void LauncherPainter::Shutdown()
{
    for (auto& [key, font] : m_fonts)
    {
        if (font.texture != nullptr)
            SDL_DestroyTexture(font.texture);
    }
    m_fonts.clear();
    for (SDL_Texture* texture : m_ownedTextures)
        SDL_DestroyTexture(texture);
    m_ownedTextures.clear();
    if (m_background != nullptr)
        SDL_DestroyTexture(m_background);
    if (m_circle != nullptr)
        SDL_DestroyTexture(m_circle);
    if (m_ring != nullptr)
        SDL_DestroyTexture(m_ring);
    m_background = nullptr;
    m_circle = nullptr;
    m_ring = nullptr;
    m_renderer = nullptr;
}

bool LauncherPainter::LoadFonts()
{
    std::vector<std::string> directories;
    if (const char* overrideDirectory = SDL_getenv("SSA_LAUNCHER_FONT_DIR"))
        directories.emplace_back(overrideDirectory);

    if (const char* base = SDL_GetBasePath())
    {
        directories.emplace_back(std::string(base) + "fonts");
        directories.emplace_back(std::string(base) + "../Resources/fonts");
    }
#ifdef SSA_SOURCE_FONT_DIR
    directories.emplace_back(SSA_SOURCE_FONT_DIR);
#endif

    for (const std::string& directory : directories)
    {
        if (ReadWholeFile(directory + "/Outfit-Variable.ttf", m_fontData[0])
            && ReadWholeFile(directory + "/Bungee-Regular.ttf", m_fontData[1]))
        {
            m_fontDirectory = directory;
            return true;
        }
    }

    fprintf(stderr, "launcher: fonts not found (looked for Outfit-Variable.ttf and Bungee-Regular.ttf in the app's fonts folder)\n");
    return false;
}

SDL_Texture* LauncherPainter::CreateCircle(int size)
{
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    const float center = size * 0.5f;
    const float radius = size * 0.5f - 1.0f;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float coverage = 0;
            for (int sy = 0; sy < 4; sy++)
            {
                for (int sx = 0; sx < 4; sx++)
                {
                    float px = x + (sx + 0.5f) / 4.0f;
                    float py = y + (sy + 0.5f) / 4.0f;
                    float dx = px - center;
                    float dy = py - center;
                    if (dx * dx + dy * dy <= radius * radius)
                        coverage += 1.0f / 16.0f;
                }
            }
            size_t offset = (size_t(y) * size + x) * 4;
            pixels[offset + 0] = 255;
            pixels[offset + 1] = 255;
            pixels[offset + 2] = 255;
            pixels[offset + 3] = uint8_t(std::clamp(coverage, 0.0f, 1.0f) * 255.0f);
        }
    }

    SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);
    if (texture == nullptr)
        return nullptr;
    SDL_UpdateTexture(texture, nullptr, pixels.data(), size * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

SDL_Texture* LauncherPainter::CreateRing(int size, float thicknessRatio)
{
    std::vector<uint8_t> pixels(size_t(size) * size * 4);
    const float center = size * 0.5f;
    const float outer = size * 0.5f - 1.0f;
    const float inner = outer * (1.0f - thicknessRatio * 2.0f);
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float coverage = 0;
            for (int sy = 0; sy < 4; sy++)
            {
                for (int sx = 0; sx < 4; sx++)
                {
                    float dx = x + (sx + 0.5f) / 4.0f - center;
                    float dy = y + (sy + 0.5f) / 4.0f - center;
                    float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance <= outer && distance >= inner)
                        coverage += 1.0f / 16.0f;
                }
            }
            size_t offset = (size_t(y) * size + x) * 4;
            pixels[offset + 0] = 255;
            pixels[offset + 1] = 255;
            pixels[offset + 2] = 255;
            pixels[offset + 3] = uint8_t(std::clamp(coverage, 0.0f, 1.0f) * 255.0f);
        }
    }

    SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, size, size);
    if (texture == nullptr)
        return nullptr;
    SDL_UpdateTexture(texture, nullptr, pixels.data(), size * 4);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

void LauncherPainter::Background(float time)
{
    m_time = time;
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    SDL_FRect full{ 0, 0, float(m_width), float(m_height) };
    SDL_RenderTexture(m_renderer, m_background, nullptr, &full);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

    // Slow, low-contrast drifting orbs: motion without stealing attention from the content.
    const SDL_Color aura{ 60, 220, 255, 255 };
    for (int i = 0; i < 3; i++)
    {
        float phase = time * (0.05f + i * 0.012f) + i * 2.1f;
        float x = 0.18f + i * 0.32f + std::sin(phase) * 0.05f;
        float y = 0.30f + std::cos(phase * 0.8f) * 0.08f + i * 0.18f;
        Circle(x * 1280, y * 800, 150 + i * 40, SDL_Color{ aura.r, aura.g, aura.b, 7 });
    }
    SDL_SetTextureColorMod(m_circle, 255, 255, 255);
    SDL_SetTextureAlphaMod(m_circle, 255);
}

void LauncherPainter::FillRect(const SDL_FRect& rect, SDL_Color color)
{
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(m_renderer, &rect);
}

void LauncherPainter::RoundRect(const SDL_FRect& rect, float radius, SDL_Color color)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    SetTint(m_circle, color);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    if (r <= 0.5f)
    {
        FillRect(rect, color);
        return;
    }

    // Center cross. It must use the same tint as the corner discs: SDL_RenderFillRect ignores
    // texture tinting and would otherwise draw whatever colour the last FillRect left behind.
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_FRect vertical{ rect.x + r, rect.y, rect.w - 2 * r, rect.h };
    SDL_FRect horizontal{ rect.x, rect.y + r, rect.w, rect.h - 2 * r };
    SDL_RenderFillRect(m_renderer, &vertical);
    SDL_RenderFillRect(m_renderer, &horizontal);

    // Four quarter discs (the circle texture is split through its center).
    const float half = CircleTextureSize * 0.5f;
    SDL_FRect topLeftSource{ 0, 0, half, half };
    SDL_FRect topRightSource{ half, 0, half, half };
    SDL_FRect bottomLeftSource{ 0, half, half, half };
    SDL_FRect bottomRightSource{ half, half, half, half };
    SDL_FRect topLeft{ rect.x, rect.y, r, r };
    SDL_FRect topRight{ rect.x + rect.w - r, rect.y, r, r };
    SDL_FRect bottomLeft{ rect.x, rect.y + rect.h - r, r, r };
    SDL_FRect bottomRight{ rect.x + rect.w - r, rect.y + rect.h - r, r, r };
    SDL_RenderTexture(m_renderer, m_circle, &topLeftSource, &topLeft);
    SDL_RenderTexture(m_renderer, m_circle, &topRightSource, &topRight);
    SDL_RenderTexture(m_renderer, m_circle, &bottomLeftSource, &bottomLeft);
    SDL_RenderTexture(m_renderer, m_circle, &bottomRightSource, &bottomRight);
}

void LauncherPainter::RoundRectGradient(const SDL_FRect& rect, float radius, SDL_Color left, SDL_Color right)
{
    float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    int strips = std::max(8, int(rect.w));
    for (int i = 0; i < strips; i++)
    {
        float x0 = rect.x + rect.w * i / strips;
        float x1 = rect.x + rect.w * (i + 1) / strips;
        float inset = 0;
        auto cornerInset = [&](float x)
        {
            if (r <= 0)
                return 0.0f;
            float dx = x < rect.x + r ? rect.x + r - x : (x > rect.x + rect.w - r ? x - (rect.x + rect.w - r) : 0.0f);
            if (dx <= 0)
                return 0.0f;
            return r - std::sqrt(std::max(0.0f, r * r - dx * dx));
        };
        inset = std::max(cornerInset(x0), cornerInset(x1));
        SDL_Color color = Mix(left, right, float(i) / std::max(1, strips - 1));
        SDL_FRect strip{ x0, rect.y + inset, x1 - x0 + 0.5f, rect.h - inset * 2 };
        FillRect(strip, color);
    }
}

void LauncherPainter::RoundRectOutline(const SDL_FRect& rect, float radius, float thickness, SDL_Color color, SDL_Color inner)
{
    (void)inner;
    if (thickness <= 0 || rect.w <= 0 || rect.h <= 0)
        return;
    float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    float half = thickness * 0.5f;
    // Straight edges as capsules, corners as a fan of overlapping circles: anti-aliased at any size.
    RoundRect({ rect.x + r, rect.y, rect.w - 2 * r, thickness }, half, color);
    RoundRect({ rect.x + r, rect.y + rect.h - thickness, rect.w - 2 * r, thickness }, half, color);
    RoundRect({ rect.x, rect.y + r, thickness, rect.h - 2 * r }, half, color);
    RoundRect({ rect.x + rect.w - thickness, rect.y + r, thickness, rect.h - 2 * r }, half, color);
    constexpr float Pi = 3.14159265358979323846f;
    for (int corner = 0; corner < 4; corner++)
    {
        float cx = corner == 0 || corner == 3 ? rect.x + r : rect.x + rect.w - r;
        float cy = corner < 2 ? rect.y + r : rect.y + rect.h - r;
        float start = (corner == 0 ? 180.0f : corner == 1 ? 270.0f : corner == 2 ? 0.0f : 90.0f);
        for (int step = 0; step <= 12; step++)
        {
            float angle = (start + step * 7.5f) * Pi / 180.0f;
            Circle(cx + std::cos(angle) * (r - half), cy + std::sin(angle) * (r - half), half, color);
        }
    }
}

void LauncherPainter::Circle(float centerX, float centerY, float radius, SDL_Color color)
{
    SetTint(m_circle, color);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_FRect destination{ centerX - radius, centerY - radius, radius * 2, radius * 2 };
    SDL_RenderTexture(m_renderer, m_circle, nullptr, &destination);
}

void LauncherPainter::Ring(float centerX, float centerY, float radius, float thickness, SDL_Color color)
{
    if (radius <= 0)
        return;
    SetTint(m_ring, color);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    // The baked ring's stroke is about 11% of its diameter; scale the radius so the requested
    // thickness is close, then let the caller's radius dominate the size.
    float outer = radius + thickness * 0.5f;
    SDL_FRect destination{ centerX - outer, centerY - outer, outer * 2, outer * 2 };
    SDL_RenderTexture(m_renderer, m_ring, nullptr, &destination);
}

void LauncherPainter::Glow(const SDL_FRect& rect, float radius, SDL_Color color, float strength)
{
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_ADD);
    for (int i = 5; i >= 0; i--)
    {
        float expand = 2.0f + i * 4.5f;
        SDL_Color layer{ color.r, color.g, color.b, uint8_t(std::clamp(strength * (0.05f + 0.03f * (5 - i)), 0.0f, 1.0f) * 255.0f) };
        SDL_FRect expanded{ rect.x - expand, rect.y - expand, rect.w + expand * 2, rect.h + expand * 2 };
        RoundRect(expanded, radius + expand, layer);
    }
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
}

void LauncherPainter::DashedLine(float x1, float y1, float x2, float y2, float thickness, SDL_Color color, float dash, float gap)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0)
        return;
    dx /= length;
    dy /= length;
    for (float position = 0; position < length; position += dash + gap)
    {
        float end = std::min(position + dash, length);
        SDL_FRect segment{ x1 + dx * position, y1 + dy * position, 0, 0 };
        if (std::abs(dx) > std::abs(dy))
        {
            segment.w = std::max(thickness, end - position);
            segment.h = thickness;
            segment.y -= thickness * 0.5f;
        }
        else
        {
            segment.w = thickness;
            segment.h = std::max(thickness, end - position);
            segment.x -= thickness * 0.5f;
        }
        RoundRect(segment, thickness * 0.5f, color);
    }
}

void LauncherPainter::DashedRoundRect(const SDL_FRect& rect, float radius, float thickness, SDL_Color color, float dash, float gap)
{
    float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    DashedLine(rect.x + r, rect.y, rect.x + rect.w - r, rect.y, thickness, color, dash, gap);
    DashedLine(rect.x + r, rect.y + rect.h - thickness, rect.x + rect.w - r, rect.y + rect.h - thickness, thickness, color, dash, gap);
    DashedLine(rect.x, rect.y + r, rect.x, rect.y + rect.h - r, thickness, color, dash, gap);
    DashedLine(rect.x + rect.w - thickness, rect.y + r, rect.x + rect.w - thickness, rect.y + rect.h - r, thickness, color, dash, gap);

    // Arc dashes at the four corners.
    for (int corner = 0; corner < 4; corner++)
    {
        float cx = corner == 0 || corner == 3 ? rect.x + r : rect.x + rect.w - r;
        float cy = corner < 2 ? rect.y + r : rect.y + rect.h - r;
        float start = (corner == 0 ? 180.0f : corner == 1 ? 270.0f : corner == 2 ? 0.0f : 90.0f);
        constexpr float Pi = 3.14159265358979323846f;
        for (int step = 0; step < 4; step++)
        {
            float angle = (start + step * 22.5f + 11.25f) * Pi / 180.0f;
            float x = cx + std::cos(angle) * (r - thickness * 0.5f);
            float y = cy + std::sin(angle) * (r - thickness * 0.5f);
            Circle(x, y, thickness * 0.5f, color);
        }
    }
}

void LauncherPainter::Triangle(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color color)
{
    SDL_Vertex vertices[3] = {
        { { x1, y1 }, { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f } },
        { { x2, y2 }, { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f } },
        { { x3, y3 }, { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f } },
    };
    SDL_RenderGeometry(m_renderer, nullptr, vertices, 3, nullptr, 0);
}

SDL_Texture* LauncherPainter::LoadPng(std::span<const uint8_t> png)
{
    int width = 0, height = 0, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load_from_memory(png.data(), int(png.size()), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        stbi_image_free(pixels);
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (texture != nullptr)
    {
        SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        m_ownedTextures.push_back(texture);
    }
    stbi_image_free(pixels);
    return texture;
}

void LauncherPainter::DrawTexture(SDL_Texture* texture, const SDL_FRect& rect, SDL_Color tint)
{
    if (texture == nullptr)
        return;
    SetTint(texture, tint);
    SDL_RenderTexture(m_renderer, texture, nullptr, &rect);
}

uint32_t LauncherPainter::NextCodepoint(std::string_view& text)
{
    if (text.empty())
        return 0;
    unsigned char first = static_cast<unsigned char>(text[0]);
    uint32_t codepoint = '?';
    size_t length = 1;
    if (first < 0x80)
    {
        codepoint = first;
    }
    else if ((first >> 5) == 0x6 && text.size() >= 2)
    {
        codepoint = ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[1]) & 0x3F);
        length = 2;
    }
    else if ((first >> 4) == 0xE && text.size() >= 3)
    {
        codepoint = ((first & 0x0F) << 12) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 6)
            | (static_cast<unsigned char>(text[2]) & 0x3F);
        length = 3;
    }
    else if ((first >> 3) == 0x1E && text.size() >= 4)
    {
        codepoint = ((first & 0x07) << 18) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 12)
            | ((static_cast<unsigned char>(text[2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[3]) & 0x3F);
        length = 4;
    }
    text.remove_prefix(length);
    return codepoint;
}

int LauncherPainter::MapCodepoint(uint32_t codepoint)
{
    switch (codepoint)
    {
    case 0x00A0: return ' ';
    case 0x2013:
    case 0x2014: return '-';
    case 0x2018:
    case 0x2019: return '\'';
    case 0x201C:
    case 0x201D: return '"';
    case 0x2022: return '*';
    case 0x2026: return '.';
    case 0x00B7: return '.';
    default: break;
    }
    if ((codepoint >= 0x20 && codepoint <= 0x7E) || (codepoint >= 0xA0 && codepoint <= 0xFF))
        return int(codepoint);
    return '?';
}

bool LauncherPainter::BakeFont(Font& font, float px, bool display)
{
    const std::vector<uint8_t>& data = m_fontData[display ? 1 : 0];
    if (data.empty())
        return false;

    stbtt_fontinfo info{};
    int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (!stbtt_InitFont(&info, data.data(), offset))
        return false;

    font.size = px;
    font.scale = stbtt_ScaleForPixelHeight(&info, px);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    font.ascent = ascent * font.scale;
    font.descent = descent * font.scale;
    font.lineGap = lineGap * font.scale;

    int atlasWidth = 512;
    int atlasHeight = 512;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        std::vector<uint8_t> bitmap(size_t(atlasWidth) * atlasHeight);
        stbtt_pack_context pack{};
        if (!stbtt_PackBegin(&pack, bitmap.data(), atlasWidth, atlasHeight, 0, 4, nullptr))
            break;

        stbtt_packedchar packed[256]{};
        stbtt_pack_range range{};
        range.font_size = px;
        range.first_unicode_codepoint_in_range = 0x20;
        range.num_chars = 0xE0;
        range.chardata_for_range = packed + 0x20;
        bool ok = stbtt_PackFontRanges(&pack, data.data(), 0, &range, 1);
        stbtt_PackEnd(&pack);
        if (!ok)
        {
            atlasWidth *= 2;
            if (atlasWidth > 2048)
            {
                atlasWidth = 1024;
                atlasHeight *= 2;
            }
            continue;
        }

        std::vector<uint8_t> rgba(size_t(atlasWidth) * atlasHeight * 4);
        for (size_t i = 0; i < bitmap.size(); i++)
        {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = bitmap[i];
        }
        font.texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, atlasWidth, atlasHeight);
        if (font.texture == nullptr)
            return false;
        SDL_UpdateTexture(font.texture, nullptr, rgba.data(), atlasWidth * 4);
        SDL_SetTextureBlendMode(font.texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(font.texture, SDL_SCALEMODE_LINEAR);
        font.atlasWidth = atlasWidth;
        font.atlasHeight = atlasHeight;

        for (int c = 0x20; c <= 0xFF; c++)
        {
            stbtt_aligned_quad quad{};
            float xpos = 0, ypos = 0;
            stbtt_GetPackedQuad(packed, atlasWidth, atlasHeight, c, &xpos, &ypos, &quad, 0);
            font.glyphs[c] = Glyph{ quad.x0, quad.y0, quad.x1, quad.y1,
                quad.s0 * atlasWidth, quad.t0 * atlasHeight, quad.s1 * atlasWidth, quad.t1 * atlasHeight,
                packed[c].xadvance };
        }
        font.loaded = true;
        return true;
    }
    return false;
}

LauncherPainter::Font* LauncherPainter::GetFont(float px, bool display)
{
    px = std::round(std::clamp(px, 8.0f, 120.0f));
    uint64_t key = (uint64_t(display ? 1 : 0) << 32) | uint64_t(px);
    auto found = m_fonts.find(key);
    if (found != m_fonts.end())
        return found->second.loaded ? &found->second : nullptr;

    Font font;
    BakeFont(font, px, display);
    auto [inserted, _] = m_fonts.emplace(key, std::move(font));
    return inserted->second.loaded ? &inserted->second : nullptr;
}

float LauncherPainter::Ascent(float px, bool display)
{
    Font* font = GetFont(px, display);
    return font != nullptr ? font->ascent : px * 0.78f;
}

float LauncherPainter::LineHeight(float px, bool display)
{
    Font* font = GetFont(px, display);
    if (font == nullptr)
        return px * 1.25f;
    return font->ascent - font->descent + font->lineGap;
}

float LauncherPainter::Measure(std::string_view text, float px, bool display, float tracking)
{
    Font* font = GetFont(px, display);
    if (font == nullptr)
        return 0;
    float width = 0;
    while (!text.empty())
    {
        uint32_t codepoint = NextCodepoint(text);
        width += font->glyphs[MapCodepoint(codepoint)].advance + tracking;
    }
    return width;
}

void LauncherPainter::Text(std::string_view text, float x, float top, float px, SDL_Color color, bool display,
    float tracking, bool shadow)
{
    Font* font = GetFont(px, display);
    if (font == nullptr)
        return;

    float baseline = top + font->ascent;
    auto draw = [&](float ox, float oy, SDL_Color tint)
    {
        SetTint(font->texture, tint);
        float pen = x + ox;
        while (!text.empty())
        {
            uint32_t codepoint = NextCodepoint(text);
            int mapped = MapCodepoint(codepoint);
            const Glyph& glyph = font->glyphs[mapped];
            SDL_FRect source{ glyph.u0, glyph.v0, glyph.u1 - glyph.u0, glyph.v1 - glyph.v0 };
            SDL_FRect destination{ std::round(pen + glyph.x0), std::round(baseline + glyph.y0 + oy),
                std::round(glyph.x1 - glyph.x0), std::round(glyph.y1 - glyph.y0) };
            if (destination.w > 0 && destination.h > 0)
                SDL_RenderTexture(m_renderer, font->texture, &source, &destination);
            pen += glyph.advance + tracking;
        }
    };

    if (shadow)
        draw(1.5f, 2.0f, SDL_Color{ 0, 0, 0, uint8_t(color.a * 0.55f) });
    draw(0, 0, color);
}

void LauncherPainter::TextCentered(std::string_view text, float centerX, float top, float px, SDL_Color color,
    bool display, float tracking)
{
    Text(text, centerX - Measure(text, px, display, tracking) * 0.5f, top, px, color, display, tracking);
}

void LauncherPainter::TextRight(std::string_view text, float rightX, float top, float px, SDL_Color color,
    bool display, float tracking)
{
    Text(text, rightX - Measure(text, px, display, tracking), top, px, color, display, tracking);
}

std::string LauncherPainter::Ellipsize(std::string_view text, float maxWidth, float px, bool display, float tracking)
{
    if (Measure(text, px, display, tracking) <= maxWidth)
        return std::string(text);
    std::vector<size_t> boundaries{ 0 };
    std::string_view copy = text;
    while (!copy.empty())
    {
        NextCodepoint(copy);
        boundaries.push_back(text.size() - copy.size());
    }
    const std::string dots = "...";
    for (size_t i = boundaries.size() - 1; i-- > 0;)
    {
        std::string candidate(text.substr(0, boundaries[i]));
        candidate += dots;
        if (Measure(candidate, px, display, tracking) <= maxWidth)
            return candidate;
    }
    return dots;
}

std::vector<std::string> LauncherPainter::Wrap(std::string_view text, float maxWidth, float px, bool display,
    float tracking)
{
    std::vector<std::string> lines;
    std::string line;
    std::string word;
    auto flushWord = [&]()
    {
        if (word.empty())
            return;
        std::string candidate = line.empty() ? word : line + " " + word;
        if (Measure(candidate, px, display, tracking) <= maxWidth)
        {
            line = candidate;
        }
        else if (!line.empty())
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            // A single word wider than the panel: break it at a codepoint boundary.
            std::string_view rest = word;
            std::string piece;
            while (!rest.empty())
            {
                std::string_view before = rest;
                uint32_t codepoint = NextCodepoint(rest);
                std::string next = piece;
                size_t length = before.size() - rest.size();
                next.append(before.data(), length);
                if (!piece.empty() && Measure(next, px, display, tracking) > maxWidth)
                {
                    lines.push_back(piece);
                    piece.clear();
                }
                piece.append(before.data(), length);
                (void)codepoint;
            }
            line = piece;
        }
        word.clear();
    };

    for (size_t i = 0; i < text.size();)
    {
        if (text[i] == '\n')
        {
            flushWord();
            lines.push_back(line);
            line.clear();
            i++;
            continue;
        }
        if (text[i] == ' ' || text[i] == '\t')
        {
            flushWord();
            i++;
            continue;
        }
        word.push_back(text[i]);
        i++;
    }
    flushWord();
    if (!line.empty())
        lines.push_back(line);
    return lines;
}
