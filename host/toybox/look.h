#pragma once

// The in-game interface's design (docs/runtime/host/ingame_ui.md): one palette shared by every game, one type scale, one
// spacing scale, the TV safe area, the toast's motion and the F3 menu's rows. draw.cpp draws with these; the parts that are
// pure logic are here so that look_test.cpp checks them without fonts or a window. Sizes are canvas pixels (1280x720, the
// overlay is letterboxed over the game at any window size).
#include <algorithm>
#include <cstdint>

namespace toybox::look
{
    constexpr float CanvasWidth = 1280, CanvasHeight = 720;

    // TV action-safe area: 5 % of each side. Everything drawn over the game stays inside it.
    constexpr float SafeLeft = 64, SafeTop = 36, SafeRight = CanvasWidth - 64, SafeBottom = CanvasHeight - 36;

    // Colours (0xRRGGBB). A dark, slightly blue glass that lets the game's own art carry the colour; gold is the only
    // accent, white the focus.
    namespace Colour
    {
        constexpr uint32_t Scrim = 0x05060D;      // over the game behind a menu
        constexpr uint32_t Surface = 0x121527;    // panels, toasts
        constexpr uint32_t Raised = 0x1B1F38;     // cards, rows, chips
        constexpr uint32_t Raised2 = 0x262A4A;    // key caps, controls on a raised surface
        constexpr uint32_t Focus = 0x2C3263;      // a focused card or row
        constexpr uint32_t Hairline = 0x2E3360;   // borders
        constexpr uint32_t Control = 0x454A78;    // a switch's track when off, stepper marks
        constexpr uint32_t Well = 0x0B0D1C;       // fields, slider tracks, picture backings
        constexpr uint32_t Text = 0xF5F6FA, TextSoft = 0xCDD0E4, TextDim = 0xA2A6C4, TextDimmer = 0x8085A6;
        constexpr uint32_t Accent = 0xFFC233, AccentInk = 0x2A1A00, AccentText = 0xFFD970;
        constexpr uint32_t FocusRing = 0xFFFFFF;
        constexpr uint32_t Success = 0x48D17A, Danger = 0xF85149, DangerText = 0xFFB4AE, Warning = 0xFFB454;
    }

    // Type scale (Outfit Black for labels and titles, Outfit Bold for running text).
    namespace Type
    {
        constexpr float Caption = 12, Small = 14, Body = 16, Label = 17, Heading = 22, Title = 28;
    }

    // Spacing scale and corner radii.
    namespace Space
    {
        constexpr float XS = 4, S = 8, M = 12, L = 16, XL = 24, XXL = 32;
    }
    namespace Radius
    {
        constexpr float Key = 6, Control = 10, Card = 14, Panel = 18;
    }

    // --- The toast: a soft slide and fade in, a fade out ----------------------------------------------------------------

    constexpr uint64_t ToastIn = 240'000'000, ToastOut = 200'000'000; // nanoseconds
    constexpr uint64_t ToastFade = 120'000'000;                        // in and out, with reduced motion
    constexpr float ToastRise = 20;                                    // pixels it slides in from

    struct Motion
    {
        float alpha = 1;   // 0-1
        float offset = 0;  // pixels away from its place, towards the edge it came from
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

    // --- The F3 menu: a sheet on the left of the safe area -----------------------------------------------------------------

    constexpr float MenuWidth = 520;
    constexpr float MenuHeader = 76; // the title, above the first row
    constexpr float MenuFooter = 52; // the note and the button hints, under the last row
    constexpr float MenuPitchMax = 46, MenuGap = 4;

    struct MenuLayout
    {
        float left, top, right, bottom; // the sheet
        float firstRow;                 // y of the first row
        float pitch;                    // from one row to the next
        float rowHeight;                // pitch less the gap
    };

    // The sheet for 10-14 rows: full safe height, with Debug screen and Commands above the hints.
    inline MenuLayout Menu(int rows)
    {
        MenuLayout l;
        l.left = SafeLeft;
        l.right = SafeLeft + MenuWidth;
        l.top = SafeTop;
        l.bottom = SafeBottom;
        l.firstRow = l.top + MenuHeader;
        float room = l.bottom - MenuFooter - l.firstRow;
        l.pitch = std::min(MenuPitchMax, room / float(std::max(rows, 1)));
        l.rowHeight = l.pitch - MenuGap;
        return l;
    }

    // Commands uses pages rather than shrinking its text and switches to fit every binding on one screen.
    constexpr float CommandsHeader = 88, CommandsPitch = 32;
    struct CommandsLayout
    {
        float firstRow, rowHeight;
        int first, count, capacity;
    };

    inline CommandsLayout Commands(int rows, int selected, bool notice)
    {
        CommandsLayout l;
        l.firstRow = SafeTop + CommandsHeader;
        l.rowHeight = CommandsPitch - MenuGap;
        float footer = notice ? 144.0f : MenuFooter;
        l.capacity = std::max(1, int((SafeBottom - footer - l.firstRow) / CommandsPitch));
        l.first = std::clamp(selected, 0, std::max(0, rows - 1)) / l.capacity * l.capacity;
        l.count = std::min(l.capacity, std::max(0, rows - l.first));
        return l;
    }
}
