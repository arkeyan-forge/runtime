// Checks the settings parser: launcher.ini, SKY_* variables (and the older SSA_* names) and the command line, and their precedence.
// usage: SSASettingsTest
#include "host/env.h"
#include "host/debug_screen.h"
#include "host/gamemenu.h"
#include "host/pad_map.h"
#include "host/settings.h"
#include "host/settings_fallback.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

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

    struct Run
    {
        RuntimeSettings settings;
        std::vector<std::string> warnings;
        std::string error;
        bool ok;
    };

    Run Parse(
        const LauncherConfig& config, const std::map<std::string, std::string>& env, std::vector<std::string> args)
    {
        Run run;
        std::vector<char*> argv{ const_cast<char*>("SSARecomp") };
        for (std::string& arg : args)
            argv.push_back(arg.data());
        auto getEnv = [&](const char* name) -> const char* {
            auto it = env.find(name);
            return it != env.end() ? it->second.c_str() : nullptr;
        };
        run.ok =
            ParseSettings(config, "ssa", getEnv, int(argv.size()), argv.data(), run.settings, run.warnings, run.error);
        return run;
    }

    size_t KeyIndex(const char* action)
    {
        for (size_t i = 0; i < std::size(g_keyActions); i++)
            if (std::string_view(g_keyActions[i].name) == action)
                return i;
        return 0;
    }
}

