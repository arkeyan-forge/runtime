#pragma once

// The Toy Box link: the messages between a running game and the launcher's Toy Box window (docs/runtime/host/toybox_link.md).
// One text line per message, fields separated by tabs, ended by '\n'; the first field is the verb. No sockets here (host/
// toybox_socket.h carries the lines), no game: the same file is the launcher's (src/shared/toybox_link.h, copied by its
// tools/sync_collection.sh), so both sides parse and write the same words.
//
//   window -> game                          game -> window
//   hello <version> <token> <client>        hello <version> <game id>
//   put <seat> <path>                       refused version <its version> | token | busy   (then it closes)
//   off <seat>                              seat <i> empty
//   swap                                    seat <i> virtual <path> <bottom path> <taken 0|1>
//   clear                                   seat <i> real <toy type> <variant> <known 0|1>
//                                           sync <seat count>        (the seats sent since the last sync are the portal now)
//                                           done <verb> <seat> ok | <refusal>
//                                           bye                      (the game is closing)
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace toyboxlink
{
    // Bumped whenever a message changes meaning; both sides must speak the same one ("refused version" otherwise).
    constexpr int Version = 1;
    // A longer line is an error: the connection is closed.
    constexpr size_t MaxLine = 4096;
    // The file the game writes in its data folder while it listens (beside portal.txt), removed when it quits.
    constexpr const char* FileName = "toybox.link";

    struct Message
    {
        std::string verb;
        std::vector<std::string> fields;
    };

    // The line for `message`, '\n' included; empty when a field holds a tab or a line break (it cannot be sent).
    std::string Format(const Message& message);
    // One line without its '\n' (a trailing '\r' is dropped). False for an empty line.
    bool Parse(std::string_view line, Message& message);

    // Bytes in, lines out.
    class LineBuffer
    {
    public:
        void Append(const char* data, size_t size);
        // The next complete line, without its '\n'. False when none is complete yet.
        bool Next(std::string& line);
        // A line grew past MaxLine without its end: the peer is not speaking this protocol.
        bool Overflow() const { return m_overflow; }

    private:
        std::string m_data;
        bool m_overflow = false;
    };

    // What one seat of the game's portal holds, as the game's Toy Box sees it.
    struct Seat
    {
        enum class Kind
        {
            Empty,
            Virtual, // a dump of the collection on the emulated part of the portal
            Real     // a toy on the real portal (read only)
        } kind = Kind::Empty;
        std::string path, path2; // Virtual: the dump, a swapper's bottom (empty: none)
        bool taken = false;      // Virtual: the game has read it since it was put on
        uint16_t type = 0, variant = 0; // Real: what the game read from the toy
        bool known = false;             // Real: the toy's type was read
        bool operator==(const Seat&) const = default;
    };
    Message SeatMessage(int index, const Seat& seat);
    // False when `message` is not a well-formed seat line.
    bool ParseSeat(const Message& message, int& index, Seat& seat);

    // Why a command was refused (the window words it in its own language).
    namespace refusal
    {
        constexpr const char* Real = "real";           // the seat holds a real toy
        constexpr const char* Unknown = "unknown";     // no such figure in the collection, or no such seat
        constexpr const char* Busy = "busy";           // another program uses the figure
        constexpr const char* CannotRead = "cannot_read"; // the game does not read that figure on that seat
        constexpr const char* Already = "already";     // it is already there
        constexpr const char* Full = "full";           // the portal has no free slot
        constexpr const char* NoPortal = "no_portal";  // this game has no virtual portal now
    }

    // The link file: "skytoybox <version> <port> <token> <pid>".
    struct Endpoint
    {
        int version = 0;
        int port = 0;
        std::string token;
        uint32_t pid = 0;
    };
    std::string FormatEndpoint(const Endpoint& endpoint);
    bool ParseEndpoint(std::string_view text, Endpoint& endpoint);
    // 32 hexadecimal digits from the system's random source.
    std::string NewToken();
}
