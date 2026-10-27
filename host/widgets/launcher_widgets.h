#pragma once

#include "../launcher_theme.h"
#include "../launcher_ui_draw.h"
#include <cstdint>
#include <string>
#include <string_view>

// Small, reusable pieces of the launcher. Screens compose these and own all interaction;
// the widgets only draw, so adding a screen never means copying button code.
enum class ButtonStyle
{
    Primary,   // the big green Play button
    Accent,    // cyan outlined action, e.g. Browse
    Secondary, // neutral panel button
    Ghost,     // text-only / low emphasis
};

class LauncherWidgets
{
public:
    explicit LauncherWidgets(LauncherPainter* painter) : m_painter(painter) {}

    void Button(const SDL_FRect& rect, std::string_view text, ButtonStyle style, bool enabled, bool hovered,
        float textPx = 13.0f);
    void Toggle(const SDL_FRect& rect, bool on, bool enabled, bool hovered);
    void Chip(const SDL_FRect& rect, std::string_view text, SDL_Color color, bool filled);
    void Panel(const SDL_FRect& rect, float radius = LauncherTheme::RadiusXl, bool glow = false,
        SDL_Color glowColor = LauncherTheme::Cyan);
    void SidebarItem(const SDL_FRect& rect, std::string_view name, std::string_view status, SDL_Color accent,
        bool enabled, bool selected, bool hovered);
    void ProgressBar(const SDL_FRect& rect, float progress, SDL_Color from, SDL_Color to);
    void Label(std::string_view text, float x, float y, SDL_Color color = LauncherTheme::TextDim);
    void Heading(std::string_view text, float x, float y, float px, SDL_Color color = LauncherTheme::TextPrimary);

    float LineHeight(float px) const { return m_painter->LineHeight(px); }

private:
    LauncherPainter* m_painter;
};
