// Unit test of the collection library: JSON, the catalogue (SSA's toys exactly as the old built-in table), the
// collection folder (create, stats, metadata, rename, locks, backups, migration) and the icons. It works in a scratch
// folder of its own, with dumps it makes itself; never the player's folders.
//   SSACollectionTest [--icons <folder>]   (--icons also writes a contact sheet of the generated icons)
// Exit code 0 = pass.
#include "host/collection/catalogue.h"
#include "host/collection/collection.h"
#include "host/collection/icons.h"
#include "host/collection/json.h"
#include "host/collection/search.h"
#include "host/collection/upgrades.h"
#include "host/collection/datadir.h"
#include "host/figures/figure_catalog.h"
#include <SDL3/SDL.h>
#include <iterator>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <set>
#include <string>
#include "../env.h"

namespace
{
    int g_checks;
    int g_failures;

    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    std::string g_scratch; // the test's own folder

    // What Giants makes of a figure's variant id [V] (its script "Save - Get Tag Subtype Properties" and the toy
    // definitions of character/Init_Setup.bld, dumped by tools/toy_tables.py; docs/runtime/host/figures.md): bit 11
    // repose (Series 2) for the toys it knows a repose of, bit 10 an alternate deco when the toy has one with the low
    // byte's deco id, bit 9 LightCore for its LightCore toys. The high nibble is not read.
    struct GiantsSubtype
    {
        bool series2 = false, alternate = false, lightcore = false;
    };
    const std::set<uint16_t> GiantsSeries2 = { 0, 1, 3, 4, 5, 7, 9, 10, 11, 12, 14, 15, 16, 18, 19, 20, 21, 23, 25, 26,
        27, 29, 30, 32 };
    const std::set<uint16_t> GiantsLightCore = { 7, 9, 20, 29, 100, 106, 108, 113 };
    const std::set<std::pair<uint16_t, uint8_t>> GiantsDecos = { { 0, 2 }, { 10, 3 }, { 15, 3 }, { 18, 2 }, { 26, 3 },
        { 100, 3 }, { 102, 2 }, { 103, 2 }, { 105, 2 }, { 106, 3 }, { 108, 2 }, { 109, 2 }, { 110, 3 }, { 112, 2 },
        { 208, 2 } };

    GiantsSubtype GiantsReads(uint16_t type, uint16_t variant)
    {
        GiantsSubtype out;
        out.series2 = (variant & 0x800) && GiantsSeries2.contains(type);
        out.alternate = (variant & 0x400) && GiantsDecos.contains({ type, uint8_t(variant & 0xFF) });
        out.lightcore = (variant & 0x200) && GiantsLightCore.contains(type);
        return out;
    }

    // A listed variant Giants knows: its kind is what the game reads from its id. Series 2 and LightCore show as such;
    // an alternate deco (Polar, Granite, Golden Dragonfire...) never lights up as a LightCore, except Legendary Chill,
    // whose only picture on the disc is the LightCore one ("icevalkyrielegendarylightcore").
    bool GiantsKnows(const collection::Toy& toy, const collection::Variant& variant)
    {
        using collection::VariantKind;
        GiantsSubtype reads = GiantsReads(toy.type, variant.id);
        if (variant.sameTag)
            return false;
        if (variant.kind == VariantKind::Series)
            return reads.series2 && !reads.alternate && variant.id >> 12 == 1;
        if (variant.kind == VariantKind::LightCore)
            return reads.lightcore && !reads.alternate;
        return reads.alternate && (!reads.lightcore || (toy.type == 106 && variant.kind == VariantKind::Legendary));
    }

    // A live process that is not the test: launchd or init; on Windows the System process.
#ifdef _WIN32
    const std::string LivePid = "4";
#else
    const std::string LivePid = "1";
#endif

