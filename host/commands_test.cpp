// Checks the F3 menu's Commands page on a scratch launcher.ini (host/commands.h): a key written to the game's copy, the
// copy seeded once from [input], the swap with the row that had the key, the pad jobs swapped so none is lost, the mouse's
// switch and buttons (one action per button), the Toy Box's keys (swapped with the game's), Reset, the game's keys put in
// [input] as Skylauncher's PLAY does, and the bindings the running game takes.
// usage: ArkeyanCommandsTest [<scratch dir>] (default: the system's temporary folder)
#include "host/commands.h"
#include "host/settings.h"
#include "host/toybox/keys.h"
#include "host/toybox/hit.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    constexpr commands::Row Rows[] = {
        { "left_up", "Move up", "Haut", nullptr },
        { "a", "Jump", "Sauter", "a" },
        { "x", "Attack 1", "Attaque 1", "x" },
        { "b", "Attack 2", "Attaque 2", "b" },
        { "rb", "Attack 2", "Attaque 2", "rb" },
    };
    constexpr size_t RowA = 1, RowX = 2, RowB = 3;
    constexpr size_t SlotToybox = std::size(Rows), SlotToyboxAdd = SlotToybox + 1;
    constexpr mouse::Default MouseDefaults[] = { { "x", mouse::Left }, { "b", mouse::Right } };

    std::string Value(const LauncherConfig& config, const std::string& section, const std::string& key)
    {
        auto entry = config.find(section);
        if (entry == config.end())
            return "<none>";
        auto value = entry->second.find(key);
        return value == entry->second.end() ? "<none>" : value->second;
    }

    size_t KeyIndex(const char* action)
    {
        for (size_t i = 0; i < std::size(g_keyActions); i++)
            if (std::string_view(action) == g_keyActions[i].name)
                return i;
        return 0;
    }

    // The bindings Read gives are the ones a start with that file gets (ParseSettings).
    bool SameAsParse(const LauncherConfig& config)
    {
        RuntimeSettings settings;
        std::vector<std::string> warnings;
        std::string error;
        if (!ParseSettings(
                config, "game", [](const char*) -> const char* { return nullptr; }, 0, nullptr, settings, warnings,
                error))
            return false;
        commands::Bindings read = commands::Read(config, "game");
        for (size_t i = 0; i < std::size(g_keyActions); i++)
            if (read.keys[i] != settings.keys[i])
                return false;
        for (int i = 0; i < PadButtonCount; i++)
            if (read.padMap.target[i] != settings.padMap.target[i])
                return false;
        for (size_t i = 0; i < mouse::ActionCount; i++)
            if (read.mouse.set[i] != settings.mouse.set[i] || read.mouse.stored[i] != settings.mouse.stored[i])
                return false;
        for (int i = 0; i < commands::ToyboxKeyCount; i++)
            if (read.toyboxKeys[i] != settings.toyboxKeys[i])
                return false;
        return read.mouse.enabled == settings.mouse.enabled && read.toyboxTab == settings.toyboxTab;
    }
}

