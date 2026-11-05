#include "catalogue.h"
#include "collection.h"
#include "json.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

// resources/collection/catalogue.json, built in by cmake/embed_file.cmake.
extern const unsigned char g_embeddedCatalogue[];
extern const size_t g_embeddedCatalogueSize;

namespace collection
{
    namespace
    {
        const char* const ElementIds[] = { "air", "earth", "fire", "water", "magic", "tech", "life", "undead", "light",
            "dark", "none" };
        const char* const CategoryIds[] = { "core", "giant", "swap_top", "swap_bottom", "trap_master", "supercharger",
            "sensei", "mini", "trap", "villain", "vehicle", "creation_crystal", "magic_item", "adventure", "trophy" };
        const char* const CategoryNames[] = { "Skylanders", "Giants", "Swap Force tops", "Swap Force bottoms",
            "Trap Masters", "SuperChargers", "Senseis", "Minis and sidekicks", "Traps", "Villains", "Vehicles",
            "Creation Crystals", "Magic Items", "Adventure Packs", "Trophies" };
        const char* const KindIds[] = { "base", "legendary", "dark", "crystal", "glow", "metallic", "gold", "silver",
            "chrome", "flocked", "series", "lightcore", "eons_elite", "alt_deco", "event", "shape", "other" };
        const char* const KindNames[] = { "Standard", "Legendary", "Dark", "Crystal Clear", "Glow in the Dark",
            "Metallic", "Gold", "Silver", "Chrome", "Flocked", "Series", "LightCore", "Eon's Elite", "Alternate deco",
            "Event and exclusive", "Trap shape", "Other" };
        static_assert(
            std::size(ElementIds) == size_t(Element::Count) && std::size(CategoryIds) == size_t(Category::Count));
        static_assert(
            std::size(CategoryNames) == size_t(Category::Count) && std::size(KindIds) == size_t(VariantKind::Count));
        static_assert(std::size(KindNames) == size_t(VariantKind::Count));

        template <size_t N>
        int Index(const char* const (&ids)[N], const std::string& id)
        {
            for (size_t i = 0; i < N; i++)
            {
                if (id == ids[i])
                    return int(i);
            }
            return int(N);
        }

        bool ReadToy(const Json& json, Toy& toy, std::string& error)
        {
            int64_t type = json["type"].AsInt(-1);
            toy.name = json["name"].AsString();
            if (type < 0 || type > 0xFFFF || toy.name.empty())
            {
                error = "a toy needs a type (0-65535) and a name";
                return false;
            }
            toy.type = uint16_t(type);
            toy.element = json.Has("element") ? ParseElement(json["element"].AsString()) : Element::None;
            toy.category = ParseCategory(json["category"].AsString());
            toy.edition = json.Has("edition") ? ParseVariantKind(json["edition"].AsString()) : VariantKind::Base;
            toy.game = json["game"].AsString();
            if (toy.element == Element::Count || toy.category == Category::Count || toy.edition == VariantKind::Count)
            {
                error = toy.name + ": unknown element, category or edition";
                return false;
            }
            for (const Json& item : json["variants"].Items())
            {
                Variant variant;
                variant.sameTag = item["same_tag"].AsBool();
                variant.id = uint16_t(item["id"].AsInt(0));
                variant.name = item["name"].AsString();
                variant.kind = ParseVariantKind(item["kind"].AsString());
                if (variant.name.empty() || variant.kind == VariantKind::Count || (!variant.sameTag && variant.id == 0))
                {
                    error = toy.name + ": a variant needs a name, a known kind and an id (or \"same_tag\": true)";
                    return false;
                }
                toy.variants.push_back(std::move(variant));
            }
            return true;
        }

        void ReadMap(const Json& json, std::map<uint16_t, std::string>& out)
        {
            for (const auto& [key, value] : json.Members())
                out[uint16_t(strtoul(key.c_str(), nullptr, 0))] = value.AsString();
        }
    }

    const Catalogue& Catalogue::Get()
    {
        static const Catalogue catalogue = [] {
            Catalogue loaded;
            std::string error;
            if (!loaded.Load(
                    std::string(reinterpret_cast<const char*>(g_embeddedCatalogue), g_embeddedCatalogueSize), error))
                fprintf(stderr, "[collection] built-in catalogue: %s\n", error.c_str());
            std::string path = Folder() + "/catalogue.json";
            size_t size = 0;
            if (char* text = static_cast<char*>(SDL_LoadFile(path.c_str(), &size)))
            {
                if (loaded.Load(std::string(text, size), error))
                    fprintf(stderr, "[collection] catalogue additions from %s\n", path.c_str());
                else
                    fprintf(stderr, "[collection] %s ignored: %s\n", path.c_str(), error.c_str());
                SDL_free(text);
            }
            return loaded;
        }();
        return catalogue;
    }

