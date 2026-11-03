#include "host/settings.h"
#include "host/env.h"
#include <SDL3/SDL.h>
#include <charconv>
#include <atomic>
#include <fmt/format.h>

const KeyAction g_keyActions[22] = {
    { "a", "Space" },
    { "b", "X" },
    { "x", "C" },
    { "y", "V" },
    { "start", "Return" },
    { "back", "Backspace" },
    { "lb", "Q" },
    { "rb", "E" },
    { "lt", "1" },
    { "rt", "3" },
    { "left_up", "W" },
    { "left_down", "S" },
    { "left_left", "A" },
    { "left_right", "D" },
    { "right_up", "I" },
    { "right_down", "K" },
    { "right_left", "J" },
    { "right_right", "L" },
    { "dpad_up", "Up" },
    { "dpad_down", "Down" },
    { "dpad_left", "Left" },
    { "dpad_right", "Right" },
};

namespace
{
    bool g_skipIntroMovies = false;
    std::atomic<bool> g_fpsAutoFallback{ true };

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

        bool EdgeSmoothing(const std::string& source, const std::string& value)
        {
            if (!edgesmoothing::Parse(value, s.edgeSmoothing))
                return Warn(source, "edge_smoothing", value, "auto, game, light or off");
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
            if (value == "display") // the launcher's default: the display's refresh rate, up to 60 until 120 is proven
            {
                s.fpsLimit = 60;
                return true;
            }
            uint32_t n;
            if (!ParseNumber(value, n))
                return Warn(source, "fps_limit", value, "a number (0: none)");
            // 30 and 60 are the game's own pacing; above 60 the host paces the frames. 0: no limit. Another number is
            // lowered to the rate below it.
            constexpr uint32_t Rates[] = { 240, 144, 120, 60 };
            uint32_t fps = n == 0 ? 0 : 30;
            for (uint32_t rate : Rates)
                if (n != 0 && n >= rate)
                {
                    fps = rate;
                    break;
                }
            if (n != fps)
                warnings.push_back(
                    fmt::format("{}: fps_limit={}: the frame rate is 30, 60, 120, 144, 240 or 0 (no limit), using {}",
                        source, value, fps));
            s.fpsLimit = fps;
            return true;
        }

