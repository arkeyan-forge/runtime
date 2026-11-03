#pragma once

// A portable game (docs/getting-started/portable.md): one file exported by Skylauncher that holds the built game and its data
// (host/collection/portable_pack.h). The game finds that data itself (ResolveGameSource) and keeps its saves and settings in
// the player's folder, the same ones as the installed game.
#include "launcher.h"
#include "collection/portable_pack.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace portable
{
    // The pack that holds this program's game data: the program itself (Windows, Linux), or game.skypack in its bundle
    // (macOS). Empty when this program is not a portable. Looked up once.
    const std::string& OwnPack();
    // An entry of that pack (icon.png, portable.txt), empty when there is none.
    std::vector<uint8_t> ReadOwn(std::string_view name);
    // The first start of this exported copy on this machine: its figures are added to the shared collection (collection::Folder())
    // and its keys to launcher.ini ([<game>.input]), never over the player's own, once (portable_pack.h, FirstLaunchMarker).
    // Before the settings are read (and so before the log): returns the line to log, empty when there was nothing to do.
    std::string FirstLaunch(const GameInfo& game, const std::string& collectionFolder);
}
