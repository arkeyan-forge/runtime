#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Everything the launcher draws: rounded shapes, glows, text from a TrueType atlas, and a procedural
// background. SDL_Renderer only: no UI toolkit, no game assets.
class LauncherPainter
{
public:
    bool Init(SDL_Renderer* renderer);
    void Shutdown();

    void Background(float time);

    void FillRect(const SDL_FRect& rect, SDL_Color color);
    void RoundRect(const SDL_FRect& rect, float radius, SDL_Color color);
    void RoundRectGradient(const SDL_FRect& rect, float radius, SDL_Color left, SDL_Color right);
    void RoundRectOutline(const SDL_FRect& rect, float radius, float thickness, SDL_Color color, SDL_Color inner);
    void DashedRoundRect(const SDL_FRect& rect, float radius, float thickness, SDL_Color color, float dash, float gap);
    void Circle(float centerX, float centerY, float radius, SDL_Color color);
    void Ring(float centerX, float centerY, float radius, float thickness, SDL_Color color);
    void Glow(const SDL_FRect& rect, float radius, SDL_Color color, float strength);
    void DashedLine(float x1, float y1, float x2, float y2, float thickness, SDL_Color color, float dash, float gap);
    void Triangle(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color color);

    // `top` is the top of the line; `px` is the pixel size. `display` selects the display font (Bungee).
    void Text(std::string_view text, float x, float top, float px, SDL_Color color, bool display = false,
        float tracking = 0, bool shadow = false);
    void TextCentered(std::string_view text, float centerX, float top, float px, SDL_Color color, bool display = false,
        float tracking = 0);
    void TextRight(std::string_view text, float rightX, float top, float px, SDL_Color color, bool display = false,
        float tracking = 0);
    float Measure(std::string_view text, float px, bool display = false, float tracking = 0);
    float LineHeight(float px, bool display = false);
    float Ascent(float px, bool display = false);
    std::string Ellipsize(std::string_view text, float maxWidth, float px, bool display = false, float tracking = 0);
    std::vector<std::string> Wrap(std::string_view text, float maxWidth, float px, bool display = false,
        float tracking = 0);

    // PNG decoded with stb_image; the painter owns the texture and destroys it on Shutdown.
    SDL_Texture* LoadPng(std::span<const uint8_t> png);
    void DrawTexture(SDL_Texture* texture, const SDL_FRect& rect, SDL_Color tint = { 255, 255, 255, 255 });

    SDL_Renderer* Renderer() const { return m_renderer; }

private:
    struct Glyph
    {
        float x0, y0, x1, y1;
        float u0, v0, u1, v1;
        float advance;
    };

    struct Font
    {
        SDL_Texture* texture = nullptr;
        float size = 0;
        float scale = 0;
        float ascent = 0;
        float descent = 0;
        float lineGap = 0;
        int atlasWidth = 0;
        int atlasHeight = 0;
        Glyph glyphs[256]{};
        bool loaded = false;
    };

    struct CircleTexture
    {
        SDL_Texture* texture = nullptr;
        int size = 0;
    };

    Font* GetFont(float px, bool display);
    bool BakeFont(Font& font, float px, bool display);
    bool LoadFonts();
    static uint32_t NextCodepoint(std::string_view& text);
    static int MapCodepoint(uint32_t codepoint);
    SDL_Texture* CreateCircle(int size);
    SDL_Texture* CreateRing(int size, float thicknessRatio);

    SDL_Renderer* m_renderer = nullptr;
    std::vector<uint8_t> m_fontData[2]; // Outfit, Bungee
    std::string m_fontDirectory;
    std::unordered_map<uint64_t, Font> m_fonts;
    std::vector<SDL_Texture*> m_ownedTextures;
    SDL_Texture* m_background = nullptr;
    SDL_Texture* m_circle = nullptr;
    SDL_Texture* m_ring = nullptr;
    int m_width = 1280;
    int m_height = 800;
    float m_time = 0;
};
