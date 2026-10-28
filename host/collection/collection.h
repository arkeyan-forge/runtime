#pragma once

// The player's Skylanders collection, shared by every game and the launcher (docs/runtime/host/collection.md, docs/runtime/host/collection_api.md):
// one folder of figure dumps (the source of truth for each figure's progress), collection.json beside them for what
// the dumps do not hold (labels, favourites, editions, last use, migrations), a <dump>.lock file while a game has
// the figure on its portal, and backups/<dump>/ for saved copies.
#include "catalogue.h"
#include "../figures/figure_tag.h"
#include <cstdint>
#include <string>
#include <vector>

namespace collection
{
    // The collection folder, without a trailing slash: SKY_COLLECTION, else SKY_FIGURES (the old variable), else
    // DefaultFolder(). Created when first written to.
    std::string Folder();
    // <data root>/Collection (datadir.h): ~/Library/Application Support/Skylanders/Collection on macOS,
    // ~/.local/share/Skylanders/Collection on Linux, %APPDATA%\Skylanders\Collection on Windows.
    std::string DefaultFolder();
    std::string PathOf(const std::string& file); // Folder() + "/" + file
    bool IsDumpName(const std::string& name);    // .sky, .bin, .dmp

    // --- Locks: a figure on a game's portal is in use ------------------------------------------------------------------

    struct LockInfo
    {
        bool locked = false; // a <dump>.lock exists
        bool mine = false;   // taken by this process
        bool stale = false;  // its process is gone (or the lock is unreadable)
        std::string game;    // who holds it ("ssa", "launcher")
        int64_t pid = 0;
        std::string since;   // ISO 8601, UTC

        // Someone else is using the figure right now: do not write it.
        bool Busy() const { return locked && !mine && !stale; }
    };

    LockInfo GetLock(const std::string& path);
    // False if another live process holds it. A stale lock is taken over.
    bool Lock(const std::string& path, const std::string& game);
    // Removes the lock if this process holds it.
    void Unlock(const std::string& path);
    // Removes the lock whoever holds it (the UI's "it is not in use any more").
    void BreakLock(const std::string& path);
    // Drops the locks whose process is gone (a game that crashed or was killed). Returns how many.
    int ClearStaleLocks();

    // --- Figures ---------------------------------------------------------------------------------------------------------

    struct Figure
    {
        std::string path, file;
        uint16_t type = 0, variant = 0;
        const Toy* toy = nullptr;  // null: a toy the catalogue does not know
        VariantInfo variantInfo;   // name and kind from the variant id (the toy's own edition for variant 0)
        // collection.json
        std::string label;         // the player's name for this dump
        std::string edition;       // a same-tag edition the player picked ("Crystal Clear Wham-Shell")
        VariantKind editionKind = VariantKind::Base;
        bool favourite = false;
        std::string lastGame, lastUsed;
        std::string origin;        // "created" (Add a figure, a duplicate), "imported", "portal" (copy of a real toy); empty: older
        // A Swap Force half linked to the other half of its swapper (file name; empty: none, the half is a card of its
        // own). Set only when the link is good: both dumps exist, link each other, and are one top and one bottom.
        std::string partner;
        struct Use
        {
            std::string game, lastUsed; // ISO 8601, UTC
            int uses = 0;
        };
        std::vector<Use> uses;     // per game (Touch)
        // The dump
        figures::AreaState state = figures::AreaState::Blank;
        figures::FigureStats stats;
        int level = 1;
        LockInfo lock;

        // What to show: the edition the player picked, else the variant's.
        VariantKind Kind() const { return editionKind != VariantKind::Base ? editionKind : variantInfo.kind; }
        std::string DisplayName() const;
        bool IsSwapTop() const;
        bool IsSwapBottom() const;
        // When this game last took it ("" if never).
        std::string LastUsedIn(const std::string& game) const;
    };

    // The figure as `game` sees it (Catalogue::Read): a variant the game does not tell apart plays as the plain toy, or as the
    // variant it matches (a later repose as Series 2 in Giants). The player's own label and same-tag edition always stand.
    VariantKind KindIn(const Figure& figure, const std::string& game);
    std::string NameIn(const Figure& figure, const std::string& game);

    // Whether a toy keeps a level and gold (characters, minis and sidekicks), unlike items, adventure packs and traps.
    bool HasProgress(const Figure& figure);
    // The level a card shows for `game`, the same in the Toy Box and Skylauncher: "Level 6"; "Level 10 (15)" when the
    // figure's own level is above the game's cap (a figure Giants took to 15, shown in Spyro's Adventure); "New" for a
    // character never played; "Unreadable" for a damaged one; empty for toys without progress. French with `french`.
    std::string LevelText(const Figure& figure, const std::string& game, bool french = false);

