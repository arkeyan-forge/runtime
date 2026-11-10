#pragma once

// The in-game interface's design (docs/runtime/host/ingame_ui.md): Skylauncher's look over the game. The launcher's
// neutral greys, one accent (the running game's own colour, made readable on the panels), Bungee for page titles and the
// game's name, Outfit for everything else, a 2 px focus ring in the accent; the layouts of the full screens (a left rail
// shared by the Toy Box, Achievements, Settings and Controls), of the F3 menu's side panel and of the corner toasts. draw.cpp
// draws with these; the parts that are pure logic are here so that look_test.cpp checks them without fonts or a window.
// Sizes are canvas pixels (1280x720, the overlay is letterboxed over the game at any window size).
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace toybox::look
{
    constexpr float CanvasWidth = 1280, CanvasHeight = 720;

    // TV action-safe area: 5 % of each side. What is drawn over the game while it plays (toasts, the FPS counter) stays
    // inside it; the full screens and the F3 menu's panel reach the edges, as the launcher's pages do.
    constexpr float SafeLeft = 64, SafeTop = 36, SafeRight = CanvasWidth - 64, SafeBottom = CanvasHeight - 36;

    // Colours (0xRRGGBB): Skylauncher's greys and inks (its launcher_theme.h). The accent is not here: it is the game's.
    namespace Colour
    {
        constexpr uint32_t Scrim = 0x0A0A0E;      // over the game beside the F3 menu's panel
        constexpr uint32_t Page = 0x1E1E24;       // the full screens, the panel over the game, the toasts
        constexpr uint32_t Surface = 0x26262C;    // the rail, cards, panels on a page
        constexpr uint32_t Raised = 0x2C2C34;     // chips, the profile's pill, controls on a card
        constexpr uint32_t Raised2 = 0x3A3A42;    // buttons, the selected rail row, a focused row or card
        constexpr uint32_t Focus = Raised2;       // a focused row or card's fill
        constexpr uint32_t Hairline = 0x3A3A44;   // borders, separators
        constexpr uint32_t Border = 0x4A4A55;     // a button's or a toast's border
        constexpr uint32_t Control = 0x55555E;    // a switch's track when off, a meter's empty steps
        constexpr uint32_t KeyOutline = 0x7A7A88; // the outlined button hints, dashed empty seats
        constexpr uint32_t Well = 0x15171C;       // slider tracks, picture backings, ink on the accent
        constexpr uint32_t Field = 0x1C1C21;      // the search field
        constexpr uint32_t Text = 0xF4F4F6, TextSoft = 0xD0D0D8, TextDim = 0xB8B8C4, TextDimmer = 0x8C8C98;
        constexpr uint32_t Success = 0x48D17A, Warning = 0xFFB43A, DangerText = 0xFF817A;
        // The out-of-health cross: tools/regress/out_of_health.sh counts this exact red.
        constexpr uint32_t Danger = 0xF85149;
    }

    // Type scale (Outfit SemiBold for running text, Outfit Bold for labels and values, Bungee for page titles).
    namespace Type
    {
        constexpr float Caption = 12, Small = 14, Body = 16, Label = 17, Heading = 20, Title = 30;
    }

    // Spacing scale and corner radii (the launcher's 14 / 12 / 10).
    namespace Space
    {
        constexpr float XS = 4, S = 8, M = 12, L = 16, XL = 24, XXL = 32;
    }
    namespace Radius
    {
        constexpr float Key = 6, Control = 10, Row = 12, Card = 14, Panel = 14;
    }

    // --- The accent: the game's colour, readable ---------------------------------------------------------------------

    // WCAG relative luminance of 0xRRGGBB, and the contrast ratio of two colours (1 to 21).
    inline double Luminance(uint32_t rgb)
    {
        auto channel = [](uint32_t c) {
            double v = double(c) / 255.0;
            return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(rgb >> 16 & 0xFF) + 0.7152 * channel(rgb >> 8 & 0xFF) + 0.0722 * channel(rgb & 0xFF);
    }
    inline double Contrast(uint32_t a, uint32_t b)
    {
        double la = Luminance(a) + 0.05, lb = Luminance(b) + 0.05;
        return la > lb ? la / lb : lb / la;
    }

    // `a` mixed toward `b` by t (0-1), per channel.
    inline uint32_t Mix(uint32_t a, uint32_t b, float t)
    {
        uint32_t out = 0;
        for (int shift : { 16, 8, 0 })
        {
            float ca = float(a >> shift & 0xFF), cb = float(b >> shift & 0xFF);
            out |= uint32_t(std::lround(ca + (cb - ca) * t)) << shift;
        }
        return out;
    }

    // The game's glow (Skylauncher's StyleOf(game).glow, GameInfo::glow) lightened toward white in tenths until it reads
    // at 4.5:1 on the rail and the cards (Surface), as Skylauncher's ReadableAccent does for Trap Team's red.
    inline uint32_t ReadableAccent(uint32_t glow, uint32_t background = Colour::Surface, double contrast = 4.5)
    {
        uint32_t accent = glow;
        for (int step = 1; Contrast(accent, background) < contrast && step <= 10; step++)
            accent = Mix(glow, 0xFFFFFF, float(step) / 10);
        return accent;
    }

    // --- The toast: a soft slide and fade in, a fade out ----------------------------------------------------------------

    constexpr uint64_t ToastIn = 240'000'000, ToastOut = 200'000'000; // nanoseconds
    constexpr uint64_t ToastFade = 120'000'000;                        // in and out, with reduced motion
    constexpr float ToastRise = 20;                                    // pixels it slides in from

    struct Motion
    {
        float alpha = 1;     // 0-1
        float offset = 0;    // pixels away from its place, towards the edge it came from
        bool moving = false; // the next frame differs: redraw
    };

    // The toast shown from `since` until `until` (then fading out) at `now`. Eased: out-cubic in, in-quad out.
    inline Motion ToastMotion(uint64_t now, uint64_t since, uint64_t until, bool reduced)
    {
        uint64_t in = reduced ? ToastFade : ToastIn, out = reduced ? ToastFade : ToastOut;
        Motion m;
        if (now < since + in)
        {
            float t = now <= since ? 0.0f : float(now - since) / float(in);
            float eased = 1 - (1 - t) * (1 - t) * (1 - t);
            m.alpha = eased;
            m.offset = reduced ? 0 : ToastRise * (1 - eased);
            m.moving = true;
        }
        else if (now >= until)
        {
            float t = std::min(float(now - until) / float(out), 1.0f);
            m.alpha = 1 - t * t;
            m.offset = reduced ? 0 : ToastRise * 0.4f * t * t;
            m.moving = t < 1;
        }
        return m;
    }

    // When the toast is gone for good (the fade out done).
    inline bool ToastGone(uint64_t now, uint64_t until, bool reduced)
    {
        return now >= until + (reduced ? ToastFade : ToastOut);
    }

    // Where the corner toasts rest: the portal's prompt at the top centre, an achievement at the top right, a figure put on
    // the portal at the bottom left, any other notice at the bottom right (32 px from the canvas' edges: inside the safe
    // area's corners).
    constexpr float ToastMargin = 32;
    constexpr float AchievementToastWidth = 340, FigureToastWidth = 320, NoticeToastMaxWidth = 460;

    // --- The full screens: a left rail and the page beside it -------------------------------------------------------------

    // The rail's pages, in order. Performance is the F3 debug screen over the game: its row turns it on and goes back to
    // the game.
    enum class Page
    {
        ToyBox,
        Achievements,
        Settings,
        Controls,
        Performance,
        Count
    };
    constexpr float RailWidth = 232, RailPadTop = 28, RailPadX = 16, RailRowHeight = 46, RailGap = 6;
    constexpr float RailTitleHeight = 38; // the game's name above the rows
    // The top of rail row `index`.
    inline float RailRowTop(int index)
    {
        return RailPadTop + RailTitleHeight + float(index) * (RailRowHeight + RailGap);
    }

    // The page beside the rail: its margins and the hint bar at its bottom.
    constexpr float PageLeft = RailWidth + 36, PageRight = CanvasWidth - 36, PageTop = 32;
    constexpr float FooterHeight = 64, FooterTop = CanvasHeight - FooterHeight;

    // LB / RB on a page with no other use for them (Achievements' tabs aside): the page before or after among
    // Achievements, Settings and Controls, round the end. The Toy Box keeps LB / RB for its seats.
    inline Page PageStep(Page page, int step)
    {
        constexpr Page Cycle[] = { Page::Achievements, Page::Settings, Page::Controls };
        int at = 0;
        for (int i = 0; i < 3; i++)
            if (Cycle[i] == page)
                at = i;
        return Cycle[((at + step) % 3 + 3) % 3];
    }

    // --- The F3 menu: a side panel over the running game, the game visible on its left -------------------------------------

    constexpr float MainWidth = 420, MainLeft = CanvasWidth - MainWidth, MainPad = 28;
    constexpr float MainHeader = 40;                     // the game's name and the profile
    constexpr float MainVolumeHeight = 72;               // the volume's card
    constexpr float MainRowHeight = 52, MainSmallRowHeight = 48, MainRowGap = 6;
    constexpr float MainSeparator = 13;                  // the line before Report a problem and Quit
    constexpr float MainFooterHeight = 54;               // the hints, under a line
    constexpr int MainRows = 8; // Volume, Resume, Toy Box, Achievements, Settings, Performance, Report, Quit
    constexpr int MainSmallFrom = 6; // Report and Quit are the smaller rows, after the separator

    struct MainLayout
    {
        float rowTop[MainRows]; // the top of each row (row 0: the volume's card)
        float rowHeight[MainRows];
        float footerTop; // the hints' line
    };

    inline MainLayout Main()
    {
        MainLayout l{};
        float y = MainPad + MainHeader + 20;
        l.rowTop[0] = y;
        l.rowHeight[0] = MainVolumeHeight;
        y += MainVolumeHeight + 20;
        for (int row = 1; row < MainRows; row++)
        {
            if (row == MainSmallFrom)
                y += MainSeparator;
            l.rowTop[row] = y;
            l.rowHeight[row] = row >= MainSmallFrom ? MainSmallRowHeight : MainRowHeight;
            y += l.rowHeight[row] + MainRowGap;
        }
        l.footerTop = CanvasHeight - 22 - MainFooterHeight;
        return l;
    }

    // --- Settings: one list with group headings, the focused row's details on the right -------------------------------------

    constexpr float SettingsListTop = PageTop + 70; // under the title
    constexpr float SettingsRowHeight = 50, SettingsRowGap = 4, SettingsHeadingHeight = 34, SettingsGroupGap = 14;
    constexpr float SettingsDetailWidth = 340, SettingsDetailGap = 28;

    // A list scrolled so that the focused item shows whole: the offset (pixels, 0 or more) to use given the one in use.
    // `top` and `bottom` are the focused item's place from the list's top (its group's heading included when it is the
    // group's first row), `end` the list's height, `visible` the room it has.
    inline float ListScroll(float top, float bottom, float end, float visible, float scroll)
    {
        if (top < scroll)
            scroll = top;
        if (bottom > scroll + visible)
            scroll = bottom - visible;
        return std::clamp(scroll, 0.0f, std::max(0.0f, end - visible));
    }

    // --- Achievements: the ring and the count, the tabs, a grid of four columns ---------------------------------------------

    constexpr int AchievementColumns = 4;
    constexpr float AchievementCardHeight = 196, AchievementGap = 14;
    constexpr float AchievementGridTop = PageTop + 96 + 22; // under the ring

    // The first grid row shown so that `focusRow` is in view, given the first one shown now and how many fit.
    inline int GridFirstRow(int focusRow, int firstRow, int visibleRows, int rows)
    {
        visibleRows = std::max(visibleRows, 1);
        if (focusRow < firstRow)
            firstRow = focusRow;
        if (focusRow >= firstRow + visibleRows)
            firstRow = focusRow - visibleRows + 1;
        return std::clamp(firstRow, 0, std::max(0, rows - visibleRows));
    }

    // --- Controls (the Commands page): paged rows, a column per device ------------------------------------------------------

    constexpr float CommandsTop = PageTop + 104, CommandsPitch = 44, CommandsGap = 6;
    struct CommandsLayout
    {
        float firstRow, rowHeight;
        int first, count, capacity;
    };

    // The page that shows `selected`; `notice`: a toast takes room above the hints.
    inline CommandsLayout Commands(int rows, int selected, bool notice)
    {
        CommandsLayout l;
        l.firstRow = CommandsTop;
        l.rowHeight = CommandsPitch - CommandsGap;
        float bottom = FooterTop - (notice ? 64.0f : 12.0f);
        l.capacity = std::max(1, int((bottom - l.firstRow) / CommandsPitch));
        l.first = std::clamp(selected, 0, std::max(0, rows - 1)) / l.capacity * l.capacity;
        l.count = std::min(l.capacity, std::max(0, rows - l.first));
        return l;
    }
}
