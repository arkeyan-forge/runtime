#pragma once

// Whether each seat's figure has played in the current level (docs/runtime/host/toybox_ux.md, "Out of health"): the Toy
// Box marks a figure out of health only when the game asks for its seat after that. The game also asks with the figure
// on its seat when an adventure or a level starts and after a portal reset (Swap Force asks for Player 1 right after a
// new adventure's save): none of those asks is a death. Plain C++, so a test can drive it (ArkeyanToyboxInPlayTest).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace toybox
{
    class InPlay
    {
    public:
        // The figure must play this long in the level, its seat not asked for, before an ask may mark it: an ask in the
        // first seconds of a level is the game seating its players, not a death.
        static constexpr uint64_t PlayNs = 20'000'000'000ull;

        // What the frame says of one seat: the dump on it and when it was put there (Model::SeatState), whether the
        // title took it, whether the game asks for that seat, and whether the game is playing a level (not one of its
        // menus, no movie).
        struct Seat
        {
            std::string_view path;
            uint64_t placedAt = 0;
            bool taken = false;
            bool asked = false;
            bool playing = false;
        };

        // A level starts, or the game resets the portal: every figure starts over.
        void StartOver()
        {
            for (State& s : m_seats)
                s = State{ s.path, s.placedAt };
        }

        // Once a frame for each seat, before its ask is acted on.
        void Update(size_t seat, const Seat& in, uint64_t now)
        {
            if (seat >= m_seats.size())
                m_seats.resize(seat + 1);
            State& s = m_seats[seat];
            if (in.path != s.path || in.placedAt != s.placedAt)
                s = State{ std::string(in.path), in.placedAt }; // another figure, or the same one put back
            if (in.path.empty() || !in.taken || in.asked || !in.playing)
            {
                s.since = 0; // the play stops; what it already earned stays
                return;
            }
            if (s.since == 0)
                s.since = now;
            if (now - s.since >= PlayNs)
                s.played = true;
        }

        // An ask for this seat now may mark its figure out of health.
        bool Played(size_t seat) const { return seat < m_seats.size() && m_seats[seat].played; }

    private:
        struct State
        {
            std::string path;
            uint64_t placedAt = 0;
            uint64_t since = 0; // when the current stretch of play began, 0: not playing
            bool played = false;
        };
        std::vector<State> m_seats;
    };
}
