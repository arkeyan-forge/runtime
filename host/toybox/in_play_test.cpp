// Checks when an ask for a seat may mark its figure out of health (host/toybox/in_play.h): only once the figure has
// played in the current level; never for an ask at a level's start, after a level change or a portal reset. usage:
// ArkeyanToyboxInPlayTest
#include "host/toybox/in_play.h"
#include <cstdio>

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

    constexpr uint64_t S = 1'000'000'000ull;

    // `seconds` of frames (30 a second) of one seat from `from`; returns the time after them.
    uint64_t Run(toybox::InPlay& play, const toybox::InPlay::Seat& seat, uint64_t from, double seconds)
    {
        const uint64_t end = from + uint64_t(seconds * double(S));
        for (uint64_t t = from; t < end; t += S / 30)
            play.Update(0, seat, t);
        return end;
    }
}

int main()
{
    using Seat = toybox::InPlay::Seat;
    const Seat playing{ "Spyro.sky", 0, true, false, true };
    Seat asked = playing;
    asked.asked = true;

    // A seat filled before the level started: the game asks for it as the level begins (a new adventure): not marked.
    {
        toybox::InPlay play;
        uint64_t t = Run(play, { "Spyro.sky", 0, true, false, false }, S, 30); // the menus: not a level
        play.StartOver(); // the level starts
        t = Run(play, playing, t, 5);
        t = Run(play, asked, t, 1);
        CHECK(!play.Played(0));
        // Taken again after the ask and played long enough: a later ask is a death.
        t = Run(play, playing, t, 19);
        CHECK(!play.Played(0));
        t = Run(play, playing, t, 2);
        CHECK(play.Played(0));
        Run(play, asked, t, 1);
        CHECK(play.Played(0));
    }

    // A level change: the figure played in the last level, the new one's first ask is not a death.
    {
        toybox::InPlay play;
        uint64_t t = Run(play, playing, S, 40);
        CHECK(play.Played(0));
        play.StartOver();
        Run(play, asked, t, 1);
        CHECK(!play.Played(0));
    }

    // A portal reset (the game loading): the same.
    {
        toybox::InPlay play;
        uint64_t t = Run(play, playing, S, 25);
        CHECK(play.Played(0));
        play.StartOver();
        t = Run(play, playing, t, 3);
        CHECK(!play.Played(0));
    }

    // A menu level, a movie, a figure the title has not taken yet: none of it is play.
    {
        toybox::InPlay play;
        uint64_t t = Run(play, { "Spyro.sky", 0, true, false, false }, S, 60);
        t = Run(play, { "Spyro.sky", 0, false, false, true }, t, 60);
        CHECK(!play.Played(0));
        // The play must be one stretch: 15 s, a movie, 15 s again is not enough.
        t = Run(play, playing, t, 15);
        t = Run(play, { "Spyro.sky", 0, true, false, false }, t, 5);
        t = Run(play, playing, t, 15);
        CHECK(!play.Played(0));
        // Once played, a movie or an ask afterwards does not take it back.
        t = Run(play, playing, t, 6);
        CHECK(play.Played(0));
        Run(play, { "Spyro.sky", 0, true, false, false }, t, 30);
        CHECK(play.Played(0));
    }

    // Another figure on the seat, or the same one put back (a new placedAt): it has not played yet.
    {
        toybox::InPlay play;
        uint64_t t = Run(play, playing, S, 30);
        CHECK(play.Played(0));
        t = Run(play, { "Eruptor.sky", t, true, false, true }, t, 1);
        CHECK(!play.Played(0));
        t = Run(play, { "Spyro.sky", t, true, false, true }, t, 1);
        CHECK(!play.Played(0));
        play.Update(0, {}, t); // the seat emptied
        CHECK(!play.Played(0));
    }

    // Seats apart: one seat's play is its own; an unknown seat has not played.
    {
        toybox::InPlay play;
        for (uint64_t t = S; t < 30 * S; t += S / 30)
        {
            play.Update(0, playing, t);
            play.Update(1, { "Eruptor.sky", 0, true, true, true }, t); // its seat asked for the whole time
        }
        CHECK(play.Played(0) && !play.Played(1) && !play.Played(5));
    }

    if (g_failures == 0)
        printf("ArkeyanToyboxInPlayTest: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
