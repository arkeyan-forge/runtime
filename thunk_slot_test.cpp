// Checks where host thunks go in the code's padding. usage: ArkeyanThunkSlotTest
#include "thunk_slot.h"
#include <cstdio>
#include <set>

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

    constexpr uint32_t Base = 0x82000000, End = Base + 0x400;
}

int main()
{
    // A run of 16 padding words (the earlier games' gaps between code sections): its top word, then the next below.
    std::set<uint32_t> padding;
    for (uint32_t i = 0; i < 20; ++i)
        padding.insert(Base + 0x100 + i * 4);
    auto isPadding = [&](uint32_t address) { return padding.count(address) != 0; };
    uint32_t first = FindThunkSlot(End, Base, 16, isPadding);
    CHECK(first == Base + 0x100 + 19 * 4);
    CHECK(FindThunkSlot(first, Base, 16, isPadding) == Base + 0x100 + 18 * 4);
    CHECK(FindThunkSlot(Base + 0x100 + 15 * 4, Base, 16, isPadding) == 0);

    // A title whose code is one section: only lone padding words between functions, no run of 16.
    padding = { Base + 0x40, Base + 0x200, Base + 0x3FC };
    CHECK(FindThunkSlot(End, Base, 16, isPadding) == 0);
    CHECK(FindThunkSlot(End, Base, 1, isPadding) == Base + 0x3FC);
    CHECK(FindThunkSlot(Base + 0x3FC, Base, 1, isPadding) == Base + 0x200);
    CHECK(FindThunkSlot(Base + 0x40, Base, 1, isPadding) == 0);

    // The first word of the range counts, and the search stops there.
    padding = { Base };
    CHECK(FindThunkSlot(End, Base, 1, isPadding) == Base);

    printf("thunk slot test: %d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