    void SetVariable(const char* name, const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    void UnsetVariable(const char* name)
    {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    void RemoveTree(const std::string& path)
    {
        SDL_EnumerateDirectory(
            path.c_str(),
            [](void* parent, const char*, const char* name) {
                std::string child = std::string(static_cast<const char*>(parent)) + "/" + name;
                SDL_PathInfo info;
                if (SDL_GetPathInfo(child.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY)
                    RemoveTree(child);
                else
                    SDL_RemovePath(child.c_str());
                return SDL_ENUM_CONTINUE;
            },
            const_cast<char*>(path.c_str()));
        SDL_RemovePath(path.c_str());
    }

    std::string ReadText(const std::string& path)
    {
        size_t size = 0;
        char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
        std::string text = data != nullptr ? std::string(data, size) : std::string();
        SDL_free(data);
        return text;
    }

    void TestJson()
    {
        using collection::Json;
        Json json;
        std::string error;
        CHECK(Json::Parse(R"({"a": [1, 2.5, "xé\n"], "b": {"c": true, "d": null}, "id": "0x1801"})", json, error));
        CHECK(json["a"].Items().size() == 3 && json["a"].Items()[1].AsNumber() == 2.5);
        CHECK(json["a"].Items()[2].AsString() == "x\xC3\xA9\n");
        CHECK(json["b"]["c"].AsBool() && json["b"]["d"].IsNull() && json["missing"]["deeper"].IsNull());
        CHECK(json["id"].AsInt() == 0x1801 && Json("6145").AsInt() == 6145 && Json("abc").AsInt(-1) == -1);
        Json again;
        CHECK(Json::Parse(json.Dump(), again, error) && again.Dump() == json.Dump());
        CHECK(!Json::Parse("{\"a\": 1,}", json, error) && error.find("line 1") == 0);
        CHECK(!Json::Parse("[1, 2]\n x", json, error) && error.find("line 2") == 0);
        CHECK(!Json::Parse("\"open", json, error));
        Json object = Json::Object();
        object.Set("k", 1);
        object.Set("k", "two");
        object.Set("z", Json::Array());
        CHECK(object.Members().size() == 2 && object["k"].AsString() == "two");
        object.Erase("k");
        CHECK(!object.Has("k") && object.Has("z"));
    }

    // Spyro's Adventure's toys before the catalogue became data (figure_catalog.cpp of 2026-10-23): type, name, the
    // title's internal name, element, kind. The data-driven table must give exactly this, in this order.
    struct OldToy
    {
        uint16_t type;
        const char* name;
        const char* portrait;
        figures::Element element;
        figures::ToyKind kind;
    };

    void TestSsaCatalog()
    {
        using enum figures::Element;
        using enum figures::ToyKind;
        static const OldToy Old[] = {
            { 0, "Whirlwind", "AirDragon", Air, Character },
            { 1, "Sonic Boom", "Griffin", Air, Character },
            { 2, "Warnado", "SkyTurtle", Air, Character },
            { 3, "Lightning Rod", "StormGiant", Air, Character },
            { 4, "Bash", "RockDragon", Earth, Character },
            { 5, "Terrafin", "LandShark", Earth, Character },
            { 6, "Dino-Rang", "DinoRang", Earth, Character },
            { 7, "Prism Break", "GemGolem", Earth, Character },
            { 8, "Sunburn", "PhoenixDragon", Fire, Character },
            { 9, "Eruptor", "Eruptor", Fire, Character },
            { 10, "Ignitor", "FlameKnight", Fire, Character },
            { 11, "Flameslinger", "FireElf", Fire, Character },
            { 12, "Zap", "WaterDragon", Water, Character },
            { 13, "Wham-Shell", "CrustBuckler", Water, Character },
            { 14, "Gill Grunt", "GillGrunt", Water, Character },
            { 15, "Slam Bam", "Yeti", Water, Character },
            { 16, "Spyro", "SpyroJr", Magic, Character },
            { 17, "Voodood", "SkullOrc", Magic, Character },
            { 18, "Double Trouble", "TikiWizard", Magic, Character },
            { 19, "Trigger Happy", "GoldenCheat", Tech, Character },
            { 20, "Drobot", "MetalDragon", Tech, Character },
            { 21, "Drill Sergeant", "DrillBot", Tech, Character },
            { 22, "Boomer", "BombTroll", Tech, Character },
            { 23, "Wrecking Ball", "ForceFieldGrub", Magic, Character },
            { 24, "Camo", "PlantDragon", Life, Character },
            { 25, "Zook", "Bambazooker", Life, Character },
            { 26, "Stealth Elf", "StealthElf", Life, Character },
            { 27, "Stump Smash", "StumpSmashEnt", Life, Character },
            { 28, "Dark Spyro", "DarkSpyro", Magic, Character },
            { 29, "Hex", "ShadowMaid", Undead, Character },
            { 30, "Chop Chop", "PandoranGuard", Undead, Character },
            { 31, "Ghost Roaster", "GhostEater", Undead, Character },
            { 32, "Cynder", "Cynder", Undead, Character },
            { 404, "Legendary Bash", "LegendaryRockDragon", Earth, Legendary },
            { 416, "Legendary Spyro", "LegendarySpyro", Magic, Legendary },
            { 419, "Legendary Trigger Happy", "LegendaryGoldenCheat", Tech, Legendary },
            { 430, "Legendary Chop Chop", "LegendaryPandoranGuard", Undead, Legendary },
            { 505, "Terrabite", "LandShark", Earth, Sidekick },
            { 514, "Gill Runt", "GillGrunt", Water, Sidekick },
            { 519, "Trigger Snappy", "GoldenCheat", Tech, Sidekick },
            { 526, "Whisper Elf", "StealthElf", Life, Sidekick },
            { 200, "Anvil Rain", nullptr, None, MagicItem },
            { 201, "Hidden Treasure", nullptr, None, MagicItem },
            { 202, "Healing Elixir", nullptr, None, MagicItem },
            { 203, "Ghost Pirate Swords", nullptr, None, MagicItem },
            { 204, "Time Twister", nullptr, None, MagicItem },
            { 205, "Sky-Iron Shield", nullptr, None, MagicItem },
            { 206, "Winged Boots", nullptr, None, MagicItem },
            { 207, "Sparx Dragonfly", nullptr, None, MagicItem },
            { 300, "Dragon's Peak", nullptr, None, Adventure },
            { 301, "Empire of Ice", nullptr, None, Adventure },
            { 302, "Pirate Seas", nullptr, None, Adventure },
            { 303, "Darklight Crypt", nullptr, None, Adventure },
            { 304, "Volcanic Vault", nullptr, None, Adventure },
        };
        auto catalog = figures::Catalog();
        CHECK(catalog.size() == std::size(Old));
        for (size_t i = 0; i < std::min(catalog.size(), std::size(Old)); i++)
        {
            const figures::ToyInfo& toy = catalog[i];
            bool same = toy.toyType == Old[i].type && strcmp(toy.name, Old[i].name) == 0 &&
                        toy.element == Old[i].element && toy.kind == Old[i].kind &&
                        (toy.portrait == nullptr) == (Old[i].portrait == nullptr) &&
                        (toy.portrait == nullptr || strcmp(toy.portrait, Old[i].portrait) == 0);
            CHECK(same);
            if (!same)
                fprintf(stderr, "  entry %zu: %u %s\n", i, toy.toyType, toy.name);
        }
        CHECK(figures::FindToy(33) == nullptr && figures::FindToy(100) == nullptr);
        // The disc pictures found for four of the items.
        CHECK(figures::PortraitImageName(*figures::FindToy(202)) == "potion");
        CHECK(figures::PortraitImageName(*figures::FindToy(200)).empty());
        CHECK(figures::PortraitImageName(*figures::FindToy(514)) == "toy_gillgrunt_hud");
    }

    void TestCatalogue()
    {
        using namespace collection;
        const Catalogue& catalogue = Catalogue::Get();
        std::set<uint16_t> types;
        int perGame[6] = {};
        const char* games[] = { "ssa", "giants", "swapforce", "trapteam", "superchargers", "imaginators" };
        for (const Toy& toy : catalogue.Toys())
        {
            CHECK(types.insert(toy.type).second);
            CHECK(catalogue.FindGame(toy.game) != nullptr);
            for (int i = 0; i < 6; i++)
                perGame[i] += toy.game == games[i];
            std::set<uint16_t> ids;
            for (const Variant& variant : toy.variants)
                CHECK(variant.sameTag || ids.insert(variant.id).second);
        }
        printf("catalogue: %zu toys (", catalogue.Toys().size());
        for (int i = 0; i < 6; i++)
            printf("%s%s %d", i ? ", " : "", games[i], perGame[i]);
        printf(")\n");
        for (int count : perGame)
            CHECK(count > 0);
        CHECK(catalogue.Games().size() == 6);

        // Backward compatibility: later games take the older toys, SSA only its own set.
        CHECK(catalogue.Accepts("ssa", 14) && !catalogue.Accepts("ssa", 100) && catalogue.Accepts("ssa", 514));
        CHECK(
            catalogue.Accepts("giants", 14) && catalogue.Accepts("giants", 100) && !catalogue.Accepts("giants", 3000));
        CHECK(catalogue.Accepts("imaginators", 14) && catalogue.Accepts("imaginators", 601) &&
              !catalogue.Accepts("nope", 14));
        CHECK(catalogue.Accepted("ssa").size() == figures::Catalog().size());
        CHECK(catalogue.Accepted("giants").size() == figures::Catalog("giants").size());

        // Giants: SSA's toys and its own (figures, items, sidekicks, adventure packs), level cap 15, one seat for an
        // item or an adventure pack (together they are "too many toys" in the title [V]).
        for (uint16_t type : { 0, 32, 100, 101, 115, 200, 208, 209, 300, 303, 404, 430, 505, 526 })
            CHECK(catalogue.Accepts("giants", type));
        CHECK(!catalogue.Accepts("giants", 600) && !catalogue.Accepts("giants", 1000));
        CHECK(catalogue.Find(101)->category == Category::Giant && catalogue.Find(208)->category == Category::MagicItem);
        CHECK(catalogue.Describe(catalogue.Find(100), 0x1206).kind == VariantKind::LightCore);
        auto seatIds = [&](const char* id) {
            std::string ids;
            for (const Seat& seat : catalogue.FindGame(id)->seats)
                ids += seat.id + ",";
            return ids;
        };
        CHECK(seatIds("giants") == "p1,p2,item," && seatIds("ssa") == "p1,p2,item,adventure,");
        const Seat& giantsItem = catalogue.FindGame("giants")->seats[2];
        CHECK(std::find(giantsItem.accepts.begin(), giantsItem.accepts.end(), Category::Adventure) !=
              giantsItem.accepts.end());

        // What each game reads of a figure (Catalogue::Read, VersionsFor): backward compatible variants play as the game sees them.
        {
            const Toy* eruptor = catalogue.Find(9);
            auto read = [&](const char* game, const Toy* toy, uint16_t variant) {
                return catalogue.Read(game, toy, variant);
            };
            // Spyro's Adventure reads the toy type only: every variant plays as the plain toy.
            Catalogue::Reading r = read("ssa", eruptor, 0x1801);
            CHECK(r.works && !r.exact && r.seen == nullptr && r.kind == VariantKind::Base);
            CHECK(!read("ssa", catalogue.Find(100), 0).works && !read("giants", catalogue.Find(3000), 0).works);
            CHECK(catalogue.VersionsFor("ssa", *eruptor).empty());
            // Giants: Series 2 and LightCore of Eruptor as themselves; a later repose as Series 2; the rest plain.
            r = read("giants", eruptor, 0x1801);
            CHECK(r.exact && r.kind == VariantKind::Series);
            r = read("giants", eruptor, 0x1206);
            CHECK(r.exact && r.kind == VariantKind::LightCore);
            r = read("giants", eruptor, 0x2805);
            CHECK(!r.exact && r.kind == VariantKind::Series && r.seen != nullptr && r.seen->id == 0x1801);
            r = read(
                "giants", eruptor, 0x2C02); // Volcanic: Giants has no deco for it, its repose bit makes it Series 2
            CHECK(!r.exact && r.kind == VariantKind::Series);
            r = read(
                "giants", eruptor, 0x3810); // Eon's Elite carries the repose bit too: Giants shows the Series 2 pose
            CHECK(!r.exact && r.kind == VariantKind::Series && r.seen != nullptr && r.seen->id == 0x1801);
            r = read("giants", eruptor, 0x1814); // a chase figure: only the repose bit counts
            CHECK(!r.exact && r.kind == VariantKind::Series);
            r = read("giants", catalogue.Find(2), 0x1812); // Warnado has no repose: plain
            CHECK(!r.exact && r.kind == VariantKind::Base && r.seen == nullptr);
            r = read("giants", catalogue.Find(2), 0x2206); // Warnado is not a LightCore toy for Giants
            CHECK(r.works && !r.exact && r.kind == VariantKind::Base);
            std::string giantsVersions;
            for (const Variant* v : catalogue.VersionsFor("giants", *eruptor))
                giantsVersions += v->name + ";";
            CHECK(giantsVersions == "Series 2 Eruptor;LightCore Eruptor;");
            // Swap Force tells the 2013 decorations apart, Trap Team Eon's Elite too.
            CHECK(read("swapforce", eruptor, 0x2C02).exact &&
                  read("swapforce", eruptor, 0x2C02).kind == VariantKind::AltDeco);
            CHECK(!read("swapforce", eruptor, 0x3810).exact && read("trapteam", eruptor, 0x3810).exact);
            // Trap Team's re-release of a sidekick (0x3000) is a version only in the games that tell it apart.
            const Toy* terrabite = catalogue.Find(505);
            CHECK(terrabite != nullptr && catalogue.VersionsFor("giants", *terrabite).empty() &&
                  catalogue.VersionsFor("swapforce", *terrabite).empty());
            CHECK(catalogue.VersionsFor("trapteam", *terrabite).size() == 1);
            // No version list repeats a toy's own name, shows a base entry or lists a variant its game plays as the plain toy.
            for (const Game& game : catalogue.Games())
                for (const Toy& toy : catalogue.Toys())
                    for (const Variant* v : catalogue.VersionsFor(game.id, toy))
                        CHECK(v->kind != VariantKind::Base && v->name != toy.name &&
                              (v->sameTag || catalogue.Read(game.id, &toy, v->id).exact));
            // Figures as the game names them.
            Figure seen;
            seen.toy = eruptor;
            seen.type = 9;
            seen.variant = 0x3810;
            seen.variantInfo = catalogue.Describe(eruptor, seen.variant);
            CHECK(NameIn(seen, "giants") == "Series 2 Eruptor" && NameIn(seen, "trapteam") == "Eon's Elite Eruptor" &&
                  KindIn(seen, "giants") == VariantKind::Series && KindIn(seen, "trapteam") == VariantKind::EonsElite &&
                  seen.DisplayName() == "Eon's Elite Eruptor");
        }

        // Variants: listed, worked out from the bits, unknown toys.
        const Toy* gill = catalogue.Find(14);
        CHECK(gill != nullptr && gill->category == Category::Core && gill->element == Element::Water);
        VariantInfo info = catalogue.Describe(gill, 0);
        CHECK(info.name == "Gill Grunt" && info.kind == VariantKind::Base && info.known);
        info = catalogue.Describe(gill, 0x3810);
        CHECK(info.name == "Eon's Elite Gill Grunt" && info.kind == VariantKind::EonsElite && info.known);
        info = catalogue.Describe(gill, 0x1801);
        CHECK(info.name == "Series 2 Gill Grunt" && info.kind == VariantKind::Series && info.known);
        info = catalogue.Describe(gill, 0x2801); // another year, the same deco and flags: Giants reads it as Series 2
        CHECK(info.name == "Series 2 Gill Grunt" && info.kind == VariantKind::Series && !info.known);
        info = catalogue.Describe(catalogue.Find(4), 0x1206);
        CHECK(info.name == "LightCore Bash" && info.kind == VariantKind::LightCore);
        info = catalogue.Describe(gill, 0x7777);
        CHECK(info.name == "Gill Grunt (variant 7777)" && info.kind == VariantKind::Other);
        CHECK(catalogue.Describe(nullptr, 0).name == "Unknown toy" && !catalogue.Describe(nullptr, 0).known);
        CHECK(
            catalogue.Find(28)->edition == VariantKind::Dark && catalogue.Find(404)->edition == VariantKind::Legendary);
        // Swap Force [V] (its disc's toy data and real figures): bottoms (legs) are 1000-1015, tops (bodies) 2000-2015,
        // its figures carry 0x2000 as sold, and its variants are listed with the ids its data gives.
        CHECK(catalogue.Find(1000)->category == Category::SwapBottom &&
              catalogue.Find(2000)->category == Category::SwapTop);
        CHECK(catalogue.Find(2015)->name == "Wash Buckler (top)" &&
              catalogue.Find(1015)->name == "Wash Buckler (bottom)");
        info = catalogue.Describe(catalogue.Find(2015), 0x2402);
        CHECK(info.name == "Dark Wash Buckler (top)" && info.kind == VariantKind::Dark && info.known);
        info = catalogue.Describe(catalogue.Find(3000), 0x2000);
        CHECK(info.name == "Scratch" && info.kind == VariantKind::Base && info.known);
        CHECK(catalogue.Describe(catalogue.Find(3005), 0x2206).kind == VariantKind::LightCore);
        CHECK(catalogue.Describe(catalogue.Find(3013), 0x2603).name == "Legendary Grim Creeper");
        CHECK(catalogue.Accepts("swapforce", 14) && catalogue.Accepts("swapforce", 2000) &&
              !catalogue.Accepts("swapforce", 450));
        CHECK(catalogue.Find(210)->category == Category::Trap && catalogue.Find(3220)->category == Category::Vehicle);

        // Every variant Giants knows is listed, with an id Giants reads as that variant [V] (the disc's own tables), and
        // none of the listed Giants-era ids reads as something else (Punch Pop Fizz must not light up as a LightCore).
        int giantsKnown = 0;
        for (const Toy& toy : catalogue.Toys())
        {
            if (toy.game != "ssa" && toy.game != "giants")
                continue;
            bool series2 = false, lightcore = false;
            std::set<uint8_t> decos;
            for (const Variant& variant : toy.variants)
                if (GiantsKnows(toy, variant))
                {
                    giantsKnown++;
                    series2 |= variant.kind == VariantKind::Series;
                    lightcore |= variant.kind == VariantKind::LightCore;
                    if (variant.kind != VariantKind::Series && variant.kind != VariantKind::LightCore)
                        decos.insert(uint8_t(variant.id & 0xFF));
                }
            CHECK(series2 == GiantsSeries2.contains(toy.type) && lightcore == GiantsLightCore.contains(toy.type));
            for (const auto& [type, deco] : GiantsDecos)
                if (type == toy.type)
                    CHECK(decos.contains(deco));
        }
        printf("catalogue: %d variants of SSA and Giants toys read by Giants as listed\n", giantsKnown);
        CHECK(
            giantsKnown ==
            49); // 47 the disc defines, and the public Rare LightCore Prism Break and Love Potion Pop Fizz (read as Punch)
        // Giants' pictures name listed variants only; every toy and variant it has a picture of is mapped.
        const Game* giants = catalogue.FindGame("giants");
        for (const auto* pictures : { &giants->portraits, &giants->artworks })
            for (const auto& [key, name] : *pictures)
            {
                const Toy* toy = catalogue.Find(uint16_t(key >> 16));
                uint16_t variant = uint16_t(key & 0xFFFF);
                CHECK(toy != nullptr && (variant == 0 || catalogue.Describe(toy, variant).known));
            }
        CHECK(giants->portraits.size() == 133 && giants->artworks.size() == 109);

        // A toy's "base" entry is its standard figure, not a second version (the Toy Box and the launcher list "Standard" and then
        // the others): a versions list never repeats the toy's own name, a "Standard" is made with the id the toy is sold with,
        // and the 85 toys with a base entry (the 22 Giants ones among them) all have one with an id.
        int withBase = 0;
        for (const Toy& toy : catalogue.Toys())
        {
            std::string own = toy.name;
            for (const char* half : { " (top)", " (bottom)" })
                if (own.size() > strlen(half) && own.compare(own.size() - strlen(half), strlen(half), half) == 0)
                    own.erase(own.size() - strlen(half));
            size_t listed = 0;
            for (const Variant& variant : toy.variants)
            {
                if (variant.kind == VariantKind::Base)
                    continue;
                listed++;
                CHECK(variant.name != toy.name && variant.name != own);
            }
            CHECK(VersionCount(toy) == listed);
            bool hasBase = StandardVariant(toy) != 0;
            withBase += hasBase ? 1 : 0;
            if (hasBase)
                CHECK(toy.variants[0].kind == VariantKind::Base ||
                      catalogue.Describe(&toy, StandardVariant(toy)).kind == VariantKind::Base);
        }
        CHECK(withBase == 85 && StandardVariant(*catalogue.Find(103)) == 0x1000 &&
              StandardVariant(*catalogue.Find(3000)) == 0x2000 && StandardVariant(*catalogue.Find(404)) == 0x0403 &&
              StandardVariant(*catalogue.Find(9)) == 0 && VersionCount(*catalogue.Find(103)) == 2 &&
              VersionCount(*catalogue.Find(404)) == 0 && VersionCount(*catalogue.Find(3000)) == 0 &&
              VersionCount(*catalogue.Find(9)) == 6);
        int giantsBase = 0; // the Giants toys: Jet-Vac 0x1000, the LightCore ones 0x1206
        for (const Toy& toy : catalogue.Toys())
            giantsBase += toy.game == "giants" && StandardVariant(toy) != 0 ? 1 : 0;
        CHECK(giantsBase == 22 && StandardVariant(*catalogue.Find(101)) == 0x1206);

        // A collection's additions: same type replaces, new types add, bad files change nothing.
        Catalogue copy = catalogue;
        std::string error;
        const char* additions = R"json({"format": 1, "toys": [
            {"type": 14, "name": "Gill Grunt (edited)", "element": "water", "category": "core", "game": "ssa"},
            {"type": 9999, "name": "Homemade", "category": "magic_item", "game": "ssa",
             "variants": [{"name": "Crystal Homemade", "kind": "crystal", "same_tag": true}]}]})json";
        CHECK(copy.Load(additions, error));
        CHECK(copy.Find(14)->name == "Gill Grunt (edited)" && copy.Find(9999) != nullptr &&
              copy.Toys().size() == catalogue.Toys().size() + 1);
        CHECK(copy.Find(9999)->variants.size() == 1 && copy.Find(9999)->variants[0].sameTag);
        size_t before = copy.Toys().size();
        CHECK(!copy.Load(R"({"format": 1, "toys": [{"type": 15, "name": "X", "category": "nonsense"}]})", error));
        CHECK(!copy.Load(R"({"format": 2})", error) && !copy.Load("{", error));
        CHECK(copy.Toys().size() == before && copy.Find(15)->name == "Slam Bam");
    }