int main(int argc, char** argv)
{
    // The exact key classification used by ToyboxHandleEvent, both at startup and after a live rebind.
    {
        toybox::Keys keys;
        CHECK(keys.Open(SDL_SCANCODE_F6) && keys.Hold(SDL_SCANCODE_TAB));
        CHECK(!keys.Hold(SDL_SCANCODE_F6) && !keys.Open(SDL_SCANCODE_TAB));
        CHECK(keys.add == SDL_SCANCODE_F7 && keys.clear == SDL_SCANCODE_F8);
        LauncherConfig config;
        config["input"] = { { "key_toybox", "O" }, { "key_toybox_add", "P" }, { "key_toybox_clear", "U" } };
        RuntimeSettings settings;
        std::vector<std::string> warnings;
        std::string error;
        CHECK(ParseSettings(
            config, "game", [](const char*) -> const char* { return nullptr; }, 0, nullptr, settings, warnings, error));
        keys.Set(settings.toyboxKeys, settings.toyboxTab);
        CHECK(keys.Hold(SDL_SCANCODE_O) && !keys.Open(SDL_SCANCODE_O)); // tap/hold uses this key now
        CHECK(!keys.Hold(SDL_SCANCODE_TAB) && !keys.Open(SDL_SCANCODE_F6));
        CHECK(keys.add == SDL_SCANCODE_P && keys.clear == SDL_SCANCODE_U);
        config["input"]["key_toybox"] = "Tab";
        auto bindings = commands::Read(config, "game");
        keys.Set(bindings.toyboxKeys, bindings.toyboxTab);
        CHECK(keys.Hold(SDL_SCANCODE_TAB) && !keys.Open(SDL_SCANCODE_TAB));
        commands::Reset(config, "game", Rows);
        bindings = commands::Read(config, "game");
        keys.Set(bindings.toyboxKeys, bindings.toyboxTab);
        CHECK(keys.Hold(SDL_SCANCODE_TAB) && keys.Open(SDL_SCANCODE_F6));
        CHECK(keys.add == SDL_SCANCODE_F7 && keys.clear == SDL_SCANCODE_F8);
    }
    // Both clicks and hover use the topmost drawn hit, with half-open edges and blocking backdrops.
    {
        struct Point
        {
            float x, y;
        };
        struct Hit
        {
            Point min, max;
        };
        const Hit hits[] = { { { 0, 0 }, { 20, 20 } }, { { 10, 10 }, { 30, 30 } } };
        CHECK(toybox::HitAt(std::span<const Hit>(hits), 0, 0) == 0);
        CHECK(toybox::HitAt(std::span<const Hit>(hits), 15, 15) == 1);
        CHECK(toybox::HitAt(std::span<const Hit>(hits), 20, 5) == -1);
        CHECK(toybox::HitAt(std::span<const Hit>(hits), 30, 30) == -1);
        CHECK(toybox::HitAt(std::span<const Hit>{}, 0, 0) == -1);
    }
    // A swap into any right-stick row names its direction, never the page's abbreviated "Same".
    {
        constexpr commands::Row rows[] = {
            { "b", "Special Power / Back", "Pouvoir spécial / Retour", "b" },
            { "right_up", "Shake / Insert / Swing", "Agiter / Insérer / Balancer", nullptr },
            { "right_down", "Same", "Idem", nullptr },
            { "right_left", "Same", "Idem", nullptr },
            { "right_right", "Same", "Idem", nullptr },
        };
        const char* keys[] = { "I", "K", "J", "L" };
        const char* english[] = { "R stick up", "R stick down", "R stick left", "R stick right" };
        const char* french[] = { "stick droit haut", "stick droit bas", "stick droit gauche", "stick droit droite" };
        for (size_t i = 0; i < std::size(keys); i++)
        {
            LauncherConfig config;
            int other = commands::SetKey(config, "game", rows, 0, keys[i]);
            CHECK(other == int(i + 1));
            CHECK(commands::SlotLabel(rows, size_t(other), false) == english[i]);
            CHECK(commands::SlotLabel(rows, size_t(other), true) == french[i]);
            CHECK(commands::SetKey(config, "game", rows, 0, keys[i]) == -1);
            // Rebinding from a right-stick row must name the other game action as well.
            config.clear();
            other = commands::SetKey(config, "game", rows, i + 1, "X");
            CHECK(other == 0);
            CHECK(commands::SlotLabel(rows, size_t(other), false) == "Special Power / Back");
        }
        CHECK(commands::SlotLabel(rows, 0, false) == "Special Power / Back");
        CHECK(commands::SlotLabel(rows, std::size(rows), false) == "Open the Toy Box");
        CHECK(commands::SlotLabel(rows, std::size(rows) + 3, false).empty());
        LauncherConfig config;
        CHECK(commands::SetKey(config, "game", rows, 2, "T") == -1);
    }
    namespace fs = std::filesystem;
    fs::path scratch = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ArkeyanCommandsTest";
    fs::create_directories(scratch);
    const std::string path = (scratch / "launcher.ini").string();
    {
        // A file as an older launcher left it: shared keys in [input], none of the game's own yet.
        std::ofstream file(path, std::ios::binary);
        file << "[input]\ndevice=auto\nkey_a=Z\nkey_lb=Q\nkey_dpad_up=Up\n[game.video]\nfps_limit=60\n";
    }
    const std::string section = "game.input";

    // A key: the game's copy is seeded from [input] once (seeded=1), then gets the key; [input] gets the game's keys.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        CHECK(commands::Key(config, "game", Rows[RowA]) == "Space"); // not seeded yet: the default
        commands::Seed(config, "game");
        CHECK(commands::Key(config, "game", Rows[RowA]) == "Z"); // seeded: the player's shared key
        CHECK(commands::SetKey(config, "game", Rows, RowB, "K") == -1);
        CHECK(SaveLauncherConfig(path, config));
    }
    {
        LauncherConfig config = LoadLauncherConfig(path);
        CHECK(Value(config, section, "seeded") == "1");
        CHECK(Value(config, section, "key_b") == "K");
        CHECK(Value(config, section, "key_a") == "Z");
        CHECK(Value(config, "input", "key_b") == "K");
        CHECK(Value(config, "input", "key_a") == "Z");
        CHECK(Value(config, "input", "key_x") == "C"); // a row of the game without a key: its default
        CHECK(Value(config, "input", "key_lb") == "Q"); // seeded into the game's copy, so kept
        CHECK(Value(config, "input", "key_right_up") == "<none>"); // not the game's: the runtime's default
        CHECK(Value(config, "input", "device") == "auto");
        CHECK(Value(config, "game.video", "fps_limit") == "60"); // the other sections stay
        std::ifstream file(path, std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        CHECK(text.str().find("[game.input]") != std::string::npos);
        CHECK(text.str().find("key_b=K") != std::string::npos);
        CHECK(SameAsParse(config));

        // Seeded once: a later [input] change is not copied again.
        config["input"]["key_x"] = "M";
        commands::Seed(config, "game");
        CHECK(Value(config, section, "key_x") == "<none>");
    }

    // A key another row has: the two rows swap.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        CHECK(commands::SetKey(config, "game", Rows, RowX, "z") == int(RowA)); // the same key, whatever its case
        CHECK(Value(config, section, "key_x") == "z");
        CHECK(Value(config, section, "key_a") == "C"); // x's old key
        CHECK(commands::Key(config, "game", Rows[RowA]) == "C");
        CHECK(SameAsParse(config));
        CHECK(SaveLauncherConfig(path, config));
    }

    // Pad: the physical B does A's job; A, which did it, takes B's old job.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        const int a = PadButtonIndex("a"), b = PadButtonIndex("b"), rb = PadButtonIndex("rb");
        CHECK(commands::SetPad(config, "game", Rows, a, b) == a);
        CHECK(Value(config, section, "pad_b") == "a");
        CHECK(Value(config, section, "pad_a") == "b");
        CHECK(commands::PadTarget(config, "game", b) == a);
        CHECK(commands::PadTarget(config, "game", a) == b);
        CHECK(SameAsParse(config));
        // The physical RB does B's job (which A does now): A takes RB's.
        CHECK(commands::SetPad(config, "game", Rows, b, rb) == a);
        CHECK(Value(config, section, "pad_rb") == "b");
        CHECK(Value(config, section, "pad_a") == "rb");
        // Back: A does its own job again, so its line goes; B did it and takes A's last one (RB's).
        CHECK(commands::SetPad(config, "game", Rows, a, a) == b);
        CHECK(Value(config, section, "pad_a") == "<none>");
        CHECK(Value(config, section, "pad_b") == "rb");
        // The same job again changes nothing.
        CHECK(commands::SetPad(config, "game", Rows, a, a) == -1);
        // A "none" from the launcher: that button does nothing; giving it a job leaves the old holder without one.
        config[section]["pad_y"] = "none";
        CHECK(commands::PadTarget(config, "game", PadButtonIndex("y")) == -1);
        CHECK(SameAsParse(config));
        CHECK(SaveLauncherConfig(path, config));
    }

    // The mouse: off by default; on, the game's defaults; a button moves to another action, which loses it.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        mouse::Button now[mouse::ActionCount];
        CHECK(!commands::MouseOn(config, "game"));
        mouse::Effective(commands::MouseLines(config, "game"), MouseDefaults, now);
        CHECK(!mouse::AnyMapped(now));
        commands::SetMouseOn(config, "game", Rows, true);
        CHECK(Value(config, section, "mouse") == "1");
        mouse::Effective(commands::MouseLines(config, "game"), MouseDefaults, now);
        CHECK(now[KeyIndex("x")] == mouse::Left && now[KeyIndex("b")] == mouse::Right);
        // The left button for A: X (its default) loses it.
        CHECK(
            commands::SetMouse(config, "game", Rows, MouseDefaults, KeyIndex("a"), mouse::Left) == int(KeyIndex("x")));
        CHECK(Value(config, section, "mouse_a") == "Mouse Left");
        CHECK(Value(config, section, "mouse_x") == "none");
        mouse::Effective(commands::MouseLines(config, "game"), MouseDefaults, now);
        CHECK(now[KeyIndex("a")] == mouse::Left && now[KeyIndex("x")] == mouse::None &&
              now[KeyIndex("b")] == mouse::Right);
        // The wheel for a stick direction; nobody had it.
        CHECK(commands::SetMouse(config, "game", Rows, MouseDefaults, KeyIndex("left_up"), mouse::WheelUp) == -1);
        CHECK(Value(config, section, "mouse_left_up") == "Wheel Up");
        CHECK(SameAsParse(config));
        // Off again: the lines stay, nothing is mapped.
        commands::SetMouseOn(config, "game", Rows, false);
        mouse::Effective(commands::MouseLines(config, "game"), MouseDefaults, now);
        CHECK(!mouse::AnyMapped(now));
        CHECK(Value(config, section, "mouse_a") == "Mouse Left");
        commands::SetMouseOn(config, "game", Rows, true);
        CHECK(SaveLauncherConfig(path, config));
    }

    // The Toy Box's keys: F6 (Tab too), F7, F8 until set; in [input]; swapped with a row of the game.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        CHECK(commands::ToyboxKey(config, 0) == "F6" && commands::ToyboxKey(config, 2) == "F8");
        CHECK(commands::ToyboxTab(config));
        CHECK(commands::SetKey(config, "game", Rows, SlotToyboxAdd, "G") == -1);
        CHECK(Value(config, "input", "key_toybox_add") == "G");
        CHECK(commands::ToyboxTab(config)); // only key_toybox takes Tab's place
        // A row takes the Toy Box's key: the Toy Box gets the row's.
        const std::string xKey = commands::Key(config, "game", Rows[RowX]);
        CHECK(commands::SetKey(config, "game", Rows, RowX, "F6") == int(SlotToybox));
        CHECK(Value(config, "input", "key_toybox") == xKey);
        CHECK(!commands::ToyboxTab(config));
        CHECK(SameAsParse(config));
        commands::Bindings bindings = commands::Read(config, "game");
        CHECK(bindings.toyboxKeys[0] == xKey && bindings.toyboxKeys[1] == "G" && bindings.toyboxKeys[2] == "F8");
        CHECK(!bindings.toyboxTab);
        CHECK(SaveLauncherConfig(path, config));
    }

    // Reset: the game's key_, pad_ and mouse_ lines and the Toy Box's keys go, seeded and the switch stay, [input] gets the
    // game's defaults.
    {
        LauncherConfig config = LoadLauncherConfig(path);
        commands::Reset(config, "game", Rows);
        CHECK(SaveLauncherConfig(path, config));
        config = LoadLauncherConfig(path);
        CHECK(Value(config, section, "seeded") == "1");
        for (const auto& [key, value] : config[section])
            CHECK(!key.starts_with("key_") && !key.starts_with("pad_") && !key.starts_with("mouse_"));
        CHECK(Value(config, section, "mouse") == "1");
        CHECK(Value(config, "input", "key_toybox") == "<none>" && Value(config, "input", "key_toybox_add") == "<none>");
        CHECK(commands::ToyboxTab(config));
        CHECK(Value(config, "input", "key_a") == "Space");
        CHECK(Value(config, "input", "key_x") == "C");
        CHECK(Value(config, "input", "key_lb") == "<none>"); // not a row of the game
        CHECK(commands::Key(config, "game", Rows[RowB]) == "X");
        CHECK(SameAsParse(config));
        commands::Bindings bindings = commands::Read(config, "game");
        CHECK(bindings.keys[KeyIndex("a")] == "Space");
        CHECK(bindings.padMap.Identity());
    }

    // The running game takes what the page saved, once.
    {
        RuntimeSettings settings;
        CHECK(!commands::Take(settings));
        LauncherConfig config = LoadLauncherConfig(path);
        commands::SetKey(config, "game", Rows, RowB, "K");
        commands::SetPad(config, "game", Rows, PadButtonIndex("a"), PadButtonIndex("b"));
        commands::Publish(commands::Read(config, "game"));
        CHECK(commands::Take(settings));
        CHECK(settings.keys[KeyIndex("b")] == "K");
        CHECK(settings.padMap.target[PadButtonIndex("b")] == PadButtonIndex("a"));
        CHECK(settings.mouse.enabled); // the switch Reset kept
        CHECK(settings.toyboxKeys[0] == "F6" && settings.toyboxTab);
        CHECK(!commands::Take(settings));
        commands::SetMouse(config, "game", Rows, MouseDefaults, KeyIndex("y"), mouse::X1);
        commands::SetKey(config, "game", Rows, SlotToybox, "T");
        commands::Publish(commands::Read(config, "game"));
        CHECK(commands::Take(settings));
        CHECK(settings.mouse.set[KeyIndex("y")] && settings.mouse.stored[KeyIndex("y")] == mouse::X1);
        CHECK(settings.toyboxKeys[0] == "T" && !settings.toyboxTab);
    }

    std::error_code ignored;
    fs::remove(path, ignored);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
