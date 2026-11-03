// Checks a trap's Villain row on the Toy Box's Figure page (host/toybox/trap_villain.h): the villains each trap's
// element offers, the step through them, the write of a choice and the Evolved switch, a villain chosen again keeping
// its evolution. It works in a scratch collection with dumps it makes itself; no game data. usage: SSAToyboxTrapTest
#include "host/toybox/trap_villain.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    using namespace collection;
    using namespace toybox;

    std::string Names(const std::vector<const Villain*>& villains)
    {
        std::string names;
        for (const Villain* villain : villains)
            names += (names.empty() ? "" : ", ") + villain->name;
        return names;
    }

    figures::TrapStats TrapOf(const std::string& path)
    {
        FigureData tag{};
        figures::TrapStats stats;
        size_t size = 0;
        void* data = SDL_LoadFile(path.c_str(), &size);
        if (data != nullptr && size == tag.size())
        {
            memcpy(tag.data(), data, size);
            figures::ReadTrapStats(tag, stats);
        }
        SDL_free(data);
        return stats;
    }

    // The row's choices for a trap of each element: the game's own table, in its order.
    void TestChoices(const Game* game)
    {
        const std::map<uint16_t, std::string> expected = {
            { 210, "Bomb Shell, Rage Mage, Pain-Yatta" },
            { 211, "The Gulper, Cross Crow, Brawl And Chain, Chill Bill, Slobber Trap, Threatpack" },
            { 212, "Dreamcatcher, Buzzer Beak, Krankenstein, Bad Juju" },
            { 213, "Wolfgang, Bone Chompy, Masker Mind, Hood Sickle" },
            { 214, "Dr Krankcase, Shrednaught, Trolling Thunder, Bruiser Cruiser, Brawlrus, Mab Lobs" },
            { 215, "Pepper Jack, Scrap Shooter, Grinnade, Smoke Scream" },
            { 216, "Golden Queen, Chomp Chest, Tussle Sprout, Grave Clobber" },
            { 217, "Chompy Mage, Cuckoo Clocker, Shield Shredder, Sheep Creep, Broccoli Guy, Chompy" },
            { 218, "Nightshade, Fisticuffs, Tae Kwon Crow, Eye Scream" },
            { 219, "Luminous, Lob Goblin, Blaster-Tron, Eye Five" },
            { 220, "Kaos" },
        };
        size_t all = 0;
        for (const auto& [type, names] : expected)
        {
            Figure trap;
            CHECK(Read(Create(type), trap));
            std::vector<const Villain*> villains = TrapVillains(game, trap);
            if (Names(villains) != names)
                fprintf(stderr, "trap %u offers: %s\n", type, Names(villains).c_str());
            CHECK(Names(villains) == names);
            all += villains.size();
            CHECK(Delete(trap.path));
        }
        CHECK(all == 46); // every villain of the table, each in one element's row
        // Not a trap, or no game with villains: no row.
        Figure master;
        CHECK(Read(Create(450), master) && TrapVillains(game, master).empty());
        CHECK(TrapVillains(nullptr, master).empty() && TrapVillains(Catalogue::Get().FindGame("ssa"), master).empty());
        CHECK(Delete(master.path));
    }

    // Left / Right round the ends; from none, the first or the last.
    void TestStep(const Game* game)
    {
        Figure trap;
        CHECK(Read(Create(211), trap));
        std::vector<const Villain*> water = TrapVillains(game, trap);
        CHECK(water.size() == 6 && VillainId(*water[0]) == 9 && VillainId(*water[3]) == 19);
        CHECK(StepVillain(water, 0, 1) == 9 && StepVillain(water, 0, -1) == 44);
        CHECK(StepVillain(water, 9, 1) == 14 && StepVillain(water, 9, -1) == 44 && StepVillain(water, 44, 1) == 9);
        CHECK(StepVillain(water, 20, 1) == 9); // Sheep Creep is not a Water villain
        CHECK(StepVillain({}, 9, 1) == 0);
        CHECK(Delete(trap.path));
    }

    // The write: the choice becomes the selected villain, the switch its evolution, and a villain chosen again keeps
    // its own record (its evolution) while the others stay in the trap.
    void TestWrite(const Game* game)
    {
        std::string path = Create(211, 0x3002);
        Figure trap;
        CHECK(Read(path, trap) && trap.stats.villain == 0 && TrapVillains(game, trap).size() == 6);
        CHECK(SetTrapVillain(path, 19, false) && Read(path, trap) && trap.state == figures::AreaState::Valid &&
              trap.stats.villain == 19 && !trap.stats.villainEvolved);
        CHECK(SetTrapVillain(path, 19, true) && Read(path, trap) && trap.stats.villain == 19 &&
              trap.stats.villainEvolved);
        CHECK(SetTrapVillain(path, 9, KeptEvolved(TrapOf(path), 9)) && Read(path, trap) && trap.stats.villain == 9 &&
              !trap.stats.villainEvolved);
        figures::TrapStats held = TrapOf(path);
        CHECK(held.villainCount == 2 && KeptEvolved(held, 19) && !KeptEvolved(held, 9) && !KeptEvolved(held, 0));
        CHECK(SetTrapVillain(path, 19, KeptEvolved(held, 19)) && Read(path, trap) && trap.stats.villain == 19 &&
              trap.stats.villainEvolved);
        CHECK(TrapOf(path).villainCount == 2);
        // The Evolved switch off, then the row on another villain and back: off is kept too.
        CHECK(SetTrapVillain(path, 19, false) && Read(path, trap) && !trap.stats.villainEvolved);
        CHECK(!KeptEvolved(TrapOf(path), 19));
        // Each write keeps a backup of the trap before it.
        CHECK(!Backups(path).empty());
        CHECK(Delete(path));
    }
}

int main()
{
    // A scratch collection, never the player's: in SKYLANDERS_DATA (test_all sets a scratch one), else the temporary folder.
    const char* data = getenv("SKYLANDERS_DATA");
    std::string scratch =
        (data != nullptr && *data ? std::string(data) : std::filesystem::temp_directory_path().string()) +
        "/SSAToyboxTrapTest";
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    std::filesystem::create_directories(scratch, ignored);
#ifdef _WIN32
    _putenv_s("SKY_COLLECTION", (scratch + "/Collection").c_str());
    _putenv_s("SKY_FIGURES", "");
    _putenv_s("SKY_MIGRATE_FROM", "");
#else
    setenv("SKY_COLLECTION", (scratch + "/Collection").c_str(), 1);
    unsetenv("SKY_FIGURES");
    unsetenv("SKY_MIGRATE_FROM");
#endif
    const Game* game = Catalogue::Get().FindGame("trapteam");
    CHECK(game != nullptr && game->villains.size() == 46);
    if (game != nullptr)
    {
        TestChoices(game);
        TestStep(game);
        TestWrite(game);
    }
    std::filesystem::remove_all(scratch, ignored);
    printf("toybox trap test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
