#pragma once

// The addresses of the game being built (games/<SSA_GAME>/addresses.h, on the include path). A hook is written
//
//     #ifdef GAME_HOOK_Name
//     PPC_FUNC_IMPL(GAME_IMP(Name));   // the recompiled original, only if the hook calls it
//     PPC_FUNC(GAME_HOOK(Name)) { ... GAME_IMP(Name)(ctx, base); }
//     #endif
//
// and exists only in a game whose address file defines GAME_HOOK_Name as the guest function's symbol (sub_XXXXXXXX).
// Data globals are constants in namespace game (game::DeviceGlobal ...).
#include "addresses.h"

#define GAME_CONCAT_(a, b) a##b
#define GAME_CONCAT(a, b) GAME_CONCAT_(a, b)

// The symbol of the hooked function (sub_XXXXXXXX).
#define GAME_HOOK(name) GAME_HOOK_##name
// The symbol of the recompiled original (__imp__sub_XXXXXXXX).
#define GAME_IMP(name) GAME_CONCAT(__imp__, GAME_HOOK_##name)
