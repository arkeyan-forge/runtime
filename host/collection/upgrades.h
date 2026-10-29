#pragma once

// What each bit of a figure's upgrade field means, with the names, descriptions and prices the game shows: read from
// the player's disc at install (art::Extract, art/game_text.cpp) into <data>/<Game>/upgrades.json (game text: never
// shipped), with a plain fallback ("Upgrade 1"...) when nothing was extracted. For the Toy Box and Skylauncher.
// docs/runtime/host/figures.md ("Upgrades"), docs/runtime/host/collection.md ("Upgrades").
#include "catalogue.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace collection
{
    // The upgrade field (figures::FigureStats::upgrades, block 8 +0x10) [V] in Spyro's Adventure and Giants.
    constexpr int UpgradePathChosenBit = 0; // a path is chosen
    constexpr int UpgradePathSecondBit = 1; // ...and it is the second one (clear: the first)
    constexpr int UpgradeSoulGemBit = 9;
    // Giants' Wow Pow (Series 2 figures) is not in this field: the game keeps it in the extra area (FigureStats::wowPow).
    // The editors' working bits (EditorBits) carry it at bit 10, so that one 16-bit value drives the Edit sheets.
    constexpr int UpgradeWowPowBit = 10;

    enum class UpgradePath
    {
        Basic = 0,  // bits 2-5
        First = 1,  // bits 6-8 when the first path is chosen
        Second = 2, // bits 6-8 when the second path is chosen
        SoulGem = 3,
        WowPow = 4,
    };

    struct Upgrade
    {
        int bit = 0;
        UpgradePath path = UpgradePath::Basic;
        int price = 0;           // gold
        std::string name; // as the game writes it; its button marks are kept ("{" in Giants, "&ICON(...)" in SSA)
        std::string description;
    };

    struct UpgradeTable
    {
        std::vector<Upgrade> upgrades; // the game's order: basic, first path, second path, Soul Gem, Wow Pow
        std::string pathNames[2], pathDescriptions[2];
        bool fromDisc = false; // false: the plain fallback
    };

    // The upgrades of `toy` in `game` ("ssa", "giants"), in `language` ("en", "fr"; English when the text is missing).
    // A toy type the game defines as another's alternate (Legendary Bash, Dark Spyro) gets its base toy's list.
    UpgradeTable Upgrades(const Toy& toy, const std::string& game, const std::string& language = "en");

    // The Edit sheets' rules (Toy Box and Skylauncher), on the 16-bit field. A path upgrade (bits 6-8, shared by both paths)
    // counts as bought only on the path that is chosen; the other path's are shown greyed.
    bool UpgradeBought(uint16_t bits, const Upgrade& upgrade);
    bool UpgradeGreyed(uint16_t bits, const Upgrade& upgrade);
    // Buys or gives back one upgrade. On a path that is not chosen it first switches to it (as the game does: the bought
    // path upgrades move to the new path), then toggles.
    uint16_t ToggleUpgrade(uint16_t bits, const Upgrade& upgrade);
    // Everything of the table bought (the chosen path, else the first); nothing (no path either).
    uint16_t AllUpgrades(uint16_t bits, const UpgradeTable& table);
    uint16_t NoUpgrades(uint16_t bits);
    int BoughtCount(uint16_t bits, const UpgradeTable& table);
    // The Edit sheets' working bits from a figure: its upgrade field (FigureStats::upgrades) with bit 10 = its Wow Pow
    // (FigureStats::wowPow); and back. Bits 10-11 of the field itself are another Giants field (cleared by the game on a
    // figure's first use) and are kept as they were.
    uint16_t EditorBits(uint16_t field, uint8_t wowPow);
    void StoreEditorBits(uint16_t bits, uint16_t& field, uint8_t& wowPow);
    // A name or description for display: the game's button marks become "the button" / "le bouton", line breaks and
    // doubled spaces go; the description is cut to its first sentence.
    std::string UpgradeLabel(const std::string& text, const std::string& language);
    std::string UpgradeName(
        const std::string& name, const std::string& language); // UpgradeLabel without a final full stop
    std::string UpgradeLine(const std::string& description, const std::string& language);

    // <data>/<Game>/upgrades.json (SKYLANDERS_DATA moves the data root for tests).
    std::filesystem::path UpgradeFile(std::string_view game);
}