    // Trap Team [TrapTeam]: a trap, a Trap Master, a mini and an Eon's Elite through the collection; only the trap has no level.
    void TestTrapTeam()
    {
        using namespace collection;
        std::string trap = Create(210, 0x3014), master = Create(450), mini = Create(502), elite = Create(9, 0x3810);
        Figure figure;
        CHECK(Read(trap, figure) && figure.type == 210 && figure.Kind() == VariantKind::Shape && !HasProgress(figure) &&
              LevelText(figure, "trapteam").empty());
        CHECK(Read(master, figure) && HasProgress(figure) && LevelText(figure, "trapteam") == "New");
        CHECK(Read(mini, figure) && HasProgress(figure) && Read(elite, figure) &&
              figure.Kind() == VariantKind::EonsElite && HasProgress(figure));
        figures::FigureStats stats;
        figures::SetTotalExperience(stats, figures::LevelExperience[19]);
        CHECK(WriteStats(master, stats) && Read(master, figure) && figure.level == 20 &&
              LevelText(figure, "trapteam") == "Level 20" && LevelText(figure, "giants") == "Level 15 (20)");
        // A trap's villain and evolved switch: the choice is stored, another can follow, a busy dump is refused.
        std::string error;
        CHECK(SetTrapVillain(trap, 9, true) && Read(trap, figure) && figure.state == figures::AreaState::Valid &&
              figure.stats.villain == 9 && figure.stats.villainEvolved);
        CHECK(SetTrapVillain(trap, 12, false) && Read(trap, figure) && figure.stats.villain == 12 &&
              !figure.stats.villainEvolved);
        CHECK(SetTrapVillain(trap, 9, false) && Read(trap, figure) && figure.stats.villain == 9);
        CHECK(!SetTrapVillain(trap, 99, false, &error) && error == "no such villain");
        CHECK(!SetTrapVillain(master, 9, false, &error) && Read(master, figure) && figure.stats.villain == 0);
        const std::string busy = R"({"game": "launcher", "pid": )" + LivePid + R"(, "since": "2026-10-24T10:00:00Z"})";
        SDL_SaveFile((trap + ".lock").c_str(), busy.data(), busy.size());
        CHECK(GetLock(trap).Busy() && !SetTrapVillain(trap, 12, false, &error) && error == "in use by launcher");
        SDL_RemovePath((trap + ".lock").c_str());
        for (const std::string& path : { trap, master, mini, elite })
            CHECK(Delete(path));
    }

    // SuperChargers' and Imaginators' toys: a plain figure's year-and-flag id reads as the toy, a crystal's shape as its
    // name, a crystal shows its battle class.
    void TestLaterGames()
    {
        using namespace collection;
        const Catalogue& catalogue = Catalogue::Get();
        const Toy* fiesta = catalogue.Find(3400);
        const Toy* vehicle = catalogue.Find(3227);
        const Toy* crystal = catalogue.Find(682);
        CHECK(fiesta != nullptr && vehicle != nullptr && vehicle->category == Category::Vehicle && crystal != nullptr &&
              crystal->category == Category::CreationCrystal && crystal->element == Element::Air);
        VariantInfo plain = catalogue.Describe(fiesta, 0x4100);
        CHECK(plain.name == "Fiesta" && plain.kind == VariantKind::Base && plain.known);
        CHECK(catalogue.Describe(vehicle, 0x4000).name == "Crypt Crusher");
        CHECK(catalogue.Describe(catalogue.Find(601), 0x5000).name == "King Pen");
        CHECK(catalogue.Describe(fiesta, 0x4515).name == "Frightful Fiesta");
        CHECK(catalogue.Describe(catalogue.Find(3500), 0x4000).name == "Sky Trophy");
        Catalogue::Reading seen = catalogue.Read("superchargers", fiesta, 0x4100);
        CHECK(seen.works && seen.kind == VariantKind::Base);
        VariantInfo shape = catalogue.Describe(crystal, 0x5202);
        CHECK(shape.name == "Air Angel Crystal" && shape.kind == VariantKind::Shape);
        CHECK(catalogue.Describe(crystal, 0x5299).kind == VariantKind::Shape); // an unlisted shape stays a shape
        seen = catalogue.Read("imaginators", crystal, 0x5207);
        CHECK(seen.works && seen.exact && seen.seen != nullptr && seen.seen->name == "Air Lantern Crystal");
        CHECK(!catalogue.Read("superchargers", crystal, 0x5207).works); // SuperChargers does not read crystals

        std::string made = Create(682, 0x5202);
        Figure figure;
        CHECK(Read(made, figure) && figure.DisplayName() == "Air Angel Crystal" && !HasProgress(figure) &&
              figure.state == figures::AreaState::Blank && LevelText(figure, "imaginators") == "Ready to create" &&
              LevelText(figure, "imaginators", true) == "À créer");
        CHECK(BattleClassText(4) == "Ninja" && BattleClassText(1, true) == "Chevalier" && BattleClassText(11).empty());
        // Its first creation, then an edit: the class and the level show on the card.
        figures::CrystalStats creation;
        creation.name = "ZAPPER";
        creation.battleClass = 7;
        creation.experience = 6000; // level 5
        std::string why;
        CHECK(WriteCrystal(made, creation, &why) && Read(made, figure) && figure.state == figures::AreaState::Valid &&
              figure.level == 5 && LevelText(figure, "imaginators") == "Virtual · Sorcerer · Level 5" &&
              figures::NicknameUtf8(figure.stats) == "ZAPPER");
        figures::CrystalStats back;
        CHECK(ReadCrystal(made, back) == figures::AreaState::Valid && back.hasRecipe && back.battleClass == 7);
        back.experience = figures::CrystalLevelExperience(42);
        CHECK(WriteCrystal(made, back) && Read(made, figure) && figure.level == 42 &&
              LevelText(figure, "imaginators", true) == "Virtuel · Sorcier · Niveau 42");
        CHECK(std::string(VirtualToyText("de")).rfind("Virtuell", 0) == 0 &&
              std::string(VirtualToyText("xx")) == VirtualToyText("en"));
        CHECK(Read(made, figure) && figure.missingSignature); // made here: no factory signature
        FigureData other3 = figures::CreateCrystal(683, 0x5209, back);
        CHECK(ReplaceDump(made, other3) && Read(made, figure) && figure.type == 683 && figure.level == 42);
        std::string other = Create(3227);
        CHECK(!WriteCrystal(other, back, &why) && why == "not a creation crystal" && Delete(other));
        // The launcher's and the Toy Box's plain crystal: no base variant in the catalogue, so variant 0; it still gets
        // the 2016 sector-0 trailer (access FF 07 80 69, key byte 0x51), or the game calls it corrupted.
        const Toy* undead = catalogue.Find(683);
        std::string plainCrystal = Create(683, StandardVariant(*undead));
        std::string plainTag = ReadText(plainCrystal);
        CHECK(StandardVariant(*undead) == 0 && plainTag.size() == 1024 && plainTag[0x1C] == 0 && plainTag[0x1D] == 0 &&
              uint8_t(plainTag[0x36]) == 0xFF && uint8_t(plainTag[0x37]) == 0x07 && uint8_t(plainTag[0x38]) == 0x80 &&
              uint8_t(plainTag[0x3F]) == 0x51 && Delete(plainCrystal));
        CHECK(Delete(made));

        // A vehicle's gearbits: only once the game used it (its first use writes the fields beside them).
        std::string car = Create(3227), fiestaDump = Create(3400), error;
        CHECK(!SetGearbits(car, 500, &error) && error == "never used by the game");
        CHECK(!SetGearbits(fiestaDump, 500, &error) && error == "not a vehicle");
        figures::FigureStats used;
        used.platforms = 2;
        used.giantsExperience = 9;
        CHECK(WriteStats(car, used) && SetGearbits(car, 500) && Read(car, figure) &&
              figures::Gearbits(figure.stats) == 500 && figure.stats.giantsExperience == 9 && !HasProgress(figure));
        // Its garage upgrades on the card: bits 0-4 the shield's, 5-9 the weapon's (0x0023 = shield 2, weapon 1).
        CHECK(LevelText(figure, "superchargers") == "Shield 0 · Weapon 0");
        used.upgrades = 0x0023;
        CHECK(WriteStats(car, used) && Read(car, figure) &&
              LevelText(figure, "superchargers") == "Shield 2 · Weapon 1" &&
              LevelText(figure, "superchargers", true) == "Bouclier 2 · Arme 1");
        CHECK(Delete(car) && Delete(fiestaDump));
    }

    // A blank LightCore toy: the Giants toy with its own variant, as the Toy Box creates it. Last, so that the counts
    // the other tests make are not changed.
    void TestGiants()
    {
        using namespace collection;
        std::string lightcore = Create(100, 0x1206);
        Figure figure;
        CHECK(Read(lightcore, figure) && figure.type == 100 && figure.variant == 0x1206 &&
              figure.Kind() == VariantKind::LightCore);
        CHECK(figure.DisplayName() == "LightCore Jet-Vac" && figure.state == figures::AreaState::Blank &&
              figure.level == 1);
        figures::FigureStats stats;
        figures::SetTotalExperience(stats, 42700);
        CHECK(
            WriteStats(lightcore, stats) && Read(lightcore, figure) && figure.level == 11 && figure.variant == 0x1206);
        CHECK(Duplicate(lightcore) != "" && Read(lightcore, figure) && figure.level == 11);

        // Every variant Giants knows, created as the Toy Box and Skylauncher create it: the tag carries the toy type
        // and the variant id the catalogue gives, the collection names it, and Giants reads the subtype it shows
        // (Spyro's Adventure reads the toy type only: the same character, docs/runtime/host/figures.md).
        int made = 0;
        for (const Toy& toy : Catalogue::Get().Toys())
            for (const Variant& variant : toy.variants)
            {
                if ((toy.game != "ssa" && toy.game != "giants") || !GiantsKnows(toy, variant))
                    continue;
                std::string path = Create(toy.type, variant.id);
                bool ok = Read(path, figure) && figure.type == toy.type && figure.variant == variant.id &&
                          figure.DisplayName() == variant.name && figure.Kind() == variant.kind;
                CHECK(ok);
                if (!ok)
                    fprintf(stderr, "  created %s (%u:%04X) is not that figure\n", variant.name.c_str(), toy.type,
                        variant.id);
                made += ok;
                CHECK(Delete(path, "test"));
            }
        CHECK(made == 49);
    }

    // collection::Upgrades on a scratch data folder: the plain list without a file, then a file as art::Extract writes
    // it (a made-up one: game text never goes into a test), its languages, a Legendary sharing its base toy's list.
    // "On the wheel": added to the wheel of every player seat that reads the figure, removed for good.
    void TestWheel()
    {
        using namespace collection;
        auto fileOf = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
        auto count = [](const char* game, const char* seat, const std::string& file) {
            std::vector<WheelPlace> wheel = GetWheel(game, seat);
            return std::count_if(
                wheel.begin(), wheel.end(), [&](const WheelPlace& p) { return p.file == file && p.pinned; });
        };
        std::string file = fileOf(Create(14)); // Gill Grunt
        CHECK(!OnWheel("ssa", file) && SetOnWheel("ssa", file, true) && OnWheel("ssa", file));
        CHECK(count("ssa", "p1", file) == 1 && count("ssa", "p2", file) == 1);
        CHECK(SetOnWheel("ssa", file, true) && count("ssa", "p1", file) == 1);
        CHECK(SetOnWheel("ssa", file, false) && !OnWheel("ssa", file) && count("ssa", "p1", file) == 0);
        std::vector<std::string> skipped = GetWheelSkipped("ssa", "p1");
        CHECK(std::find(skipped.begin(), skipped.end(), file) != skipped.end());
        CHECK(SetOnWheel("ssa", file, true));
        skipped = GetWheelSkipped("ssa", "p1");
        CHECK(std::find(skipped.begin(), skipped.end(), file) == skipped.end());
        // Giants does not read a Swap Force toy; with all eight places pinned a ninth figure is refused.
        CHECK(!SetOnWheel("giants", fileOf(Create(3000)), true));
        std::vector<WheelPlace> full(8);
        for (size_t i = 0; i < full.size(); i++)
            full[i] = { "x" + std::to_string(i) + ".sky", true };
        CHECK(SetWheel("giants", "p1", full) && SetWheel("giants", "p2", full) && !SetOnWheel("giants", file, true) &&
              !OnWheel("giants", file));
    }