    bool Catalogue::Load(const std::string& text, std::string& error)
    {
        Json root;
        if (!Json::Parse(text, root, error))
            return false;
        if (!root.IsObject() || root["format"].AsInt() != 1)
        {
            error = "not a catalogue (\"format\": 1 expected)";
            return false;
        }

        std::vector<Toy> toys;
        for (const Json& item : root["toys"].Items())
        {
            Toy toy;
            if (!ReadToy(item, toy, error))
                return false;
            toys.push_back(std::move(toy));
        }
        std::vector<Game> games;
        for (const Json& item : root["games"].Items())
        {
            Game game;
            game.id = item["id"].AsString();
            game.name = item["name"].AsString();
            game.year = int(item["year"].AsInt());
            game.runtime = item["runtime"].AsString();
            if (game.id.empty())
            {
                error = "a game needs an id";
                return false;
            }
            for (const Json& type : item["accepts"].Items())
                game.accepts.push_back(uint16_t(type.AsInt()));
            for (const Json& id : item["accepts_games"].Items())
                game.acceptsGames.push_back(id.AsString());
            for (const Json& entry : item["seats"].Items())
            {
                Seat seat;
                seat.id = entry["id"].AsString();
                seat.player = int(entry["player"].AsInt());
                for (const Json& id : entry["accepts"].Items())
                {
                    Category category = ParseCategory(id.AsString());
                    if (category == Category::Count)
                    {
                        error = "unknown category \"" + id.AsString() + "\" in a seat of " + game.id;
                        return false;
                    }
                    seat.accepts.push_back(category);
                }
                game.seats.push_back(std::move(seat));
            }
            for (const Json& entry : item["villains"].Items())
            {
                Villain villain;
                villain.id = uint16_t(entry["id"].AsInt());
                villain.name = entry["name"].AsString();
                villain.model = entry["model"].AsString();
                villain.element = ParseElement(entry["element"].AsString());
                villain.vault = int(entry["vault"].AsInt());
                if (villain.id == 0 || villain.name.empty() || villain.element == Element::Count)
                {
                    error = "a villain needs an id, a name and an element in " + game.id;
                    return false;
                }
                game.villains.push_back(std::move(villain));
            }
            ReadMap(item["names"], game.names);
            ReadMap(item["images"], game.images);
            ReadMap(item["hats"], game.hats);
            for (const auto& [key, value] : item["kind_pictures"].Members())
            {
                Category category = ParseCategory(key);
                if (category == Category::Count)
                {
                    error = "unknown category \"" + key + "\" in the kind pictures of " + game.id;
                    return false;
                }
                game.kindPictures[category] = value.AsString();
            }
            for (auto [field, pictures] : { std::pair{ "portraits", &game.portraits },
                     std::pair{ "artworks", &game.artworks }, std::pair{ "halves", &game.halves } })
                for (const auto& [key, value] : item[field].Members())
                {
                    // "<type>" or "<type>:<variant id>"
                    char* end = nullptr;
                    uint32_t type = uint32_t(strtoul(key.c_str(), &end, 0)), variant = 0;
                    if (end != nullptr && *end == ':')
                        variant = uint32_t(strtoul(end + 1, nullptr, 0));
                    (*pictures)[(type & 0xFFFF) << 16 | (variant & 0xFFFF)] = value.AsString();
                }
            games.push_back(std::move(game));
        }

        for (Toy& toy : toys)
        {
            auto same =
                std::find_if(m_toys.begin(), m_toys.end(), [&](const Toy& other) { return other.type == toy.type; });
            if (same != m_toys.end())
                *same = std::move(toy);
            else
                m_toys.push_back(std::move(toy));
        }
        for (Game& game : games)
        {
            auto same =
                std::find_if(m_games.begin(), m_games.end(), [&](const Game& other) { return other.id == game.id; });
            if (same != m_games.end())
                *same = std::move(game);
            else
                m_games.push_back(std::move(game));
        }
        return true;
    }

    const Toy* Catalogue::Find(uint16_t type) const
    {
        for (const Toy& toy : m_toys)
        {
            if (toy.type == type)
                return &toy;
        }
        return nullptr;
    }

    const Game* Catalogue::FindGame(const std::string& id) const
    {
        for (const Game& game : m_games)
        {
            if (game.id == id)
                return &game;
        }
        return nullptr;
    }

