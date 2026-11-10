// Checks the Toy Box script's portal items (host/portal_script.h): place=<dump>, remove=<slot>, remove=all; anything
// else is not a portal item. No game data. usage: ArkeyanPortalScriptTest
#include "host/portal_script.h"
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

    using Kind = portalscript::Item::Kind;
}

int main()
{
    portalscript::Item place = portalscript::Parse("place=/tmp/run/collection/toy 1.sky");
    CHECK(place.kind == Kind::Place);
    CHECK(place.path == "/tmp/run/collection/toy 1.sky");
    CHECK(portalscript::Parse("place=gill.sky").path == "gill.sky");
    CHECK(portalscript::Parse("place=").kind == Kind::None);

    CHECK(portalscript::Parse("remove=all").kind == Kind::RemoveAll);
    portalscript::Item remove = portalscript::Parse("remove=3");
    CHECK(remove.kind == Kind::Remove);
    CHECK(remove.slot == 3);
    CHECK(portalscript::Parse("remove=").kind == Kind::None);
    CHECK(portalscript::Parse("remove=-1").kind == Kind::None);
    CHECK(portalscript::Parse("remove=16").kind == Kind::None);
    CHECK(portalscript::Parse("remove=2x").kind == Kind::None);

    CHECK(portalscript::Parse("F6").kind == Kind::None);
    CHECK(portalscript::Parse("pad=a").kind == Kind::None);
    CHECK(portalscript::Parse("placed=x").kind == Kind::None);

    if (g_failures == 0)
        printf("portal script items: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
