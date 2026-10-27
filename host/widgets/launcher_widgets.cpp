#include "launcher_widgets.h"
#include <algorithm>
#include <cmath>

using namespace LauncherTheme;

void LauncherWidgets::Button(const SDL_FRect& rect, std::string_view text, ButtonStyle style, bool enabled,
    bool hovered, float textPx)
{
    if (!m_painter) return;

    switch (style)
    {
    case ButtonStyle::Primary:
    {
        SDL_Color c1 = enabled ? (hovered ? PlayGreenHover : PlayGreen) : Alpha(TextDim, 0.45f);
        SDL_Color c2 = enabled ? Mix(c1, Gold, hovered ? 0.18f : 0.10f) : Alpha(TextDim, 0.35f);
        if (enabled) m_painter->Glow(rect, RadiusXl, c1, hovered ? 0.95f : 0.68f);
        m_painter->RoundRectGradient(rect, RadiusXl, c1, c2);
        m_painter->RoundRectOutline(rect, RadiusXl, 1.6f, Alpha(TextPrimary, enabled ? 0.28f : 0.16f), Alpha(LauncherTheme::Panel, 0.0f));
        SDL_Color tc = enabled ? SDL_Color{ 10, 18, 12, 255 } : TextDim;
        m_painter->TextCentered(text, rect.x + rect.w * 0.5f, rect.y + (rect.h - m_painter->LineHeight(textPx)) * 0.5f,
            textPx, tc, true, 0.9f);
        break;
    }
    case ButtonStyle::Accent:
    {
        SDL_Color bg = enabled ? (hovered ? Alpha(Cyan, 0.22f) : Alpha(Cyan, 0.14f)) : Alpha(LauncherTheme::PanelRaised, 0.4f);
        m_painter->RoundRect(rect, RadiusLg, bg);
        m_painter->RoundRectOutline(rect, RadiusLg, 1.4f, enabled ? (hovered ? Cyan : Alpha(Cyan, 0.8f)) : Border,
            Alpha(LauncherTheme::Panel, 0.0f));
        SDL_Color tc = enabled ? (hovered ? TextPrimary : TextSecondary) : TextDim;
        m_painter->TextCentered(text, rect.x + rect.w * 0.5f, rect.y + (rect.h - m_painter->LineHeight(textPx)) * 0.5f,
            textPx, tc, false, 1.6f);
        break;
    }
    case ButtonStyle::Secondary:
    {
        SDL_Color bg = enabled ? (hovered ? LauncherTheme::PanelRaised : LauncherTheme::Panel) : Alpha(LauncherTheme::Panel, 0.5f);
        m_painter->RoundRect(rect, RadiusLg, bg);
        m_painter->RoundRectOutline(rect, RadiusLg, 1.3f, enabled ? (hovered ? BorderBright : Border) : Border,
            Alpha(LauncherTheme::Panel, 0.0f));
        SDL_Color tc = enabled ? (hovered ? TextPrimary : TextSecondary) : TextDim;
        m_painter->TextCentered(text, rect.x + rect.w * 0.5f, rect.y + (rect.h - m_painter->LineHeight(textPx)) * 0.5f,
            textPx, tc, false, 1.6f);
        break;
    }
    case ButtonStyle::Ghost:
    {
        SDL_Color tc = enabled ? (hovered ? TextPrimary : TextSecondary) : TextDim;
        m_painter->TextCentered(text, rect.x + rect.w * 0.5f, rect.y + (rect.h - m_painter->LineHeight(textPx)) * 0.5f,
            textPx, tc, false, 1.4f);
        if (hovered && enabled)
            m_painter->RoundRect(rect, RadiusMd, Alpha(LauncherTheme::PanelRaised, 0.5f));
        break;
    }
    }
}

void LauncherWidgets::Toggle(const SDL_FRect& rect, bool on, bool enabled, bool hovered)
{
    if (!m_painter) return;
    SDL_Color track = on ? Alpha(PlayGreen, enabled ? 0.9f : 0.5f) : Alpha(Border, enabled ? 0.9f : 0.5f);
    m_painter->RoundRect(rect, rect.h * 0.5f, track);
    if (hovered && enabled) m_painter->RoundRectOutline(rect, rect.h * 0.5f, 1.0f, BorderBright, Alpha(LauncherTheme::Panel, 0.0f));
    float knobX = on ? rect.x + rect.w - rect.h * 0.5f : rect.x + rect.h * 0.5f;
    m_painter->Circle(knobX, rect.y + rect.h * 0.5f, rect.h * 0.38f, enabled ? TextPrimary : TextDim);
}

void LauncherWidgets::Chip(const SDL_FRect& rect, std::string_view text, SDL_Color color, bool filled)
{
    if (!m_painter) return;
    if (filled)
    {
        m_painter->RoundRect(rect, RadiusSm, Alpha(color, 0.18f));
        m_painter->RoundRectOutline(rect, RadiusSm, 1.0f, Alpha(color, 0.6f), Alpha(LauncherTheme::Panel, 0.0f));
    }
    else
    {
        m_painter->RoundRect(rect, RadiusSm, Alpha(color, 0.12f));
        m_painter->RoundRectOutline(rect, RadiusSm, 1.0f, Alpha(color, 0.46f), Alpha(LauncherTheme::Panel, 0.0f));
    }
    m_painter->TextCentered(text, rect.x + rect.w * 0.5f, rect.y + (rect.h - m_painter->LineHeight(9.0f)) * 0.5f,
        9.0f, color, false, 1.8f);
}

