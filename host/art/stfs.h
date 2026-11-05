#pragma once

// One file of an STFS package on the disc (the Xbox 360's package format: "CON ", "LIVE", "PIRS"). Every disc has one at
// its root, `nxeart`: the dashboard's art for the game, with its 1920x1080 backdrop `nxebg.jpg` and its tile
// `nxeslot.jpg` (docs/runtime/host/game_art.md, "Dashboard art").
#include "game_art.h"
#include <cstdint>
#include <string>
#include <vector>

namespace art
{
    // The bytes of `name` (its name in the package's file table, any case) in the package at `path` on the disc; empty when
    // the package or the file is not there, or when the file's blocks cannot be followed: a package of one hash table
    // (170 data blocks, 680 KiB, as every game's dashboard art seen) follows the table's chain, a larger one only a file
    // the table marks as consecutive.
    std::vector<uint8_t> StfsFile(const Disc& disc, const std::string& path, const std::string& name);

    // The same, from the package's bytes (tests).
    std::vector<uint8_t> StfsFile(const std::vector<uint8_t>& package, const std::string& name);
}
