#pragma once

// The Toy Box link's transport (docs/runtime/host/toybox_link.md): a TCP connection on the loopback address only
// (127.0.0.1), so nothing outside this computer can reach it; the port is chosen by the system and written, with a token,
// in the game's link file. BSD sockets on macOS and Linux, Winsock on Windows. Blocking sockets: Wait says when one is
// readable. The same file is the launcher's (src/shared/toybox_socket.h).
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace toyboxsocket
{
    using Handle = intptr_t;
    constexpr Handle Invalid = -1;

    // Once per process before any other call (Winsock); true elsewhere.
    bool Startup();
    // A listener on 127.0.0.1 with a port of the system's choosing (`port`). Invalid on failure.
    Handle Listen(int& port);
    // The next connection waiting on `listener` (call when Wait says it is readable). Invalid if none.
    Handle Accept(Handle listener);
    // A connection to 127.0.0.1:`port`, within `timeoutMs`. Invalid when nothing listens there.
    Handle Connect(int port, int timeoutMs);
    // Waits up to `timeoutMs` for one of `handles` (Invalid ones are skipped) to be readable or closed: bit i set for
    // handles[i]. 0 on timeout.
    unsigned Wait(const Handle* handles, int count, int timeoutMs);
    // What is there to read: the byte count, 0 when the peer closed, -1 on an error.
    long Receive(Handle handle, char* buffer, size_t capacity);
    // Everything, or false (the peer is gone or did not read for a second).
    bool Send(Handle handle, std::string_view data);
    void Close(Handle handle);
}
