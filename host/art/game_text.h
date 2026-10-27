#pragma once

// Each game's texts about its toys, read from the player's disc: today the upgrades of every Skylander (name,
// description, price, path, its bit in the figure's upgrade field) in English and French, written to
// <data>/<Game>/upgrades.json for collection::Upgrades (collection/upgrades.h). Game text: never shipped.
// Where each game keeps them is the "upgrades" part of its manifest (games/<id>/art.json); the formats:
// docs/runtime/host/figures.md ("Upgrades").
#include "game_art.h"
#include "../collection/json.h"
#include <filesystem>
#include <string_view>

namespace art
{
    // Reads the upgrades of `game` as its manifest's "upgrades" part says and writes them to `out` (a temporary file
    // renamed over). Returns how many toys have a list; 0 (and nothing written) when the disc does not have them.
    int ExtractUpgrades(std::string_view game, const collection::Json& manifest, const Disc& disc, const std::filesystem::path& out);
}