    void TestUpgrades()
    {
        using namespace collection;
        SetVariable("SKYLANDERS_DATA", g_scratch + "/data");
        const Toy* spyro = Catalogue::Get().Find(16);
        UpgradeTable plain = Upgrades(*spyro, "giants", "en");
        CHECK(!plain.fromDisc && plain.upgrades.size() == 11 && plain.upgrades[0].bit == 2 &&
              plain.upgrades[0].name == "Upgrade 1");
        CHECK(plain.upgrades[4].bit == 6 && plain.upgrades[4].path == UpgradePath::First &&
              plain.upgrades[7].bit == 6 && plain.upgrades[7].path == UpgradePath::Second &&
              plain.upgrades[10].bit == UpgradeSoulGemBit && plain.upgrades[10].price == 4000);
        CHECK(Upgrades(*spyro, "giants", "fr").upgrades[0].name == "Amélioration 1" && plain.pathNames[1] == "Path 2");

        // The Edit rules: basic ones toggle; a path upgrade chooses its path; the other path is greyed and switching to it
        // keeps the bought count; All / None.
        const Upgrade& basic = plain.upgrades[0];
        const Upgrade& first = plain.upgrades[4];
        const Upgrade& second = plain.upgrades[7];
        uint16_t bits = ToggleUpgrade(0, basic);
        CHECK(bits == 0x0004 && UpgradeBought(bits, basic) && ToggleUpgrade(bits, basic) == 0);
        bits = ToggleUpgrade(bits, first);
        CHECK(bits == 0x0045 && UpgradeBought(bits, first) && !UpgradeBought(bits, second) &&
              UpgradeGreyed(bits, second) && !UpgradeGreyed(bits, first));
        bits = ToggleUpgrade(bits, plain.upgrades[5]);
        CHECK(bits == 0x00C5 && BoughtCount(bits, plain) == 3);
        bits = ToggleUpgrade(
            bits, second); // switches to the second path; the two bought path upgrades move, this one stays bought
        CHECK(bits == 0x00C7 && UpgradeBought(bits, second) && UpgradeBought(bits, plain.upgrades[8]) &&
              UpgradeGreyed(bits, first));
        CHECK(AllUpgrades(0, plain) == 0x03FD && AllUpgrades(bits, plain) == 0x03FF && NoUpgrades(0x7FFF) == 0x7000);
        CHECK(BoughtCount(AllUpgrades(0, plain), plain) == 8);
        // Giants' Wow Pow: bit 10 of the editors' bits, stored in FigureStats::wowPow; the field's own bits 10-11 kept.
        CHECK(EditorBits(0x0C7F, 0) == 0x087F && EditorBits(0x007F, 1) == 0x047F);
        uint16_t field = 0x0800;
        uint8_t wow = 0;
        StoreEditorBits(0x07FF, field, wow);
        CHECK(field == 0x0BFF && wow == 1);
        StoreEditorBits(NoUpgrades(EditorBits(field, wow)), field, wow);
        CHECK(field == 0x0800 && wow == 0);
        // The game's marks become words, the description is its first sentence.
        CHECK(UpgradeLabel("Press &ICON(0,ICON_ENTER) to fly.\nFaster", "en") == "Press the button to fly. Faster");
        CHECK(UpgradeLine("Press { to fly. Faster", "fr") == "Press le bouton to fly" &&
              UpgradeLine("Big hit!  Really.", "en") == "Big hit!");

        std::filesystem::path file = UpgradeFile("giants");
        CHECK(file == std::filesystem::path(g_scratch) / "data" / "Giants" / "upgrades.json");
        std::filesystem::create_directories(file.parent_path());
        const char* made = R"json({"format": 1, "game": "giants", "toys": {"16": {
            "paths": [{"name": {"en": "Path A", "fr": "Voie A"}, "description": {"en": "First"}}, {"name": {"en": "Path B"}}],
            "upgrades": [{"bit": 2, "path": 0, "price": 500, "name": {"en": "Upgrade A", "fr": "Amélioration A"}, "description": {"en": "Does A"}},
                         {"bit": 10, "path": 4, "price": 5000, "name": {"en": "Wow"}, "description": {}}]}}})json";
        SDL_SaveFile(datadir::Text(file).c_str(), made, strlen(made));
        UpgradeTable read = Upgrades(*spyro, "giants", "fr");
        CHECK(read.fromDisc && read.upgrades.size() == 2 && read.upgrades[0].name == "Amélioration A" &&
              read.upgrades[0].description == "Does A");
        CHECK(read.upgrades[1].bit == UpgradeWowPowBit && read.upgrades[1].path == UpgradePath::WowPow &&
              read.upgrades[1].price == 5000);
        CHECK(read.pathNames[0] == "Voie A" && read.pathNames[1] == "Path B" && read.pathDescriptions[0] == "First");
        CHECK(Upgrades(*Catalogue::Get().Find(416), "giants", "en").upgrades[0].name ==
              "Upgrade A"); // Legendary Spyro: Spyro's
        CHECK(Upgrades(*Catalogue::Get().Find(28), "giants", "en").fromDisc); // Dark Spyro too
        CHECK(!Upgrades(*Catalogue::Get().Find(14), "giants", "en").fromDisc); // not in the file
        CHECK(!Upgrades(*spyro, "ssa", "en").fromDisc); // the other game's file
        std::filesystem::remove(file);
        CHECK(!Upgrades(*spyro, "giants", "en").fromDisc);
    }

    void TestCollection()
    {
        using namespace collection;
        CHECK(Folder() == g_scratch + "/Collection");
        CHECK(List().empty());

        // Create: names from the catalogue, never over an existing file.
        std::string gill = Create(14);
        CHECK(gill == PathOf("014 Gill Grunt.sky"));
        CHECK(Create(14) == PathOf("014 Gill Grunt (2).sky"));
        std::string series = Create(14, 0x1801);
        CHECK(series == PathOf("014 Series 2 Gill Grunt.sky"));
        std::string elite = Create(14, 0x3810);
        std::string item = Create(202);
        std::string giant = Create(102);
        CHECK(Create(5, 0, "a/b:c.sky") == PathOf("a_b_c.sky"));
        CHECK(CreateMissing({ 14, 15, 202 }) == 1); // only Slam Bam was missing

        Figure figure;
        CHECK(Read(gill, figure) && figure.type == 14 && figure.variant == 0 && figure.toy != nullptr);
        CHECK(figure.state == figures::AreaState::Blank && figure.level == 1 && !figure.lock.locked);
        CHECK(Read(series, figure) && figure.Kind() == VariantKind::Series &&
              figure.DisplayName() == "Series 2 Gill Grunt");

        // Stats go into the dump itself.
        figures::FigureStats stats;
        stats.experience = 9000;
        stats.gold = 3000;
        CHECK(WriteStats(gill, stats));
        CHECK(SetNickname(gill, "Ace"));
        CHECK(Read(gill, figure) && figure.state == figures::AreaState::Valid && figure.level == 6 &&
              figure.stats.gold == 3000);
        CHECK(figures::NicknameUtf8(figure.stats) == "Ace");

        // Giants through the collection: level 15 with a hat in a Giants toy (a LightCore: TestGiants).
        figures::FigureStats high;
        figures::SetTotalExperience(high, 96500);
        high.hat = 5;
        CHECK(WriteStats(giant, high) && Read(giant, figure) && figure.level == 15 && figure.stats.hat == 5);
        CHECK(figures::TotalExperience(figure.stats) == 96500 && figure.stats.experience == 33000);
        CHECK(ResetToNew(giant) && Read(giant, figure) && figure.level == 1 && figure.stats.giantsExperience == 0 &&
              figure.state == figures::AreaState::Blank);

        // Metadata in collection.json.
        CHECK(SetLabel(item, "My elixir") && SetFavourite(item, true));
        CHECK(SetEdition(gill, "Crystal Clear Gill Grunt", VariantKind::Crystal));
        Touch(gill, "ssa");
        CHECK(Read(item, figure) && figure.label == "My elixir" && figure.favourite &&
              figure.DisplayName() == "My elixir");
        CHECK(Read(gill, figure) && figure.Kind() == VariantKind::Crystal && figure.lastGame == "ssa" &&
              figure.lastUsed.size() == 20);
        CHECK(SetFavourite(item, false) && Read(item, figure) && !figure.favourite);
#ifndef _WIN32
        {
            // A link left where a temporary could be is never followed.
            const std::filesystem::path index = datadir::FromText(Folder() + "/collection.json"),
                                        sentinel = index.parent_path() / "sentinel.txt";
            std::ofstream(sentinel) << "keep";
            std::error_code linked;
            std::filesystem::create_symlink(sentinel, std::filesystem::path(index) += ".tmp", linked);
            CHECK(!linked && SetFavourite(item, true));
            std::ifstream in(sentinel);
            CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "keep");
            std::filesystem::remove(std::filesystem::path(index) += ".tmp");
            std::filesystem::remove(sentinel);
            CHECK(SetFavourite(item, false));
        }
