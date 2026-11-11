#pragma once

#include <cstdint>

// Synthetic hooks, with no guest addresses or translated instructions.
#define GAME_HOOK_BinkOpen TestBinkOpen
#define GAME_HOOK_BinkClose TestBinkClose
#define GAME_HOOK_BinkDoFrame TestBinkDoFrame

namespace game
{
    constexpr uint32_t UltrawideSceneWidth = 2, UltrawideSceneHeight = 2;
}
