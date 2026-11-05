#pragma once

#include "host/commands.h"
#include "host/glyphs.h"
#include "host/launcher.h"
#include "host/mouse.h"
#include "host/portal_prompt.h"
#include <string>
#include <string_view>

extern const GameInfo g_gameInfo; // the game's table entry: games/<id>/info.cpp

// What the runtime asks of the game it was built for (-DSSA_GAME=<id>): the small interface every game's adapter
// implements, games/<id>/adapter.cpp (docs/architecture/hooks.md, class c). Everything else a game needs is a hook
// (game_hooks.h) or a constant in its addresses.h.
namespace game
{
    struct Adapter
    {
        // Catalogue id ("ssa"), display name, the releases the disc check accepts, and the executable that runs it
        // (games/<id>/info.cpp, which has no other dependency so that the launcher test can link it).
        const GameInfo& info;

        // The game's "place a Skylander" prompt, read from its memory every frame (host/portal_prompt.h); nullptr when
        // the game's prompt is not known yet (the Toy Box then never opens by itself).
        portalprompt::Prompt (*ReadPortalPrompt)();

        // True when the adapter hooks the game's camera aspect, so that ultrawide can widen the picture
        // (d3d/ultrawide.h). Without it the window keeps the game's own 16:9.
        bool ultrawide;

        // Plays the voices in another language than the text ([audio] voice_language: "en", "fr"...); false for a code
        // the game does not have. nullptr when the game's voices cannot be chosen yet.
        bool (*SetVoiceLanguage)(std::string_view code);

        // The game's Xbox 360 button glyph textures, which host/glyphs.cpp redraws for the device in use. Found per
        // game from a texture dump (docs/runtime/xbox360/input.md, "Glyphs").
        std::span<const glyphs::Texture> glyphTextures;

        // The start-up logos that [video] skip_intro_movies skips (file names, lower case, no extension); the story
        // movies always play.
        std::span<const std::string_view> introMovies;

        // True when the game presents at every vertical blank by its own configuration (Alchemy: alchemy.xml's
        // presentationInterval = 1) and so runs as fast as it can up to 60 FPS: fps_limit 30 then shows every second
        // frame (kernel/video.cpp). False when the game asks for interval 2 itself (Lux), which fps_limit 60 lifts (see presentIntervalPacesGame for a game that also reads the interval).
        bool presentsAtEveryBlank = false;

        // The text language when the console's alone cannot say it ([<game>] language: Giants' Latin-American Spanish,
        // and Dutch, Swedish, Norwegian, Danish and Finnish, which no console language names); false for a code the game
        // does not have. Called after SetTextLanguage (language.h), which has set the console language it knows.
        // nullptr: the console language is all there is.
        bool (*SetTextLanguage)(std::string_view code) = nullptr;

        // SKY_LEVEL=<level> (test aid, kernel/files.cpp RedirectLevel): the disc path of the first level's files up to
        // the end of the level's name (its name starts after the last backslash), and the characters that may follow
        // the name in a path the redirect rewrites ('\0': the path ends there). Empty: the game has no SKY_LEVEL.
        struct FirstLevel
        {
            std::string_view path;
            std::string_view next;
        };
        FirstLevel firstLevel = {};

        // Pictures of buttons in guest memory that the game's CPU text renderer copies into its strings
        // (host/glyphs.h, MemoryImage): redrawn in place for the glyph style in use.
        std::span<const glyphs::MemoryImage> glyphImages = {};

        // True when the game reads D3DRS_PRESENTINTERVAL from its device to pace itself (Swap Force, Alchemy): at 2 it steps
        // its logic by a fixed 1/30 s and waits for two blanks in its own code, whatever the host does at the swap. With
        // fps_limit 60 the runtime then writes 1 there before every swap, and the game measures its frame time instead
        // (d3d/render.cpp, Swap). False for a game that steps by the time it measures at any interval (Lux, Giants).
        bool presentIntervalPacesGame = false;

