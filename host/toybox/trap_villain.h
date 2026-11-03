#pragma once

// A trap's villain on its Figure page (docs/runtime/host/toybox_ux.md, "A trap's villain"): the villains its element
// can hold, as Skylauncher's Edit offers them, and the step through them. The write is collection::SetTrapVillain; the
// model lifts the trap off the emulated portal around it. No drawing, no portal here.
#include "host/collection/catalogue.h"
#include "host/collection/collection.h"
#include "host/figures/figure_tag.h"
#include <cstdint>
#include <vector>

namespace toybox
{
    // The villains a trap of the figure's element can hold, in the game's table order (Game::villains). Empty for any
    // other figure, a damaged trap, or a game without villains.
    std::vector<const collection::Villain*> TrapVillains(const collection::Game* game, const collection::Figure& f);

    // The storage ID a trap holds for a villain (1-46): its catalogue ID minus figures::VillainResourceBase.
    uint8_t VillainId(const collection::Villain& villain);

    // The villain `step` places from `current` (a storage ID; 0: none), round the ends. From none or a villain not in
    // the list: the first (step > 0) or the last (step < 0). 0 when the list is empty.
    uint8_t StepVillain(const std::vector<const collection::Villain*>& villains, uint8_t current, int step);

    // Whether the trap's own record of villain `id` is evolved: choosing a villain again keeps its evolution. False when
    // the trap holds no record of it.
    bool KeptEvolved(const figures::TrapStats& stats, uint8_t id);
}
