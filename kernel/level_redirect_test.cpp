// Checks SKY_LEVEL's file redirect (kernel/level_redirect.h): the first level's files, a level named by SKY_LEVEL_FROM
// (a Battle Mode arena), the characters that may follow a name. No game data. usage: ArkeyanLevelRedirectTest
#include "kernel/level_redirect.h"
#include <cstdio>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    using std::string_view_literals::operator""sv;
    constexpr std::string_view Follows = ".\0"sv;
}

int main()
{
    const std::string first = levelredirect::FirstWithName("\\level\\level_213_tutotial", "");
    CHECK(first == "\\level\\level_213_tutotial");
    CHECK(levelredirect::Apply("game:\\level\\level_213_tutotial.arc", first, Follows, "level_200") ==
          "game:\\level\\level_200.arc");
    CHECK(levelredirect::Apply("game:\\level\\level_213_tutotial", first, Follows, "level_200") ==
          "game:\\level\\level_200");
    // A longer name that starts the same is another level: left alone.
    CHECK(levelredirect::Apply("game:\\level\\level_213_tutotial_2.arc", first, Follows, "level_200") ==
          "game:\\level\\level_213_tutotial_2.arc");
    CHECK(levelredirect::Apply("game:\\level\\level_213_tutotial.arc", first, Follows, "") ==
          "game:\\level\\level_213_tutotial.arc");

    CHECK(levelredirect::FirstWithName("", "academy/academy").empty()); // no file redirect in the zone games

    // SKY_LEVEL_FROM: the arena the Battle Mode menus start is redirected, the first level is not.
    const std::string arena = levelredirect::FirstWithName("\\level\\level_213_tutotial", "pvp_level_200_two_towers");
    CHECK(arena == "\\level\\pvp_level_200_two_towers");
    CHECK(levelredirect::Apply("game:\\level\\pvp_level_200_two_towers.bld", arena, Follows,
              "pvp_level_003_darkforest") == "game:\\level\\pvp_level_003_darkforest.bld");
    CHECK(levelredirect::Apply("game:\\level\\level_213_tutotial.arc", arena, Follows, "pvp_level_003_darkforest") ==
          "game:\\level\\level_213_tutotial.arc");
    // SSA: a level's folder and its other files (the adapter's "\\level_027", followed by a backslash or a dot).
    const std::string ssa = levelredirect::FirstWithName("\\level_027", "pvp_level_007");
    CHECK(ssa == "\\pvp_level_007");
    CHECK(levelredirect::Apply("game:\\levels\\pvp_level_007\\mission.str", ssa, "\\.", "pvp_level_001") ==
          "game:\\levels\\pvp_level_001\\mission.str");
    CHECK(levelredirect::Apply("game:\\audio\\levels\\pvp_level_007.bnk", ssa, "\\.", "pvp_level_001") ==
          "game:\\audio\\levels\\pvp_level_001.bnk");

    if (g_failures == 0)
        printf("level redirect: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