    bool Catalogue::Accepts(const std::string& id, uint16_t type) const
    {
        const Game* game = FindGame(id);
        if (game == nullptr)
            return false;
        if (std::find(game->accepts.begin(), game->accepts.end(), type) != game->accepts.end())
            return true;
        const Toy* toy = Find(type);
        return toy != nullptr &&
               std::find(game->acceptsGames.begin(), game->acceptsGames.end(), toy->game) != game->acceptsGames.end();
    }

    std::vector<const Toy*> Catalogue::Accepted(const std::string& id) const
    {
        std::vector<const Toy*> toys;
        const Game* game = FindGame(id);
        if (game == nullptr)
            return toys;
        for (uint16_t type : game->accepts)
        {
            if (const Toy* toy = Find(type))
                toys.push_back(toy);
        }
        for (const Toy& toy : m_toys)
        {
            if (std::find(toys.begin(), toys.end(), &toy) == toys.end() && Accepts(id, toy.type))
                toys.push_back(&toy);
        }
        return toys;
    }

    uint16_t StandardVariant(const Toy& toy)
    {
        for (const Variant& variant : toy.variants)
            if (variant.kind == VariantKind::Base && !variant.sameTag)
                return variant.id;
        return 0;
    }

    size_t VersionCount(const Toy& toy)
    {
        return size_t(std::count_if(
            toy.variants.begin(), toy.variants.end(), [](const Variant& v) { return v.kind != VariantKind::Base; }));
    }

    Catalogue::Reading Catalogue::Read(const std::string& id, const Toy* toy, uint16_t variant) const
    {
        Reading reading;
        const Game* game = FindGame(id);
        reading.works = toy != nullptr && game != nullptr && Accepts(id, toy->type);
        if (!reading.works)
            return reading;
        reading.kind = toy->edition;
        const Variant* entry = nullptr;
        for (const Variant& v : toy->variants)
        {
            if (!v.sameTag && v.id == variant)
                entry = &v;
        }
        if (variant == 0 || (entry != nullptr && entry->kind == VariantKind::Base))
            return reading;
        // A game tells a variant apart when the toy has a definition for it: Giants' and Swap Force's tables carry a figure's
        // id as bit flags (11: repose, 10: alternate deco or Legendary, 9: LightCore) and a deco in the low byte, and only a
        // listed one of the game's own years or before counts (the id's high nibble is the release year, 1: 2012, Giants).
        // Spyro's Adventure (2011) reads the toy type only [V]. Chase and event figures carry none of the flags: a game plays
        // them as the plain toy, or as Series 2 when they carry the repose bit and the toy has one (figures.md) [V Giants];
        // Swap Force the same [?]; Trap Team and later games are taken to tell every variant of the older years apart [?].
        const int newest = game->year - 2011;
        auto known = [&](const Variant& v) {
            if (v.sameTag || v.kind == VariantKind::Base || toy->category == Category::Trap || int(v.id >> 12) > newest)
                return false;
            if (newest >= 3)
                return true;
            if (v.kind == VariantKind::Series)
                return (v.id & 0xFF) == 0x01 || (v.id & 0xFF) == 0x05;
            return (v.id & 0x0600) != 0;
        };
        reading.exact = false;
        if (entry != nullptr && known(*entry))
        {
            reading.seen = entry;
            reading.kind = entry->kind;
            reading.exact = true;
            return reading;
        }
        if (toy->category == Category::Trap)
        {
            // A trap's variant is its shape: every game that reads the toy reads its shapes.
            if (entry != nullptr && newest >= 3)
            {
                reading.seen = entry;
                reading.kind = entry->kind;
                reading.exact = true;
            }
            return reading;
        }
        const Variant* same = nullptr;
        int fits = 0;
        for (const Variant& v : toy->variants)
        {
            if (known(v) && (v.id & 0x0FFF) == (variant & 0x0FFF))
                same = &v, fits++;
        }
        if (fits == 0 && newest >= 1 && (variant & 0x0800) != 0)
        {
            // A repose (bit 11) of a toy that has one (Series 2): Giants shows a later pose as Series 2 [V].
            for (const Variant& v : toy->variants)
            {
                if (known(v) && v.kind == VariantKind::Series && (v.id & 0x0FFF) == 0x0801)
                    same = &v, fits = 1;
            }
        }
        if (fits == 1)
        {
            reading.seen = same;
            reading.kind = same->kind;
        }
        return reading;
    }

    std::vector<const Variant*> Catalogue::VersionsFor(const std::string& game, const Toy& toy) const
    {
        std::vector<const Variant*> versions;
        for (const Variant& v : toy.variants)
        {
            if (v.kind == VariantKind::Base)
                continue;
            if (v.sameTag || Read(game, &toy, v.id).exact)
                versions.push_back(&v);
        }
        return versions;
    }

