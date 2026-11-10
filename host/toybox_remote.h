#pragma once

// The game's end of the Toy Box link (docs/runtime/host/toybox_link.md): a thread listens on the loopback address and
// writes the link file (toybox.link: port and token) in the game's data folder; the launcher's Toy Box window connects,
// sends commands and gets the seats. One window at a time. The commands are run on the main thread by the Toy Box
// (toybox.cpp: the same actions as its own seats), so the in-game Toy Box and the window always agree.
#include "host/toybox_link.h"
#include <string>
#include <vector>

namespace toyboxremote
{
    // Listens and writes `folder`/toybox.link. Once; nothing when SKY_TOYBOX_LINK=0 or the socket cannot be opened.
    void Start(const std::string& folder, const std::string& game);
    // A window is connected and said hello (an atomic: cheap every frame).
    bool Connected();
    // Main thread: the next command of the window, if any.
    bool Next(toyboxlink::Message& command);
    // Main thread: true once after a window connects (send it the whole portal).
    bool TakeHello();
    // Any thread: lines for the window (dropped when none is connected).
    void Send(const std::vector<toyboxlink::Message>& messages);
    // Says bye, closes and removes the link file (at exit).
    void Stop();
}