#endif

        // Listing and filters.
        CHECK(List().size() == 8);
        Filter filter;
        filter.game = "ssa";
        CHECK(List(filter).size() == 7); // Crusher is a Giant: not an SSA toy
        filter = {};
        filter.category = int(Category::Giant);
        CHECK(List(filter).size() == 1 && List(filter)[0].type == 102);
        filter = {};
        filter.kind = int(VariantKind::EonsElite);
        CHECK(List(filter).size() == 1 && List(filter)[0].path == elite);
        filter = {};
        filter.text = "ace";
        CHECK(List(filter).size() == 1);
        filter = {};
        filter.element = int(Element::Water);
        CHECK(List(filter).size() == 5);
        std::vector<Figure> all = List();
        CHECK(all.front().type == 5 && all.back().type == 202); // core first, items last

        // Rename keeps the metadata.
        std::string error;
        CHECK(Rename(item, "Elixir"));
        std::string renamed = PathOf("Elixir.sky");
        CHECK(Read(renamed, figure) && figure.label == "My elixir");
        CHECK(!Rename(renamed, "014 Gill Grunt.sky", &error) && error.find("exists") != std::string::npos);

        // Locks: ours, another live process's, a dead one's.
        CHECK(Lock(gill, "ssa") && GetLock(gill).mine && !GetLock(gill).Busy());
        CHECK(WriteStats(gill, stats));
        Unlock(gill);
        CHECK(!GetLock(gill).locked);
        std::string other = R"({"game": "launcher", "pid": )" + LivePid + R"(, "since": "2026-10-24T10:00:00Z"})";
        SDL_SaveFile((gill + ".lock").c_str(), other.data(), other.size());
        CHECK(GetLock(gill).Busy() && GetLock(gill).game == "launcher");
        CHECK(!WriteStats(gill, stats, &error) && error == "in use by launcher");
        CHECK(!Lock(gill, "ssa") && !Rename(gill, "x"));
        BreakLock(gill);
        std::string dead = R"({"game": "ssa", "pid": 2147483000, "since": "2026-10-24T10:00:00Z"})";
        SDL_SaveFile((series + ".lock").c_str(), dead.data(), dead.size());
        CHECK(GetLock(series).stale && !GetLock(series).Busy());
        CHECK(ClearStaleLocks() == 1 && !GetLock(series).locked);

        // Backups and restore.
        size_t before = Backups(gill).size(); // WriteStats backs up first
        std::string backup = Backup(gill);
        CHECK(!backup.empty() && Backups(gill).size() == before + 1);
        stats.experience = 33000;
        CHECK(WriteStats(gill, stats) && Read(gill, figure) && figure.level == 10);
        CHECK(Restore(gill, backup) && Read(gill, figure) && figure.level == 6);
        CHECK(Backups(gill).size() == before + 3); // the write and the restore backed up first
        CHECK(Rename(gill, "Gill.sky") && Backups(PathOf("Gill.sky")).size() == before + 3);
    }

    void TestMigration()
    {
        using namespace collection;
        std::string legacy = g_scratch + "/legacy figures";
        SDL_CreateDirectory(legacy.c_str());
        // One dump already in the collection byte for byte, one new, one with a taken name, one not a dump.
        std::string existing = ReadText(PathOf("Gill.sky"));
        SDL_SaveFile((legacy + "/old gill.sky").c_str(), existing.data(), existing.size());
        FigureData spyro = figures::CreateFigure(16);
        SDL_SaveFile((legacy + "/016 Spyro.sky").c_str(), spyro.data(), spyro.size());
        FigureData other = figures::CreateFigure(14);
        SDL_SaveFile((legacy + "/014 Gill Grunt (2).sky").c_str(), other.data(), other.size());
        SDL_SaveFile((legacy + "/short.bin").c_str(), "abc", 3);
        std::string before = ReadText(legacy + "/016 Spyro.sky");

        CHECK(LegacyFolder().empty()); // SKY_COLLECTION is set and SKY_MIGRATE_FROM is not
        Migration result = Migrate(legacy);
        CHECK(result.copied == 2 && result.matched == 1 && result.failed == 1);
        CHECK(ReadText(PathOf("016 Spyro.sky")) == before && ReadText(legacy + "/016 Spyro.sky") == before);
        CHECK(SDL_GetPathInfo(PathOf("014 Gill Grunt (2) (2).sky").c_str(), nullptr));
        CHECK(MigratedPath(legacy + "/016 Spyro.sky") == PathOf("016 Spyro.sky"));
        CHECK(MigratedPath(legacy + "/old gill.sky") == PathOf("Gill.sky"));
        CHECK(MigratedPath("/elsewhere/x.sky") == "/elsewhere/x.sky");
        // A second run copies nothing; originals are all still there.
        result = Migrate(legacy);
        CHECK(result.copied == 0 && result.matched == 0 && result.failed == 1);
        CHECK(SDL_GetPathInfo((legacy + "/old gill.sky").c_str(), nullptr) &&
              SDL_GetPathInfo((legacy + "/short.bin").c_str(), nullptr));
    }

    void TestIcons(const char* sheetFolder)
    {
        using namespace collection;
        const Catalogue& catalogue = Catalogue::Get();
        std::vector<const Toy*> samples;
        for (int category = 0; category < int(Category::Count); category++)
        {
            for (const Toy& toy : catalogue.Toys())
            {
                if (int(toy.category) == category)
                {
                    samples.push_back(&toy);
                    break;
                }
            }
        }
        samples.push_back(nullptr); // an unknown toy
        for (const Toy* toy : samples)
        {
            Image icon = GeneratedIcon(toy, 96);
            CHECK(icon.width == 96 && icon.rgba.size() == 96 * 96);
            CHECK((icon.rgba[0] >> 24) == 0); // transparent corner
            uint32_t opaque = 0;
            for (uint32_t pixel : icon.rgba)
                opaque += (pixel >> 24) == 255;
            CHECK(opaque > 96 * 96 / 25);
            CHECK((GeneratedIcon(toy, 96, true).rgba[96 * 48] >> 24) == 255); // the tile fills the square
        }
        // The family marks both apps draw: a potion, a compass, a paw; inside at their centre, outside at the corners.
        CHECK(HasKindGlyph(Category::MagicItem) && HasKindGlyph(Category::Adventure) && HasKindGlyph(Category::Mini));
        CHECK(!HasKindGlyph(Category::Core) && KindGlyph(Category::Core, 0, 0) == 1);
        CHECK(KindGlyph(Category::MagicItem, 0, 0.28f) < 0 && KindGlyph(Category::Adventure, 0, 0) < 0 &&
              KindGlyph(Category::Mini, 0, 0.4f) < 0);
        CHECK(KindGlyph(Category::MagicItem, 0.95f, 0.95f) > 0 && KindGlyph(Category::Adventure, 0.95f, 0.95f) > 0 &&
              KindGlyph(Category::Mini, -0.95f, 0.95f) > 0);
        CHECK(KindGlyph(Category::Adventure, 0.78f, 0) < 0 &&
              KindGlyph(Category::Adventure, 0.46f, 0.46f) > 0); // the ring, then the gap
        Image potion = GeneratedIcon(catalogue.Find(200), 64), compass = GeneratedIcon(catalogue.Find(300), 64);
        CHECK(potion.rgba != compass.rgba);
        Image plain = GeneratedIcon(catalogue.Find(14), 64), badged = plain;
        AddBadge(badged, VariantKind::Base);
        CHECK(badged.rgba == plain.rgba);
        AddBadge(badged, VariantKind::Legendary);
        CHECK(badged.rgba[64 * 52 + 52] != plain.rgba[64 * 52 + 52]);
        Image half = Resize(plain, 32, 32), twice = Resize(plain, 128, 128);
        CHECK(half.width == 32 && twice.rgba.size() == 128 * 128);
        // No disc pictures in the scratch folder: the icon is drawn.
        bool fromDisc = true;
        Image icon = Icon(catalogue.Find(14), VariantKind::Dark, 80, &fromDisc);
        CHECK(!fromDisc && icon.width == 80);

        if (sheetFolder == nullptr)
            return;
        // Every category, then the badges, on the menus' element disc and on a tile.
        const int cell = 112, columns = 8;
        std::vector<std::pair<const Toy*, VariantKind>> cells;
        for (const Toy* toy : samples)
            cells.push_back({ toy, VariantKind::Base });
        for (int kind = 1; kind < int(VariantKind::Count); kind++)
            cells.push_back({ catalogue.Find(14), VariantKind(kind) });
        int rows = int(cells.size() + columns - 1) / columns;
        Image sheet{ cell * columns, cell * rows * 2,
            std::vector<uint32_t>(size_t(cell * columns) * cell * rows * 2, 0xFF402020) };
        for (size_t i = 0; i < cells.size(); i++)
        {
            for (int tile = 0; tile < 2; tile++)
            {
                Image image = GeneratedIcon(cells[i].first, cell - 8, tile == 1);
                AddBadge(image, cells[i].second);
                int x0 = int(i % columns) * cell + 4, y0 = (int(i / columns) + tile * rows) * cell + 4;
                for (int y = 0; y < image.height; y++)
                {
                    for (int x = 0; x < image.width; x++)
                    {
                        uint32_t p = image.rgba[size_t(y) * image.width + x], a = p >> 24;
                        uint32_t& d = sheet.rgba[size_t(y0 + y) * sheet.width + x0 + x];
                        // On the element disc in the first half, as the Toy Box draws it.
                        float dx = x - image.width / 2.0f, dy = y - image.height / 2.0f;
                        if (tile == 0 && dx * dx + dy * dy < image.width * image.width / 4.0f)
                        {
                            uint32_t rgb =
                                ElementRgb(cells[i].first != nullptr ? cells[i].first->element : Element::None);
                            d = 0xFF000000 | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF);
                        }
                        auto mix = [&](int shift) {
                            return ((p >> shift & 0xFF) * a + (d >> shift & 0xFF) * (255 - a)) / 255 << shift;
                        };
                        d = 0xFF000000 | mix(0) | mix(8) | mix(16);
                    }
                }
            }
        }
        std::string path = std::string(sheetFolder) + "/collection_icons.png";
        CHECK(SavePng(sheet, path));
        printf("icons: %s\n", path.c_str());
    }

    // The Toy Box's additions (docs/runtime/host/toybox_ux.md section 9): seats, ownership and the tidy, recents, trash, duplicate,
    // reset, backups kept, import, views, wheel, sections.
    void TestManagement()
    {
        using namespace collection;
        const Catalogue& catalogue = Catalogue::Get();
        const Game* ssa = catalogue.FindGame("ssa");
        CHECK(ssa != nullptr && ssa->seats.size() == 4 && ssa->seats[0].id == "p1" && ssa->seats[0].player == 1);
        CHECK(ssa != nullptr && ssa->seats[2].id == "item" && ssa->seats[2].accepts.front() == Category::MagicItem);
        CHECK(catalogue.FindGame("trapteam")->seats.size() == 5 &&
              catalogue.FindGame("superchargers")->seats[1].id == "vehicle1");
        // Trap Team's own tables (Init_Setup.bld, tools/trapteam_catalogue.py): 46 villains with their element; a trap's
        // shapes are variants 0x3000 + the shape number; the plain figure's portrait is the newest year's.
        const Game* trap = catalogue.FindGame("trapteam");
        CHECK(trap->villains.size() == 46 && trap->villains[0].id == 1001 && trap->villains[0].name == "Chompy Mage" &&
              trap->villains[0].element == Element::Life && trap->villains[8].name == "The Gulper" &&
              trap->villains[8].element == Element::Water && trap->villains[9].element == Element::None);
        CHECK(trap->portraits.at(450u << 16) == "450_12288_gusto" &&
              trap->portraits.at(474u << 16 | 0x3403) == "474_13315_bushwhack_legendary");
        const Toy* magicTrap = catalogue.Find(210);
        CHECK(magicTrap != nullptr && magicTrap->variants.size() == 6 && magicTrap->variants.back().id == 0x3015);

        // A clean folder for this test.
        for (const Figure& f : List())
            SDL_RemovePath(f.path.c_str());
        CHECK(CreateMissing({ 0, 1, 14 }) == 3);
        std::string mine = Create(16), mineToo = Create(14);
        CHECK(!mine.empty() && mineToo.find("014 Gill Grunt (2).sky") != std::string::npos);
        std::vector<std::string> starters = UntouchedStarters();
        CHECK(starters.size() == 3);
        Figure figure;
        CHECK(Read(mine, figure) && figure.origin == "created" && !UntouchedStarter(figure));
        // A starter the player marked as a favourite is theirs.
        SetFavourite(PathOf("001 Sonic Boom.sky"), true);
        CHECK(UntouchedStarters().size() == 2);
        CHECK(!TidyDone() && Tidy() == 2 && TidyDone() && List().size() == 3);
        std::vector<TrashEntry> trash = Trash();
        CHECK(trash.size() == 2 && trash[0].reason == "tidy");
        std::string back = Undelete(trash[0].id);
        CHECK(!back.empty() && List().size() == 4 && Trash().size() == 1);
        CHECK(PurgeTrash(30) == 0 && PurgeTrash(-1) == 1 && Trash().empty());

        // Delete keeps the metadata in the trash and puts it back.
        SetLabel(mine, "Léa's");
        CHECK(Delete(mine) && !Read(mine, figure) && Trash().size() == 1);
        std::string again = Undelete(Trash()[0].id);
        CHECK(again == mine && Read(mine, figure) && figure.label == "Léa's" && figure.origin == "created");

        // Recents per game.
        Touch(mine, "ssa");
        SDL_Delay(1100);
        Touch(mineToo, "ssa");
        Touch(mineToo, "ssa");
        Touch(PathOf("001 Sonic Boom.sky"), "giants");
        std::vector<std::string> recent = Recent("ssa", 8);
        CHECK(recent.size() == 2 && recent[0] == mineToo && recent[1] == mine);
        CHECK(Read(mineToo, figure) && figure.LastUsedIn("ssa") != "" && figure.uses.size() == 1 &&
              figure.uses[0].uses == 2);

        // Stats writes are backed up first; at most 10 backups are kept.
        figures::FigureStats stats;
        stats.experience = 9000;
        stats.gold = 1234;
        for (int i = 0; i < 12; i++)
            CHECK(WriteStats(mine, stats));
        CHECK(Backups(mine).size() == 10);
        CHECK(Read(mine, figure) && figure.level == 6 && figure.stats.gold == 1234);

        // WriteStats clamps to the game's limits.
        stats.experience = 99999999;
        stats.gold = 65535;
        CHECK(WriteStats(mine, stats) && Read(mine, figure) && figure.stats.experience == 33000 &&
              figure.stats.gold == 65000);
        stats.experience = 9000;
        stats.gold = 1234;
        CHECK(WriteStats(mine, stats));

        // Duplicate: same progress, new serial; reset: blank again, serial kept.
        std::string copy = Duplicate(mine);
        Figure duplicate;
        CHECK(!copy.empty() && Read(copy, duplicate) && duplicate.stats.gold == 1234 && duplicate.level == 6);
        CHECK(duplicate.label == "Léa's (copy)" && duplicate.origin == "created" && duplicate.type == 16);
        std::string a = ReadText(mine).substr(0, 4), b = ReadText(copy).substr(0, 4);
        CHECK(a != b);
        CHECK(ResetToNew(copy) && Read(copy, duplicate) && duplicate.state == figures::AreaState::Blank);
        CHECK(ReadText(copy).substr(0, 4) == b);

        // Import: a folder with a copy of a dump, a new dump, an unknown toy and a non-dump.
        std::string from = g_scratch + "/import";
        SDL_CreateDirectory(from.c_str());
        std::string bytes = ReadText(mine);
        SDL_SaveFile((from + "/same.sky").c_str(), bytes.data(), bytes.size());
        FigureData other = figures::CreateFigure(3);
        SDL_SaveFile((from + "/other.bin").c_str(), other.data(), other.size());
        FigureData unknown = figures::CreateFigure(9999);
        SDL_SaveFile((from + "/mystery.dmp").c_str(), unknown.data(), unknown.size());
        SDL_SaveFile((from + "/notes.sky").c_str(), "hello", 5);
        ImportReport report = Import({ from });
        CHECK(report.added == 2 && report.duplicates == 1 && report.invalid == 1);
        CHECK(report.unknown.size() == 1 && report.unknown[0] == 9999);
        CHECK(Read(PathOf("other.bin"), figure) && figure.origin == "imported");
        CHECK(Import({ from }).added == 0);
        CHECK(ImportTag(other) == PathOf("other.bin"));
        FigureData real = figures::CreateFigure(20);
        std::string kept = ImportTag(real);
        CHECK(kept.find("(portal).sky") != std::string::npos && Read(kept, figure) && figure.origin == "portal");
        CHECK(Export({ mine, copy }, g_scratch + "/export") == 2);

        // Views and wheel places.
        View view = GetView("ssa", "game");
        CHECK(view == View());
        view.group = "series";
        view.foldForeign = false;
        CHECK(SetView("ssa", "game", view) && GetView("ssa", "game") == view && GetView("ssa", "launcher") == View());
        std::vector<WheelPlace> places = GetWheel("ssa", "p1");
        CHECK(places.size() == 8 && places[0].file.empty());
        places[2] = { "016 Spyro.sky", true };
        places[5] = { "014 Gill Grunt (2).sky", false };
        CHECK(SetWheel("ssa", "p1", places));
        places = GetWheel("ssa", "p1");
        CHECK(places[2].pinned && places[2].file == "016 Spyro.sky" && places[5].file.size() > 0 && !places[5].pinned);
        CHECK(GetView("ssa", "game") == view);

        // Sections: Smart is portal, recent, favourites, then elements; unknown and foreign toys at the end.
        const Toy* giantToy = nullptr;
        for (const Toy& toy : catalogue.Toys())
        {
            if (giantToy == nullptr && toy.category == Category::Giant)
                giantToy = &toy;
        }
        std::string giant = Create(giantToy->type); // SSA does not read Giants
        std::vector<Figure> list = List();
        // Smart is the shelf: every figure once, favourites first, then what the game took (newest first).
        std::vector<Section> sections = Group(list, GroupBy::Smart, OrderBy::Recent, "ssa");
        CHECK(sections.size() >= 3 && sections[0].kind == Section::Kind::Shelf);
        CHECK(sections[sections.size() - 2].kind == Section::Kind::Unknown &&
              sections.back().kind == Section::Kind::Foreign);
        CHECK(sections.back().items.size() == 1 && list[sections.back().items[0]].path == giant);
        {
            std::set<size_t> seen;
            size_t total = 0;
            for (const Section& section : sections)
            {
                total += section.items.size();
                seen.insert(section.items.begin(), section.items.end());
            }
            CHECK(total == list.size() && seen.size() == list.size()); // no figure twice, none left out
            const std::vector<size_t>& shelf = sections[0].items;
            size_t favourites = 0;
            while (favourites < shelf.size() && list[shelf[favourites]].favourite)
                favourites++;
            bool sorted = true;
            for (size_t i = favourites; i < shelf.size(); i++)
            {
                sorted = sorted && !list[shelf[i]].favourite;
                if (i + 1 < shelf.size())
                    sorted = sorted && list[shelf[i]].LastUsedIn("ssa") >= list[shelf[i + 1]].LastUsedIn("ssa");
            }
            CHECK(favourites > 0 && sorted);
        }
        // The level a card shows: the game's cap first, the figure's own level in brackets above it.
        {
            std::string high = Create(9), item = Create(205);
            Figure eruptor, shield;
            CHECK(Read(high, eruptor) && LevelText(eruptor, "ssa") == "New" && HasProgress(eruptor));
            figures::SetTotalExperience(eruptor.stats, figures::LevelExperience[figures::MaxLevelGiants - 1]);
            CHECK(WriteStats(high, eruptor.stats) && Read(high, eruptor) && eruptor.level == 15);
            CHECK(LevelText(eruptor, "ssa") == "Level 10 (15)" && LevelText(eruptor, "giants") == "Level 15");
            CHECK(LevelText(eruptor, "ssa", true) == "Niveau 10 (15)");
            // Swap Force's cap is 20 (its own table: 197500 = 33000 + 63500 + 101000): the same rule, a level 20 figure.
            figures::SetTotalExperience(eruptor.stats, figures::LevelExperience[figures::MaxLevelSwapForce - 1]);
            CHECK(WriteStats(high, eruptor.stats) && Read(high, eruptor) && eruptor.level == 20 &&
                  eruptor.stats.swapForceExperience == 101000);
            CHECK(LevelText(eruptor, "ssa") == "Level 10 (20)" && LevelText(eruptor, "giants") == "Level 15 (20)" &&
                  LevelText(eruptor, "swapforce") == "Level 20");
            CHECK(LevelText(eruptor, "swapforce", true) == "Niveau 20" &&
                  LevelText(eruptor, "giants", true) == "Niveau 15 (20)");
            figures::SetTotalExperience(eruptor.stats, figures::LevelExperience[figures::MaxLevelGiants - 1]);
            CHECK(WriteStats(high, eruptor.stats) && Read(high, eruptor) && eruptor.level == 15 &&
                  LevelText(eruptor, "swapforce") == "Level 15");
            CHECK(Read(item, shield) && !HasProgress(shield) && LevelText(shield, "ssa").empty());
            CHECK(Delete(high) && Delete(item));
            for (const TrashEntry& entry : Trash()) // leave the trash as it was
                if (entry.file == std::filesystem::path(high).filename().string() ||
                    entry.file == std::filesystem::path(item).filename().string())
                    DeleteForGood(entry.id);
        }
        // The plain Spyro and its copy sit together, the plain one... ordered by toy, then file.
        std::vector<Section> byName = Group(list, GroupBy::Name, OrderBy::Recent, "ssa");
        CHECK(!byName.empty() && byName[0].kind == Section::Kind::Letter);
        std::vector<Section> all = Group(list, GroupBy::Element, OrderBy::Level, "");
        CHECK(all.back().kind == Section::Kind::Unknown); // no game: nothing is foreign
    }
}

