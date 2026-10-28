#pragma once

// The Toy Box's state and logic (docs/runtime/host/toybox_ux.md): the seats of the portal along the top, your figures
// for the chosen seat as one grid (the shelf: each figure once, favourites and recent first), the Figure page (three
// main actions, the rest behind More), Edit stats, Add a figure, names, Manage, the quick swap wheel. No drawing here: draw.cpp
// shows a Model and calls its actions, toybox.cpp feeds it the input. Everything runs on the host's main thread.
#include "host/collection/collection.h"
#include "host/collection/search.h"
#include "host/collection/upgrades.h"
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace toybox
{
    // Everything the player can do with a pad, the keyboard or the mouse, as the design's verbs (section 8).
    enum class Command { None, Up, Down, Left, Right, Accept, Back, Options, View, PreviousSection, NextSection, Delete, Start };

    enum class Screen
    {
        Home,      // the seats along the top, your figures for the chosen seat below
        Figure,    // screen 7
        Pin,       // screen 7's "Pin to the wheel" place picker
        Backups,   // Figure → Restore a backup
        Half,      // Figure → Change the bottom / the top: the halves to choose from (docs/runtime/host/toybox_ux.md)
        Stats,     // Edit stats (the old sheet)
        Upgrades,  // Edit stats → Upgrades: the toy's upgrades by name
        Add,       // screen 8
        Name,      // screen 9
        Manage,    // screen 10
        Wheel,     // screen 4, over the game
        WheelList, // screen 5, over the game
        Menu,      // the F3 menu: volume, FPS counter, settings, Report a problem (docs/runtime/host/gamemenu.md), over the game
    };

    // A place on the portal (catalogue seats) and what is on it.
    struct SeatState
    {
        collection::Seat seat;
        int slot = -1;              // the emulated portal's hardware slot it uses
        std::string path;           // the dump on it; empty: free
        std::string previous;       // what was there before the last change (Undo)
        uint64_t placedAt = 0;      // SDL_GetTicksNS() of the last change, 0: none
        bool waiting = false;       // the title has not read it since
        bool late = false;          // ...for more than 20 s
        // A real portal: what the title read from the toy.
        bool physical = false;
        uint16_t physicalType = 0, physicalVariant = 0;
        bool physicalKnown = false; // block 1 was read
        int physicalLevel = 0;
        uint32_t physicalGold = 0;
        // A swapper (Swap Force) takes two hardware slots: `path` / `slot` hold its top, these its bottom. Empty / -1 for
        // any other figure. (Appended last: SwapPlayers fills a seat by position.)
        std::string path2;
        int slot2 = -1;
    };

    // One cell of the list: a figure, the "+ Add" card of a section, or the folded "Not for this game" line.
    struct Cell
    {
        enum class Kind { Figure, Add, Fold } kind = Kind::Figure;
        int figure = -1;     // index in Model::figures
        int section = 0;     // index in Model::sections
        int row = 0, column = 0;
        collection::Element element = collection::Element::None; // Add: the section's element
    };

    struct ListSection
    {
        collection::Section::Kind kind;
        int value = 0;
        std::string game;
        int count = 0;
        bool foreign = false; // "Not for this game": greyed, cannot be placed
        bool titled = true;   // a header above its first row (the shelf has none: the screen says what it is for)
        bool folded = false;
        int firstRow = 0;     // grid rows, the header is drawn above firstRow
    };

    // The cells of a sectioned grid with a fixed number of columns.
    struct Grid
    {
        std::vector<ListSection> sections;
        std::vector<Cell> cells;
        int rows = 0;
        int cursor = 0;  // index in cells
        int scroll = 0;  // pixels scrolled
        // Layout in pixels, set by the model for the drawing: a header above the first row of each titled section.
        int rowHeight = 100, headerHeight = 40, visibleHeight = 400;
        int RowY(int row) const; // top of a row, from the top of the list
        int HeaderY(int section) const; // top of a section's header (the row above its first, when titled)
    };

    // A modal question over the current screen.
    struct Dialog
    {
        std::string title, text;
        std::vector<std::string> buttons;
        int focused = 0;
        bool hold = false; // the first button needs A held for a second (reset, delete)
        std::function<void(int)> done; // the button chosen; -1 for B
    };

    // Add a figure: one card per character, its versions in the side panel.
    struct Version
    {
        uint16_t type = 0, variant = 0;
        std::string name, edition;   // edition: a same-tag edition (created as the plain toy, the edition recorded)
        collection::VariantKind kind = collection::VariantKind::Base;
        int owned = 0;
    };

    struct NameEntry
    {
        enum class Purpose { NewFigure, Rename, Nickname, Search, ToyName } purpose = Purpose::Rename;
        std::string text, title;
        size_t cursor = 0;       // byte offset
        size_t limit = 40;       // characters
        std::vector<std::string> suggestions;
        int focus = 0;           // 0..suggestions-1: suggestions; then the keys
        int keyRow = 0, keyColumn = 0;
        bool onKeys = false;
        bool shift = true;
        bool symbols = false;
        std::function<void(bool, const std::string&)> done;
    };

    struct ManageState
    {
        int section = 0;       // Trash, Backups, Import, Export, Unknown, Tidy
        bool inPanel = false;
        int item = 0;
        std::vector<collection::TrashEntry> trash;
        std::vector<int> withBackups;   // figures with at least one backup
        std::vector<int> unknown;       // figures the catalogue does not know
        std::vector<std::string> starters;
        std::string importFolder, exportFolder;
    };

    struct WheelState
    {
        int seat = 0;                                  // index in Model::seats
        std::vector<collection::WheelPlace> places;    // 8
        std::vector<int> figures;                      // per place: index in Model::figures, -1 empty
        int aimed = -1;                                // -1: the centre (cancel)
        uint64_t pinHoldStart = 0;
    };

    struct StatsState
    {
        int figure = -1;
        FigureData tag{};
        figures::FigureStats stats;
        bool dirty = false;
        int row = 0;
    };

    // One place of the Upgrades screen's grid: an upgrade of the table, or All / None / Done.
    struct UpgradeItem
    {
        enum class Kind { Upgrade, All, None, Done } kind = Kind::Upgrade;
        int index = 0;   // into the table's upgrades
        int column = 0, row = 0;
    };

    struct UpgradesState
    {
        collection::UpgradeTable table;
        std::vector<UpgradeItem> items;
        int cursor = 0;
    };

    // One line of the F3 menu.
    struct MenuRow
    {
        enum class Kind { Slider, Toggle, Choice, Action } kind = Kind::Action;
        std::string label, value;   // value: "70 %", "On", "60", "Auto"; empty for an action
        int percent = 0;            // Slider: 0-100
        bool nextStart = false;     // applies at the next start (the row says so)
    };

    class Model
    {
    public:
        // --- Data ------------------------------------------------------------------------------------------------------
        std::string game = SSA_GAME_ID; // the game this runtime is built for ("ssa", "giants")
        int LevelCap() const { return figures::LevelCap(game); } // the level cap of that game: 10 or 15
        // The hats of that game for the hat picker (0 first: none), empty when the catalogue lists none (Spyro's
        // Adventure: the number is edited); a hat's name ("Hat 7" when not listed).
        std::vector<uint16_t> Hats() const;
        std::string HatName(uint16_t hat) const;
        const collection::Game* gameInfo = nullptr;
        bool realPortal = false;                 // read-only seats from the traffic
        std::vector<collection::Figure> figures; // every dump but the untouched starters
        std::vector<SeatState> seats;
        std::string search;
        bool foreignOpen = false;                // "Not for this game" unfolded
        // The shelf's filters and sort (docs/runtime/host/toybox_ux.md "Search, filters and sort"); -1 or empty: all.
        int filterElement = -1;                  // a collection::Element
        std::string filterGame;                  // the game a toy came with ("giants")
        int filterCategory = -1;                 // a collection::Category
        collection::Sort sort = collection::Sort::Recent;
        bool filtersOpen = false;                // the Filters panel is open over the shelf
        int filterRow = 0;                       // its focused row: Element, Game, Type, Sort, Clear all
        int shownCount = 0;                      // the cards the shelf shows (BuildList)

        // --- Navigation ------------------------------------------------------------------------------------------------
        Screen screen = Screen::Home;
        bool open = false;
        int seat = 0;               // the focused seat
        bool inList = true;         // Home: focus in the grid (else the row of seats along the top)
        int column = 0;             // the top row's place: the seats, then Add a figure, Manage
        Grid list;                  // your figures for the focused seat (Home, WheelList)
        int figure = -1;            // the Figure page's figure
        int figureAction = 0;
        bool figureMore = false;    // the Figure page shows its other actions (More)
        bool figureDetails = false; // ...and the technical details (Details)
        int pinPlace = 0;
        int backup = 0;
        std::vector<std::string> backups;
        StatsState stats;
        UpgradesState upgrades;
        Grid add;                   // Add a figure: characters
        std::vector<const collection::Toy*> addToys; // per cell (Cell::figure indexes it)
        bool addVersions = false;
        int addVersion = 0;
        std::vector<Version> versions;
        NameEntry name;
        ManageState manage;
        WheelState wheel;
        std::vector<Dialog> dialogs;
        uint64_t holdStart = 0;     // A held on a hold button (dialog), since
        float holdProgress = 0;

        // --- The F3 menu ---------------------------------------------------------------------------------------------------
        static constexpr int MenuRowCount = 10;
        int menuRow = 0;
        std::vector<MenuRow> MenuRows() const;
        void OpenMenu();
        void FocusMenuRow(int row, int step); // the mouse: focus the row, then step -1 / +1 (a value) or 0 (do it)

        // --- Messages --------------------------------------------------------------------------------------------------
        uint32_t lightRgb = 0;      // the portal's light as the game last set it, in 4 steps per channel (0xRRGGBB); 0 = off
        std::string fps;            // the FPS counter's text ("30 FPS"), empty: off; set by toybox.cpp, drawn in the corner
        std::string toast;          // shown 3 s (5 s for long ones), in the Toy Box and over the game
        uint64_t toastUntil = 0;
        void Notice(const std::string& text, uint64_t seconds) { Toast(text, seconds); } // ToyboxNotice (host/toybox.h)
        int tidied = 0;             // figures this start's tidy moved (the one-time message)

        // --- Calls from toybox.cpp -----------------------------------------------------------------------------------
        void Init(bool real);
        void Open(Screen screen = Screen::Home, int seatIndex = 0);
        void Close();
        void Run(Command command);
        void Hold(bool held, uint64_t now); // A (Enter, the mouse) held: hold buttons
        void Type(const std::string& utf8);  // text from a keyboard
        void Backspace();
        // Once per frame while anything is shown: the portal's state, waiting seats, timeouts. True if it changed.
        bool Tick(uint64_t now);
        bool Dirty() const { return m_dirty; }
        void Clean() { m_dirty = false; }
        void Touch() { m_dirty = true; }

        // Wheel (toybox.cpp aims it from the stick, the arrows or 1-8).
        void OpenWheel(int seatIndex);
        void AimWheel(int place);           // -1: the centre
        bool WheelPossible(const collection::Figure& f) const; // a player seat of this game reads it: it can be on the wheel
        void ConfirmWheel();                // swaps to the aimed figure (or cancels) and closes
        void WheelSeat(int step);           // LB/RB: the next seat with a wheel
        void WheelList();                   // Y: the full list for that seat
        void PinFromWheel(uint64_t now, bool held); // X held on a place

        // --- Actions the drawing's mouse areas call ----------------------------------------------------------------
        void FocusSeat(int index);
        void FocusColumnRow(int row);
        void FocusCell(int cell);
        void ActivateCell(int cell);        // double click: put on
        void OptionsCell(int cell);         // right click: Figure page
        void ScrollList(int rows);
        void FocusAction(int index);
        void FocusAddCell(int cell);
        void FocusVersion(int index);
        void FocusKey(int row, int column);
        void FocusSuggestion(int index);
        void FocusManage(int section, int item, bool panel);
        void FocusStatsRow(int row);
        void FocusUpgrade(int item); // a click: focus it and do it
        void FocusDialogButton(int index);

        // --- What the drawing asks ----------------------------------------------------------------------------------
        int SeatOf(const std::string& path) const;               // -1 if not on a seat
        const collection::Figure* SeatFigure(int index) const;   // null if empty or physical
        bool Accepts(const SeatState& s, const collection::Figure& f) const;
        std::string SeatLabel(int index) const;                  // "Player 1", "Magic item"
        std::string SeatTag(int index) const;                    // "P1", "Item"
        // Every occupied seat with its hardware slot ("P1 Gill Grunt · Item Shield"), from the seats the portal shows.
        std::vector<collection::PortalEntry> Overview() const;
        // A swapper is one card: its name (FigureName), level line (LevelLine) and busy state cover both halves, and
        // `figures` lists its top (a lone half is its own card). In the Half screen every dump is its own card.
        collection::Card CardFor(const collection::Figure& f) const;
        bool CardBusy(const collection::Figure& f) const;
        // Out of health (the game asked for the seat again while its figure was on it, or said the Skylander died): the
        // dumps that cannot play until the next level. The card and the seat show a red cross; they can still be picked.
        bool OutOfHealth(const collection::Figure& f) const { return outOfHealth.count(f.path) != 0; }
        bool MarkOutOfHealth(int seatIndex, bool certain); // true when a figure was marked; `certain`: the game said it died
        void ClearOutOfHealth();
        std::set<std::string> outOfHealth;
        std::string FigureName(const collection::Figure& f) const;
        std::vector<std::string> FigureActions() const;          // the Figure page's lines
        std::vector<std::string> ColumnRows() const;             // the top row's places after the seats
        // The name of a toy on the Add a figure screen: a swapper's top is the swapper ("Boom Jet", not "Boom Jet (top)").
        static std::string AddName(const collection::Toy& toy);
        bool OwnsAnything() const { return !figures.empty(); }
        // The Filters panel: four rows (Element, Game, Type, Sort) and Clear all. Left and Right change a row's value
        // (the first is "All"), Accept changes it to the next.
        static constexpr int FilterRows = 4;
        std::string FilterRowLabel(int row) const;
        std::string FilterValueLabel(int row) const;      // the value now, "All elements"...
        void CycleFilter(int row, int step);
        void ClearSearch();
        void OpenFilters();
        void CloseFilters();
        // What is on, as the small tags above the shelf; each has an x (RemoveTag). The search is the field's own text.
        struct Tag
        {
            std::string text;
            int row; // the Filters row it belongs to
        };
        std::vector<Tag> Tags() const;
        void RemoveTag(int index);
        bool FiltersActive() const; // a search, a filter or a sort other than Recent
        void ClearFilters();        // all of them
        int OwnedCount(uint16_t type, uint16_t variant, const std::string& edition) const;
        std::string SectionLabel(const ListSection& section) const;
        std::string GameName() const;
        // The level a card shows: "Level 6", "Level 10 (15)" (collection::LevelText), or who holds the figure.
        std::string LevelLine(const collection::Figure& f) const;
        static constexpr int ListColumns = 7, AddColumns = 4, WheelPlaces = 8;
        static const std::vector<std::vector<std::string>>& Keys(bool french, bool symbols, bool shift);

    private:
        void Reload();                       // figures from the collection
        void RefreshSeats(uint64_t now);
        void BuildList();
        bool HalfMadeWithTop(const collection::Toy& toy) const;
        void BuildAdd(collection::Element element = collection::Element::Count);
        void BuildVersions();
        void FixGrid(Grid& grid, int visibleRows);
        void MoveGrid(Grid& grid, Command command);
        void JumpSection(Grid& grid, int step);
        void Toast(const std::string& text, uint64_t seconds = 3);
        void Ask(Dialog dialog);

        void RunHome(Command command);
        void RunList(Command command, bool wheelList);
        void OpenSearch();
        void RunFilters(Command command);
        void RunFigure(Command command);
        void RunPin(Command command);
        void OpenHalf();                     // Figure → Change the bottom / the top
        void RunHalf(Command command);
        void ChangeHalfTo(const std::string& keep, const std::string& replacement);
        void RunBackups(Command command);
        void RunStats(Command command);
        void OpenUpgrades();
        void RunUpgrades(Command command);
        void RunAdd(Command command);
        void RunName(Command command);
        void RunManage(Command command);
        void RunDialog(Command command);
        void RunMenu(Command command);
        void MenuStep(int row, int step);

        // Portal.
        bool PutOn(int seatIndex, const std::string& path, bool fromWheel = false);
        void TakeOff(int seatIndex);
        void SwapPlayers();
        void ClearPortal();
        void SeatOptions(int seatIndex);
        // A hardware slot for a seat's `half` (0: the figure or a swapper's top, 1: its bottom), never `avoid`.
        int FreeSlot(int seatIndex, int half = 0, int avoid = -1) const;
        // Puts a card on a free seat's portal slots (both halves of a swapper). False if the portal is full.
        bool PlaceCard(int seatIndex, const std::string& path);
        void RemoveFromPortal(int seatIndex); // the seat's figures off the portal (not the seat's other state)

        // Figures.
        void OpenFigure(int index);
        void DoFigureAction(int action);
        void OpenStats(int index);
        void SaveStats();
        void OpenName(NameEntry entry);
        void CreateFrom(const Version& version);
        void SaveCopy(int seatIndex);       // real portal: keep a dump of the toy
        void OpenManage();
        void RefreshManage();
        void ManageAction();
        int FindFigure(const std::string& path) const;
        void Refocus(const std::string& path); // the list's cursor on that figure after a reload

        bool m_dirty = true;
        bool m_holdArmed = false;
        uint64_t m_lastScan = 0;
    };

    Model& Get();
    bool French();
    // English or French by the game's text language (SKY_LANGUAGE, [ssa] language).
    inline const char* L(const char* english, const char* french) { return French() ? french : english; }
    std::string ElementLabel(collection::Element element);
    std::string CategoryLabel(collection::Category category);
    std::string VariantLabel(collection::VariantKind kind);
}