    struct Filter
    {
        std::string game;             // only toys this game reads ("ssa"); empty: all
        int category = -1;            // a Category; -1: all
        int element = -1;             // an Element; -1: all
        int kind = -1;                // a VariantKind (Figure::Kind); -1: all
        bool favouritesOnly = false;
        std::string text;             // in the name, label, nickname or file name, any case
    };

    // Reads one dump (and its metadata and lock). False if it is not a 1 KB tag image.
    bool Read(const std::string& path, Figure& figure);
    // Every dump of the folder that passes the filter, by category, element, toy type, variant, file name.
    std::vector<Figure> List(const Filter& filter = {});

    // A new never-played swapper: both halves, linked (the toy types of a top and a bottom, 2000-2015 and 1000-1015; a
    // variant for each). Returns the paths; both empty on failure (nothing is left behind).
    struct SwapperPaths
    {
        std::string top, bottom;
    };
    SwapperPaths CreateSwapper(uint16_t topType, uint16_t bottomType, uint16_t topVariant = 0, uint16_t bottomVariant = 0);
    // The other half of the same swapper: 2003 gives 1003 and 1003 gives 2003; 0 for any other toy.
    uint16_t MatchingHalf(uint16_t type);

    // A new never-played figure: "<type> <name>.sky" (" (2)"... if taken), or `file` if given. Returns its path,
    // empty on failure. The player made it: it counts as theirs (origin "created").
    std::string Create(uint16_t type, uint16_t variant = 0, const std::string& file = {});
    // The game's starter set: a blank dump of every toy in `types` that has no plain (variant 0) dump in the folder.
    // Returns how many.
    int CreateMissing(const std::vector<uint16_t>& types);

    // Writes progress into the dump (both areas, docs/runtime/host/figures.md). Refused while another process uses it.
    bool WriteStats(const std::string& path, const figures::FigureStats& stats, std::string* error = nullptr);
    bool SetNickname(const std::string& path, const std::string& utf8, std::string* error = nullptr);
    // Renames the dump (and its metadata and backups). `file` is a bare file name; the extension is kept if missing.
    bool Rename(const std::string& path, const std::string& file, std::string* error = nullptr);

    bool SetLabel(const std::string& path, const std::string& label);
    bool SetFavourite(const std::string& path, bool favourite);
    // One of the toy's same-tag editions by name (or any text with `kind`); empty clears it.
    bool SetEdition(const std::string& path, const std::string& edition, VariantKind kind);
    // A game took the figure: last game and time, and per game the time and a use count (Recent).
    void Touch(const std::string& path, const std::string& game);
    // The figures a game took last, newest first, at most `count` paths.
    std::vector<std::string> Recent(const std::string& game, size_t count);

    // backups/<file>/<UTC date and time>.sky. Returns the backup's path, empty on failure.
    std::string Backup(const std::string& path);
    // Newest first.
    std::vector<std::string> Backups(const std::string& path);
    // Keeps the newest `keep` backups of the dump. Returns how many were removed.
    int PruneBackups(const std::string& path, size_t keep = 10);
    // Puts a backup's bytes back (the current dump is backed up first). Refused while another process uses it.
    bool Restore(const std::string& path, const std::string& backup, std::string* error = nullptr);
    // Clears both progress areas (the game starts the figure anew); serial, toy and variant stay. Backed up first.
    bool ResetToNew(const std::string& path, std::string* error = nullptr);
    // A copy with a new serial and the same progress ("<name> (copy)"), its metadata copied. Returns its path. A swapper's
    // half (it has a partner) is copied with the other half and the two copies are linked; the path is the copy of the half
    // asked for.
    std::string Duplicate(const std::string& path, std::string* error = nullptr);

    // --- Swappers: two dumps, one card (docs/runtime/host/collection.md "Swappers") ------------------------------------------

    // A swapper is a top (2000-2015, SwapTop) and a bottom (1000-1015, SwapBottom), each a dump of its own, linked by a
    // "partner" entry on both in collection.json. The link follows Rename, goes to the trash with a deleted half and comes
    // back with it (Undelete), and is never used unless both sides agree (Figure::partner). A half without a link stays a
    // card of its own. Games and Skylauncher show a linked pair as ONE card.
    //
    // Links the two dumps (one top, one bottom, either order). Each one's earlier partner is left unlinked, a card of its
    // own. Refused unless one is a top and the other a bottom, and while a game uses any of the four.
    bool Pair(const std::string& a, const std::string& b, std::string* error = nullptr);
    // Cuts the half's link (both sides). True also when it had none.
    bool Unpair(const std::string& path, std::string* error = nullptr);
    // "Change the bottom" / "Change the top": the card of `keep` takes `replacement` (a dump of the other category) as its
    // other half. The old half becomes a card of its own; if `replacement` was part of another swapper, that one gets the old
    // half instead, so no card disappears. Same refusals as Pair.
    bool ChangeHalf(const std::string& keep, const std::string& replacement, std::string* error = nullptr);