        bool Volume(const std::string& source, const std::string& value)
        {
            uint32_t n;
            if (!ParseNumber(value, n) || n > 100)
                return Warn(source, "volume", value, "a number from 0 to 100");
            s.volume = n;
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
            if (value != "auto" && value != "xbox" && value != "xbox360" && value != "xboxone" &&
                value != "playstation" && value != "nintendo" && value != "keyboard")
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

        // mouse_<action>=<mouse name>|none (host/mouse.h).
        bool Mouse(const std::string& source, const std::string& action, const std::string& value)
        {
            for (size_t i = 0; i < std::size(g_keyActions); i++)
            {
                if (action != g_keyActions[i].name)
                    continue;
                mouse::Button button;
                if (!mouse::FromName(value, button))
                    return Warn(source, "mouse_" + action, value, "a mouse button (Mouse Left, Wheel Up...) or none");
                s.mouse.stored[i] = button;
                s.mouse.set[i] = true;
                return true;
            }
            warnings.push_back(fmt::format("{}: no action \"{}\" for the mouse; ignored", source, action));
            return false;
        }

        // key_toybox, key_toybox_add, key_toybox_clear: the Toy Box's keys.
        bool ToyboxKey(const std::string& source, const std::string& name, const std::string& value)
        {
            static const char* const Names[] = { "key_toybox", "key_toybox_add", "key_toybox_clear" };
            for (size_t i = 0; i < std::size(Names); i++)
            {
                if (name != Names[i])
                    continue;
                if (SDL_GetScancodeFromName(value.c_str()) == SDL_SCANCODE_UNKNOWN)
                    return Warn(source, name, value, "an SDL key name");
                s.toyboxKeys[i] = value;
                if (i == 0)
                    s.toyboxTab = false;
                return true;
            }
            return false;
        }

        // pad_<button>=<button>|none: the pad button `button` does the job of the named one.
        bool Pad(const std::string& source, const std::string& button, const std::string& value)
        {
            const int from = PadButtonIndex(button), to = PadButtonIndex(value);
            if (from < 0)
            {
                warnings.push_back(fmt::format("{}: no pad button \"{}\"; ignored", source, button));
                return false;
            }
            if (to < 0 && value != "none")
                return Warn(source, "pad_" + button, value, "a pad button or none");
            s.padMap.target[from] = int8_t(to);
            return true;
        }

        bool Rumble(const std::string& source, int player, const std::string& value)
        {
            if (value != "off" && value != "low" && value != "normal" && value != "strong")
                return Warn(source, player == 0 ? "rumble_p1" : "rumble_p2", value, "off, low, normal or strong");
            s.rumble[player] = value;
            return true;
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
    auto section = [&](const char* name) -> const std::map<std::string, std::string>& {
        static const std::map<std::string, std::string> empty;
        auto it = config.find(name);
        return it != config.end() ? it->second : empty;
    };
    const char* file = "launcher.ini";
    auto video = [&](const std::string& key, const std::string& value) {
        if (key == "resolution_scale")
            set.Scale(file, value);
        else if (key == "aspect")
            set.Aspect(file, value);
        else if (key == "vsync")
            set.Bool(file, "vsync", value, s.vsync);
        else if (key == "fullscreen")
            set.Bool(file, "fullscreen", value, s.fullscreen);
        else if (key == "anisotropy")
            set.Anisotropy(file, value);
        else if (key == "fps_auto_fallback")
            set.Bool(file, "fps_auto_fallback", value, s.fpsAutoFallback);
        else if (key == "fps_limit")
            set.FpsLimit(file, value);
        else if (key == "skip_intro_movies")
            set.Bool(file, "skip_intro_movies", value, s.skipIntroMovies);
        else if (key == "debug_screen")
            set.Bool(file, "debug_screen", value, s.debugScreen);
        else if (key == "show_fps")
            set.Bool(file, "show_fps", value, s.showFps);
        else if (key == "reduce_motion")
            set.Bool(file, "reduce_motion", value, s.reduceMotion);
        else if (key == "texture_dump")
            set.Bool(file, "texture_dump", value, s.textureDump);
        else if (key == "texture_packs")
            set.Bool(file, "texture_packs", value, s.texturePacks);
        else if (key == "original_resolution")
            set.Bool(file, "original_resolution", value, s.originalResolution);
        else if (key == "edge_smoothing")
            set.EdgeSmoothing(file, value);
    };
    for (const auto& [key, value] : section("video"))
        video(key, value);
    // The game's own video section (the launcher keeps one per game, and PLAY copies it into [video]) wins over [video],
    // so a game started without PLAY (its own icon, a shortcut to the runtime) still gets what its page shows.
    for (const auto& [key, value] : section((gameSection + ".video").c_str()))
        video(key, value);
    // The master volume and the portal's sound: [audio], then the game's own [<game>.audio].
    for (const std::string& name : { std::string("audio"), gameSection + ".audio" })
        for (const auto& [key, value] : section(name.c_str()))
            if (key == "volume")
                set.Volume(file, value);
            else if (key == "portal_sound")
                set.Bool(file, "portal_sound", value, s.portalSound);
    for (const auto& [key, value] : section("input"))
    {
        if (key == "device")
            set.Device(file, value);
        else if (key == "gamepad")
            s.gamepad = value;
        else if (key == "glyphs")
            set.Glyphs(file, value);
        else if (key.starts_with("key_toybox"))
            set.ToyboxKey(file, key, value);
        else if (key.starts_with("key_"))
            set.Key(file, key.substr(4), value);
        else if (key.starts_with("pad_"))
            set.Pad(file, key.substr(4), value);
        else if (key == "rumble_p1")
            set.Rumble(file, 0, value);
        else if (key == "rumble_p2")
            set.Rumble(file, 1, value);
    }
    // The game's own keys (Skylauncher keeps one copy per game, [<game>.input], and puts it in [input] at PLAY) win over
    // [input], so a game started without the launcher still plays with its own bindings.
    for (const auto& [key, value] : section((gameSection + ".input").c_str()))
    {
        if (key == "mouse")
            set.Bool(file, "mouse", value, s.mouse.enabled);
        else if (key.starts_with("mouse_"))
            set.Mouse(file, key.substr(6), value);
        else if (key.starts_with("key_"))
            set.Key(file, key.substr(4), value);
        else if (key.starts_with("pad_"))
            set.Pad(file, key.substr(4), value);
        else if (key == "rumble_p1")
            set.Rumble(file, 0, value);
        else if (key == "rumble_p2")
            set.Rumble(file, 1, value);
    }
    for (const auto& [key, value] : section("portal"))
    {
        if (key == "toybox_auto_open")
            set.Bool(file, "toybox_auto_open", value, s.toyboxAutoOpen);
    }
    const auto& game = section(gameSection.c_str());
    if (auto it = game.find("language"); it != game.end())
        s.language = it->second;
    if (auto it = game.find("voice_language"); it != game.end())
        s.voiceLanguage = it->second;
    if (auto it = game.find("nightmare_from_start"); it != game.end())
        set.Bool(file, "nightmare_from_start", it->second, s.nightmareFromStart);

    // Environment
    if (const char* value = skyenv::Get("SKY_LANGUAGE", getEnv))
        s.language = value;
    if (const char* value = skyenv::Get("SKY_VOICE_LANGUAGE", getEnv))
        s.voiceLanguage = value;
    if (const char* value = skyenv::Get("SKY_NIGHTMARE_FROM_START", getEnv))
        set.Bool("SKY_NIGHTMARE_FROM_START", "nightmare_from_start", value, s.nightmareFromStart);
    if (const char* value = skyenv::Get("SKY_TOYBOX_AUTO_OPEN", getEnv))
        set.Bool("SKY_TOYBOX_AUTO_OPEN", "toybox_auto_open", value, s.toyboxAutoOpen);
    if (const char* value = skyenv::Get("SKY_TEXTURE_DUMP", getEnv))
        set.Bool("SKY_TEXTURE_DUMP", "SKY_TEXTURE_DUMP", value, s.textureDump);
    if (const char* value = skyenv::Get("SKY_TEXTURE_PACKS", getEnv))
        set.Bool("SKY_TEXTURE_PACKS", "SKY_TEXTURE_PACKS", value, s.texturePacks);

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
        auto take = [&]() -> bool {
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

        if (option == "--help" || option == "-h")
            s.help = true;
        else if (option == "--version")
            s.version = true;
        else if (option == "--list-gamepads")
            s.listGamepads = true;
        else if (option == "--portal-probe")
            s.portalProbe = true;
        else if (option == "--fullscreen")
            s.fullscreen = true;
        else if (option == "--windowed")
            s.fullscreen = false;
        else if (option == "--scale")
        {
            if (!take())
                return false;
            set.Scale(cli, value);
        }
        else if (option == "--aspect")
        {
            if (!take())
                return false;
            set.Aspect(cli, value);
        }
        else if (option == "--vsync")
        {
            if (!take())
                return false;
            set.Bool(cli, "vsync", value, s.vsync);
        }
        else if (option == "--anisotropy")
        {
            if (!take())
                return false;
            set.Anisotropy(cli, value);
        }
        else if (option == "--fps-limit")
        {
            if (!take())
                return false;
            set.FpsLimit(cli, value);
        }
        else if (option == "--texture-dump")
        {
            if (!take())
                return false;
            set.Bool(cli, "texture_dump", value, s.textureDump);
        }
        else if (option == "--texture-packs")
        {
            if (!take())
                return false;
            set.Bool(cli, "texture_packs", value, s.texturePacks);
        }
        else if (option == "--original-resolution")
        {
            if (!take())
                return false;
            set.Bool(cli, "original_resolution", value, s.originalResolution);
        }
        else if (option == "--edge-smoothing")
        {
            if (!take())
                return false;
            set.EdgeSmoothing(cli, value);
        }
        else if (option == "--input")
        {
            if (!take())
                return false;
            set.Device(cli, value);
        }
        else if (option == "--gamepad")
        {
            if (!take())
                return false;
            s.gamepad = value;
        }
        else if (option == "--glyphs")
        {
            if (!take())
                return false;
            set.Glyphs(cli, value);
        }
        else if (option == "--language")
        {
            if (!take())
                return false;
            s.language = value;
        }
        else if (option == "--voice-language")
        {
            if (!take())
                return false;
            s.voiceLanguage = value;
        }
        else if (option == "--nightmare-from-start")
        {
            if (!take())
                return false;
            set.Bool(cli, "nightmare_from_start", value, s.nightmareFromStart);
        }
        else if (option == "--key")
        {
            if (!take())
                return false;
            size_t equals = value.find('=');
            if (equals == std::string::npos)
            {
                error = "--key takes <action>=<key>, e.g. --key a=Space";
                return false;
            }
            set.Key(cli, value.substr(0, equals), value.substr(equals + 1));
        }
        else if (option == "--pad")
        {
            if (!take())
                return false;
            size_t equals = value.find('=');
            if (equals == std::string::npos)
            {
                error = "--pad takes <button>=<button>, e.g. --pad rb=b";
                return false;
            }
            set.Pad(cli, value.substr(0, equals), value.substr(equals + 1));
        }
        else if (option == "--rumble")
        {
            if (!take())
                return false;
            set.Rumble(cli, 0, value);
            set.Rumble(cli, 1, value);
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
    SetFpsAutoFallback(s.fpsAutoFallback);
    return true;
}

bool FpsAutoFallback() { return g_fpsAutoFallback.load(); }
void SetFpsAutoFallback(bool enabled) { g_fpsAutoFallback.store(enabled); }

bool SkipIntroMovies() { return g_skipIntroMovies; }

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
        "  --texture-dump <0|1>        write the game's textures as DDS files (default 0; "
        "docs/runtime/gpu/texture_packs.md)\n"
        "  --texture-packs <0|1>       use the replacement textures of the textures folder (default 1)\n"
        "  --original-resolution <0|1> Swap Force: the console's 3D at 81 % of the frame, stretched (default 0: the "
        "3D\n"
        "                              at the frame's size)\n"
        "  --edge-smoothing <auto|game|light|off>\n"
        "                              the game's FXAA pass (Swap Force, Trap Team): game keeps it, light filters\n"
        "                              only strong edges, off draws a plain copy (default auto: light from\n"
        "                              resolution scale 2, game at 1)\n"
        "  --input <auto|keyboard|gamepad>\n"
        "                              what the game listens to (default auto: keyboard and gamepad)\n"
        "  --gamepad <guid>            the gamepad to use (see --list-gamepads; default the first one)\n"
        "  --key <action>=<key>        keyboard binding, SDL key names (e.g. --key a=Space)\n"
        "  --pad <button>=<button>     the first pad button does the job of the second (e.g. --pad rb=b; none: "
        "nothing)\n"
        "  --rumble <off|low|normal|strong>\n"
        "                              rumble strength, both players\n"
        "  --glyphs <auto|xbox|xbox360|xboxone|playstation|nintendo|keyboard>\n"
        "                              button glyphs shown by the game (default auto: the last device used)\n"
        "  --language <code>           text language: en fr de es it nl sv no da fi\n"
        "  --voice-language <code>     voice language, same codes\n"
        "  --nightmare-from-start <0|1>\n"
        "                              Swap Force: the Nightmare difficulty offered before the story is beaten\n"
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
    text += "(Escape also works as back.)\n\nPad buttons for --pad:";
    for (const char* button : g_padButtons)
        text += fmt::format(" {}", button);
    text += "\n";
    return text;
}

std::string DescribeSettings(const RuntimeSettings& s)
{
    std::string keys;
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        if (s.keys[i] != g_keyActions[i].defaultKey)
            keys += fmt::format("{}{}={}", keys.empty() ? "" : ",", g_keyActions[i].name, s.keys[i]);
    std::string padDescription;
    for (int i = 0; i < PadButtonCount; i++)
        if (s.padMap.target[i] != i)
            padDescription += fmt::format("{}{}->{}", padDescription.empty() ? ", pad " : ",", g_padButtons[i],
                s.padMap.target[i] < 0 ? "none" : g_padButtons[s.padMap.target[i]]);
    if (s.rumble[0] != "normal" || s.rumble[1] != "normal")
        padDescription += fmt::format(", rumble {}/{}", s.rumble[0], s.rumble[1]);
    std::string scale = s.resolutionScale == 0 ? "auto"
                                               : fmt::format("{} ({}x{})", s.resolutionScale, 1120 * s.resolutionScale,
                                                     704 * s.resolutionScale);
    return fmt::format(
               "scale {}, vsync {}, {}, anisotropy {}, fps limit {}, intro movies {}, input {}, gamepad {}, keys {}, "
               "glyphs {}, language {}, voices {}{}{}",
               scale, s.vsync ? 1 : 0, s.fullscreen ? "fullscreen" : "windowed", s.anisotropy, s.fpsLimit,
               s.skipIntroMovies ? "skipped" : "played", s.inputDevice, s.gamepad.empty() ? "first" : s.gamepad,
               keys.empty() ? "default" : keys, s.glyphs, s.language.empty() ? "game" : s.language,
               s.voiceLanguage.empty() ? "game" : s.voiceLanguage, s.toyboxAutoOpen ? "" : ", toybox auto-open off",
               padDescription) +
           fmt::format(", fps auto fallback {}", s.fpsAutoFallback ? "on" : "off") +
           (s.aspectAuto ? "" : ", aspect 16:9") + (s.textureDump ? ", texture dump" : "") +
           (s.texturePacks ? "" : ", texture packs off") + (s.originalResolution ? ", original resolution" : "") +
           (s.edgeSmoothing != edgesmoothing::Auto
                   ? fmt::format(", edge smoothing {}", edgesmoothing::Name(s.edgeSmoothing))
                   : "") +
           (s.nightmareFromStart ? ", nightmare from the start" : "") + (s.showFps ? ", fps counter" : "") +
           (s.debugScreen ? ", debug screen" : "") + (s.reduceMotion ? ", reduced motion" : "") +
           (s.volume != 100 ? fmt::format(", volume {}", s.volume) : "") + (s.portalSound ? "" : ", portal sound off");
}
