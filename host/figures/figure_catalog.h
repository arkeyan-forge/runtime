#pragma once

// Every toy a game recognises (Spyro's Adventure's list below; Giants' is SSA's plus its own, from the catalogue).
// Spyro's Adventure: The toy types are the ones its portal code accepts (sub_82643BE8 and
// sub_82643DA0 map them to per-toy ids: 0-32, the legendaries 404/416/419/430, magic items 200-207, adventure packs
// 300-304, and four Giants-era sidekicks); the order of characters 0-32 matches the title's internal name table
// (sub_829BC850). Display names are the toys' retail names (public). The data is the shared catalogue's
// (resources/collection/catalogue.json: the "ssa" game's accepts, names and images; collection/catalogue.h).
#include <cstdint>
#include <span>
#include <string>

namespace figures
{
    enum class Element : uint8_t
    {
        Air,
        Earth,
        Fire,
        Water,
        Magic,
        Tech,
        Life,
        Undead,
        None
    };
    enum class ToyKind : uint8_t
    {
        Character,
        Legendary,
        Sidekick,
        MagicItem,
        Adventure,
        Trap,      // Trap Team: holds a captured villain, no progress of its own
        TrapMaster // Trap Team: keeps progress like a character
    };

    struct ToyInfo
    {
        uint16_t toyType;
        const char* name;
        const char*
            portrait; // the title's name for the character ("GillGrunt": UI image toy_gillgrunt_hud); null if none
        Element element;
        ToyKind kind;
        const char* image;    // another UI image of the title showing the toy ("potion"); null if none
    };

    // The toys a game reads, by its catalogue id: "ssa" (the default) is Spyro's Adventure's, "giants" adds the Giants
    // and LightCore toys, its magic items and sidekicks on top of every SSA toy.
    std::span<const ToyInfo> Catalog(const std::string& game = "ssa");
    const ToyInfo* FindToy(uint16_t toyType, const std::string& game = "ssa");

    // Characters keep progress (experience, gold, upgrades...); items and adventure packs don't.
    inline bool HasProgress(const ToyInfo& toy)
    {
        return toy.kind == ToyKind::Character || toy.kind == ToyKind::Legendary || toy.kind == ToyKind::Sidekick ||
               toy.kind == ToyKind::TrapMaster;
    }

    const char* ElementName(Element element);
    // Opaque RGBA (R in the low byte), for placeholders and frames.
    uint32_t ElementColor(Element element);

    // Name of the toy's portrait in the title's UI images ("toy_<portrait>_hud", lower case, or `image`), empty if it
    // has none. Sidekicks use their full-size counterpart's.
    std::string PortraitImageName(const ToyInfo& toy);

    // File name of the starter figure for a toy: "<toy type> <name>.sky", e.g. "014 Gill Grunt.sky".
    std::string StarterFileName(const ToyInfo& toy);
}