    // What a shelf shows for one figure or one swapper: `first` is the figure itself, or a swapper's top (it stands for
    // the card: its name, label, favourite, last use and picture, "Level N"); `second` is the swapper's bottom, null for
    // any other figure (also a half with no partner).
    struct Card
    {
        const Figure* first = nullptr;
        const Figure* second = nullptr;

        bool Swapper() const { return second != nullptr; }
        // A swapper is busy when either half is.
        bool Busy() const { return first->lock.Busy() || (second != nullptr && second->lock.Busy()); }
        // Both halves of a swapper, else the one figure: what goes on the portal, and what Delete, Export... apply to.
        std::vector<const Figure*> Halves() const;
    };
    // The cards of `list` in its order: a swapper whose two halves are both in the list once (as its top's place).
    std::vector<Card> Cards(const std::vector<Figure>& list);
    // The card the figure `list[index]` belongs to (a bottom gives its swapper's card).
    Card CardOf(const std::vector<Figure>& list, size_t index);
    std::string SwapperName(const std::string& top, const std::string& bottom);
    // The card's name: the top's label if the player gave one; else the swapper's name when both halves are the same
    // swapper ("Wash Buckler"); else the mixed swapper's name as the game gives it, SwapperName ("Nitro Magna Jet");
    // the figure's DisplayName for a single figure.
    std::string CardName(const Card& card);
    // LevelText of the card's first figure (a swapper's top carries the level), `game` as for LevelText.
    std::string CardLevelText(const Card& card, const std::string& game, bool french = false);

    // --- Ownership and the tidy ------------------------------------------------------------------------------------------

    // A never-played dump the game made on its own (older builds made one per character at every start): blank, its
    // starter file name ("<type> <name>.sky"), plain variant, and nothing the player did (no origin, label, favourite,
    // edition). The player's own figures are every other dump.
    bool UntouchedStarter(const Figure& figure);
    std::vector<std::string> UntouchedStarters();
    // Moves the untouched starters to the trash (reason "tidy") and remembers it was done. Returns how many.
    int Tidy();
    // Whether Tidy ran once in this collection (the one-time message after the update).
    bool TidyDone();

    // --- Trash ------------------------------------------------------------------------------------------------------------

    struct TrashEntry
    {
        std::string id;      // its file in trash/ ("2026-10-24T101500Z 014 Gill Grunt.sky")
        std::string file;    // the name it had
        std::string deleted; // ISO 8601, UTC
        std::string reason;  // "deleted", "tidy"
        std::string path;    // the dump inside trash/
    };
    // Moves the dump (and its metadata) to trash/. Refused while another process uses it.
    bool Delete(const std::string& path, const std::string& reason = "deleted", std::string* error = nullptr);
    // Newest first.
    std::vector<TrashEntry> Trash();
    // Puts it back under its old name (" (2)" if taken). Returns the new path, empty on failure.
    std::string Undelete(const std::string& id);
    bool DeleteForGood(const std::string& id);
    // Removes what has been in the trash longer than `days`. Returns how many.
    int PurgeTrash(int days = 30);

    // --- Import and export --------------------------------------------------------------------------------------------

    struct ImportReport
    {
        int added = 0, duplicates = 0, invalid = 0;
        std::vector<std::string> paths;   // the new dumps
        std::vector<uint16_t> unknown;    // toy types the catalogue does not know, among the new dumps
    };
    // Dumps (files, or every dump of a folder): each must be a 1 KB tag; byte-for-byte copies of a dump already in the
    // collection are skipped. New ones get origin "imported".
    ImportReport Import(const std::vector<std::string>& paths);
    // Keeps a dump of a real toy (what the title read from it): origin "portal"; an identical dump is not copied twice.
    std::string ImportTag(const FigureData& tag, std::string* error = nullptr);
    // Copies dumps into `folder` (created). Returns how many.
    int Export(const std::vector<std::string>& paths, const std::string& folder);

    // --- Views and the wheel (collection.json "views") ------------------------------------------------------------------

    // How My Skylanders was shown, per game and per app ("game", "launcher"): the View menu of before the shelf. Kept
    // in collection.json; the screens no longer read it since 2026-10-26 (docs/runtime/host/toybox_ux.md section 3).
    struct View
    {
        std::string group = "smart"; // smart, element, game, category, series, level, name
        std::string order = "recent"; // recent, level, name, toy
        std::string show = "all";     // all, favourites, played, new
        bool foldForeign = true;      // "Not for this game": folded at the end, else hidden

