#pragma once

#include <cstdint>

// The game's "place a Skylander on the portal" prompt, read from the running title so the Toy Box can open on the right
// seat by itself (docs/runtime/host/portal.md). Each game's adapter knows where that game keeps the prompt (game.h:
// Adapter::ReadPortalPrompt, games/<id>/); the tracker turns what an adapter reads, polled every frame, into "a prompt
// appeared" and "it went away", once each.
namespace portalprompt
{
    enum class Kind
    {
        None,
        Player,  // the game asks `player` (1 or 2) for a Skylander
        TooMany, // too many toys on the portal: not a request for a figure
    };

    struct Prompt
    {
        Kind kind = Kind::None;
        int player = 0;
        bool operator==(const Prompt&) const = default;
    };

    // Turns a stream of polled prompts into events. A prompt must stay the same for `Stable` polls to count (the title
    // changes its text over a few frames), and one episode opens the menu once: closing it with the prompt still up does
    // not open it again.
    class Tracker
    {
    public:
        static constexpr int Stable = 3;
        enum class Event { None, Appeared, Gone };

        // Feed one poll; `appeared` receives the prompt on Event::Appeared.
        Event Update(const Prompt& polled, Prompt& appeared);
        const Prompt& Current() const { return m_current; }

    private:
        Prompt m_current;  // the prompt that is up (confirmed)
        Prompt m_pending;  // what the last polls showed
        int m_count = 0;
    };
}
