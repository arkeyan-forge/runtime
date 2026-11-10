// Checks the in-game interface's pure logic (host/toybox/look.h, nav.h): the toast's slide and fade, with and without
// reduced motion, the game's accent and the greys' contrast, the F3 menu's panel, the rail, the Settings list's scroll,
// the Achievements grid, Controls' pages, and the Toy Box's focus moves. usage:
// ArkeyanToyboxLookTest
#include "host/toybox/look.h"
#include "host/toybox/nav.h"
#include <cmath>
#include <cstdio>
#include <utility>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    using namespace toybox::look;
    constexpr uint64_t Ms = 1'000'000;
}

int main()
{
    // The safe area is 5 % of each side of the 1280x720 canvas.
    CHECK(SafeLeft == 64 && SafeRight == 1216 && SafeTop == 36 && SafeBottom == 684);

    // The toast, shown at 1 s for 4 s: slides in over ToastIn, rests, fades out over ToastOut.
    const uint64_t since = 1000 * Ms, until = since + 4000 * Ms;
    Motion m = ToastMotion(since, since, until, false);
    CHECK(m.alpha == 0 && m.offset == ToastRise && m.moving);
    float lastAlpha = -1, lastOffset = ToastRise + 1;
    for (uint64_t t = since; t < since + ToastIn; t += 10 * Ms)
    {
        m = ToastMotion(t, since, until, false);
        CHECK(m.moving && m.alpha > lastAlpha && m.offset < lastOffset && m.alpha >= 0 && m.alpha <= 1);
        lastAlpha = m.alpha;
        lastOffset = m.offset;
    }
    m = ToastMotion(since + ToastIn, since, until, false);
    CHECK(m.alpha == 1 && m.offset == 0 && !m.moving); // at rest: no redraw
    m = ToastMotion(until - 1, since, until, false);
    CHECK(m.alpha == 1 && m.offset == 0 && !m.moving);
    // Eased out: more than half way after a third of the slide.
    CHECK(ToastMotion(since + ToastIn / 3, since, until, false).alpha > 0.5f);
    lastAlpha = 2;
    for (uint64_t t = until; t < until + ToastOut; t += 10 * Ms)
    {
        m = ToastMotion(t, since, until, false);
        CHECK(m.moving && m.alpha < lastAlpha && m.alpha >= 0);
        lastAlpha = m.alpha;
    }
    m = ToastMotion(until + ToastOut, since, until, false);
    CHECK(m.alpha == 0 && !m.moving);
    CHECK(!ToastGone(until + ToastOut - 1, until, false) && ToastGone(until + ToastOut, until, false));
    // A clock read before `since` (another thread's message) counts as its start.
    CHECK(ToastMotion(since - 5 * Ms, since, until, false).alpha == 0);

    // Reduced motion: a shorter fade, never a slide.
    for (uint64_t t = since; t <= until + ToastFade; t += 5 * Ms)
        CHECK(ToastMotion(t, since, until, true).offset == 0);
    CHECK(ToastMotion(since + ToastFade, since, until, true).alpha == 1);
    CHECK(ToastMotion(since + ToastFade / 2, since, until, true).moving);
    CHECK(ToastGone(until + ToastFade, until, true) && !ToastGone(until + ToastFade - 1, until, true));

    // The accent: each game's glow (Skylauncher's StyleOf) reads at 4.5:1 or more on the rail and the cards, lightened
    // only as much as needed (Spyro's Adventure's blue as it is, Trap Team's red toward white), never darkened.
    CHECK(std::fabs(Contrast(0x000000, 0xFFFFFF) - 21) < 0.01 &&
          Contrast(0x26262C, 0x6BBAFF) == Contrast(0x6BBAFF, 0x26262C));
    CHECK(Mix(0x000000, 0xFFFFFF, 0.5f) == 0x808080 && Mix(0x123456, 0xFFFFFF, 0) == 0x123456);
    for (uint32_t glow : { 0x6BBAFFu, 0xFFB43Au, 0xBFE04Au, 0xDE0018u, 0xF07008u, 0x58A8D8u })
    {
        uint32_t accent = ReadableAccent(glow);
        CHECK(Contrast(accent, Colour::Surface) >= 4.5);
        for (int shift : { 16, 8, 0 })
            CHECK((accent >> shift & 0xFF) >= (glow >> shift & 0xFF));
        if (Contrast(glow, Colour::Surface) >= 4.5)
            CHECK(accent == glow);
        // On the page grey and the focused rows' grey it still reads as a control's part (3:1).
        CHECK(Contrast(accent, Colour::Page) >= 4.5 && Contrast(accent, Colour::Raised2) >= 3);
    }
    CHECK(ReadableAccent(0x6BBAFF) == 0x6BBAFF && ReadableAccent(0xDE0018) != 0xDE0018);
    // The text greys read on every surface they sit on.
    for (uint32_t surface : { Colour::Page, Colour::Surface, Colour::Raised2 })
        CHECK(Contrast(Colour::Text, surface) >= 7 && Contrast(Colour::TextSoft, surface) >= 4.5 &&
              Contrast(Colour::TextDim, surface) >= 4.5);

    // The F3 menu's side panel: its rows top to bottom without overlap, all above its hints, Report and Quit after the
    // separator, inside the canvas at every window size (the overlay is letterboxed).
    {
        MainLayout l = Main();
        CHECK(MainLeft + MainWidth == CanvasWidth && MainLeft > CanvasWidth / 2);
        for (int row = 0; row < MainRows; row++)
        {
            CHECK(l.rowTop[row] >= MainPad + MainHeader && l.rowHeight[row] >= 44);
            if (row > 0)
                CHECK(l.rowTop[row] >= l.rowTop[row - 1] + l.rowHeight[row - 1] + MainRowGap);
        }
        CHECK(l.rowTop[MainSmallFrom] >= l.rowTop[MainSmallFrom - 1] + l.rowHeight[MainSmallFrom - 1] + MainSeparator);
        CHECK(l.rowTop[MainRows - 1] + l.rowHeight[MainRows - 1] + 8 <= l.footerTop);
        CHECK(l.footerTop + MainFooterHeight <= CanvasHeight);
    }

    // The rail: its five pages under the game's name, above the profile at the bottom.
    CHECK(RailRowTop(0) >= RailPadTop + RailTitleHeight);
    CHECK(RailRowTop(int(Page::Count) - 1) + RailRowHeight + 16 <= CanvasHeight - 64);
    CHECK(PageLeft > RailWidth && PageRight < CanvasWidth && FooterTop + FooterHeight == CanvasHeight);
    // LB / RB: Achievements, Settings, Controls, round the end; the Toy Box (LB / RB are its seats) and Performance
    // (over the game) are not pages of that cycle.
    CHECK(PageStep(Page::Achievements, 1) == Page::Settings && PageStep(Page::Settings, 1) == Page::Controls);
    CHECK(PageStep(Page::Controls, 1) == Page::Achievements && PageStep(Page::Achievements, -1) == Page::Controls);
    CHECK(PageStep(Page::ToyBox, 1) == Page::Settings);

    // Settings: the list scrolls only as far as the focused row needs, never past its ends.
    CHECK(ListScroll(0, 50, 900, 500, 0) == 0); // the first row: no scroll
    CHECK(ListScroll(600, 650, 900, 500, 0) == 150); // a row below the fold: just enough
    CHECK(ListScroll(100, 150, 900, 500, 150) == 100); // back up: its top (or its heading's) at the top
    CHECK(ListScroll(850, 900, 900, 500, 0) == 400); // the last row: the end at the bottom
    CHECK(ListScroll(200, 250, 300, 500, 80) == 0); // a short list never scrolls
    // ...and the room it has holds the rows under the title.
    CHECK(SettingsListTop + 6 * (SettingsRowHeight + SettingsRowGap) < FooterTop);
    CHECK(PageRight - PageLeft - SettingsDetailWidth - SettingsDetailGap >= 560);

    // Achievements: the grid keeps the focused row in view, as few rows moved as possible.
    CHECK(GridFirstRow(0, 0, 2, 10) == 0 && GridFirstRow(1, 0, 2, 10) == 0 && GridFirstRow(2, 0, 2, 10) == 1);
    CHECK(GridFirstRow(5, 1, 2, 10) == 4 && GridFirstRow(3, 4, 2, 10) == 3 && GridFirstRow(9, 0, 2, 10) == 8);
    CHECK(GridFirstRow(0, 5, 3, 2) == 0); // fewer rows than fit
    CHECK(AchievementGridTop + AchievementCardHeight < FooterTop);

    // Controls keeps a readable row height for dense bindings, and every selected row appears on a reachable page.
    for (bool notice : { false, true })
        for (int rows = 1; rows <= 40; rows++)
            for (int selected = 0; selected < rows; selected++)
            {
                CommandsLayout l = Commands(rows, selected, notice);
                CHECK(l.first <= selected && selected < l.first + l.count);
                CHECK(l.count > 0 && l.count <= l.capacity && l.rowHeight >= 32);
                CHECK(l.firstRow + l.count * CommandsPitch <= FooterTop - (notice ? 64 : 12) + 0.01f);
                if (l.first + l.count < rows)
                    CHECK(Commands(rows, l.first + l.capacity, notice).first == l.first + l.capacity);
                if (l.first > 0)
                    CHECK(Commands(rows, l.first - l.capacity, notice).first == l.first - l.capacity);
            }

    // The Toy Box's focus (nav.h). A grid of 7 columns: 17 cards, so rows of 7, 7 and 3.
    using namespace toybox::nav;
    std::vector<Place> cells;
    for (int i = 0; i < 17; i++)
        cells.push_back({ i / 7, i % 7 });
    // Left and Right stay in the row: no wrap onto the row before or after.
    CHECK(GridMove(cells, 0, 0, Move::Left) == 0);
    CHECK(GridMove(cells, 6, 6, Move::Right) == 6);
    CHECK(GridMove(cells, 7, 0, Move::Left) == 7);
    CHECK(GridMove(cells, 3, 3, Move::Right) == 4);
    // Up and Down keep the column.
    CHECK(GridMove(cells, 5, 5, Move::Down) == 12);
    CHECK(GridMove(cells, 12, 5, Move::Up) == 5);
    // The short last row takes the nearest card, and the kept column comes back on the way up.
    CHECK(GridMove(cells, 12, 5, Move::Down) == 16);
    CHECK(GridMove(cells, 16, 5, Move::Up) == 12);
    // Up from the first row and Down from the last leave the grid (the bar above; nothing below).
    CHECK(GridMove(cells, 2, 2, Move::Up) == -1);
    CHECK(GridMove(cells, 15, 1, Move::Down) == -1);
    CHECK(GridMove({}, 0, 0, Move::Down) == -1);
    // A grid with a fold row (one cell, a whole row) between two shelves: Down from column 4 lands on it, then back.
    std::vector<Place> folded = { { 0, 0 }, { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 0 }, { 2, 0 }, { 2, 1 } };
    CHECK(GridMove(folded, 4, 4, Move::Down) == 5);
    CHECK(GridMove(folded, 5, 4, Move::Down) == 7);
    CHECK(GridMove(folded, 5, 4, Move::Up) == 4);

    // A row is entered where it was left; the first time, on the thing nearest under the focus.
    // Trap Team's seats row: five seats, then Add a figure and Manage stacked at the end (one span each).
    std::vector<Span> seatsRow = { { 32, 230 }, { 242, 440 }, { 452, 650 }, { 662, 860 }, { 872, 1070 }, { 1140, 1248 },
        { 1140, 1248 } };
    std::vector<Span> bar = { { 728, 1088 }, { 1098, 1248 } };
    std::vector<Span> columns;
    for (int c = 0; c < 7; c++)
        columns.push_back({ 32 + c * 176.0f, 32 + c * 176.0f + 164 });
    CHECK(Nearest(bar, 140) == 0); // Player 1 above: the search field, nearest
    CHECK(Nearest(bar, 1180) == 1); // Manage above: Filters, under it
    CHECK(Nearest(columns, bar[1].Centre()) == 6); // Filters: the last column under it
    CHECK(Nearest(columns, bar[0].Centre()) == 5); // the search field: the card under its middle
    CHECK(Nearest(seatsRow, columns[0].Centre()) == 0); // from the first card up: Player 1
    CHECK(Nearest(seatsRow, columns[2].Centre()) == 2);
    CHECK(Nearest(seatsRow, 238) == 1); // in a gap: the nearer edge
    CHECK(Land(3, 7, seatsRow, 1180) == 3); // remembered: where it was left
    CHECK(Land(-1, 2, bar, 1180) == 1); // never visited: under the focus
    CHECK(Land(9, 7, seatsRow, 40) == 0); // the remembered place is gone: under the focus
    CHECK(Land(-1, 2, {}, 500) == 0 && Land(0, 0, bar, 500) == -1); // nothing drawn yet: the first; an empty row: none
    CHECK(Nearest({}, 10) == -1);

    // LB / RB: the seat before or after, never round the end (no jump from Player 1 to the last seat).
    CHECK(SeatStep(0, -1, 5) == 0 && SeatStep(4, 1, 5) == 4 && SeatStep(2, 1, 5) == 3 && SeatStep(2, -1, 5) == 1);

    printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
