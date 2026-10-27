#pragma once

// The shelf's smart search and sort, shared by the Toy Box and Skylauncher (docs/runtime/host/collection.md "Search and
// sort"): one function says whether a card matches what the player typed, and Shelf cuts a list into the shelf in the
// player's sort order.
#include "collection.h"
#include <string>
#include <vector>

namespace collection
{
    // Lower case with the accents taken off (UTF-8 in, ASCII for Latin letters out): "Électricité" gives "electricite".
    std::string Fold(const std::string& utf8);

    // Whether `card` (both halves of a swapper) matches `query`. The query is words, all of which must match (a word
    // that does not match anything makes the card drop out). A word matches, in English or in French, whichever
    // language the app shows:
    //   - the start of a name word: the figure's label, nickname, name, version, edition or file name ("gill", "spy");
    //   - an element ("fire", "feu", "lumiere"), a game ("giants", "swap", "ssa", "spyro"), a toy type ("item", "objet",
    //     "adventure", "trap", "mini", "swapper"), a version ("legendary", "lightcore", "dark", "gold", "cristal");
    //   - "level 10", "niveau 10", "lvl 10", "lv10": a character at that level (the level the card shows in `game`, or its
    //     own level); "level" alone: any toy that has a level.
    // Case and accents do not matter; a word of 5 letters or more may have one typo ("giatns", "lightcroe", "dragn").
    // An empty query matches everything. `game` ("ssa", "swapforce"...) fixes the level cap the card shows.
    bool SearchMatches(const std::string& query, const Card& card, const std::string& game);

    // How the shelf is ordered. Recent is the shelf of old: favourites first, then what the game took last, then by
    // element; the others ignore favourites.
    enum class Sort { Recent, Name, Level, Element, Game, Count };
    const char* SortId(Sort sort);          // "recent", "name", "level", "element", "game"
    Sort ParseSort(const std::string& id);  // Recent when unknown
    // The sort's name in the apps' menus and tags ("Recent" / "Récents"), the same words in both apps.
    std::string SortLabel(Sort sort, bool french = false);

    // "12 figures" and "1 figure"; "12 figurines" and, as in French, "0 figurine" and "1 figurine": the count of the shelf.
    std::string FigureCount(int count, bool french = false);

    // Group(list, Smart, ...) with the shelf section ordered by `sort`: Name (A to Z), Level (highest first, toys with no
    // level last), Element (the menus' order, then toy type), Game (the catalogue's order); ties by name. Unknown and
    // "other games" toys keep their sections.
    std::vector<Section> Shelf(const std::vector<Figure>& list, Sort sort, const std::string& game);
}
