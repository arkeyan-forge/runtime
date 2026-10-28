// Checks the settings parser: launcher.ini, SKY_* variables (and the older SSA_* names) and the command line, and their precedence.
// usage: SSASettingsTest
#include "host/env.h"
#include "host/settings.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

#define CHECK(condition) \
    do { g_checks++; if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); g_failures++; } } while (0)

    struct Run
    {
        RuntimeSettings settings;
        std::vector<std::string> warnings;
        std::string error;
        bool ok;
    };

    Run Parse(const LauncherConfig& config, const std::map<std::string, std::string>& env, std::vector<std::string> args)
    {
        Run run;
        std::vector<char*> argv{ const_cast<char*>("SSARecomp") };
        for (std::string& arg : args)
            argv.push_back(arg.data());
        auto getEnv = [&](const char* name) -> const char*
        {
            auto it = env.find(name);
            return it != env.end() ? it->second.c_str() : nullptr;
        };
        run.ok = ParseSettings(config, "ssa", getEnv, int(argv.size()), argv.data(), run.settings, run.warnings, run.error);
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
        auto put = [](const char* name, const char* value)
        {
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
        put("SKY_ENVTEST_D", "");
        put("SSA_ENVTEST_D", "old");
        CHECK(std::string(skyenv::Get("SKY_ENVTEST_D")).empty()); // set, even empty, wins
        CHECK(std::string(skyenv::Get("PATH")) == std::string(std::getenv("PATH"))); // other names are plain
    }

    // Defaults.
    Run run = Parse({}, {}, {});
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.resolutionScale == 0 && run.settings.vsync && !run.settings.fullscreen); // 0: auto
    CHECK(run.settings.anisotropy == 1 && run.settings.fpsLimit == 30 && run.settings.inputDevice == "auto");
    CHECK(!run.settings.skipIntroMovies);
    CHECK(!SkipIntroMovies());
    CHECK(run.settings.glyphs == "auto");
    CHECK(run.settings.gamepad.empty() && run.settings.language.empty() && run.settings.arguments.empty());
    CHECK(run.settings.keys[KeyIndex("a")] == "Space" && run.settings.keys[KeyIndex("dpad_right")] == "Right");

    // launcher.ini alone.
    LauncherConfig config;
    config["video"] = { { "resolution_scale", "3" }, { "vsync", "0" }, { "fullscreen", "1" }, { "anisotropy", "8" },
        { "fps_limit", "30" }, { "skip_intro_movies", "1" }, { "unknown_key", "kept" } };
    config["input"] = { { "device", "keyboard" }, { "gamepad", "0300abcd" }, { "key_a", "Return" }, { "key_left_up", "Up" },
        { "glyphs", "nintendo" } };
    config["ssa"] = { { "language", "fr" }, { "voice_language", "de" }, { "iso", "/a.iso" } };
    run = Parse(config, {}, {});
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.resolutionScale == 3 && !run.settings.vsync && run.settings.fullscreen && run.settings.anisotropy == 8);
    CHECK(run.settings.skipIntroMovies);
    CHECK(run.settings.inputDevice == "keyboard" && run.settings.gamepad == "0300abcd" && run.settings.glyphs == "nintendo");
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
    run = Parse(config, { { "SKY_LANGUAGE", "it" } }, { "--language", "en", "--scale", "2", "--vsync=1", "--windowed",
        "--anisotropy", "16", "--input", "gamepad", "--gamepad", "ffff", "--key", "a=Space", "--glyphs=playstation", "game.iso",
        "--select-iso" });
    CHECK(run.ok && run.warnings.empty());
    CHECK(run.settings.language == "en" && run.settings.voiceLanguage == "de");
    CHECK(run.settings.resolutionScale == 2 && run.settings.vsync && !run.settings.fullscreen && run.settings.anisotropy == 16);
    CHECK(run.settings.inputDevice == "gamepad" && run.settings.gamepad == "ffff" && run.settings.keys[KeyIndex("a")] == "Space");
    CHECK(run.settings.glyphs == "playstation");
    CHECK(run.settings.arguments == (std::vector<std::string>{ "game.iso", "--select-iso" }));

    // The last of repeated options wins.
    run = Parse({}, {}, { "--windowed", "--fullscreen", "--scale", "4", "--scale", "5" });
    CHECK(run.settings.fullscreen && run.settings.resolutionScale == 5);

    // Bad values warn and keep the previous source's value.
    config["video"] = { { "resolution_scale", "11" }, { "anisotropy", "3" }, { "vsync", "yes" } };
    config["input"] = { { "device", "mouse" }, { "key_a", "NoSuchKey" }, { "key_jump", "Space" }, { "glyphs", "sega" } };
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
    run = Parse({ { "video", { { "original_resolution", "1" } } }, { "ssa.video", { { "original_resolution", "0" } } } }, {}, {});
    CHECK(run.ok && !run.settings.originalResolution);
    run = Parse({ { "ssa.video", { { "original_resolution", "1" } } } }, {}, { "--original-resolution", "0" });
    CHECK(run.ok && !run.settings.originalResolution);
    run = Parse({}, {}, { "--original-resolution=1" });
    CHECK(run.ok && run.settings.originalResolution && DescribeSettings(run.settings).find("original resolution") != std::string::npos);
    run = Parse({ { "swapforce.video", { { "original_resolution", "1" } } } }, {}, {}); // another game's section: ignored
    CHECK(run.ok && !run.settings.originalResolution);

    // The game's own [<game>.video] wins over [video] for every video key (a game started without the launcher's PLAY).
    run = Parse({ { "video", { { "fps_limit", "30" }, { "skip_intro_movies", "0" } } },
                  { "ssa.video", { { "fps_limit", "60" }, { "skip_intro_movies", "1" }, { "resolution_scale", "3" } } },
                  { "giants.video", { { "fps_limit", "30" }, { "skip_intro_movies", "0" } } } }, {}, {});
    CHECK(run.ok && run.warnings.empty() && run.settings.fpsLimit == 60 && run.settings.skipIntroMovies && SkipIntroMovies());
    run = Parse({ { "ssa.video", { { "fps_limit", "60" } } } }, {}, { "--fps-limit", "30" });
    CHECK(run.ok && run.settings.fpsLimit == 30);

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
    run = Parse({ { "video", { { "texture_dump", "1" } } } }, { { "SKY_TEXTURE_DUMP", "0" }, { "SKY_TEXTURE_PACKS", "0" } }, {});
    CHECK(!run.settings.textureDump && !run.settings.texturePacks);
    run = Parse({}, { { "SKY_TEXTURE_PACKS", "0" } }, { "--texture-packs", "1", "--texture-dump=1" });
    CHECK(run.settings.textureDump && run.settings.texturePacks);
    run = Parse({}, { { "SKY_TEXTURE_DUMP", "yes" } }, {});
    CHECK(!run.settings.textureDump && run.warnings.size() == 1);

    // fps_limit: 30 or 60; 0 and more than 60 mean 60, other values 30, with a warning.
    run = Parse({ { "video", { { "fps_limit", "60" } } } }, {}, {});
    CHECK(run.settings.fpsLimit == 60 && run.warnings.empty());
    run = Parse({}, {}, { "--fps-limit", "0" });
    CHECK(run.settings.fpsLimit == 60 && run.warnings.size() == 1);
    run = Parse({}, {}, { "--fps-limit", "144" });
    CHECK(run.settings.fpsLimit == 60 && run.warnings.size() == 1);
    run = Parse({}, {}, { "--fps-limit", "45" });
    CHECK(run.settings.fpsLimit == 30 && run.warnings.size() == 1);

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

    // Errors: unknown option, missing value, malformed --key.
    CHECK(!Parse({}, {}, { "--frobnicate" }).ok);
    CHECK(!Parse({}, {}, { "--scale" }).ok);
    CHECK(!Parse({}, {}, { "--key", "Space" }).ok);

    // Flags that end the run.
    CHECK(Parse({}, {}, { "--help" }).settings.help && Parse({}, {}, { "-h" }).settings.help);
    CHECK(Parse({}, {}, { "--version" }).settings.version);
    CHECK(Parse({}, {}, { "--list-gamepads" }).settings.listGamepads);
    CHECK(SettingsHelp("SSARecomp").find("--anisotropy") != std::string::npos);
    CHECK(DescribeSettings(Parse({}, {}, { "--scale", "2" }).settings).find("scale 2 (2240x1408)") != std::string::npos);

    printf("settings test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
