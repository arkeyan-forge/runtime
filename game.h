#pragma once

#include "host/glyphs.h"
#include "host/launcher.h"
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

        // SSA_LEVEL=<level> (test aid, kernel/files.cpp RedirectLevel): the disc path of the first level's files up to
        // the end of the level's name (its name starts after the last backslash), and the characters that may follow
        // the name in a path the redirect rewrites ('\0': the path ends there). Empty: the game has no SSA_LEVEL.
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
    };

    // The adapter of the game being built, defined by games/<id>/adapter.cpp.
    const Adapter& Current();

    // A movie's name from its path ("game:\movies\fre\legal.bik" -> "legal"): the file name, lower case, without
    // its extension.
    std::string MovieName(std::string_view path);

    // True when `path` is one of the game's start-up logos (Adapter::introMovies).
    bool IsIntroMovie(std::string_view path);
}
