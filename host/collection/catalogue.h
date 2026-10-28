#pragma once

// Every Skylanders toy of the six games, as data (resources/collection/catalogue.json, built into the program;
// docs/runtime/host/collection.md). A catalogue.json in the collection folder replaces or adds toys by toy type, and games by id.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace collection
{
    enum class Element : uint8_t { Air, Earth, Fire, Water, Magic, Tech, Life, Undead, Light, Dark, None, Count };

    enum class Category : uint8_t
    {
        Core,            // Skylanders of every game that fit no other group
        Giant,
        SwapTop,         // Swap Force swappers: two toy types, one per half
        SwapBottom,
        TrapMaster,
        SuperCharger,
        Sensei,
        Mini,            // minis and sidekicks
        Trap,
        Villain,         // villains live in a trap's data: no toy of their own so far
        Vehicle,
        CreationCrystal,
        MagicItem,
        Adventure,       // adventure packs and location pieces
        Trophy,
        Count
    };

    // What sets a toy or a variant apart. Base: the plain figure.
    enum class VariantKind : uint8_t
    {
        Base, Legendary, Dark, Crystal, Glow, Metallic, Gold, Silver, Chrome, Flocked, Series, LightCore, EonsElite,
        AltDeco, Event, Shape, Other, Count
    };

    struct Variant
    {
        uint16_t id = 0;       // block 1, 0x1C
        bool sameTag = false;  // a physical edition the tag does not tell apart (Crystal Clear...): picked by the user
        std::string name;
        VariantKind kind = VariantKind::Other;
    };

    struct Toy
    {
        uint16_t type = 0;     // block 1, 0x10
        std::string name;
        Element element = Element::None;
        Category category = Category::Core;
        std::string game;      // id of the game it first came with
        VariantKind edition = VariantKind::Base; // the toy type itself is special (Legendary Bash, Dark Spyro)
        std::vector<Variant> variants;
    };

    // A place on the portal as the game's menus show it (docs/runtime/host/toybox_ux.md section 4): Player 1, Magic item...
    struct Seat
    {
        std::string id;                   // "p1", "p2", "item", "adventure", "trap", "vehicle1"
        int player = 0;                   // 1, 2 for a player's seats; 0 for shared ones
        std::vector<Category> accepts;    // the toy families that go there
    };

    struct Game
    {
        std::string id, name;
        int year = 0;
        std::vector<uint16_t> accepts;          // toy types the game reads, in the game's order
        std::vector<std::string> acceptsGames;  // ...or every toy that first came with these games
        std::map<uint16_t, std::string> names;  // the game's internal character names (SSA: portraits)
        std::map<uint16_t, std::string> images; // other toys' pictures among the game's UI images
        // One picture per toy family among the game's UI images (SSA's magic item, location and sidekick icons): the
        // picture of a toy of that family that no game has a picture of. JSON: "kind_pictures", category id to name.
        std::map<Category, std::string> kindPictures;
        // The game's portrait of a toy (Giants: one per Skylander, deco and item), by type << 16 | variant id (0: the
        // plain toy): the picture's file name in the game's portraits folder (art/game_art.h). JSON: "9", "9:0x1206".
        std::map<uint32_t, std::string> portraits;
        // The game's large pictures of a toy or variant (Giants: the versus screen's character art, about 1000x960),
        // keyed as `portraits`: files of the portraits folder's "artworks" subfolder (art/game_art.h). JSON: "artworks".
        std::map<uint32_t, std::string> artworks;
        // Swap Force's swap screen pictures: a swapper's top above its waist (SwapTop types) and its legs (SwapBottom
        // types), each drawn in place on one shared square frame, on transparency, to be joined (icons.h, SwapperPicture). Keyed as `portraits`; files of
        // the "artworks" subfolder. JSON: "halves".
        std::map<uint32_t, std::string> halves;
        // The hats the game has, by the number a figure stores (0: none), with their English names. JSON: "hats".
        std::map<uint16_t, std::string> hats;
        std::string runtime;                    // the runtime's executable name ("SSARecomp"); its data is in datadir::Game(id)
        std::vector<Seat> seats;                // in the order the menus show them
    };

    // How a variant id reads: its catalogue entry, or a name worked out from the id's bits (docs/runtime/host/collection.md).
    struct VariantInfo
    {
        std::string name;    // full display name ("Series 2 Gill Grunt"); the toy's name for variant 0
        VariantKind kind = VariantKind::Base;
        bool known = true;   // false: not in the catalogue, guessed from the bits
    };

    class Catalogue
    {
    public:
        // The built-in catalogue plus the collection folder's catalogue.json (if any), loaded once.
        static const Catalogue& Get();

        // Adds a JSON catalogue: toys replace those of the same type, games those of the same id. False (and the
        // catalogue unchanged) if the text is not a valid catalogue.
        bool Load(const std::string& text, std::string& error);

        const std::vector<Toy>& Toys() const { return m_toys; }
        const std::vector<Game>& Games() const { return m_games; }
        const Toy* Find(uint16_t type) const;
        const Game* FindGame(const std::string& id) const;

        bool Accepts(const std::string& game, uint16_t type) const;
        // The toys a game reads: its own order for `accepts`, then catalogue order.
        std::vector<const Toy*> Accepted(const std::string& game) const;

        VariantInfo Describe(const Toy* toy, uint16_t variant) const;

    private:
        std::vector<Toy> m_toys;
        std::vector<Game> m_games;
    };

    // A toy's "base" entry is its standard figure under the id it is sold with (Jet-Vac 0x1000, Scratch 0x2000, Legendary
    // Bash 0x0403), not a second version: a versions list never shows it ("Standard" is that row) and "Standard" creates the
    // figure with that id. StandardVariant is 0 when the toy has no base entry; VersionCount counts the other variants.
    uint16_t StandardVariant(const Toy& toy);
    size_t VersionCount(const Toy& toy);

    // Ids as written in the JSON files ("air", "magic_item", "lightcore"); Parse* return Count when unknown.
    const char* ElementId(Element element);
    const char* CategoryId(Category category);
    const char* VariantKindId(VariantKind kind);
    Element ParseElement(const std::string& id);
    Category ParseCategory(const std::string& id);
    VariantKind ParseVariantKind(const std::string& id);
    // English display names ("Magic Items", "Eon's Elite").
    const char* CategoryName(Category category);
    const char* VariantKindName(VariantKind kind);
    // Opaque RGB (0xRRGGBB) for icons and frames.
    uint32_t ElementRgb(Element element);
    uint32_t VariantKindRgb(VariantKind kind);
}