        bool operator==(const View&) const = default;
    };
    View GetView(const std::string& game, const std::string& app);
    bool SetView(const std::string& game, const std::string& app, const View& view);

    // The quick swap wheel's eight places of a player's seat: a file name or empty; pinned places stay until changed,
    // the others were filled from recents and are kept until a pin replaces them.
    struct WheelPlace
    {
        std::string file;
        bool pinned = false;
    };
    std::vector<WheelPlace> GetWheel(const std::string& game, const std::string& seat);
    bool SetWheel(const std::string& game, const std::string& seat, const std::vector<WheelPlace>& places);
    // The figures the player took off a seat's wheel: the recents do not bring them back.
    std::vector<std::string> GetWheelSkipped(const std::string& game, const std::string& seat);
    bool SetWheelSkipped(const std::string& game, const std::string& seat, const std::vector<std::string>& files);

    // "On the wheel" (the player's own wheel, docs/runtime/host/toybox_ux.md section 5): a figure pinned on a place of the
    // wheel of every player seat of `game` that reads it (the Toy Box and Skylauncher's sheet toggle it). Adding takes the
    // first empty place, else the first place the recents filled (a pinned place stays); false when the game does not read
    // the figure, or all eight places are pinned. Removing empties its places and keeps the recents from bringing it back.
    bool OnWheel(const std::string& game, const std::string& file);
    bool SetOnWheel(const std::string& game, const std::string& file, bool on);

    // --- Sections (docs/runtime/host/toybox_ux.md section 3) -----------------------------------------------------------------------

    enum class GroupBy { Smart, Element, Game, Category, Series, Level, Name };
    enum class OrderBy { Recent, Level, Name, Toy };
    GroupBy ParseGroupBy(const std::string& id);
    OrderBy ParseOrderBy(const std::string& id);

    struct Section
    {
        enum class Kind { Shelf, Element, Game, Category, Series, Level, Letter, Unknown, Foreign };
        Kind kind;
        int value = 0;            // the element, category, variant kind, level or letter
        std::string game;         // Kind::Game
        std::vector<size_t> items; // indices into the list; a linked swapper is its top's index only (the bottom is left out)
    };
    // Sections of `list` for `game`. Smart is the shelf (Toy Box and Skylauncher alike): one section where every figure
    // shows once, favourites first, then the figures `game` took (newest first), then by element and category, a
    // character's variants next to it. The other groupings cut the list by element, game, category, series, level or
    // name. Every grouping ends with Unknown toys and the toys the game does not read (Foreign; none when `game` is
    // empty). A figure is in one section only; empty sections are left out.
    std::vector<Section> Group(const std::vector<Figure>& list, GroupBy group, OrderBy order, const std::string& game);

    // --- The portal at a glance ---------------------------------------------------------------------------------------

    // "P1", "P2", "Item", "Adventure", "Trap", "V1" ("J1"/"J2" in French) for a seat; its long name ("Player 1",
    // "Magic item", "Vehicle 1") with SeatName. Seats of a catalogue game: Game::seats.
    std::string SeatTag(const Seat& seat, bool french = false);
    std::string SeatName(const Seat& seat, bool french = false);

    // One occupied place of the portal, in the seats' order.
    struct PortalEntry
    {
        size_t seat = 0;     // index in the game's seats
        int slot = -1;       // the portal's hardware slot, -1 if unknown
        std::string tag, seatName;
        std::string path, name;
    };
    // Every seat with a figure on it. `paths` and `names` hold one string per seat (empty: free); `slots` (optional)
    // the hardware slot of each seat, else -1. A seat past the end of a vector counts as free.
    std::vector<PortalEntry> PortalOverview(const Game& game, const std::vector<std::string>& paths,
        const std::vector<std::string>& names, const std::vector<int>& slots = {}, bool french = false);
    // "P1 Gill Grunt · P2 Spyro · Item Shield"; `emptyText` when nothing is on the portal.
    std::string PortalOverviewLine(const std::vector<PortalEntry>& entries, const std::string& emptyText);

    // --- Migration from the per-game folders ---------------------------------------------------------------------------

    // Where an older build kept the figures: SKY_MIGRATE_FROM, else (only when neither SKY_COLLECTION nor SKY_FIGURES
    // is set) "figures" in the SSA game folder (datadir.h). Empty: nothing to migrate.
    std::string LegacyFolder();

    struct Migration
    {
        int copied = 0;   // new files in the collection
        int matched = 0;  // already there byte for byte
        int failed = 0;
    };
    // Copies every dump of `from` that was not migrated yet into the collection (never deletes or changes `from`),
    // and records which file became which in collection.json.
    Migration Migrate(const std::string& from);
    // The collection copy of a migrated file (portal.txt still names the old paths); `path` itself otherwise.
    std::string MigratedPath(const std::string& path);
}
