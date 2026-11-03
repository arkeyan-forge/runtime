#include "host/toybox/trap_villain.h"
#include <algorithm>

namespace toybox
{
    std::vector<const collection::Villain*> TrapVillains(const collection::Game* game, const collection::Figure& f)
    {
        std::vector<const collection::Villain*> villains;
        if (game == nullptr || f.toy == nullptr || f.toy->category != collection::Category::Trap ||
            f.state == figures::AreaState::Damaged)
            return villains;
        for (const collection::Villain& villain : game->villains)
            if (villain.element == f.toy->element)
                villains.push_back(&villain);
        return villains;
    }

    uint8_t VillainId(const collection::Villain& villain) { return uint8_t(villain.id - figures::VillainResourceBase); }

    uint8_t StepVillain(const std::vector<const collection::Villain*>& villains, uint8_t current, int step)
    {
        if (villains.empty())
            return 0;
        int count = int(villains.size());
        auto at = std::find_if(villains.begin(), villains.end(),
            [current](const collection::Villain* villain) { return VillainId(*villain) == current; });
        if (at == villains.end())
            return VillainId(*villains[step < 0 ? count - 1 : 0]);
        int index = int(at - villains.begin());
        return VillainId(*villains[((index + step) % count + count) % count]);
    }

    bool KeptEvolved(const figures::TrapStats& stats, uint8_t id)
    {
        for (const figures::TrapVillain& record : stats.villains)
            if (id != 0 && record.id == id)
                return record.questComplete;
        return false;
    }
}
