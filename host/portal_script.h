#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

// The Toy Box script's portal items (SKY_TOYBOX_SCRIPT, SKY_TOYBOX_SCRIPT_FILE; host/toybox.cpp): "place=<dump>" puts a
// figure dump on the emulated portal (a path, or a name in the collection), "remove=<slot>" takes one slot's figure off
// and "remove=all" every figure. A test run changes the portal while the game plays, without the F7/F8 keys, which the
// script's keys never reach (they go to the Toy Box only).
namespace portalscript
{
    struct Item
    {
        enum class Kind
        {
            None,
            Place,
            Remove,
            RemoveAll,
        };
        Kind kind = Kind::None;
        std::string path; // Place
        int slot = -1;    // Remove
    };

    inline Item Parse(std::string_view action)
    {
        Item item;
        if (action.substr(0, 6) == "place=" && action.size() > 6)
        {
            item.kind = Item::Kind::Place;
            item.path = std::string(action.substr(6));
        }
        else if (action == "remove=all")
        {
            item.kind = Item::Kind::RemoveAll;
        }
        else if (action.substr(0, 7) == "remove=" && action.size() > 7)
        {
            std::string digits(action.substr(7));
            char* end = nullptr;
            long slot = strtol(digits.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && slot >= 0 && slot < 16)
            {
                item.kind = Item::Kind::Remove;
                item.slot = int(slot);
            }
        }
        return item;
    }
}
