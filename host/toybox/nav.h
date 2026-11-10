#pragma once

// The Toy Box's focus moves, Xbox style (docs/runtime/host/toybox_ux.md, "Navigation"): pure logic, no drawing, no
// collection, so that ArkeyanToyboxLookTest checks it. The model keeps the state; the drawing records where things are.
#include <algorithm>
#include <cmath>
#include <vector>

namespace toybox::nav
{
    enum class Move
    {
        Up,
        Down,
        Left,
        Right
    };

    // A focusable thing's horizontal extent in canvas pixels, as last drawn.
    struct Span
    {
        float left = 0, right = 0;
        float Centre() const { return (left + right) / 2; }
    };

    // The span under x, else the one whose edge is nearest to it; -1 when there is none.
    inline int Nearest(const std::vector<Span>& spans, float x)
    {
        int best = -1;
        float bestDistance = 0;
        for (size_t i = 0; i < spans.size(); i++)
        {
            float distance = x < spans[i].left ? spans[i].left - x : x > spans[i].right ? x - spans[i].right : 0;
            if (best < 0 || distance < bestDistance)
            {
                best = int(i);
                bestDistance = distance;
            }
        }
        return best;
    }

    // The place a row lands on when the focus comes into it: where it was left (`remembered`, when it still exists),
    // else the thing nearest under `x` (the focus being left), else the first.
    inline int Land(int remembered, int count, const std::vector<Span>& spans, float x)
    {
        if (count <= 0)
            return -1;
        if (remembered >= 0 && remembered < count)
            return remembered;
        int nearest = Nearest(spans, x);
        return nearest >= 0 && nearest < count ? nearest : 0;
    }

    // One cell of a grid.
    struct Place
    {
        int row = 0, column = 0;
    };

    // A move in a grid: Left and Right stay in the row (no wrap onto the row before or after); Up and Down go to the next
    // row that has cells, on the cell nearest `keep`, the column the player keeps (set by Left, Right and a click, not by
    // Up and Down: a short last row does not pull the focus aside for good). Returns the new cell, or -1 when the move
    // leaves the grid (Up from its first row, Down from its last).
    inline int GridMove(const std::vector<Place>& cells, int cursor, int keep, Move move)
    {
        if (cells.empty() || cursor < 0 || cursor >= int(cells.size()))
            return -1;
        const Place at = cells[size_t(cursor)];
        if (move == Move::Left || move == Move::Right)
        {
            int next = cursor + (move == Move::Right ? 1 : -1);
            return next >= 0 && next < int(cells.size()) && cells[size_t(next)].row == at.row ? next : cursor;
        }
        int step = move == Move::Down ? 1 : -1;
        int lastRow = cells.back().row;
        for (int row = at.row + step; row >= 0 && row <= lastRow; row += step)
        {
            int best = -1;
            for (size_t i = 0; i < cells.size(); i++)
            {
                if (cells[i].row == row &&
                    (best < 0 || std::abs(cells[i].column - keep) < std::abs(cells[size_t(best)].column - keep)))
                    best = int(i);
            }
            if (best >= 0)
                return best;
        }
        return -1;
    }

    // LB / RB: the seat before or after, never round the end.
    inline int SeatStep(int seat, int step, int count) { return std::max(0, std::min(seat + step, count - 1)); }
}