namespace
{
    using namespace collection;

    // S-03: a collection.json that does not load is kept and never replaced by a near-empty index.
    // The portal at a glance: the occupied seats with their tags, in the seats' order.
    // Swappers: a top and a bottom, two dumps linked in collection.json, shown as one card.
    void TestSwappers()
    {
        using namespace collection;
        for (const Figure& f : List())
            SDL_RemovePath(f.path.c_str());
        CHECK(MatchingHalf(2003) == 1003 && MatchingHalf(1003) == 2003 && MatchingHalf(14) == 0 &&
              MatchingHalf(2016) == 0);
        CHECK(CreateSwapper(14, 1003).top.empty() && CreateSwapper(1003, 2003).top.empty() && List().empty());

        SwapperPaths a = CreateSwapper(2003, 1003); // Doom Stone
        CHECK(!a.top.empty() && !a.bottom.empty());
        Figure top, bottom;
        CHECK(Read(a.top, top) && Read(a.bottom, bottom) && top.IsSwapTop() && bottom.IsSwapBottom());
        CHECK(top.partner == bottom.file && bottom.partner == top.file && top.origin == "created");
        CHECK(top.variant == StandardVariant(*top.toy) && bottom.variant == StandardVariant(*bottom.toy) &&
              top.variant == 0x2000); // the standard swapper: each half under its sold id
        std::vector<Figure> list = List({ "swapforce" });
        std::vector<Card> cards = Cards(list);
        CHECK(list.size() == 2 && cards.size() == 1 && cards[0].Swapper() && cards[0].first->file == top.file);
        CHECK(cards[0].second->file == bottom.file && cards[0].Halves().size() == 2);
        CHECK(CardName(cards[0]) == "Doom Stone" && CardLevelText(cards[0], "swapforce") == "New");
        CHECK(CardLevelText(cards[0], "swapforce", true) == "Nouveau");
        // Either half finds the card, and the shelf shows it once, as its top.
        list = List({ "swapforce", -1, -1, -1, false, "(bottom)" });
        CHECK(list.size() == 2 && Cards(list).size() == 1);
        Card ofBottom = CardOf(list, list[0].IsSwapBottom() ? 0 : 1);
        CHECK(ofBottom.first->file == top.file && ofBottom.second->file == bottom.file);
        std::vector<Section> sections = Group(list, GroupBy::Smart, OrderBy::Recent, "swapforce");
        CHECK(sections.size() == 1 && sections[0].items.size() == 1 && list[sections[0].items[0]].file == top.file);
        sections = Group(list, GroupBy::Element, OrderBy::Recent, "swapforce");
        CHECK(sections.size() == 1 && sections[0].items.size() == 1);
        sections =
            Group(list, GroupBy::Smart, OrderBy::Recent, "ssa"); // Spyro's Adventure does not read them: still one card
        CHECK(sections.size() == 1 && sections[0].kind == Section::Kind::Foreign && sections[0].items.size() == 1);

        // Rename keeps the link; a half without the link (an unrelated dump) is a card of its own.
        std::string error;
        CHECK(Rename(a.top, "Mine top", &error) && Read(PathOf("Mine top.sky"), top) && top.partner == bottom.file);
        CHECK(Read(a.bottom, bottom) && bottom.partner == "Mine top.sky" && Cards(List({ "swapforce" })).size() == 1);
        a.top = PathOf("Mine top.sky");

        // Duplicate copies both halves and links the copies.
        std::string copyTop = Duplicate(a.top, &error);
        CHECK(!copyTop.empty() && List({ "swapforce" }).size() == 4 && Cards(List({ "swapforce" })).size() == 2);
        Figure copy, copyBottom;
        CHECK(Read(copyTop, copy) && !copy.partner.empty() && Read(PathOf(copy.partner), copyBottom) &&
              copyBottom.partner == copy.file);
        CHECK(Read(a.top, top) && top.partner == bottom.file); // the original is untouched
        for (const Figure* f : { &copy, &copyBottom })
            CHECK(Delete(f->path));
        for (const TrashEntry& entry : Trash())
            DeleteForGood(entry.id);

        // Delete: the other half is left a card of its own; Undelete links them again.
        CHECK(Delete(a.top) && Read(a.bottom, bottom) && bottom.partner.empty() &&
              Cards(List({ "swapforce" })).size() == 1);
        std::string back = Undelete(Trash()[0].id);
        CHECK(back == a.top && Read(a.bottom, bottom) && bottom.partner == "Mine top.sky" && Read(back, top) &&
              top.partner == bottom.file);

        // Unpair, Pair, and what Pair refuses.
        CHECK(Unpair(a.top) && Read(a.top, top) && top.partner.empty() && Read(a.bottom, bottom) &&
              bottom.partner.empty());
        CHECK(Cards(List({ "swapforce" })).size() == 2 && Unpair(a.top));
        CHECK(Pair(a.bottom, a.top) && Read(a.top, top) && top.partner == bottom.file);
        std::string other = Create(2005); // Fire Kraken top
        CHECK(!Pair(a.top, other, &error) && !error.empty() && !Pair(a.top, PathOf("none.sky")));
        std::string gill = Create(14);
        CHECK(!gill.empty() && !Pair(a.top, gill) && Delete(gill));

        // Change the bottom: the card keeps its top and takes another bottom. A bottom that was in another swapper
        // gives that swapper this card's old bottom, so no card disappears.
        SwapperPaths b = CreateSwapper(2005, 1005); // Fire Kraken
        CHECK(ChangeHalf(a.top, b.bottom, &error));
        Read(a.top, top);
        Read(a.bottom, bottom);
        Figure topB, bottomB;
        Read(b.top, topB);
        Read(b.bottom, bottomB);
        CHECK(top.partner == bottomB.file && bottomB.partner == top.file && topB.partner == bottom.file &&
              bottom.partner == topB.file);
        list = List({ "swapforce" });
        cards = Cards(list);
        CHECK(
            cards.size() == 3 &&
            CardName(cards[0]) == "Doom Kraken"); // Doom Stone top on Fire Kraken legs, plus the loose Fire Kraken top
        CHECK(CardName(CardOf(list, 0)) == "Doom Kraken");
        // SwapperName examples
        CHECK(SwapperName("Nitro Magna Charge", "Boom Jet") == "Nitro Magna Jet");
        CHECK(SwapperName("Wash Buckler", "Blast Zone") == "Wash Zone");
        CHECK(SwapperName("Doom Stone", "Fire Kraken") == "Doom Kraken");
        CHECK(SwapperName("Legendary Free Ranger", "Rattle Shake") == "Legendary Free Shake");
        CHECK(SwapperName("Zap", "Boom Jet") == "Zap Jet");
        // To a bottom that has no swapper: the old bottom is left alone, a card of its own.
        std::string loose = Create(1000); // Boom Jet bottom
        CHECK(ChangeHalf(a.top, loose, &error) && Read(a.top, top) && top.partner == "1000 Boom Jet (bottom).sky");
        CHECK(Read(b.bottom, bottomB) && bottomB.partner.empty());
        // Change the top, from the bottom's side; the wrong category is refused.
        CHECK(ChangeHalf(loose, b.top) && Read(b.top, topB) && topB.partner == "1000 Boom Jet (bottom).sky");
        CHECK(Read(a.top, top) && Read(a.bottom, bottom) && top.partner == bottom.file &&
              bottom.partner == top.file); // the old swapper is whole again
        CHECK(!ChangeHalf(loose, a.bottom));

        // A swapper is busy when either half is (a lock of another live process); nothing is changed meanwhile.
        std::string foreign = R"({"game": "ssa", "pid": )" + LivePid + R"(, "since": "2026-10-24T10:00:00Z"})";
        SDL_SaveFile((loose + ".lock").c_str(), foreign.data(), foreign.size());
        list = List({ "swapforce" });
        bool seen = false;
        for (const Card& card : Cards(list))
            seen = seen ||
                   (card.first->file == topB.file && card.Busy() && card.second->file == "1000 Boom Jet (bottom).sky");
        CHECK(seen);
        CHECK(!Unpair(b.top, &error) && error.find("in use") != std::string::npos && !ChangeHalf(b.top, a.bottom) &&
              !Pair(b.top, a.bottom));
        CHECK(Read(b.top, topB) && topB.partner == "1000 Boom Jet (bottom).sky");
        BreakLock(loose);
        for (const TrashEntry& entry : Trash())
            DeleteForGood(entry.id);
    }

    void TestPortalOverview()
    {
        const Game* ssa = Catalogue::Get().FindGame("ssa");
        const Game* trap = Catalogue::Get().FindGame("trapteam");
        CHECK(ssa != nullptr && trap != nullptr);
        if (ssa == nullptr || trap == nullptr)
            return;
        CHECK(ssa->seats.size() == 4 && SeatTag(ssa->seats[0]) == "P1" && SeatTag(ssa->seats[1]) == "P2");
        CHECK(SeatTag(ssa->seats[2]) == "Item" && SeatTag(ssa->seats[3]) == "Adventure");
        CHECK(SeatName(ssa->seats[0]) == "Player 1" && SeatName(ssa->seats[2]) == "Magic item");
        CHECK(SeatTag(ssa->seats[0], true) == "J1" && SeatName(ssa->seats[1], true) == "Joueur 2" &&
              SeatName(ssa->seats[2], true) == "Objet magique");
        CHECK(SeatTag(trap->seats[2]) == "Trap");

        // Nothing on the portal.
        CHECK(PortalOverview(*ssa, { "", "", "", "" }, {}).empty());
        CHECK(PortalOverview(*ssa, {}, {}).empty());
        CHECK(PortalOverviewLine({}, "Empty") == "Empty");

        // P1 and the adventure pack: the items keep the seats' order, whatever the slots.
        std::vector<PortalEntry> entries = PortalOverview(*ssa, { "/c/014 Gill Grunt.sky", "", "", "/c/300 Pack.sky" },
            { "Gill Grunt", "", "", "" }, { 5, -1, -1, 2 });
        CHECK(entries.size() == 2);
        if (entries.size() == 2)
        {
            CHECK(entries[0].seat == 0 && entries[0].slot == 5 && entries[0].tag == "P1" &&
                  entries[0].name == "Gill Grunt");
            CHECK(entries[1].seat == 3 && entries[1].slot == 2 && entries[1].tag == "Adventure" &&
                  entries[1].seatName == "Adventure");
            CHECK(entries[1].name == "300 Pack.sky"); // no name given: the file's
        }
        CHECK(PortalOverviewLine(entries, "Empty") == "P1 Gill Grunt \xC2\xB7 Adventure 300 Pack.sky");
        // Short vectors and a missing slot list: free seats, slot -1.
        entries = PortalOverview(*ssa, { "", "x.sky" }, { "", "Spyro" });
        CHECK(entries.size() == 1 && entries[0].tag == "P2" && entries[0].slot == -1);
        CHECK(PortalOverview(*ssa, { "", "", "", "", "past-the-seats.sky" }, {}).empty());
    }

