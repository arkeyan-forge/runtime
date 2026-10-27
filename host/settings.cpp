#include "host/settings.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <fmt/format.h>

const KeyAction g_keyActions[22] = {
    { "a", "Space" }, { "b", "X" }, { "x", "C" }, { "y", "V" }, { "start", "Return" }, { "back", "Backspace" },
    { "lb", "Q" }, { "rb", "E" }, { "lt", "1" }, { "rt", "3" },
    { "left_up", "W" }, { "left_down", "S" }, { "left_left", "A" }, { "left_right", "D" },
    { "right_up", "I" }, { "right_down", "K" }, { "right_left", "J" }, { "right_right", "L" },
    { "dpad_up", "Up" }, { "dpad_down", "Down" }, { "dpad_left", "Left" }, { "dpad_right", "Right" },
};

namespace
{
    bool g_skipIntroMovies = false;

    bool ParseNumber(const std::string& text, uint32_t& value)
    {
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        return error == std::errc() && end == text.data() + text.size() && !text.empty();
    }

    // Each setter checks one value and reports where it came from when it is wrong.
    struct Setter
    {
        RuntimeSettings& s;
        std::vector<std::string>& warnings;

        bool Warn(const std::string& source, const std::string& key, const std::string& value, const char* expected)
        {
            warnings.push_back(fmt::format("{}: {}={} is not {}; ignored", source, key, value, expected));
            return false;
        }

        bool Scale(const std::string& source, const std::string& value)
        {
            uint32_t n = 0;
            if (value != "auto" && (!ParseNumber(value, n) || n < 1 || n > 10))
                return Warn(source, "resolution_scale", value, "auto or 1 to 10");
            s.resolutionScale = n;
            return true;
        }

        bool Bool(const std::string& source, const char* key, const std::string& value, bool& out)
        {
            if (value != "0" && value != "1")
                return Warn(source, key, value, "0 or 1");
            out = value == "1";
            return true;
        }

        bool Aspect(const std::string& source, const std::string& value)
        {
            if (value != "auto" && value != "16:9")
                return Warn(source, "aspect", value, "auto or 16:9");
            s.aspectAuto = value == "auto";
            return true;
        }

        bool Anisotropy(const std::string& source, const std::string& value)
        {
            uint32_t n;
            if (!ParseNumber(value, n) || (n != 1 && n != 2 && n != 4 && n != 8 && n != 16))
                return Warn(source, "anisotropy", value, "1, 2, 4, 8 or 16");
            s.anisotropy = n;
            return true;
        }

        bool FpsLimit(const std::string& source, const std::string& value)
        {
            uint32_t n;
            if (!ParseNumber(value, n))
                return Warn(source, "fps_limit", value, "a number (0: none)");
            // The title shows a frame every 1 or 2 vertical blanks (60 Hz): 30 and 60 are the only rates.
            uint32_t fps = n == 0 || n >= 60 ? 60 : 30;
            if (n != fps)
                warnings.push_back(fmt::format("{}: fps_limit={}: the game runs at 30 or 60 FPS, using {}", source, value, fps));
            s.fpsLimit = fps;
            return true;
        }

        bool Device(const std::string& source, const std::string& value)
        {
            if (value != "auto" && value != "keyboard" && value != "gamepad")
                return Warn(source, "device", value, "auto, keyboard or gamepad");
            s.inputDevice = value;
            return true;
        }

        bool Glyphs(const std::string& source, const std::string& value)
        {
            if (value != "auto" && value != "xbox" && value != "xbox360" && value != "xboxone" && value != "playstation"
                && value != "nintendo" && value != "keyboard")
                return Warn(source, "glyphs", value, "auto, xbox, xbox360, xboxone, playstation, nintendo or keyboard");
            s.glyphs = value;
            return true;
        }

        bool Key(const std::string& source, const std::string& action, const std::string& value)
        {
            for (size_t i = 0; i < std::size(g_keyActions); i++)
            {
                if (action != g_keyActions[i].name)
                    continue;
                if (SDL_GetScancodeFromName(value.c_str()) == SDL_SCANCODE_UNKNOWN)
                    return Warn(source, "key_" + action, value, "an SDL key name");
                s.keys[i] = value;
                return true;
            }
            warnings.push_back(fmt::format("{}: no keyboard action \"{}\"; ignored", source, action));
            return false;
        }
    };
}

