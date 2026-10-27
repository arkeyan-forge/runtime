#include "launcher.h"

// The game this runtime was built for: games/<id>/info.cpp. The disc check accepts that game's releases only.
extern const GameInfo g_gameInfo;

const std::span<const GameInfo> g_games(&g_gameInfo, 1);
const char* const g_launcherName = "SSARecomp";

const GameInfo* FindGame(std::string_view id)
{
    for (const GameInfo& game : g_games)
    {
        if (id == game.id)
            return &game;
    }
    return nullptr;
}