    // A swapper's picture: its two halves drawn in place on one frame and joined (icons.h), on synthetic images, then through
    // the catalogue's "halves" with the files in a scratch portraits folder.
    void TestSwapperPicture()
    {
        using namespace collection;
        // A picture of `w` x `h` with an opaque rectangle (colour `rgb`) at (x, y) of size rw x rh; the rest transparent.
        auto make = [](int w, int h, int x, int y, int rw, int rh, uint32_t rgb) {
            Image image{ w, h, std::vector<uint32_t>(size_t(w) * h, 0) };
            for (int j = y; j < y + rh; j++)
                for (int i = x; i < x + rw; i++)
                    image.rgba[size_t(j) * w + i] = 0xFF000000u | rgb;
            return image;
        };
        // The crop: the bounding box of the visible pixels; a faint haze (alpha 3) does not count; nothing visible: unchanged.
        Image padded = make(40, 30, 10, 12, 8, 6, 0x0000FF);
        padded.rgba[0] = 0x03FFFFFFu;
        Image cut = CropToVisible(padded);
        CHECK(cut.width == 8 && cut.height == 6 && cut.rgba.size() == 48 && cut.rgba[0] == 0xFF0000FFu &&
              cut.rgba[47] == 0xFF0000FFu);
        Image empty{ 5, 4, std::vector<uint32_t>(20, 0) };
        CHECK(CropToVisible(empty).width == 5 && CropToVisible(empty).height == 4);

        // The join: same-size frames, the top over the bottom where they overlap, the union box.
        Image top = make(64, 64, 10, 8, 20, 20, 0x0000FF),
              bottom = make(64, 64, 20, 20, 20, 20, 0x00FF00); // overlap (20..30, 20..28)
        Image joined = JoinHalves(top, bottom);
        CHECK(joined.width == 30 && joined.height == 32 && joined.rgba.size() == 960); // (10..40) x (8..40)
        CHECK(joined.rgba[0] == 0xFF0000FFu); // the top's corner (10, 8)
        CHECK(joined.rgba[size_t(12) * 30 + 10] == 0xFF0000FFu); // (20, 20): the top over the bottom
        CHECK(joined.rgba[size_t(31) * 30 + 29] == 0xFF00FF00u); // (39, 39): the bottom alone
        CHECK(joined.rgba[size_t(31) * 30 + 0] == 0u); // (10, 39): neither
        // A half with nothing visible: no picture (the caller falls back to the inset).
        CHECK(JoinHalves(top, empty).Empty() && JoinHalves(empty, bottom).Empty());
        // A narrow top over a wide bottom keeps its own width: no rescale of either half.
        Image narrow = JoinHalves(make(64, 64, 28, 0, 8, 30, 0xFF0000), make(64, 64, 8, 30, 48, 10, 0x00FF00));
        CHECK(narrow.width == 48 && narrow.height == 40 && narrow.rgba[size_t(0) * 48 + 20] == 0xFFFF0000u &&
              narrow.rgba[size_t(0) * 48 + 19] == 0u);
        // A bottom frame at half size lands where a full-size one would (+-1 px): 32 x 32 with a 10 x 8 block at (10, 15)
        // is 64 x 64 with 20 x 16 at (20, 30).
        Image half = JoinHalves(make(64, 64, 20, 0, 20, 30, 0xFF0000), make(32, 32, 10, 15, 10, 8, 0x00FF00));
        CHECK(half.width >= 20 && half.width <= 22 && half.height >= 46 &&
              half.height <= 47); // +-1 px: the bilinear edge of the grown frame
        CHECK(half.rgba[size_t(5) * half.width + 10] == 0xFFFF0000u); // the top, row 5
        CHECK(
            half.rgba[size_t(half.height - 3) * half.width + half.width / 2] == 0xFF00FF00u); // the bottom's last rows
        CHECK((half.rgba[size_t(29) * half.width + half.width / 2] >> 24) == 255); // no gap at the waist

        // Through the catalogue: Blast Zone's halves (2004 and 1004) in the scratch portraits folder.
        const Toy* blastTop = Catalogue::Get().Find(2004);
        const Toy* blastBottom = Catalogue::Get().Find(1004);
        CHECK(blastTop != nullptr && blastBottom != nullptr);
        if (blastTop == nullptr || blastBottom == nullptr)
            return;
        CHECK(HalfPicturePath(*blastTop, 0x2000).empty() &&
              SwapperPicture(*blastTop, 0, *blastBottom, 0).Empty()); // no file yet
        std::string folder = g_scratch + "/portraits/artworks";
        SDL_CreateDirectory((g_scratch + "/portraits").c_str());
        SDL_CreateDirectory(folder.c_str());
        CHECK(SavePng(make(64, 64, 8, 4, 48, 24, 0x2040FF), folder + "/fireknightbody_wii_material.png"));
        CHECK(SavePng(make(64, 64, 16, 30, 32, 20, 0x40FF20), folder + "/fireknightlegs_wii_material.png"));
        CHECK(HalfPicturePath(*blastTop, 0x2000) ==
              folder + "/fireknightbody_wii_material.png"); // the base version: the plain toy's
        CHECK(HalfPicturePath(*blastBottom, 0) == folder + "/fireknightlegs_wii_material.png");
        Image blast = SwapperPicture(*blastTop, 0x2000, *blastBottom, 0);
        CHECK(blast.width == 48 &&
              blast.height ==
                  46); // frames of 64: the top (8..56, 4..28) and the legs (16..48, 30..50): box 8..56 x 4..50
        // Dark Blast Zone has pictures of its own, not on the disc folder yet: its halves fall back to the plain toy's.
        CHECK(HalfPicturePath(*blastTop, 0x2402) == folder + "/fireknightbody_wii_material.png");
        CHECK(SavePng(make(64, 64, 0, 0, 10, 10, 0xFF0000), folder + "/darkblasttop_wii_material.png"));
        CHECK(HalfPicturePath(*blastTop, 0x2402) == folder + "/darkblasttop_wii_material.png");
        // A toy that is not a swapper half has none.
        CHECK(Catalogue::Get().Find(0) != nullptr && HalfPicturePath(*Catalogue::Get().Find(0), 0).empty());
    }

    // Synthetic, separate per-game caches: viewing a game or changing icon size never picks older art.
    void TestPictureRule()
    {
        const std::string data = g_scratch + "/picture-rule";
        const std::string oldData = getenv("SKYLANDERS_DATA") != nullptr ? getenv("SKYLANDERS_DATA") : "";
        const std::string oldPortraits = skyenv::Get("SKY_PORTRAITS");
        SetVariable("SKYLANDERS_DATA", data);
        UnsetVariable("SKY_PORTRAITS");
        UnsetVariable("SSA_PORTRAITS");
        const Catalogue& catalogue = Catalogue::Get();
        const Toy* eruptor = catalogue.Find(9);
        const Toy* top = catalogue.Find(2004);
        const Toy* bottom = catalogue.Find(1004);
        CHECK(eruptor != nullptr && top != nullptr && bottom != nullptr);
        if (eruptor == nullptr || top == nullptr || bottom == nullptr)
            return;
        const Game& giants = *catalogue.FindGame("giants");
        const Game& swap = *catalogue.FindGame("swapforce");
        const Game& trap = *catalogue.FindGame("trapteam");
        auto path = [&](const Game& game, const std::string& name) {
            // Preferred separators, as the library builds its paths ("artworks/x" is "artworks\\x" on Windows).
            return datadir::Text((datadir::Game(game.id) / "portraits" / (name + ".png")).make_preferred());
        };
        auto write = [&](const Game& game, const std::string& name, uint32_t color) {
            std::string file = path(game, name);
            std::filesystem::create_directories(std::filesystem::path(file).parent_path());
            CHECK(SavePng({ 8, 8, std::vector<uint32_t>(64, color) }, file));
            return file;
        };
        const uint32_t key = uint32_t(eruptor->type) << 16;
        std::string oldest = write(*catalogue.FindGame("ssa"), "toy_eruptor_hud", 0xFF0000FFu);
        std::string newer = write(giants, giants.portraits.at(key), 0xFF00FF00u);
        std::string oldArtwork = write(giants, "artworks/" + giants.artworks.at(key), 0xFF00FF00u);
        std::string newest = write(trap, trap.portraits.at(key), 0xFFFF0000u);
        // Compared as paths: on Windows the library joins with '\\' what the test joined with '/' ("artworks/...").
        auto same = [](const std::string& a, const std::string& b) {
            return std::filesystem::path(a).lexically_normal() == std::filesystem::path(b).lexically_normal();
        };
        auto allViews = [&](const Toy& toy, uint16_t variant, const std::string& portrait, const std::string& best) {
            for (const char* viewed : { "ssa", "giants", "swapforce", "trapteam", "", "unknown" })
            {
                CHECK(same(PortraitPath(viewed, toy, variant, false), portrait));
                CHECK(same(PortraitPath(viewed, toy, variant, true), portrait));
                CHECK(same(BestPicturePath(viewed, toy, variant), best));
            }
        };
        allViews(*eruptor, 0, newest, newest); // a newer portrait beats older large artwork
        CHECK(same(DiscImagePath(*eruptor), newest));
        for (int size : { 32, 128, 256 })
        {
            bool fromDisc = false;
            Image icon = Icon(eruptor, VariantKind::Base, size, &fromDisc);
            CHECK(fromDisc && icon.width == size && icon.height == size && icon.rgba[size * size / 2] == 0xFFFF0000u);
        }
        // A variant's own older portrait beats a newer plain portrait or artwork, at every size and in every view.
        std::string variant = write(giants, giants.portraits.at(key | 0x1206), 0xFF00FFFFu);
        allViews(*eruptor, 0x1206, variant, variant);
        CHECK(VariantFor(*eruptor, VariantKind::LightCore) == 0x1206);
        bool fromDisc = false;
        CHECK(!Icon(eruptor, VariantKind::LightCore, 64, &fromDisc).Empty() && fromDisc);
        std::string latestVariant = write(trap, trap.portraits.at(key | 0x1206), 0xFFFFFF00u);
        allViews(*eruptor, 0x1206, latestVariant, latestVariant);
        std::filesystem::remove(latestVariant);
        std::filesystem::remove(variant);
        std::string variantArtwork = write(giants, "artworks/" + giants.artworks.at(key | 0x1206), 0xFF00FFFFu);
        allViews(*eruptor, 0x1206, variantArtwork, variantArtwork); // artwork alone still identifies the variant
        std::filesystem::remove(variantArtwork);
        allViews(*eruptor, 0x1206, newest, newest); // no variant art anywhere: the newest plain art
        std::filesystem::remove(newest);
        allViews(*eruptor, 0, newer, oldArtwork);
        std::filesystem::remove(newer);
        std::filesystem::remove(oldArtwork);
        allViews(*eruptor, 0, oldest, oldest);
        std::filesystem::remove(oldest);
        allViews(*eruptor, 0, "", "");
        Image generated = Portrait("ssa", eruptor, VariantKind::Base, 64, 0, &fromDisc);
        CHECK(!fromDisc && !generated.Empty() &&
              std::any_of(generated.rgba.begin(), generated.rgba.end(), [](uint32_t pixel) { return pixel >> 24; }));

        // Every incomplete swapper keeps two visible icons; only two genuine half files may be joined at the waist.
        const uint32_t topKey = uint32_t(top->type) << 16, bottomKey = uint32_t(bottom->type) << 16;
        std::string topHalf = write(swap, "artworks/" + swap.halves.at(topKey), 0xFF0000FFu);
        std::string bottomHalf = write(swap, "artworks/" + swap.halves.at(bottomKey), 0xFF00FF00u);
        CHECK(same(HalfPicturePath(*top), topHalf) && same(HalfPicturePath(*bottom), bottomHalf));
        CHECK(!SwapperPicture(*top, 0, *bottom, 0).Empty());
        allViews(*top, 0, topHalf, topHalf);
        allViews(*bottom, 0, bottomHalf, bottomHalf);
        std::string darkWhole = write(swap, swap.portraits.at(topKey | 0x2402), 0xFF00FFFFu);
        allViews(*top, 0x2402, darkWhole, darkWhole); // dedicated variant art before a plain half image
        CHECK(same(HalfPicturePath(*top, 0x2402), darkWhole));
        CHECK(SwapperPicture(*top, 0x2402, *bottom, 0x2402).Empty()); // show dedicated variant art instead
        std::filesystem::remove(darkWhole);
        std::string whole = write(trap, trap.portraits.at(topKey), 0xFFFF0000u);
        std::filesystem::remove(bottomHalf); // top only: bottom uses the matching whole figure, from Trap Team
        CHECK(same(HalfPicturePath(*top), topHalf) && same(HalfPicturePath(*bottom), whole));
        allViews(*bottom, 0, whole, whole);
        CHECK(SwapperPicture(*top, 0, *bottom, 0).Empty());
        bottomHalf = write(swap, "artworks/" + swap.halves.at(bottomKey), 0xFF00FF00u);
        std::filesystem::remove(topHalf); // bottom only
        CHECK(same(HalfPicturePath(*top), whole) && same(HalfPicturePath(*bottom), bottomHalf));
        CHECK(SwapperPicture(*top, 0, *bottom, 0).Empty());
        std::filesystem::remove(bottomHalf); // neither half, but whole-figure art remains
        CHECK(same(HalfPicturePath(*top), whole) && same(HalfPicturePath(*bottom), whole));
        CHECK(SwapperPicture(*top, 0, *bottom, 0).Empty());
        std::filesystem::remove(whole); // neither half nor whole: both generated icons have visible pixels
        for (const Toy* half : { top, bottom })
        {
            CHECK(HalfPicturePath(*half).empty());
            Image icon = Portrait("trapteam", half, VariantKind::Base, 64, 0, &fromDisc);
            CHECK(!fromDisc && !icon.Empty() &&
                  std::any_of(icon.rgba.begin(), icon.rgba.end(), [](uint32_t pixel) { return pixel >> 24; }));
        }
        // With only one half file and no whole figure, the other half still gets its generated icon.
        for (const Toy* present : { top, bottom })
        {
            const Toy* missing = present == top ? bottom : top;
            std::string own = write(swap, "artworks/" + swap.halves.at(uint32_t(present->type) << 16), 0xFF00FF00u);
            CHECK(same(HalfPicturePath(*present), own) && HalfPicturePath(*missing).empty());
            CHECK(!Portrait("ssa", missing, VariantKind::Base, 64, 0, &fromDisc).Empty() && !fromDisc);
            CHECK(SwapperPicture(*top, 0, *bottom, 0).Empty());
            std::filesystem::remove(own);
        }
        CHECK(HalfPicturePath(*eruptor).empty());
        SetVariable("SKY_PORTRAITS", oldPortraits);
        if (oldData.empty())
            UnsetVariable("SKYLANDERS_DATA");
        else
            SetVariable("SKYLANDERS_DATA", oldData);
    }