void LauncherWidgets::Panel(const SDL_FRect& rect, float radius, bool glow, SDL_Color glowColor)
{
    if (!m_painter) return;
    if (glow) m_painter->Glow(rect, radius, glowColor, 0.72f);
    m_painter->RoundRect({ rect.x + 2.0f, rect.y + 10.0f, rect.w, rect.h }, radius, Alpha(Ink, 0.38f));
    m_painter->RoundRect(rect, radius, LauncherTheme::Panel);
    m_painter->RoundRectOutline(rect, radius, 1.4f, Border, Alpha(LauncherTheme::Panel, 0.0f));
}

void LauncherWidgets::SidebarItem(const SDL_FRect& rect, std::string_view name, std::string_view status,
    SDL_Color accent, bool enabled, bool selected, bool hovered)
{
    if (!m_painter) return;
    if (!selected && hovered && enabled)
        m_painter->RoundRect({ rect.x + 10.0f, rect.y + 2.0f, rect.w - 20.0f, rect.h - 4.0f }, RadiusLg,
            Alpha(LauncherTheme::PanelRaised, 0.6f));
    if (selected)
    {
        m_painter->Glow({ rect.x + 12.0f, rect.y + 4.0f, rect.w - 24.0f, rect.h - 8.0f }, RadiusLg, accent, 0.58f);
        m_painter->RoundRect({ rect.x + 12.0f, rect.y + 4.0f, rect.w - 24.0f, rect.h - 8.0f }, RadiusLg,
            Alpha(LauncherTheme::PanelRaised, 0.92f));
        m_painter->RoundRectOutline({ rect.x + 12.0f, rect.y + 4.0f, rect.w - 24.0f, rect.h - 8.0f }, RadiusLg,
            1.4f, Alpha(accent, 0.92f), Alpha(LauncherTheme::Panel, 0.0f));
        m_painter->FillRect({ rect.x + 12.0f, rect.y + 14.0f, 3.0f, rect.h - 28.0f }, accent);
    }

    SDL_FRect tile{ rect.x + 26.0f, rect.y + (rect.h - 34.0f) * 0.5f, 34.0f, 34.0f };
    m_painter->RoundRect(tile, RadiusMd, Alpha(accent, enabled ? 0.2f : 0.12f));
    m_painter->RoundRectOutline(tile, RadiusMd, 1.0f, Alpha(accent, enabled ? 0.56f : 0.34f), Alpha(LauncherTheme::Panel, 0.0f));
    if (!name.empty())
    {
        char initial = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        char buf[2] = { initial, 0 };
        m_painter->TextCentered(buf, tile.x + tile.w * 0.5f, tile.y + 7.0f, 18.0f, Alpha(accent, enabled ? 1.0f : 0.62f), true);
    }

    float tx = tile.x + tile.w + 14.0f;
    float ty = rect.y + (rect.h - 28.0f) * 0.5f;
    SDL_Color nc = enabled ? TextPrimary : TextDim;
    m_painter->Text(name, tx, ty, 12.5f, Alpha(nc, enabled ? 0.98f : 0.72f), false, 0.6f);
    m_painter->Text(status, tx, ty + 18.0f, 9.5f, Alpha(enabled ? (selected ? Life : accent) : TextSecondary, enabled ? 0.92f : 0.86f), false, 1.1f);
}

void LauncherWidgets::ProgressBar(const SDL_FRect& rect, float progress, SDL_Color from, SDL_Color to)
{
    if (!m_painter) return;
    progress = std::clamp(progress, 0.0f, 1.0f);
    m_painter->RoundRect(rect, rect.h * 0.5f, Alpha(Border, 0.6f));
    if (progress > 0.01f)
    {
        SDL_FRect bar{ rect.x, rect.y, std::max(rect.h, rect.w * progress), rect.h };
        m_painter->Glow(bar, rect.h * 0.5f, from, 0.64f);
        m_painter->RoundRectGradient(bar, rect.h * 0.5f, from, to);
        float shimmer = std::fmod(static_cast<float>(SDL_GetTicksNS()) / 1'000'000'000.0f * 0.38f, 1.6f) - 0.3f;
        float sx = bar.x + bar.w * shimmer;
        if (sx > bar.x && sx < bar.x + bar.w)
            m_painter->RoundRect({ sx, bar.y, 26.0f, bar.h }, rect.h * 0.5f, Alpha(TextPrimary, 0.2f));
    }
}

void LauncherWidgets::Label(std::string_view text, float x, float y, SDL_Color color)
{
    if (!m_painter) return;
    m_painter->Text(text, x, y, 10.0f, color, false, 2.0f);
}

void LauncherWidgets::Heading(std::string_view text, float x, float y, float px, SDL_Color color)
{
    if (!m_painter) return;
    m_painter->Text(text, x, y, px, color, true, 0.7f);
}
