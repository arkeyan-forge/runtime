#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>

// Central theme for the Skylanders launcher: every colour, size and font lives here so the
// look can be changed without touching the screens. The art direction is magical/cartoon
// (deep blue and purple night sky, gold/orange accents, eight elemental colours, a glowing
// Portal of Power ring). There is no publisher artwork anywhere: the background is drawn
// procedurally and the game icon comes from the player's own disc image at runtime.
namespace LauncherTheme
{
    // ---- Palette -----------------------------------------------------------------------
    inline constexpr SDL_Color Ink{ 7, 10, 24, 255 };             // window backdrop / shadows
    inline constexpr SDL_Color Night{ 12, 17, 44, 255 };          // deep blue night
    inline constexpr SDL_Color Twilight{ 27, 15, 58, 255 };       // purple horizon
    inline constexpr SDL_Color Panel{ 17, 23, 52, 246 };          // cards
    inline constexpr SDL_Color PanelRaised{ 28, 37, 74, 255 };    // hovered cards, chips
    inline constexpr SDL_Color Sidebar{ 10, 14, 36, 250 };        // left rail
    inline constexpr SDL_Color Border{ 58, 72, 122, 255 };
    inline constexpr SDL_Color BorderBright{ 112, 152, 255, 255 };

    inline constexpr SDL_Color TextPrimary{ 242, 246, 255, 255 };
    inline constexpr SDL_Color TextSecondary{ 178, 190, 226, 255 };
    inline constexpr SDL_Color TextDim{ 126, 140, 184, 255 };

    // Skylanders elemental accents (used by the game list and the portal decoration).
    inline constexpr SDL_Color Fire{ 255, 122, 74, 255 };
    inline constexpr SDL_Color Water{ 82, 170, 255, 255 };
    inline constexpr SDL_Color Air{ 156, 226, 255, 255 };
    inline constexpr SDL_Color Earth{ 166, 208, 112, 255 };
    inline constexpr SDL_Color Life{ 128, 236, 142, 255 };
    inline constexpr SDL_Color Undead{ 184, 146, 255, 255 };
    inline constexpr SDL_Color Magic{ 224, 150, 255, 255 };
    inline constexpr SDL_Color Tech{ 255, 214, 96, 255 };

    // Interface accents.
    inline constexpr SDL_Color Gold{ 255, 198, 92, 255 };
    inline constexpr SDL_Color Portal{ 196, 132, 255, 255 };       // Portal of Power violet
    inline constexpr SDL_Color Orange{ 255, 154, 64, 255 };
    inline constexpr SDL_Color Cyan{ 90, 228, 255, 255 };
    inline constexpr SDL_Color Green{ 108, 238, 178, 255 };
    inline constexpr SDL_Color PlayGreen{ 105, 214, 116, 255 };       // Minecraft-like play button
    inline constexpr SDL_Color PlayGreenHover{ 132, 240, 144, 255 };
    inline constexpr SDL_Color Red{ 255, 112, 124, 255 };

    // Short aliases used by the screen code and the widget layer.
    inline constexpr SDL_Color White = TextPrimary;
    inline constexpr SDL_Color Muted = TextSecondary;
    inline constexpr SDL_Color Dim = TextDim;
    inline constexpr SDL_Color Card = Panel;
    inline constexpr SDL_Color CardRaised = PanelRaised;
    inline constexpr SDL_Color Line = Border;
    inline constexpr SDL_Color GreenPlay = PlayGreen;
    inline constexpr SDL_Color GreenPlayHover = PlayGreenHover;
    inline constexpr SDL_Color Magenta{ 255, 85, 168, 255 };

    // ---- Layout (1280 x 800 design space) ----------------------------------------------
    inline constexpr float DesignW = 1280.0f;
    inline constexpr float DesignH = 800.0f;
    inline constexpr float DesignWidth = DesignW;
    inline constexpr float DesignHeight = DesignH;

    inline constexpr float SidebarW = 248.0f;
    inline constexpr float ContentX = 284.0f;
    inline constexpr float ContentW = 952.0f;        // 284 .. 1236
    inline constexpr float ContentRight = ContentX + ContentW;

    inline constexpr float HeroY = 64.0f;
    inline constexpr float HeroH = 416.0f;
    inline constexpr float BarY = 496.0f;
    inline constexpr float BarH = 104.0f;
    inline constexpr float FooterY = 616.0f;
    inline constexpr float FooterH = 80.0f;

    inline constexpr float Gap = 16.0f;
    inline constexpr float Pad = 24.0f;

    // ---- Shape and type ----------------------------------------------------------------
    inline constexpr float RadiusSm = 8.0f;
    inline constexpr float RadiusMd = 12.0f;
    inline constexpr float RadiusLg = 16.0f;
    inline constexpr float RadiusXl = 22.0f;
    inline constexpr float RadiusPill = 999.0f;

    inline constexpr float ButtonH = 46.0f;
    inline constexpr float PlayButtonH = 68.0f;

    inline constexpr const char* FontBody = "Outfit";     // SIL Open Font License 1.1
    inline constexpr const char* FontDisplay = "Bungee";   // SIL Open Font License 1.1

    inline SDL_Color Alpha(SDL_Color color, float alpha)
    {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        color.a = static_cast<uint8_t>(alpha * 255.0f);
        return color;
    }

    inline SDL_Color Mix(SDL_Color a, SDL_Color b, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return SDL_Color{
            static_cast<uint8_t>(a.r + (b.r - a.r) * t),
            static_cast<uint8_t>(a.g + (b.g - a.g) * t),
            static_cast<uint8_t>(a.b + (b.b - a.b) * t),
            static_cast<uint8_t>(a.a + (b.a - a.a) * t),
        };
    }

    inline SDL_Color FromRgb(uint32_t rgb)
    {
        return SDL_Color{ static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
            static_cast<uint8_t>(rgb), 255 };
    }

    inline SDL_Color ThemeAlpha(SDL_Color color, float alpha) { return Alpha(color, alpha); }
    inline SDL_Color ThemeMix(SDL_Color a, SDL_Color b, float t) { return Mix(a, b, t); }

    // Common text sizes, so screens do not repeat magic numbers.
    namespace Type
    {
        inline constexpr float DisplayLarge = 30.0f;
        inline constexpr float DisplayMedium = 24.0f;
        inline constexpr float Heading = 22.0f;
        inline constexpr float BodyLarge = 15.0f;
        inline constexpr float Body = 12.0f;
        inline constexpr float BodySmall = 11.0f;
        inline constexpr float Label = 10.0f;
        inline constexpr float Caption = 9.0f;
    }

    inline constexpr SDL_Color BackgroundTop{ 6, 9, 23, 255 };
    inline constexpr SDL_Color BackgroundMiddle{ 15, 12, 37, 255 };
    inline constexpr SDL_Color BackgroundBottom{ 27, 10, 42, 255 };
}

// Screens use the short names directly (White, Cyan, Type::Body, ...).
using namespace LauncherTheme;
