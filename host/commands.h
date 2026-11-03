#pragma once

// The F3 menu's Commands page on launcher.ini (docs/runtime/host/gamemenu.md, "Commands"): a game's actions with their
// keyboard key, pad button and mouse button, stored as Skylauncher stores them (its launcher/game_keys.cpp): [<game>.input]
// key_<action>=<SDL scancode name>, pad_<button>=<button>|none, mouse=0|1 and mouse_<action>=<mouse name>|none, and the
// Toy Box's keys of every game in [input] key_toybox, key_toybox_add, key_toybox_clear. The page itself is
// host/toybox/menu.cpp.
#include "host/launcher.h"
#include "host/mouse.h"
#include "host/pad_map.h"
#include <span>
#include <string>
#include <string_view>

struct RuntimeSettings;

namespace commands
{
    // One action of a game (game::Adapter::inputRows), in Skylauncher's order and words.
    struct Row
    {
        const char* action; // key_<action>: one of g_keyActions ("a", "left_up")
        const char* label;  // English, as Skylauncher words it
        const char* french; // its French (Skylauncher's lang/fr.txt)
        const char*
            pad; // the pad button whose job it is (g_padButtons: "a", "lb"); nullptr: a stick direction (no pad cell)
    };

    std::string Section(std::string_view game); // "ssa.input"
    // Starts the game's copy from the shared [input] keys once and marks it seeded=1 (Skylauncher's SeedGameInput).
    void Seed(LauncherConfig& config, std::string_view game);
    // A row's key: the game's copy, else the default (g_keyActions, Skylauncher's too).
    std::string Key(const LauncherConfig& config, std::string_view game, const Row& row);
    // The pad button whose job `button` does: its own when nothing is stored, -1 for none.
    int PadTarget(const LauncherConfig& config, std::string_view game, int button);

    // The Toy Box's keys (all games): open it, add the next figure, clear the portal; F6, F7, F8 when not set. While
    // key_toybox is not set Tab opens it too (ToyboxTab).
    inline constexpr int ToyboxKeyCount = 3;
    inline constexpr const char* ToyboxLines[ToyboxKeyCount] = { "key_toybox", "key_toybox_add", "key_toybox_clear" };
    std::string ToyboxKey(const LauncherConfig& config, int which);
    bool ToyboxTab(const LauncherConfig& config);

    // A key slot of the page: [0, rows.size()) the game's rows, then the Toy Box's three keys.
    std::string SlotKey(const LauncherConfig& config, std::string_view game, std::span<const Row> rows, size_t slot);
    // A swap notice names a direction even when the page abbreviates its action as "Same".
    std::string SlotLabel(std::span<const Row> rows, size_t slot, bool french);

    // Each change seeds the game's copy first and then puts the game's keys in [input] (as Skylauncher's PLAY does), so
    // that a start without the launcher has them too.
    // `key` for a slot; the slot that had it (a row of the game or a Toy Box key) gets the slot's old key. Returns that
    // slot, -1: none.
    int SetKey(
        LauncherConfig& config, std::string_view game, std::span<const Row> rows, size_t slot, const std::string& key);
    // The physical `button` does the job `job` (g_padButtons' indexes); the button that did `job` takes `button`'s old
    // job, so no job is lost. Returns that button, -1: none.
    int SetPad(LauncherConfig& config, std::string_view game, std::span<const Row> rows, int job, int button);
    // The mouse's switch (off by default) and the button each action is on (mouse::Effective: None everywhere when off).
    bool MouseOn(const LauncherConfig& config, std::string_view game);
    void SetMouseOn(LauncherConfig& config, std::string_view game, std::span<const Row> rows, bool on);
    mouse::Bindings MouseLines(const LauncherConfig& config, std::string_view game);
    // `button` for g_keyActions[action] (one action per button: the action that had it gets none). Returns that action
    // (an index of g_keyActions), -1: none.
    int SetMouse(LauncherConfig& config, std::string_view game, std::span<const Row> rows,
        std::span<const mouse::Default> defaults, size_t action, mouse::Button button);

    // Back to the defaults: the game's key_, pad_ and mouse_ lines and the Toy Box's keys go (the mouse switch stays).
    void Reset(LauncherConfig& config, std::string_view game, std::span<const Row> rows);

    // The keys, pad map, mouse and Toy Box keys a start with `config` uses (as ParseSettings reads them: [input], then
    // [<game>.input]).
    struct Bindings
    {
        std::string keys[22]; // per g_keyActions entry
        PadMap padMap;
        mouse::Bindings mouse;
        std::string toyboxKeys[ToyboxKeyCount] = { "F6", "F7", "F8" };
        bool toyboxTab = true;
    };
    Bindings Read(const LauncherConfig& config, std::string_view game);

    // The running game: the page hands over the bindings it saved; the main loop takes them once (true) and rebuilds
    // its input from `settings`.
    void Publish(const Bindings& bindings);
    bool Take(RuntimeSettings& settings);
}
