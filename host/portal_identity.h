#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Which Portal of Power a reply to the 'R' (reset) command names, and what a game thinks of it. Every portal model
// answers 'R' with its own id (two bytes, four on SuperChargers and Imaginators); the first byte is 1 for Spyro's
// Adventure and Giants, 2 for Swap Force and later. The ids and the per-game notes are public community knowledge
// (docs/runtime/host/portal.md, THIRD_PARTY_NOTICES.md). Nothing here is read from game data.

struct PortalModel
{
    uint8_t id[4];
    size_t idLength;          // 2 or 4
    const char* name;         // "Spyro's Adventure portal (Xbox 360)"
};

// What a game does with a portal.
enum class PortalVerdict
{
    Works,    // the game's own check passes
    Warns,    // the game warns (Swap Force with an older portal)
    Rejected, // the game refuses it ("out of date", "wrong portal")
    Unknown,  // no id, or nothing is known about this game with this model
};

// The model whose id starts `id`; null if the id is not in the table.
const PortalModel* FindPortalModel(const uint8_t* id, size_t length);

// "01 28", the bytes as hex.
std::string PortalIdText(const uint8_t* id, size_t length);

// What `game` ("ssa", "giants"...) does with a portal that answered `id`. `reason` (when not null) says why.
PortalVerdict PortalVerdictFor(const char* game, const uint8_t* id, size_t length, std::string* reason = nullptr);

// The id the emulated portal answers for `game`: the Xbox 360 portal of the game's own generation, which is the one
// its retail disc was made for. Two bytes (SSA, Giants), or four (SuperChargers, Imaginators). Unknown games get the
// newest two-byte Xbox 360 id, which every older game accepts too.
const PortalModel& EmulatedPortalModel(const char* game);

// One line for the log and the launcher: "01 28 Spyro's Adventure portal (Xbox 360)", or "02 7F unknown portal".
// With a game: "...; Spyro's Adventure: works" / ": the game rejects it (...)".
std::string DescribePortal(const char* game, const uint8_t* id, size_t length);

// The id in a portal's answer to 'R' (`report` starts at the 'R'): four bytes when the table has that four-byte id,
// else two. False if the report is not an 'R' answer or is too short.
bool PortalIdFromReset(const uint8_t* report, size_t size, uint8_t id[4], size_t& length);
