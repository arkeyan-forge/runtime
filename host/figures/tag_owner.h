#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

// Who a toy belongs to: its block 6 (plain), written by the games from Swap Force on when they register a toy, zero on
// a toy only the earlier games used. A kind byte, then the owner's id, least significant byte first: kind 1 the id a
// game made for an offline profile (4 bytes; Imaginators writes kind 6), kind 0x80 an Xbox Live account's XUID (8
// bytes) [V: SuperChargers' writer, docs/runtime/host/figures.md "Owner"]; another platform's kinds are not known [?].
// A game counts a toy whose owner is not its player's as someone else's (SuperChargers leaves it out of its collection
// until the toy's "Ownership").
namespace figures
{
    struct TagOwner
    {
        uint8_t kind = 0; // 0: none
        uint64_t id = 0;
    };

    inline TagOwner ReadTagOwner(const uint8_t* block6)
    {
        TagOwner owner;
        owner.kind = block6[0];
        for (int i = 8; i >= 1; i--)
            owner.id = owner.id << 8 | block6[i];
        return owner;
    }

    // "none", "offline 1F3AB27A", "Xbox Live 0009000001234567", "kind 02, id ...": for the log.
    inline void FormatTagOwner(const TagOwner& owner, char* text, size_t size)
    {
        const auto id = static_cast<unsigned long long>(owner.id);
        if (owner.kind == 0 && owner.id == 0)
            snprintf(text, size, "none");
        else if (owner.kind == 1 && owner.id <= 0xFFFFFFFFu)
            snprintf(text, size, "offline %08llX", id);
        else if (owner.kind == 0x80)
            snprintf(text, size, "Xbox Live %016llX", id);
        else
            snprintf(text, size, "kind %02X, id %016llX", owner.kind, id);
    }
}