    VariantInfo Catalogue::Describe(const Toy* toy, uint16_t variant) const
    {
        std::string base = toy != nullptr ? toy->name : "Unknown toy";
        if (variant == 0)
            return { base, toy != nullptr ? toy->edition : VariantKind::Base, toy != nullptr };
        if (toy != nullptr)
        {
            for (const Variant& known : toy->variants)
            {
                if (!known.sameTag && known.id == variant)
                    return { known.name, known.kind, true };
            }
            // The same decoration with another year in the high nibble: Giants reads only the low 12 bits (the deco id
            // and the repose, alternate-deco and LightCore flags, docs/runtime/host/figures.md), so it is that variant
            // for the game. Only when one listed variant fits, and not for trap shapes (their ids are not decos).
            const Variant* same = nullptr;
            int fits = 0;
            for (const Variant& known : toy->variants)
                if (!known.sameTag && toy->category != Category::Trap && (known.id & 0x0FFF) == (variant & 0x0FFF))
                    same = &known, fits++;
            if (fits == 1)
                return { same->name, same->kind, false };
        }

        // Not listed: the id's bits as public descriptions give them [?]: the high nibble is the year of the
        // release (1 = 2012 ... 5 = 2016), the low byte the decoration (1 or 5 a new pose, 2 another paint job,
        // 3 Legendary, 6 LightCore, 0x10 Eon's Elite).
        char text[64];
        int year = variant >> 12, deco = variant & 0xFF;
        VariantInfo info{ {}, VariantKind::Other, false };
        if (toy != nullptr && toy->category == Category::Trap)
        {
            snprintf(text, sizeof(text), " (shape %04X)", variant);
            info = { base + text, VariantKind::Shape, false };
        }
        else if (deco == 0x10)
            info = { "Eon's Elite " + base, VariantKind::EonsElite, false };
        else if (deco == 0x06)
            info = { "LightCore " + base, VariantKind::LightCore, false };
        else if (deco == 0x03)
            info = { "Legendary " + base, VariantKind::Legendary, false };
        else if ((deco == 0x01 || deco == 0x05) && year >= 1 && year <= 3)
        {
            snprintf(text, sizeof(text), "Series %d ", year + 1);
            info = { text + base, VariantKind::Series, false };
        }
        else
        {
            snprintf(text, sizeof(text), " (variant %04X)", variant);
            info = { base + text, deco == 0x02 ? VariantKind::AltDeco : VariantKind::Other, false };
        }
        return info;
    }

    const char* ElementId(Element element) { return element < Element::Count ? ElementIds[size_t(element)] : ""; }
    const char* CategoryId(Category category)
    {
        return category < Category::Count ? CategoryIds[size_t(category)] : "";
    }
    const char* VariantKindId(VariantKind kind) { return kind < VariantKind::Count ? KindIds[size_t(kind)] : ""; }
    Element ParseElement(const std::string& id) { return Element(Index(ElementIds, id)); }
    Category ParseCategory(const std::string& id) { return Category(Index(CategoryIds, id)); }
    VariantKind ParseVariantKind(const std::string& id) { return VariantKind(Index(KindIds, id)); }
    const char* CategoryName(Category category)
    {
        return category < Category::Count ? CategoryNames[size_t(category)] : "";
    }
    const char* VariantKindName(VariantKind kind) { return kind < VariantKind::Count ? KindNames[size_t(kind)] : ""; }

    uint32_t ElementRgb(Element element)
    {
        // The Toy Box's element colours; light and dark added.
        static const uint32_t Colors[] = { 0x3E9FD8, 0x9A6233, 0xE0531F, 0x1F74D1, 0x8E44C9, 0xD08A00, 0x3E9B3F,
            0x5E5A86, 0xE8D27A, 0x3A2E58, 0x6E7180 };
        static_assert(std::size(Colors) == size_t(Element::Count));
        return element < Element::Count ? Colors[size_t(element)] : 0x6E7180;
    }

    uint32_t VariantKindRgb(VariantKind kind)
    {
        static const uint32_t Colors[] = { 0x6E7180, 0xF2B632, 0x2B1F3D, 0xBFE8F2, 0x9BF06A, 0xA9B4C8, 0xE0B020,
            0xC8CCD4, 0xD8DEE6, 0x7FB069, 0x4F7BD9, 0xFFF2A8, 0x6B3FA0, 0xE2668C, 0xF07F2B, 0x46B3A0, 0x808080 };
        static_assert(std::size(Colors) == size_t(VariantKind::Count));
        return kind < VariantKind::Count ? Colors[size_t(kind)] : 0x808080;
    }
}