        // The 3D scene's size at full resolution when the game renders it smaller than the 1280x720 frame and stretches
        // it (Swap Force: 1040x620, 81 % of the frame): the runtime renders it at this size instead unless [video]
        // original_resolution is set (d3d/ultrawide.h SetScene16x9). {0, 0}: the game's scene is already full size.
        struct Size
        {
            uint32_t width, height;
        };
        Size fullScene = {};

        // [<game>] nightmare_from_start: the game's hardest difficulty offered from a new game, whatever the save says, and
        // locked again when the setting is off; the save is never written for it. nullptr: the game has no locked
        // difficulty (Spyro's Adventure has none to choose, Giants' hardest is open from the start; Swap Force's Nightmare
        // is locked until the story is beaten).
        void (*SetNightmareFromStart)(bool on) = nullptr;

        // The level a file open starts ("level_027", the first component of the disc path it opens, lower case; empty for
        // any other file), so the Toy Box knows a new level began (NoteFileOpen; a figure out of health can play again).
        // nullptr: the game says it itself (Swap Force: engines/alchemy/zone.cpp calls LevelStarted).
        std::string (*LevelOfFile)(std::string_view path) = nullptr;

        // The highest [video] fps_limit the game is given (0: no cap); a higher one, and no limit, are lowered to it, with a log line.
        // Giants: 60. Its summon cutscene lasts 10 % less at 120 than at 30, every run (docs/runtime/xbox360/video_vblank.md, "Above 60,
        // measured"): a fade or a wait counted in frames, not found yet.
        uint32_t maxFpsLimit = 0;

        // Pixel shader hashes of the passes that advance a simulation by one step for every frame the game renders (a
        // water ripple height field, constants that never change): above fps_limit 30 the runtime draws them (and
        // resolves their targets) in one frame of the 30 FPS cadence only, so the simulation keeps its rate in seconds
        // (d3d/step_pacer.h). Empty: the game has none known.
        std::span<const uint64_t> stepPassShaders = {};

        // The game's actions on the F3 menu's Commands page, in Skylauncher's order and words (its launcher/game_keys.cpp):
        // the keyboard key and the pad button of each (host/commands.h). Empty: the page has no rows.
        std::span<const commands::Row> inputRows = {};

        // The mouse once its switch is on (host/mouse.h): the game's attack 1 on the left button, attack 2 on the right.
        std::span<const mouse::Default> mouseDefaults = {};

        // The levels that are the game's own menus (game::LevelStarted's names: its front end), where the mouse does
        // nothing and the pointer is not held. The menus inside a level (pause) cannot be told from play.
        std::span<const std::string_view> menuLevels = {};

        // The widest full-resolution scene (fullScene) a wide window gets: past it the scene keeps the window's shape at a
        // smaller height (d3d/ultrawide.h). 0: no limit. SuperChargers stops in its hub with a 2560x720 scene (32:9).
        uint32_t maxSceneWidth = 0;
    };

    // The adapter of the game being built, defined by games/<id>/adapter.cpp.
    const Adapter& Current();

    // A movie's name from its path ("game:\movies\fre\legal.bik" -> "legal"): the file name, lower case, without
    // its extension.
    std::string MovieName(std::string_view path);
    // The level a file starts, for Adapter::LevelOfFile: \level\<name>.arc -> "<name>", lower case (the title screen's is
    // one too, "title"); empty for any other file. Giants and Trap Team.
    std::string LevelArcOfFile(std::string_view path);

    // A level began (a file of a level the title opens, or the zone Swap Force enters): said once for each change of level;
    // the Toy Box's figures that ran out of health can play again. `name` is only for the log.
    void LevelStarted(std::string_view name);
    // Copies the existing level label under its lock, without allocating; empty before the first level.
    void CopyCurrentLevel(char* text, size_t capacity);
    // True before the first level and while the level is one of the game's menus (Adapter::menuLevels): the mouse does
    // nothing there.
    bool InMenuLevel();
    // Called for every file the title opens: asks the adapter whether it is a level's.
    void NoteFileOpen(std::string_view path);

    // True when `path` is one of the game's start-up logos (Adapter::introMovies).
    bool IsIntroMovie(std::string_view path);
}