int main()
{
    // The helper on the real environment: the current name, then SSA_*, and for the launcher SKYLAUNCHER_*.
    {
        auto put = [](const char* name, const char* value) {
#if defined(_WIN32)
            _putenv_s(name, value);
#else
            setenv(name, value, 1);
#endif
        };
        CHECK(skyenv::Get("SKY_ENVTEST_A") == nullptr);
        put("SSA_ENVTEST_A", "old");
        CHECK(std::string(skyenv::Get("SKY_ENVTEST_A")) == "old");
        put("SKY_ENVTEST_A", "new");
        CHECK(std::string(skyenv::Get("SKY_ENVTEST_A")) == "new");
        put("SKYLAUNCHER_ENVTEST_B", "launcher");
        CHECK(std::string(skyenv::Get("SKY_LAUNCHER_ENVTEST_B")) == "launcher");
        put("SSA_LAUNCHER_ENVTEST_C", "ini");
        CHECK(std::string(skyenv::Get("SKY_LAUNCHER_ENVTEST_C")) == "ini");
        // Set, even empty, wins; through a table, as Windows has no empty variable (_putenv_s(name, "") removes it).
        auto table = [](const char* name) -> const char* {
            std::string_view variable = name;
            return variable == "SKY_ENVTEST_D" ? "" : variable == "SSA_ENVTEST_D" ? "old" : nullptr;
        };
        CHECK(std::string(skyenv::Get("SKY_ENVTEST_D", table)).empty());
        CHECK(std::string(skyenv::Get("PATH")) == std::string(std::getenv("PATH"))); // other names are plain
    }

    // Defaults.
    Run run = Parse({}, {}, {});
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.resolutionScale == 0 && run.settings.vsync && !run.settings.fullscreen); // 0: auto
    CHECK(run.settings.anisotropy == 1 && run.settings.fpsLimit == 30 && run.settings.inputDevice == "auto");
    CHECK(!run.settings.skipIntroMovies);
    CHECK(!SkipIntroMovies());
    CHECK(run.settings.fpsAutoFallback && FpsAutoFallback());
    for (const char* game : { "ssa", "giants", "swapforce", "trapteam" })
    {
        RuntimeSettings settings;
        std::vector<std::string> warnings;
        std::string error;
        char program[] = "runtime";
        char* argv[] = { program };
        LauncherConfig fallbackConfig{ { "video", { { "fps_auto_fallback", "1" } } },
            { std::string(game) + ".video", { { "fps_auto_fallback", "0" }, { "fps_limit", "60" } } } };
        CHECK(ParseSettings(
            fallbackConfig, game, [](const char*) -> const char* { return nullptr; }, 1, argv, settings, warnings,
            error));
        CHECK(!settings.fpsAutoFallback && !FpsAutoFallback() && settings.fpsLimit == 60 && warnings.empty());
    }
    run = Parse(
        { { "video", { { "fps_auto_fallback", "0" } } }, { "ssa.video", { { "fps_auto_fallback", "1" } } } }, {}, {});
    CHECK(run.settings.fpsAutoFallback && FpsAutoFallback());
    run = Parse({ { "ssa.video", { { "fps_auto_fallback", "invalid" } } } }, {}, {});
    CHECK(run.settings.fpsAutoFallback && run.warnings.size() == 1);
    CHECK(DescribeSettings(run.settings).find("fps auto fallback on") != std::string::npos);

    // Twenty misses in the rolling 120-frame window, recovery, short returns and disabling while active.
    {
        using Change = FrameRateFallback::Change;
        FrameRateFallback startup;
        for (int i = 0; i < 19; i++)
            CHECK(startup.Sample(true, true) == Change::None);
        CHECK(startup.Sample(true, true) == Change::Halved && startup.fitsToRestore == 600);
        FrameRateFallback fallback;
        for (int i = 0; i < 600; i++)
            CHECK(fallback.Sample(true, false) == Change::None);
        // A burst of 19 slow frames (a load, a camera cut) keeps 60.
        for (int i = 0; i < 19; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
        for (int i = 0; i < 120; i++)
            CHECK(fallback.Sample(true, false) == Change::None);
        // The burst has left the window; 20 misses spread over it halve.
        for (int i = 0; i < 10; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
        for (int i = 0; i < 50; i++)
            CHECK(fallback.Sample(true, false) == Change::None);
        for (int i = 0; i < 9; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
        CHECK(fallback.Sample(true, true) == Change::Halved && fallback.active);
        CHECK(fallback.fitsToRestore == 300);
        // 30 not held (17 late frames in 300, more than 1 in 20): the count starts again.
        for (int i = 0; i < 300; i++)
            CHECK(fallback.Sample(true, i % 18 == 0) == Change::None);
        // A late frame now and then (15 in 300, 1 in 20) does not hold 60 back.
        for (int i = 0; i < 299; i++)
            CHECK(fallback.Sample(true, i % 20 == 19) == Change::None);
        CHECK(fallback.Sample(true, true) == Change::Restored && !fallback.active);
        for (int cycle = 0; cycle < 6; cycle++)
        {
            for (int i = 0; i < 19; i++)
                fallback.Sample(true, true);
            CHECK(fallback.Sample(true, true) == Change::Halved);
            CHECK(fallback.fitsToRestore == std::min(300u << (cycle + 1), 1800u));
            for (uint32_t i = 1; i < fallback.fitsToRestore; i++)
                CHECK(fallback.Sample(true, false) == Change::None);
            CHECK(fallback.Sample(true, false) == Change::Restored);
        }
        for (int i = 0; i < 600; i++)
            fallback.Sample(true, false);
        for (int i = 0; i < 19; i++)
            fallback.Sample(true, true);
        CHECK(fallback.Sample(true, true) == Change::Halved && fallback.fitsToRestore == 300);
        CHECK(fallback.Sample(false, true) == Change::None && !fallback.active);
        for (int i = 0; i < 100; i++)
            CHECK(fallback.Sample(false, true) == Change::None && !fallback.active);
        CHECK(fallback.Sample(true, true) == Change::None); // re-enabling starts fresh
    }
    // A load's frames (a file opened in the last 5 s) are not counted; after it the window starts empty, and a game at
    // 30 is back at 60 at once.
    {
        using Change = FrameRateFallback::Change;
        FrameRateFallback fallback;
        for (int i = 0; i < 600; i++)
            fallback.Sample(true, false);
        for (int i = 0; i < 19; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
        for (int i = 0; i < 200; i++)
            CHECK(fallback.Sample(true, true, true) == Change::None && !fallback.active);
        for (int i = 0; i < 19; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
        CHECK(fallback.Sample(true, true) == Change::Halved);
        for (int i = 0; i < 50; i++)
            CHECK(fallback.Sample(true, false, true) == Change::None && fallback.active);
        CHECK(fallback.Sample(true, false) == Change::Restored && !fallback.active && fallback.fitsToRestore == 300);
        for (int i = 0; i < 19; i++)
            CHECK(fallback.Sample(true, true) == Change::None);
    }
    CHECK(run.settings.glyphs == "auto");
    CHECK(run.settings.gamepad.empty() && run.settings.language.empty() && run.settings.arguments.empty());
    CHECK(run.settings.keys[KeyIndex("a")] == "Space" && run.settings.keys[KeyIndex("dpad_right")] == "Right");

    // launcher.ini alone.
    LauncherConfig config;
    config["video"] = { { "resolution_scale", "3" }, { "vsync", "0" }, { "fullscreen", "1" }, { "anisotropy", "8" },
        { "fps_limit", "30" }, { "skip_intro_movies", "1" }, { "unknown_key", "kept" } };
    config["input"] = { { "device", "keyboard" }, { "gamepad", "0300abcd" }, { "key_a", "Return" },
        { "key_left_up", "Up" }, { "glyphs", "nintendo" } };
    config["ssa"] = { { "language", "fr" }, { "voice_language", "de" }, { "iso", "/a.iso" } };
    run = Parse(config, {}, {});
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.resolutionScale == 3 && !run.settings.vsync && run.settings.fullscreen &&
          run.settings.anisotropy == 8);
    CHECK(run.settings.skipIntroMovies);
    CHECK(run.settings.inputDevice == "keyboard" && run.settings.gamepad == "0300abcd" &&
          run.settings.glyphs == "nintendo");
    CHECK(SkipIntroMovies());
    CHECK(run.settings.keys[KeyIndex("a")] == "Return" && run.settings.keys[KeyIndex("left_up")] == "Up");
    CHECK(run.settings.keys[KeyIndex("b")] == "X");
    CHECK(run.settings.language == "fr" && run.settings.voiceLanguage == "de");

    // SKY_* variables over the file, the command line over both. The older SSA_* names still work, and SKY_* wins.
    run = Parse(config, { { "SSA_LANGUAGE", "it" }, { "SSA_VOICE_LANGUAGE", "es" } }, {});
    CHECK(run.settings.language == "it" && run.settings.voiceLanguage == "es");
    run = Parse(config, { { "SSA_LANGUAGE", "it" }, { "SKY_LANGUAGE", "pt" } }, {});
    CHECK(run.settings.language == "pt");
    run = Parse(config, { { "SKY_LANGUAGE", "it" }, { "SKY_VOICE_LANGUAGE", "es" } }, {});
    CHECK(run.settings.language == "it" && run.settings.voiceLanguage == "es");
    run = Parse(config, { { "SKY_LANGUAGE", "it" } },
        { "--language", "en", "--scale", "2", "--vsync=1", "--windowed", "--anisotropy", "16", "--input", "gamepad",
            "--gamepad", "ffff", "--key", "a=Space", "--glyphs=playstation", "game.iso", "--select-iso" });
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.language == "en" && run.settings.voiceLanguage == "de");
    CHECK(run.settings.resolutionScale == 2 && run.settings.vsync && !run.settings.fullscreen &&
          run.settings.anisotropy == 16);
    CHECK(run.settings.inputDevice == "gamepad" && run.settings.gamepad == "ffff" &&
          run.settings.keys[KeyIndex("a")] == "Space");
    CHECK(run.settings.glyphs == "playstation");
    CHECK(run.settings.arguments == (std::vector<std::string>{ "game.iso", "--select-iso" }));

    // The last of repeated options wins.
    run = Parse({}, {}, { "--windowed", "--fullscreen", "--scale", "4", "--scale", "5" });
    CHECK(run.settings.fullscreen && run.settings.resolutionScale == 5);

    // Bad values warn and keep the previous source's value.
    config["video"] = { { "resolution_scale", "11" }, { "anisotropy", "3" }, { "vsync", "yes" } };
    config["input"] = { { "device", "mouse" }, { "key_a", "NoSuchKey" }, { "key_jump", "Space" },
        { "glyphs", "sega" } };
    run = Parse(config, {}, { "--scale", "0" });
    CHECK(run.ok && run.warnings.size() == 8 && run.settings.glyphs == "auto");
    CHECK(run.settings.resolutionScale == 0 && run.settings.anisotropy == 1 && run.settings.vsync);
    CHECK(run.settings.inputDevice == "auto" && run.settings.keys[KeyIndex("a")] == "Space");
    run = Parse({ { "video", { { "resolution_scale", "6" } } } }, {}, { "--scale", "abc" });
    CHECK(run.settings.resolutionScale == 6 && run.warnings.size() == 1);
    // "auto" (the default) from either source.
    run = Parse({ { "video", { { "resolution_scale", "3" } } } }, {}, { "--scale", "auto" });
    CHECK(run.ok && run.warnings.empty() && run.settings.resolutionScale == 0);
    run = Parse({ { "video", { { "resolution_scale", "auto" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.resolutionScale == 0);
    run = Parse({ { "video", { { "skip_intro_movies", "yes" } } } }, {}, {});
    CHECK(run.ok && !run.settings.skipIntroMovies && run.warnings.size() == 1);

    // original_resolution: off by default; [video], then the game's [<game>.video], then the command line.
    CHECK(!Parse({}, {}, {}).settings.originalResolution);
    run = Parse({ { "video", { { "original_resolution", "1" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.originalResolution);
    run =
        Parse({ { "video", { { "original_resolution", "1" } } }, { "ssa.video", { { "original_resolution", "0" } } } },
            {}, {});
    CHECK(run.ok && !run.settings.originalResolution);
    run = Parse({ { "ssa.video", { { "original_resolution", "1" } } } }, {}, { "--original-resolution", "0" });
    CHECK(run.ok && !run.settings.originalResolution);
    run = Parse({}, {}, { "--original-resolution=1" });
    CHECK(run.ok && run.settings.originalResolution &&
          DescribeSettings(run.settings).find("original resolution") != std::string::npos);
    run =
        Parse({ { "swapforce.video", { { "original_resolution", "1" } } } }, {}, {}); // another game's section: ignored
    CHECK(run.ok && !run.settings.originalResolution);

    // edge_smoothing: not chosen by default (auto); [video], then the game's [<game>.video], then the command line.
    CHECK(Parse({}, {}, {}).settings.edgeSmoothing == edgesmoothing::Auto);
    run = Parse({ { "video", { { "edge_smoothing", "off" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.edgeSmoothing == edgesmoothing::Off);
    run = Parse(
        { { "video", { { "edge_smoothing", "off" } } }, { "ssa.video", { { "edge_smoothing", "light" } } } }, {}, {});
    CHECK(run.ok && run.settings.edgeSmoothing == edgesmoothing::Light);
    run = Parse({ { "ssa.video", { { "edge_smoothing", "off" } } } }, {}, { "--edge-smoothing", "game" });
    CHECK(run.ok && run.settings.edgeSmoothing == edgesmoothing::Game);
    run = Parse({ { "swapforce.video", { { "edge_smoothing", "off" } } } }, {}, {}); // another game's section: ignored
    CHECK(run.ok && run.settings.edgeSmoothing == edgesmoothing::Auto);
    run = Parse({}, {}, { "--edge-smoothing=bogus" });
    CHECK(run.ok && run.warnings.size() == 1 && run.settings.edgeSmoothing == edgesmoothing::Auto);
    run = Parse({ { "video", { { "edge_smoothing", "auto" } } } }, {}, { "--edge-smoothing=light" });
    CHECK(run.ok && DescribeSettings(run.settings).find("edge smoothing light") != std::string::npos);
    CHECK(DescribeSettings(Parse({}, {}, {}).settings).find("edge smoothing") == std::string::npos);
    // Not chosen: light from resolution scale 2, game at 1; a choice stays whatever the scale.
    CHECK(edgesmoothing::Resolve(edgesmoothing::Auto, 1, false) == edgesmoothing::Game);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Auto, 2, false) == edgesmoothing::Light);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Auto, 4, false) == edgesmoothing::Light);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Game, 4, false) == edgesmoothing::Game);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Light, 1, false) == edgesmoothing::Light);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Off, 2, false) == edgesmoothing::Off);
    // The title and the menus keep the game's pass at any scale unless a mode was chosen (Trap Team's logo).
    CHECK(edgesmoothing::Resolve(edgesmoothing::Auto, 2, true) == edgesmoothing::Game);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Auto, 1, true) == edgesmoothing::Game);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Light, 2, true) == edgesmoothing::Light);
    CHECK(edgesmoothing::Resolve(edgesmoothing::Off, 2, true) == edgesmoothing::Off);

    // The game's own [<game>.video] wins over [video] for every video key (a game started without the launcher's PLAY).
    run =
        Parse({ { "video", { { "fps_limit", "30" }, { "skip_intro_movies", "0" } } },
                  { "ssa.video", { { "fps_limit", "60" }, { "skip_intro_movies", "1" }, { "resolution_scale", "3" } } },
                  { "giants.video", { { "fps_limit", "30" }, { "skip_intro_movies", "0" } } } },
            {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.fpsLimit == 60 && run.settings.skipIntroMovies &&
          SkipIntroMovies());
    run = Parse({ { "ssa.video", { { "fps_limit", "60" } } } }, {}, { "--fps-limit", "30" });
    CHECK(run.ok && run.settings.fpsLimit == 30);
    // The F3 menu's keys: the volume ([audio], then the game's own [<game>.audio]) and the FPS counter ([<game>.video] show_fps).
    run = Parse({}, {}, {});
    CHECK(run.ok && run.settings.volume == 100 && !run.settings.showFps);
    run = Parse({ { "audio", { { "volume", "40" } } }, { "ssa.audio", { { "volume", "90" } } },
                    { "giants.audio", { { "volume", "10" } } }, { "ssa.video", { { "show_fps", "1" } } } },
        {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.volume == 90 && run.settings.showFps &&
          DescribeSettings(run.settings).find("fps counter") != std::string::npos &&
          DescribeSettings(run.settings).find("volume 90") != std::string::npos);
    run = Parse({ { "audio", { { "volume", "150" } } } }, {}, {});
    CHECK(run.ok && run.warnings.size() == 1 && run.settings.volume == 100);
    // portal_sound: on by default; [audio], then the game's own [<game>.audio]; a bad value warns and keeps it on.
    CHECK(Parse({}, {}, {}).settings.portalSound);
    run = Parse({ { "audio", { { "portal_sound", "1" } } }, { "ssa.audio", { { "portal_sound", "0" } } },
                    { "giants.audio", { { "portal_sound", "1" } } } },
        {}, {});
    CHECK(run.ok && run.warnings.empty() && !run.settings.portalSound &&
          DescribeSettings(run.settings).find("portal sound off") != std::string::npos);
    run = Parse({ { "ssa.audio", { { "portal_sound", "yes" } } } }, {}, {});
    CHECK(run.ok && run.warnings.size() == 1 && run.settings.portalSound);
    // reduce_motion: off by default; [video] or the game's own [<game>.video]; a bad value warns and keeps the default.
    run = Parse({}, {}, {});
    CHECK(!run.settings.reduceMotion && DescribeSettings(run.settings).find("reduced motion") == std::string::npos);
    run = Parse({ { "ssa.video", { { "reduce_motion", "1" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.reduceMotion &&
          DescribeSettings(run.settings).find("reduced motion") != std::string::npos);
    run = Parse({ { "video", { { "reduce_motion", "maybe" } } } }, {}, {});
    CHECK(run.ok && run.warnings.size() == 1 && !run.settings.reduceMotion);
    // Keys: [input], then the game's own [<game>.input] (another game's section is ignored).
    run = Parse({ { "input", { { "key_y", "G" } } }, { "ssa.input", { { "key_y", "H" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.keys[KeyIndex("y")] == "H" &&
          run.settings.keys[KeyIndex("x")] == "C");
    run = Parse({ { "input", { { "key_y", "G" } } }, { "giants.input", { { "key_y", "H" } } } }, {}, {});
    CHECK(run.ok && run.settings.keys[KeyIndex("y")] == "G");

    // aspect: auto by default; 16:9 from the file or the command line (the last wins); anything else warns.
    run = Parse({}, {}, {});
    CHECK(run.settings.aspectAuto);
    run = Parse({ { "video", { { "aspect", "16:9" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && !run.settings.aspectAuto);
    run = Parse({ { "video", { { "aspect", "16:9" } } } }, {}, { "--aspect", "auto" });
    CHECK(run.ok && run.warnings.empty() && run.settings.aspectAuto);
    run = Parse({}, {}, { "--aspect=16:9" });
    CHECK(run.ok && run.warnings.empty() && !run.settings.aspectAuto);
    run = Parse({ { "video", { { "aspect", "21:9" } } } }, {}, {});
    CHECK(run.ok && run.warnings.size() == 1 && run.settings.aspectAuto);

    // Texture packs: packs on and dump off by default; launcher.ini < variable < command line.
    run = Parse({}, {}, {});
    CHECK(!run.settings.textureDump && run.settings.texturePacks);
    run = Parse({ { "video", { { "texture_dump", "1" }, { "texture_packs", "0" } } } }, {}, {});
    CHECK(run.settings.textureDump && !run.settings.texturePacks && run.warnings.empty());
    run = Parse({ { "video", { { "texture_dump", "1" } } } },
        { { "SKY_TEXTURE_DUMP", "0" }, { "SKY_TEXTURE_PACKS", "0" } }, {});
    CHECK(!run.settings.textureDump && !run.settings.texturePacks);
    run = Parse({}, { { "SKY_TEXTURE_PACKS", "0" } }, { "--texture-packs", "1", "--texture-dump=1" });
    CHECK(run.settings.textureDump && run.settings.texturePacks);
    run = Parse({}, { { "SKY_TEXTURE_DUMP", "yes" } }, {});
    CHECK(!run.settings.textureDump && run.warnings.size() == 1);

    // fps_limit: 30, 60, 120, 144, 240 or 0 (no limit); another value is lowered to the rate below it, with a warning.
    run = Parse({ { "video", { { "fps_limit", "60" } } } }, {}, {});
    CHECK(run.settings.fpsLimit == 60 && run.warnings.empty());
    for (const char* rate : { "30", "120", "144", "240", "0" })
    {
        run = Parse({}, {}, { "--fps-limit", rate });
        CHECK(run.settings.fpsLimit == uint32_t(atoi(rate)) && run.warnings.empty());
    }
    run = Parse({ { "video", { { "fps_limit", "display" } } } }, {}, {});
    CHECK(run.settings.fpsLimit == 60 && run.warnings.empty());
    run = Parse({}, {}, { "--fps-limit", "165" });
    CHECK(run.settings.fpsLimit == 144 && run.warnings.size() == 1);
    run = Parse({}, {}, { "--fps-limit", "1000" });
    CHECK(run.settings.fpsLimit == 240 && run.warnings.size() == 1);
    run = Parse({}, {}, { "--fps-limit", "100" });
    CHECK(run.settings.fpsLimit == 60 && run.warnings.size() == 1);
    run = Parse({}, {}, { "--fps-limit", "45" });
    CHECK(run.settings.fpsLimit == 30 && run.warnings.size() == 1);

    // The F3 menu keeps a limit above 60 and lowers none by opening it: every rate stands for itself.
    for (uint32_t rate : gamemenu::FrameRates)
        CHECK(gamemenu::NormalFrameRate(rate) == rate);
    CHECK(gamemenu::NormalFrameRate(165) == 144 && gamemenu::NormalFrameRate(45) == 30 &&
          gamemenu::NormalFrameRate(1000) == 240);

    // A game's cap (Giants 60): higher rates and no limit are lowered to it, lower ones stay, 0 is no cap.
    CHECK(gamemenu::CapFrameRate(120, 60) == 60 && gamemenu::CapFrameRate(0, 60) == 60 &&
          gamemenu::CapFrameRate(30, 60) == 30);
    CHECK(gamemenu::CapFrameRate(60, 60) == 60 && gamemenu::CapFrameRate(0, 0) == 0 &&
          gamemenu::CapFrameRate(240, 0) == 240);

    // [portal] toybox_auto_open: on by default, launcher.ini turns it off, SKY_TOYBOX_AUTO_OPEN overrides, bad values warn.
    CHECK(Parse({}, {}, {}).settings.toyboxAutoOpen);
    LauncherConfig portal;
    portal["portal"] = { { "mode", "auto" }, { "toybox_auto_open", "0" } };
    run = Parse(portal, {}, {});
    CHECK(run.ok && run.warnings.empty() && !run.settings.toyboxAutoOpen);
    CHECK(DescribeSettings(run.settings).find("toybox auto-open off") != std::string::npos);
    CHECK(DescribeSettings(Parse({}, {}, {}).settings).find("auto-open") == std::string::npos);
    run = Parse(portal, { { "SKY_TOYBOX_AUTO_OPEN", "1" } }, {});
    CHECK(run.warnings.empty() && run.settings.toyboxAutoOpen);
    portal["portal"]["toybox_auto_open"] = "maybe";
    run = Parse(portal, {}, {});
    CHECK(run.settings.toyboxAutoOpen && run.warnings.size() == 1);

    // Pad remapping and rumble: [input] then the game's [ssa.input]; a bad value or button warns; --pad and --rumble.
    CHECK(Parse({}, {}, {}).settings.padMap.Identity() && Parse({}, {}, {}).settings.rumble[0] == "normal");
    LauncherConfig pads;
    pads["input"] = { { "pad_rb", "b" }, { "rumble_p1", "low" } };
    pads["ssa.input"] = { { "pad_rb", "x" }, { "pad_lt", "none" }, { "pad_a", "dpad_up" }, { "rumble_p2", "strong" } };
    pads["giants.input"] = { { "pad_y", "a" } };
    run = Parse(pads, {}, {});
    CHECK(run.ok && run.warnings.empty() && !run.settings.padMap.Identity());
    CHECK(run.settings.padMap.target[PadButtonIndex("rb")] == PadButtonIndex("x")); // the game's wins over [input]
    CHECK(run.settings.padMap.target[PadButtonIndex("lt")] == -1 &&
          run.settings.padMap.target[PadButtonIndex("a")] == PadButtonIndex("dpad_up"));
    CHECK(run.settings.padMap.target[PadButtonIndex("y")] == PadButtonIndex("y")); // another game's remap is not read
    CHECK(run.settings.rumble[0] == "low" && run.settings.rumble[1] == "strong");
    CHECK(DescribeSettings(run.settings).find("rb->x") != std::string::npos);
    LauncherConfig badPads;
    badPads["ssa.input"] = { { "pad_rb", "banana" }, { "pad_nothing", "a" }, { "rumble_p1", "huge" } };
    run = Parse(badPads, {}, {});
    CHECK(run.warnings.size() == 3 && run.settings.padMap.Identity() && run.settings.rumble[0] == "normal");
    run = Parse({}, {}, { "--pad", "lb=a", "--rumble", "off" });
    CHECK(run.ok && run.settings.padMap.target[PadButtonIndex("lb")] == PadButtonIndex("a") &&
          run.settings.rumble[1] == "off");
    CHECK(!Parse({}, {}, { "--pad", "lb" }).ok);

    // The mouse (host/mouse.h): off by default, a file without its lines reads as before; the switch and mouse_ lines in the
    // game's section only; a bad name warns. The Toy Box keys in [input]; Tab opens it too until key_toybox is set.
    {
        Run plain = Parse(pads, {}, {});
        CHECK(!plain.settings.mouse.enabled && plain.settings.toyboxKeys[0] == "F6" &&
              plain.settings.toyboxKeys[1] == "F7" && plain.settings.toyboxKeys[2] == "F8" && plain.settings.toyboxTab);
        bool none = true;
        for (bool set : plain.settings.mouse.set)
            none = none && !set;
        CHECK(none);
        LauncherConfig mice;
        mice["input"] = { { "mouse", "1" }, { "key_toybox", "O" }, { "key_toybox_clear", "P" } };
        mice["ssa.input"] = { { "mouse", "1" }, { "mouse_y", "Mouse Middle" }, { "mouse_a", "none" },
            { "mouse_b", "Wheel Up" } };
        mice["giants.input"] = { { "mouse_x", "Mouse 4" } };
        run = Parse(mice, {}, {});
        CHECK(run.ok && run.warnings.empty() && run.settings.mouse.enabled);
        CHECK(run.settings.mouse.set[KeyIndex("y")] && run.settings.mouse.stored[KeyIndex("y")] == mouse::Middle);
        CHECK(run.settings.mouse.set[KeyIndex("a")] && run.settings.mouse.stored[KeyIndex("a")] == mouse::None);
        CHECK(run.settings.mouse.stored[KeyIndex("b")] == mouse::WheelUp && !run.settings.mouse.set[KeyIndex("x")]);
        CHECK(run.settings.toyboxKeys[0] == "O" && !run.settings.toyboxTab && run.settings.toyboxKeys[1] == "F7" &&
              run.settings.toyboxKeys[2] == "P");
        LauncherConfig onlyShared;
        onlyShared["input"] = { { "mouse", "1" } }; // the switch is per game: [input] does not turn it on
        run = Parse(onlyShared, {}, {});
        CHECK(!run.settings.mouse.enabled);
        LauncherConfig badMice;
        badMice["ssa.input"] = { { "mouse", "maybe" }, { "mouse_y", "Mouse 9" }, { "mouse_nothing", "Mouse Left" } };
        badMice["input"] = { { "key_toybox", "NotAKey" } };
        run = Parse(badMice, {}, {});
        CHECK(run.warnings.size() == 4 && !run.settings.mouse.enabled && run.settings.toyboxKeys[0] == "F6" &&
              run.settings.toyboxTab);
    }

    {
        PadMap map;
        GamepadState pad;
        pad.buttons = Button::RightShoulder | Button::A;
        pad.thumbLX = 1234;
        CHECK(RemapPad(pad, map).buttons == pad.buttons); // nothing remapped: untouched
        map.target[PadButtonIndex("rb")] = int8_t(PadButtonIndex("b"));
        GamepadState out = RemapPad(pad, map);
        CHECK(out.buttons == (Button::B | Button::A) && out.thumbLX == 1234); // RB does B's job and no longer RB's
        map.target[PadButtonIndex("a")] = int8_t(PadButtonIndex("b")); // two buttons on one
        CHECK(RemapPad(pad, map).buttons == Button::B);
        map = PadMap();
        map.target[PadButtonIndex("a")] = int8_t(PadButtonIndex("b")); // a swap
        map.target[PadButtonIndex("b")] = int8_t(PadButtonIndex("a"));
        pad.buttons = Button::A;
        CHECK(RemapPad(pad, map).buttons == Button::B);
        pad.buttons = Button::B;
        CHECK(RemapPad(pad, map).buttons == Button::A);
        pad.buttons = Button::A | Button::B;
        CHECK(RemapPad(pad, map).buttons == (Button::A | Button::B));
        map = PadMap();
        map.target[PadButtonIndex("a")] = -1; // off
        pad.buttons = Button::A | Button::X;
        CHECK(RemapPad(pad, map).buttons == Button::X);
        map =
            PadMap(); // triggers: LT -> A (digital past 30), B -> RT (all the way), RT stays analog under another name
        map.target[PadButtonIndex("lt")] = int8_t(PadButtonIndex("a"));
        map.target[PadButtonIndex("b")] = int8_t(PadButtonIndex("rt"));
        pad = GamepadState();
        pad.leftTrigger = 200;
        pad.buttons = Button::B;
        out = RemapPad(pad, map);
        CHECK(out.buttons == Button::A && out.leftTrigger == 0 && out.rightTrigger == 255);
        pad.leftTrigger = 20; // under the threshold: not pressed
        CHECK(RemapPad(pad, map).buttons == 0);
        map = PadMap();
        map.target[PadButtonIndex("lt")] = int8_t(PadButtonIndex("rt"));
        pad = GamepadState();
        pad.leftTrigger = 90;
        out = RemapPad(pad, map);
        CHECK(out.leftTrigger == 0 && out.rightTrigger == 90);
    }
    CHECK(
        ScaleRumble(40000, "off") == 0 && ScaleRumble(40000, "normal") == 40000 && ScaleRumble(40000, "low") == 16000);
    CHECK(ScaleRumble(20000, "strong") == 32000 && ScaleRumble(60000, "strong") == 65535 &&
          ScaleRumble(40000, "bogus") == 40000);

    // Errors: unknown option, missing value, malformed --key.
    CHECK(!Parse({}, {}, { "--frobnicate" }).ok);
    CHECK(!Parse({}, {}, { "--scale" }).ok);
    CHECK(!Parse({}, {}, { "--key", "Space" }).ok);

    // Flags that end the run.
    CHECK(Parse({}, {}, { "--help" }).settings.help && Parse({}, {}, { "-h" }).settings.help);
    CHECK(Parse({}, {}, { "--version" }).settings.version);
    CHECK(Parse({}, {}, { "--list-gamepads" }).settings.listGamepads);
    CHECK(SettingsHelp("SSARecomp").find("--anisotropy") != std::string::npos);
    CHECK(
        DescribeSettings(Parse({}, {}, { "--scale", "2" }).settings).find("scale 2 (2240x1408)") != std::string::npos);

    // Debug screen: off by default, per-game persistence, strict booleans and isolation.
    CHECK(!Parse({}, {}, {}).settings.debugScreen);
    CHECK(Parse({ { "ssa.video", { { "debug_screen", "1" } } } }, {}, {}).settings.debugScreen);
    CHECK(!Parse({ { "giants.video", { { "debug_screen", "1" } } } }, {}, {}).settings.debugScreen);
    CHECK(!Parse({ { "video", { { "debug_screen", "1" } } }, { "ssa.video", { { "debug_screen", "0" } } } }, {}, {})
            .settings.debugScreen);
    run = Parse({ { "ssa.video", { { "debug_screen", "bad" } } } }, {}, {});
    CHECK(!run.settings.debugScreen && run.warnings.size() == 1);
    using namespace debugscreen;
    Rate rate;
    CHECK(rate.Update(0, 10) && rate.fps == 0 && !rate.ready);
    CHECK(!rate.Update(RefreshNs - 1, 25));
    CHECK(rate.Update(RefreshNs, 25) && rate.fps == 30 && rate.ready);
    CHECK(rate.Update(2 * RefreshNs, 25) && rate.fps == 0);
    CHECK(rate.Update(3 * RefreshNs, 1) && rate.fps == 0); // renderer reset
    CHECK(rate.Update(1, 1) && rate.fps == 0); // clock reset
    CHECK(LowOnePercent({}) == 0);
    std::array<uint64_t, 200> durations;
    durations.fill(10'000'000);
    CHECK(LowOnePercent(durations) == 100);
    durations[0] = 100'000'000;
    durations[1] = 50'000'000;
    CHECK(std::abs(LowOnePercent(durations) - 1000.0 / 75.0) < 0.00001);
    CHECK(LowOnePercent(std::span(durations).first(100)) == 10);
    CHECK(LowOnePercent(std::span(durations).first(99)) == 10);
    CHECK(std::abs(LowOnePercent(std::span(durations).first(101)) - 1000.0 / 75.0) < 0.00001);
    CHECK(durations[0] == 100'000'000 && durations[1] == 50'000'000); // sorting preserves graph order
    durations.fill(0);
    CHECK(LowOnePercent(durations) == 0);
    std::array<uint64_t, MaxFrames + 1> bounded;
    bounded.fill(10'000'000);
    bounded[0] = 1'000'000'000;
    CHECK(LowOnePercent(bounded) == 100); // the old sample lies outside the bounded history

    char text[32];
    Bytes(text, sizeof(text), 3ull << 29);
    CHECK(std::string_view(text) == "1.50 GiB");
    Bytes(text, sizeof(text), 100ull << 20);
    CHECK(std::string_view(text) == "100 MiB");
    Bytes(text, sizeof(text), 0, false);
    CHECK(std::string_view(text) == "n/a");
    Label(text, sizeof(text), "folder/private");
    CHECK(std::string_view(text) == "n/a");
    Label(text, 5, "abcdef");
    CHECK(std::string_view(text) == "abcd");
    Label(text, sizeof(text), "a\nb");
    CHECK(std::string_view(text) == "a b");
    Label(text, 2, "écran");
    CHECK(std::string_view(text).empty());
    Label(text, 3, "écran");
    CHECK(std::string_view(text) == "é");
    Label(text, sizeof(text), "C:\\private\\data");
    CHECK(std::string_view(text) == "n/a");
    Label(text, sizeof(text), "Skylanders: Trap Team");
    CHECK(std::string_view(text) == "Skylanders: Trap Team");
    Label(text, sizeof(text), "@");
    CHECK(std::string_view(text) == "n/a");
    Panel panel;
    panel.Add(Group::Game, "Game", "Example");
    panel.Add(Group::Game, "Version", "1");
    panel.Add(Group::Frame, "FPS", "30");
    CHECK(panel.count == 3 && panel.StartsGroup(0) && !panel.StartsGroup(1) && panel.StartsGroup(2));
    for (int row = 0; row < 50; row++)
        panel.Add(Group::Frame, "FPS", "30");
    CHECK(panel.count == panel.rows.size());

    printf("settings test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
