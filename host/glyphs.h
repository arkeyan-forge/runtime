#pragma once

// Button glyphs that follow the device being played. The game's Xbox 360 glyphs are textures: whole ones (SSA: A, B, X,
// Y, START, the right stick) or cells of a font atlas (Giants: the buttons sit after the letters). When the renderer
// uploads one of them (known by its content hash, games/<id>/adapter.cpp), a glyph drawn here for the current style
// replaces the texture or its cells: Xbox One / Series buttons (Menu instead of START, View instead of BACK),
// PlayStation shapes, Nintendo letters by position, or keycaps showing the bound keys.
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace gpu { struct Texture; }
struct RuntimeSettings;
struct SDL_Gamepad;
union SDL_Event;

namespace glyphs
{
    // PlayStation: the PS4 / PS5 look (Options, the touchpad for BACK); PlayStation3: START and SELECT.
    enum class Style : uint8_t { Xbox360, XboxOne, PlayStation, Nintendo, Keyboard, PlayStation3 };

    // A, B, X, Y and Start first (their order indexes the key bindings). RightStick is SSA's 3D prompt (256x256, opaque,
    // the stick seen from above); LeftStickIcon and RightStickIcon are the small stick pictures of a font atlas.
    // Controller, ArrowsLeftRight and ArrowsUpDown are drawn on the keyboard style only (pads keep the game's picture):
    // SSA's Controls page pad (keycaps where its buttons are) and its blue arrows (the left stick's keys).
    enum class Glyph : uint8_t { A, B, X, Y, Start, RightStick, Back, LeftStickIcon, RightStickIcon,
        LeftBumper, RightBumper, LeftTrigger, RightTrigger, Controller, ArrowsLeftRight, ArrowsUpDown };

    // A cell of an atlas, in base-level pixels, inclusive (the picture's bounding box as the game drew it).
    struct Cell
    {
        Glyph glyph;
        uint16_t x0, y0, x1, y1;
    };

    // One of the game's glyph textures, by the XXH3 hash of its base level as the renderer reads it (guest layout:
    // tiled, big-endian). No cells: the whole texture is the glyph. Cells: only they are redrawn, the rest is the game's.
    struct Texture
    {
        uint64_t hash;
        const char* name;
        Glyph glyph;
        std::span<const Cell> cells = {};
        bool flipped = false; // stored upside down (the game draws it with V flipped): drawn upside down too
    };

    // A picture of buttons the game's CPU text renderer copies into its strings (Swap Force: the fonts' "button" images,
    // 32-bit ARGB, tiled, big-endian), so that no texture of the GPU holds it. It sits in guest memory, found by the XXH3
    // of its first 4096 bytes (pixels start on a 4 KB boundary) and redrawn in place for the style in use; strings the game
    // composes afterwards carry the new buttons. Cells are in the image's stored rows; `bytes` is its storage size.
    struct MemoryImage
    {
        uint64_t prefixHash;
        const char* name;
        uint16_t width, height;
        uint32_t bytes;
        std::span<const Cell> cells;
    };

    // On the host's main thread, before the title runs: the glyphs setting, the input device and the key bindings.
    void Init(const RuntimeSettings& settings);

    // Every host event, after the main loop has handled it (`gamepad`: the one in use). With glyphs=auto, a key press
    // or a button / stick / trigger of the gamepad in use selects that device's style, and so does a gamepad being
    // opened before any input. With glyphs=xbox, the same among the two Xbox styles only.
    void OnEvent(const SDL_Event& event, SDL_Gamepad* gamepad);

    Style Current();

    // Test aid, from the host's main loop before it reads events: SKY_GLYPHS_SCRIPT="ps5:25,key:45,switch:55,..." at
    // those seconds since launch attaches a virtual SDL gamepad of that family (ps5, ps4, ps3, switch, xboxone, xbox360; the
    // previous one is detached) and presses its Guide button half a second later, or ("key") sends a key press, so that
    // glyphs=auto switches through the real event path.
    void RunTestScript();

    // Renderer, after hashing a guest texture's base level. Returns true when `texture` now holds our glyph for the
    // current style (created or kept; `hostHash` records which). For the Xbox 360 style, or a texture that is not a glyph,
    // returns false and the renderer uploads the guest data as usual (a previous replacement is dropped first).
    // `guestBase` decodes the guest's base level to RGBA8 (R in the low byte; empty if it cannot): atlases need it.
    bool Replace(uint64_t guestHash, uint32_t width, uint32_t height, uint32_t levels,
        const std::function<std::vector<uint32_t>()>& guestBase, gpu::Texture*& texture, uint64_t& hostHash);
}