bool ParseSettings(const LauncherConfig& config, const std::string& gameSection, const GetEnv& getEnv, int argc,
    char** argv, RuntimeSettings& settings, std::vector<std::string>& warnings, std::string& error)
{
    RuntimeSettings& s = settings;
    Setter set{ s, warnings };
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        s.keys[i] = g_keyActions[i].defaultKey;

    // launcher.ini
    auto section = [&](const char* name) -> const std::map<std::string, std::string>&
    {
        static const std::map<std::string, std::string> empty;
        auto it = config.find(name);
        return it != config.end() ? it->second : empty;
    };
    const char* file = "launcher.ini";
    for (const auto& [key, value] : section("video"))
    {
        if (key == "resolution_scale") set.Scale(file, value);
        else if (key == "aspect") set.Aspect(file, value);
        else if (key == "vsync") set.Bool(file, "vsync", value, s.vsync);
        else if (key == "fullscreen") set.Bool(file, "fullscreen", value, s.fullscreen);
        else if (key == "anisotropy") set.Anisotropy(file, value);
        else if (key == "fps_limit") set.FpsLimit(file, value);
        else if (key == "skip_intro_movies") set.Bool(file, "skip_intro_movies", value, s.skipIntroMovies);
        else if (key == "texture_dump") set.Bool(file, "texture_dump", value, s.textureDump);
        else if (key == "texture_packs") set.Bool(file, "texture_packs", value, s.texturePacks);
    }
    for (const auto& [key, value] : section("input"))
    {
        if (key == "device") set.Device(file, value);
        else if (key == "gamepad") s.gamepad = value;
        else if (key == "glyphs") set.Glyphs(file, value);
        else if (key.starts_with("key_")) set.Key(file, key.substr(4), value);
    }
    for (const auto& [key, value] : section("portal"))
    {
        if (key == "toybox_auto_open") set.Bool(file, "toybox_auto_open", value, s.toyboxAutoOpen);
    }
    const auto& game = section(gameSection.c_str());
    if (auto it = game.find("language"); it != game.end()) s.language = it->second;
    if (auto it = game.find("voice_language"); it != game.end()) s.voiceLanguage = it->second;

    // Environment
    if (const char* value = getEnv("SSA_LANGUAGE")) s.language = value;
    if (const char* value = getEnv("SSA_VOICE_LANGUAGE")) s.voiceLanguage = value;
    if (const char* value = getEnv("SSA_TOYBOX_AUTO_OPEN")) set.Bool("SSA_TOYBOX_AUTO_OPEN", "toybox_auto_open", value, s.toyboxAutoOpen);
    if (const char* value = getEnv("SSA_TEXTURE_DUMP")) set.Bool("SSA_TEXTURE_DUMP", "SSA_TEXTURE_DUMP", value, s.textureDump);
    if (const char* value = getEnv("SSA_TEXTURE_PACKS")) set.Bool("SSA_TEXTURE_PACKS", "SSA_TEXTURE_PACKS", value, s.texturePacks);

    // Command line: "--option value" or "--option=value".
    const char* cli = "command line";
    for (int i = 1; i < argc; i++)
    {
        std::string option = argv[i], value;
        bool inlineValue = false;
        if (size_t equals = option.find('='); option.starts_with("--") && equals != std::string::npos)
        {
            value = option.substr(equals + 1);
            option.resize(equals);
            inlineValue = true;
        }
        auto take = [&]() -> bool
        {
            if (inlineValue)
                return true;
            if (i + 1 >= argc)
            {
                error = option + " needs a value (see --help)";
                return false;
            }
            value = argv[++i];
            return true;
        };

        if (option == "--help" || option == "-h") s.help = true;
        else if (option == "--version") s.version = true;
        else if (option == "--list-gamepads") s.listGamepads = true;
        else if (option == "--portal-probe") s.portalProbe = true;
        else if (option == "--fullscreen") s.fullscreen = true;
        else if (option == "--windowed") s.fullscreen = false;
        else if (option == "--scale") { if (!take()) return false; set.Scale(cli, value); }
        else if (option == "--aspect") { if (!take()) return false; set.Aspect(cli, value); }
        else if (option == "--vsync") { if (!take()) return false; set.Bool(cli, "vsync", value, s.vsync); }
        else if (option == "--anisotropy") { if (!take()) return false; set.Anisotropy(cli, value); }
        else if (option == "--fps-limit") { if (!take()) return false; set.FpsLimit(cli, value); }
        else if (option == "--texture-dump") { if (!take()) return false; set.Bool(cli, "texture_dump", value, s.textureDump); }
        else if (option == "--texture-packs") { if (!take()) return false; set.Bool(cli, "texture_packs", value, s.texturePacks); }
        else if (option == "--input") { if (!take()) return false; set.Device(cli, value); }
        else if (option == "--gamepad") { if (!take()) return false; s.gamepad = value; }
        else if (option == "--glyphs") { if (!take()) return false; set.Glyphs(cli, value); }
        else if (option == "--language") { if (!take()) return false; s.language = value; }
        else if (option == "--voice-language") { if (!take()) return false; s.voiceLanguage = value; }
        else if (option == "--key")
        {
            if (!take()) return false;
            size_t equals = value.find('=');
            if (equals == std::string::npos)
            {
                error = "--key takes <action>=<key>, e.g. --key a=Space";
                return false;
            }
            set.Key(cli, value.substr(0, equals), value.substr(equals + 1));
        }
        else if (option == "--select-iso" || option == "--launcher" || !option.starts_with("--"))
            s.arguments.push_back(argv[i]);
        else
        {
            error = "unknown option " + option + " (see --help)";
            return false;
        }
    }
    g_skipIntroMovies = s.skipIntroMovies;
    return true;
}

