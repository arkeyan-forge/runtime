// Checks the in-game interface's pure logic (host/toybox/look.h, nav.h): the toast's slide and fade, with and without
// reduced motion, the F3 menu and Commands rows fitting the TV safe area, and the Toy Box's focus moves. usage:
// SSAToyboxLookTest
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

    // The F3 menu: 10 to 14 rows, including Debug screen and Commands, above the hints and readable.
    for (int rows = 10; rows <= 14; rows++)
    {
        MenuLayout l = Menu(rows);
        CHECK(l.left >= SafeLeft && l.top >= SafeTop && l.bottom <= SafeBottom && l.right <= SafeRight);
        CHECK(l.firstRow + rows * l.pitch <= l.bottom - MenuFooter + 0.01f);
        CHECK(l.pitch <= MenuPitchMax && l.rowHeight >= 32 && l.rowHeight == l.pitch - MenuGap);
        // The overlay is a fixed canvas, letterboxed at every window size: the last row stays above the hints.
        for (auto size : { std::pair{ 640.0f, 480.0f }, { 1280.0f, 720.0f }, { 1920.0f, 1080.0f }, { 2560.0f, 1080.0f },
                 { 2560.0f, 1440.0f } })
        {
            float scale = std::min(size.first / CanvasWidth, size.second / CanvasHeight);
            float offset = (size.second - CanvasHeight * scale) / 2;
            float lastBottom = offset + (l.firstRow + (rows - 1) * l.pitch + l.rowHeight) * scale;
            float hintTop = offset + (l.bottom - MenuFooter / 2 - 13) * scale;
            CHECK(lastBottom < hintTop && offset + l.bottom * scale <= size.second);
        }
    }
    CHECK(Menu(4).pitch == MenuPitchMax); // few rows: the full height, not stretched
    CHECK(Menu(14).pitch < Menu(13).pitch);

    // Commands keeps a readable row height for dense bindings, and every selected row appears on a reachable page.
    for (bool notice : { false, true })
        for (int rows = 1; rows <= 40; rows++)
            for (int selected = 0; selected < rows; selected++)
            {
                CommandsLayout l = Commands(rows, selected, notice);
                CHECK(l.first <= selected && selected < l.first + l.count);
                CHECK(l.count > 0 && l.count <= l.capacity && l.rowHeight >= 24);
                CHECK(l.firstRow + l.count * CommandsPitch <= SafeBottom - (notice ? 144 : MenuFooter));
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
    CHECK(Nearest(bar, 140) == 0);                                  // Player 1 above: the search field, nearest
    CHECK(Nearest(bar, 1180) == 1);                                 // Manage above: Filters, under it
    CHECK(Nearest(columns, bar[1].Centre()) == 6);                  // Filters: the last column under it
    CHECK(Nearest(columns, bar[0].Centre()) == 5);                  // the search field: the card under its middle
    CHECK(Nearest(seatsRow, columns[0].Centre()) == 0);             // from the first card up: Player 1
    CHECK(Nearest(seatsRow, columns[2].Centre()) == 2);
    CHECK(Nearest(seatsRow, 238) == 1); // in a gap: the nearer edge
    CHECK(Land(3, 7, seatsRow, 1180) == 3);                         // remembered: where it was left
    CHECK(Land(-1, 2, bar, 1180) == 1);                             // never visited: under the focus
    CHECK(Land(9, 7, seatsRow, 40) == 0);                           // the remembered place is gone: under the focus
    CHECK(Land(-1, 2, {}, 500) == 0 && Land(0, 0, bar, 500) == -1); // nothing drawn yet: the first; an empty row: none
    CHECK(Nearest({}, 10) == -1);

    // LB / RB: the seat before or after, never round the end (no jump from Player 1 to the last seat).
    CHECK(SeatStep(0, -1, 5) == 0 && SeatStep(4, 1, 5) == 4 && SeatStep(2, 1, 5) == 3 && SeatStep(2, -1, 5) == 1);

    printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