    // The smart search (search.h): names, elements, games, toy types, versions, levels, both languages, accents, typos.
    void TestSearch()
    {
        using namespace collection;
        for (const Figure& f : List())
            SDL_RemovePath(f.path.c_str());
        auto make = [](uint16_t type, uint16_t variant = 0) {
            Figure f;
            Read(Create(type, variant), f);
            return f.path;
        };
        std::string spyro = make(16), gill = make(14), flash = make(103, 0x1000), jet = make(100, 0x1206),
                    trigger = make(419, 0x0403);
        std::string peak = make(300), anvil = make(200), terra = make(505), scratch = make(3000),
                    trapMaster = make(450);
        SwapperPaths swapper = CreateSwapper(2003, 1003); // Doom Stone
        figures::FigureStats stats;
        stats.experience = 33000; // level 10
        CHECK(WriteStats(gill, stats));
        CHECK(SetLabel(spyro, "Étincelle") && SetLabel(swapper.top, "Mon Rocher"));
        std::vector<Figure> list = List();
        std::vector<Card> cards = Cards(list);
        CHECK(cards.size() == 11);
        auto found = [&](const std::string& query, const std::string& game = "ssa") {
            std::set<std::string> names;
            for (const Card& card : cards)
                if (SearchMatches(query, card, game))
                    names.insert(CardName(card));
            return names;
        };
        auto only = [&](const std::string& query, const std::string& name, const std::string& game = "ssa") {
            std::set<std::string> names = found(query, game);
            if (names != std::set<std::string>{ name })
            {
                fprintf(stderr, "search '%s' found %zu cards (first: %s)\n", query.c_str(), names.size(),
                    names.empty() ? "-" : names.begin()->c_str());
                return false;
            }
            return true;
        };
        auto has = [&](const std::string& query, const std::string& name, const std::string& game = "ssa") {
            return found(query, game).count(name) != 0;
        };

        // Empty and spaces: everything.
        CHECK(found("").size() == 11 && found("   ").size() == 11);
        // Names, any case, any part of a word, label and file name.
        CHECK(only("gill", "Gill Grunt") && only("GRUNT", "Gill Grunt") && only("ill gr", "Gill Grunt"));
        CHECK(only("etincelle", "Étincelle") && only("ÉTINCELLE", "Étincelle") && only("Etincelle", "Étincelle"));
        CHECK(only("rocher", "Mon Rocher") && has("doom", "Mon Rocher") &&
              has("stone", "Mon Rocher")); // the swapper: found by its toy's name too
        CHECK(has("bottom", "Mon Rocher") && has("top", "Mon Rocher"));
        // Elements, English and French; accents and case.
        CHECK(has("fire", "Gill Grunt") == false && has("water", "Gill Grunt") && has("eau", "Gill Grunt") &&
              has("EAU", "Gill Grunt"));
        CHECK(has("magic", "Étincelle") && has("magie", "Étincelle") && has("magique", "Étincelle") == false);
        CHECK(has("terre", "Flashwing") && has("earth", "Flashwing") && has("earth", "Mon Rocher") &&
              has("terre", "Mon Rocher"));
        CHECK(!has("undead", "Scratch") && has("air", "LightCore Jet-Vac") && has("air", "Scratch") &&
              has("tech", "Legendary Trigger Happy"));
        CHECK(has("technologie", "Legendary Trigger Happy"));
        CHECK(only("tech", "Legendary Trigger Happy") && found("feu").empty() && found("fire").empty());
        // Games.
        CHECK(has("giants", "Flashwing") && has("geants", "Flashwing") && has("giants", "LightCore Jet-Vac") &&
              !has("giants", "Gill Grunt"));
        CHECK(has("ssa", "Gill Grunt") && has("spyro", "Gill Grunt") && !has("ssa", "Flashwing"));
        CHECK(has("swap force", "Mon Rocher") && has("swapforce", "Scratch") && has("swap", "Scratch") &&
              !has("swap force", "Gill Grunt"));
        CHECK(has("trap team", "Gusto") && !has("trap team", "Scratch"));
        // Toy types and kinds, both languages.
        CHECK(only("adventure", "Dragon's Peak") && only("aventure", "Dragon's Peak") && only("pack", "Dragon's Peak"));
        CHECK(has("item", "Anvil Rain") && has("objet", "Anvil Rain") && has("magic item", "Anvil Rain") &&
              has("objet magique", "Anvil Rain"));
        CHECK(!has("magic item", "Étincelle") && !has("item", "Gill Grunt"));
        CHECK(has("mini", "Terrabite") && has("sidekick", "Terrabite") && has("acolyte", "Terrabite"));
        CHECK(has("trap master", "Gusto") && has("trapmaster", "Gusto"));
        CHECK(has("swapper", "Mon Rocher") && has("swappers", "Mon Rocher") && !has("swapper", "Scratch") &&
              !has("swapper", "Gill Grunt"));
        CHECK(has("legendary", "Legendary Trigger Happy") && has("légendaire", "Legendary Trigger Happy") &&
              has("legendaire", "Legendary Trigger Happy"));
        CHECK(has("lightcore", "LightCore Jet-Vac") && has("light core", "LightCore Jet-Vac") &&
              !has("lightcore", "Flashwing") && !has("lightcore", "Gill Grunt"));
        CHECK(has("giant", "LightCore Jet-Vac") == true); // the game and the toy type share the word
        // Several words: all of them.
        CHECK(found("giants earth") == std::set<std::string>({ "Flashwing", "Terrabite" }) &&
              found("terre geants") == found("giants earth") && only("water ssa", "Gill Grunt"));
        CHECK(found("fire giants").empty() && found("giants zzzz").empty());
        CHECK(only("legendary tech", "Legendary Trigger Happy") && only("tech legendary", "Legendary Trigger Happy"));
        // Levels.
        CHECK(only("level 10", "Gill Grunt") && only("niveau 10", "Gill Grunt") && only("lvl 10", "Gill Grunt") &&
              only("lv10", "Gill Grunt"));
        CHECK(only("Level10", "Gill Grunt") && only("NIVEAU10", "Gill Grunt") && only("water level 10", "Gill Grunt") &&
              found("fire level 10").empty());
        CHECK(found("level 9").empty() && found("level 11").empty() && found("niveau 10 giants").empty());
        CHECK(found("level").size() == 9 &&
              found("niveau").size() == 9); // the characters, minis and swappers: no items or adventure packs
        CHECK(found("level 1").empty() && found("level 1 water").empty()); // a figure never played has no level to find
        stats.experience = 33000;
        CHECK(WriteStats(flash,
            stats)); // 15 cap in Giants: the card shows "Level 10 (15)" in Spyro's Adventure only for a higher one
        Figure flashFigure;
        CHECK(Read(flash, flashFigure));
        stats = flashFigure.stats;
        figures::SetTotalExperience(stats, 96500); // Giants' top level, 15
        CHECK(WriteStats(flash, stats));
        list = List();
        cards = Cards(list);
        CHECK(has("level 15", "Flashwing", "giants") && has("level 15", "Flashwing", "ssa")); // the figure's own level
        CHECK(has("level 10", "Flashwing", "ssa") &&
              !has("level 10", "Flashwing", "giants")); // what Spyro's Adventure shows: "Level 10 (15)"
        // Typos: one in a word of 5 letters or more, not in shorter ones.
        CHECK(has("giatns", "Flashwing") && has("gaints", "Flashwing") && has("gientz", "Flashwing") == false);
        CHECK(has("lightcroe", "LightCore Jet-Vac") && has("lihtcore", "LightCore Jet-Vac") &&
              has("legendray", "Legendary Trigger Happy") && has("legandary", "Legendary Trigger Happy"));
        CHECK(has("flashwng", "Flashwing") && has("flashwinng", "Flashwing") && has("flashwig", "Flashwing") &&
              has("flaxhwing", "Flashwing"));
        CHECK(has("sceratch", "Scratch") && has("scrach", "Scratch"));
        CHECK(has("etincele", "Étincelle") && has("etincelles", "Étincelle") && has("etincelel", "Étincelle"));
        CHECK(has("advnture", "Dragon's Peak") && has("aventrue", "Dragon's Peak"));
        CHECK(!has("scrtch", "Gill Grunt") && !has("flashxxng", "Flashwing"));
        CHECK(!has("gil", "Scratch") && !has("tehc", "Legendary Trigger Happy")); // four letters: no typo allowed
        // The old behaviour stays: a substring of the name.
        CHECK(has("rigger", "Legendary Trigger Happy") && has("hwing", "Flashwing"));
        // Folding and the sorts' words.
        CHECK(Fold("Électricité Ça Œuf À") == "electricite ca oeuf a" && Fold("MAJ") == "maj" &&
              Fold("d\xE2\x80\x99"
                   "Eon") == "d'eon");
        CHECK(std::string(SortId(Sort::Name)) == "name" && ParseSort("level") == Sort::Level &&
              ParseSort("nonsense") == Sort::Recent);
        CHECK(FigureCount(0) == "0 figures" && FigureCount(1) == "1 figure" && FigureCount(12) == "12 figures");
        CHECK(FigureCount(0, true) == "0 figurine" && FigureCount(1, true) == "1 figurine" &&
              FigureCount(12, true) == "12 figurines");
        CHECK(SortLabel(Sort::Recent) == "Recent" && SortLabel(Sort::Recent, true) == "Récents" &&
              SortLabel(Sort::Element, true) == "Élément");

        // Sort: Recent is the shelf of old; the others order the shelf section.
        auto order = [&](Sort sort) {
            std::vector<std::string> names;
            for (const Section& section : Shelf(list, sort, ""))
                if (section.kind == Section::Kind::Shelf)
                    for (size_t i : section.items)
                        names.push_back(CardName(CardOf(list, i)));
            return names;
        };
        std::vector<Section> plain = Group(list, GroupBy::Smart, OrderBy::Recent, "");
        CHECK(Shelf(list, Sort::Recent, "").size() == plain.size() &&
              Shelf(list, Sort::Recent, "")[0].items == plain[0].items);
        auto pos = [](const std::vector<std::string>& names, const std::string& name) {
            return std::find(names.begin(), names.end(), name) - names.begin();
        };
        std::vector<std::string> byName = order(Sort::Name);
        CHECK(!byName.empty() && std::is_sorted(byName.begin(), byName.end(),
                                     [](const std::string& a, const std::string& b) { return Fold(a) < Fold(b); }));
        CHECK(byName.front() == "Anvil Rain" && byName.back() == "Terrabite" && pos(byName, "Étincelle") == 2);
        std::vector<std::string> byLevel = order(Sort::Level);
        CHECK(byLevel.size() == byName.size() && byLevel.front() == "Flashwing" && byLevel[1] == "Gill Grunt" &&
              byLevel.back() != "Gill Grunt"); // Giants level 15 first, items and adventure packs last
        std::vector<std::string> byElement = order(Sort::Element);
        CHECK(byElement.size() == byName.size() &&
              byElement.front() == "Étincelle"); // Magic comes first in the menus' order
        std::vector<std::string> byGame = order(Sort::Game);
        CHECK(byGame.size() == byName.size());
        CHECK(pos(byGame, "Gill Grunt") < pos(byGame, "Flashwing") &&
              pos(byGame, "Flashwing") < pos(byGame, "Scratch") && pos(byGame, "Scratch") < pos(byGame, "Gusto"));
        // A swapper stays one card in every order.
        for (Sort sort : { Sort::Recent, Sort::Name, Sort::Level, Sort::Element, Sort::Game })
        {
            std::vector<std::string> names = order(sort);
            CHECK(std::count(names.begin(), names.end(), "Mon Rocher") == 1 && names.size() == cards.size());
        }
        (void)spyro;
        (void)jet;
        (void)trigger;
        (void)peak;
        (void)anvil;
        (void)terra;
        (void)scratch;
        (void)trapMaster;
    }

    void TestBadIndex()
    {
        std::string index = PathOf("collection.json");
        SetFavourite(PathOf("none.sky"), true); // creates collection.json
        std::string doomed = Create(14);
        auto there = [](const std::string& path) {
            SDL_PathInfo info;
            return SDL_GetPathInfo(path.c_str(), &info);
        };
        size_t inTrash = Trash().size();
        CHECK(!doomed.empty());
        std::string garbage = "{\"format\": 1, \"figures\": {\"a.sky\": {\"label\": \"keep me\"";
        CHECK(SDL_SaveFile(index.c_str(), garbage.data(), garbage.size()));
        CHECK(!SetFavourite(PathOf("a.sky"), true));
        CHECK(!SetLabel(PathOf("a.sky"), "new"));
        std::string now;
        size_t size = 0;
        void* data = SDL_LoadFile(index.c_str(), &size);
        now.assign(data != nullptr ? static_cast<char*>(data) : "", size);
        SDL_free(data);
        CHECK(now == garbage);
        int copies = 0, count = 0;
        if (char** found = SDL_GlobDirectory(Folder().c_str(), "collection.json.bad-*", 0, &count))
        {
            copies = count;
            SDL_free(found);
        }
        CHECK(copies == 1);

        // Delete is refused with a reason, and nothing moves: the dump stays, the trash gets no unrecorded file.
        std::string why;
        CHECK(!Delete(doomed, "deleted", &why) && !why.empty() && there(doomed) && Trash().size() == inTrash);

        // Repaired: writes resume.
        std::string fixed = "{\"format\": 1, \"figures\": {\"a.sky\": {\"label\": \"keep me\"}}}";
        CHECK(SDL_SaveFile(index.c_str(), fixed.data(), fixed.size()));
        CHECK(SetFavourite(PathOf("a.sky"), true));
        CHECK(Delete(doomed) && !there(doomed) && Trash().size() == inTrash + 1);
    }
}

int main(int argc, char** argv)
{
    const char* sheetFolder = argc > 2 && strcmp(argv[1], "--icons") == 0 ? argv[2] : nullptr;

    // A scratch collection, set before the library first reads its folder: in SKYLANDERS_DATA (the harness sets a
    // scratch one), else the temporary folder. Never under the player's application data.
    const char* data = getenv("SKYLANDERS_DATA");
    g_scratch = (data != nullptr && *data ? std::string(data) : std::filesystem::temp_directory_path().string()) +
                "/SSACollectionTest";
    RemoveTree(g_scratch);
    SDL_CreateDirectory(g_scratch.c_str());
    SetVariable("SKY_COLLECTION", g_scratch + "/Collection");
    SetVariable("SKY_PORTRAITS", g_scratch + "/portraits");
#ifdef _WIN32
    _putenv_s("SKY_MIGRATE_FROM", "");
    _putenv_s("SKY_FIGURES", "");
#else
    unsetenv("SKY_MIGRATE_FROM");
    unsetenv("SKY_FIGURES");
#endif

    TestJson();
    TestSsaCatalog();
    TestCatalogue();
    TestCollection();
    TestMigration();
    TestIcons(sheetFolder);
    TestManagement();
    TestSwappers();
    TestSwapperPicture();
    TestPictureRule();
    TestSearch();
    TestBadIndex();
    TestPortalOverview();
    TestGiants();
    TestTrapTeam();
    TestLaterGames();
    TestUpgrades();
    TestWheel();

    if (skyenv::Get("SKY_KEEP_COLLECTION") == nullptr)
        RemoveTree(g_scratch);
    else
        printf("kept in %s\n", g_scratch.c_str());
    printf("collection test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