bool SkipIntroMovies()
{
    return g_skipIntroMovies;
}

std::string SettingsHelp(const char* program)
{
    std::string text = fmt::format(
        "usage: {} [options] [<disc image .iso>]\n"
        "\n"
        "Options override launcher.ini ([video], [input], the game's language keys) and the SSA_* variables.\n"
        "\n"
        "  --scale <auto|1-10>         render the 3D at this multiple of 1120x704 (default auto: 1 on integrated\n"
        "                              GPUs, 2 on discrete GPUs and Apple silicon)\n"
        "  --aspect <auto|16:9>        auto: a window wider than 16:9 shows more to the sides (default); 16:9: the\n"
        "                              game's own picture, with bars on a wider window\n"
        "  --vsync <0|1>               wait for the display's refresh (default 1)\n"
        "  --fullscreen, --windowed    desktop fullscreen or a window (default windowed)\n"
        "  --anisotropy <1|2|4|8|16>   anisotropic texture filtering (default 1)\n"
        "  --fps-limit <n>             30 or 60 (experimental); 0 and more than 60 mean 60\n"
        "  --texture-dump <0|1>        write the game's textures as DDS files (default 0; docs/runtime/gpu/texture_packs.md)\n"
        "  --texture-packs <0|1>       use the replacement textures of the textures folder (default 1)\n"
        "  --input <auto|keyboard|gamepad>\n"
        "                              what the game listens to (default auto: keyboard and gamepad)\n"
        "  --gamepad <guid>            the gamepad to use (see --list-gamepads; default the first one)\n"
        "  --key <action>=<key>        keyboard binding, SDL key names (e.g. --key a=Space)\n"
        "  --glyphs <auto|xbox|xbox360|xboxone|playstation|nintendo|keyboard>\n"
        "                              button glyphs shown by the game (default auto: the last device used)\n"
        "  --language <code>           text language: en fr de es it nl sv no da fi\n"
        "  --voice-language <code>     voice language, same codes\n"
        "  --select-iso                choose the disc image again\n"
        "  --launcher                  open the launcher window\n"
        "  --list-gamepads             print the connected gamepads and their GUID, then quit\n"
        "  --portal-probe              look for real portals, print one JSON line per portal, then quit\n"
        "  --help                      this text\n"
        "  --version                   print the version, then quit\n"
        "\n"
        "Keyboard actions and default keys:\n",
        program);
    for (const KeyAction& action : g_keyActions)
        text += fmt::format("  {:<12} {}\n", action.name, action.defaultKey);
    text += "(Escape also works as back.)\n";
    return text;
}

std::string DescribeSettings(const RuntimeSettings& s)
{
    std::string keys;
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        if (s.keys[i] != g_keyActions[i].defaultKey)
            keys += fmt::format("{}{}={}", keys.empty() ? "" : ",", g_keyActions[i].name, s.keys[i]);
    std::string scale = s.resolutionScale == 0 ? "auto" : fmt::format("{} ({}x{})", s.resolutionScale, 1120 * s.resolutionScale,
        704 * s.resolutionScale);
    return fmt::format("scale {}, vsync {}, {}, anisotropy {}, fps limit {}, intro movies {}, input {}, gamepad {}, keys {}, "
        "glyphs {}, language {}, voices {}{}", scale, s.vsync ? 1 : 0,
        s.fullscreen ? "fullscreen" : "windowed", s.anisotropy, s.fpsLimit, s.skipIntroMovies ? "skipped" : "played",
        s.inputDevice, s.gamepad.empty() ? "first" : s.gamepad, keys.empty() ? "default" : keys, s.glyphs,
        s.language.empty() ? "game" : s.language, s.voiceLanguage.empty() ? "game" : s.voiceLanguage,
        s.toyboxAutoOpen ? "" : ", toybox auto-open off")
        + (s.aspectAuto ? "" : ", aspect 16:9")
        + (s.textureDump ? ", texture dump" : "") + (s.texturePacks ? "" : ", texture packs off");
}
