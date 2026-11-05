#pragma once

// Button glyphs that follow the device being played. The game's Xbox 360 glyphs are textures: whole ones (SSA: A, B, X,
// Y, START, the right stick) or cells of a font atlas (Giants: the buttons sit after the letters). When the renderer
// uploads one of them (known by its content hash, games/<id>/adapter.cpp), a glyph drawn here for the current style
// replaces the texture or its cells: Xbox One / Series buttons (Menu instead of START, View instead of BACK),
// PlayStation shapes, Nintendo letters by position, or keycaps showing the bound keys.
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace gpu
{
    struct Texture;
}
struct RuntimeSettings;
struct SDL_Gamepad;
union SDL_Event;

namespace glyphs
{
    // PlayStation: the PS4 / PS5 look (Options, the touchpad for BACK); PlayStation3: START and SELECT.
    enum class Style : uint8_t
    {
        Xbox360,
        XboxOne,
        PlayStation,
        Nintendo,
        Keyboard,
        PlayStation3
    };

    // A, B, X, Y and Start first (their order indexes the key bindings). RightStick is SSA's 3D prompt (256x256, opaque,
    // the stick seen from above); LeftStickIcon and RightStickIcon are the small stick pictures of a font atlas.
    // Controller, ArrowsLeftRight and ArrowsUpDown are drawn on the keyboard style only (pads keep the game's picture):
    // SSA's Controls page pad (keycaps where its buttons are) and its blue arrows (the left stick's keys).
    enum class Glyph : uint8_t
    {
        A,
        B,
        X,
        Y,
        Start,
        RightStick,
        Back,
        LeftStickIcon,
        RightStickIcon,
        LeftBumper,
        RightBumper,
        LeftTrigger,
        RightTrigger,
        Controller,
        ArrowsLeftRight,
        ArrowsUpDown
    };

    // A 3D model that shows its button over a glyph texture with letters of geometry (Trap Team's RT prompt: a card with a
    // grey gradient, "RT" extruded on it). While the texture is drawn as a glyph, the letters' indexed draw (its index
    // count and the XXH3 of its indices as stored) is skipped, and the model's vertex shaders (its passes), which turn
    // the texture's coordinates by the 4x4 matrix at float constant `uvMatrix` (an animated sheen), get the identity
    // there, so that the glyph stays upright and in place.
    struct Model
    {
        uint32_t lettersIndexCount;
        uint64_t lettersIndexHash;
        std::span<const uint64_t> vertexShaders;
        uint32_t uvMatrix;
    };

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
        bool over = false;    // cells drawn over the game's pixels, not in cleared ones (an opaque model's texture)
        const Model* model = nullptr;
        bool plate = false; // a world prompt's picture (Prompt): the glyph centred on a dark plate, opaque
    };

    // A 3D prompt of two meshes, a flat face (a disc, a pill) and the letters of the button on it, both coloured by the
    // texels of one tiny colour map (SuperChargers, Imaginators: models/UI/ButtonPrompt). While a style other than the 360's is
    // in use the renderer pairs the two draws (PromptPartOf, PromptOf), skips the letters and issues the face with the style's glyph: the glyph's picture
    // (`picture`, a Texture row with `plate`) replaces the colour map, and the face's texture coordinates become a planar
    // projection of its positions (PlanarUv). `faceCount` / `faceHash`: the face's indices as stored.
    struct Prompt
    {
        uint32_t faceCount;
        uint64_t faceHash;
        uint32_t lettersCount;
        uint64_t lettersHash;
        uint64_t picture;      // `hash` of the Texture row of the glyph's picture
        uint16_t width, height; // that picture's size
        uint8_t uAxis, vAxis;   // the position components (0 x, 1 y, 2 z) that run left to right and top to bottom
        bool flipU, flipV;
        float uScale; // the game's shader stretches u about 1/2 by this much (a pill's aspect): written divided
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

    // The texture coordinates that cover a mesh's bounding box with one picture. `vertices` is a vertex stream in the renderer's
    // host order: positions as three SNORM int16 at `positionOffset` (x, y, z in this order), the coordinates as FLOAT16_2 at
    // `uvOffset` with the halves swapped (v, u: what the translated shaders expect for 16-bit pairs). Only the vertices
    // `indices` (plus `baseVertex`) name are read and written. False when the mesh has no area in the prompt's plane.
    bool PlanarUv(const Prompt& prompt, std::span<uint8_t> vertices, uint32_t stride, uint32_t positionOffset,
        uint32_t uvOffset, std::span<const uint16_t> indices, int32_t baseVertex);

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

    // Renderer, before an indexed draw of 16-bit `indices` (as stored, `count` of them): true when they are the letters of
    // a glyph texture's model (Model) and that texture is drawn as a glyph in the current style, so that the 360's letters
    // do not cover it. Hashes only a draw whose count is listed.
    bool HidesDraw(uint32_t count, const void* indices);
    bool HidesDraw(std::span<const Texture> textures, Style style, uint32_t count, const void* indices);
    // Renderer, before an indexed draw: whether its indices are the face or the letters of a listed prompt (and a style other
    // than the 360's is in use), with their XXH3 in `hash`. The renderer holds such a draw for one draw: the face and the
    // letters of a prompt come one after the other, in either order (SuperChargers: face first; Imaginators' second pass: letters
    // first). When the next draw is the other part of a listed prompt (PromptOf) it issues the face with the prompt's glyph and
    // drops the letters, so that prompts sharing a face (A and Y) are told by their letters; any other draw issues the held
    // draw as the game drew it first.
    enum class PromptPart : uint8_t
    {
        None,
        Face,
        Letters
    };
    PromptPart PromptPartOf(uint32_t count, const void* indices, uint64_t& hash);
    PromptPart PromptPartOf(
        std::span<const Prompt> prompts, Style style, uint32_t count, const void* indices, uint64_t& hash);
    // The prompt with this face and these letters, or nullptr.
    const Prompt* PromptOf(uint32_t faceCount, uint64_t faceHash, uint32_t lettersCount, uint64_t lettersHash);
    const Prompt* PromptOf(std::span<const Prompt> prompts, uint32_t faceCount, uint64_t faceHash,
        uint32_t lettersCount, uint64_t lettersHash);

    // Renderer, for a draw by `vertexShader` with these textures (the guest hashes of those drawn as glyphs, 0 for the
    // others): the float constant of the model's texture matrix to set to the identity (Model::uvMatrix), or -1.
    int UvMatrix(uint64_t vertexShader, std::span<const uint64_t> glyphHashes);
    int UvMatrix(std::span<const Texture> textures, uint64_t vertexShader, std::span<const uint64_t> glyphHashes);

    // What a keycap shows for a bound key named `plain` on the keyboard's layout (`shifted` with Shift held; `settingName`,
    // the setting's own name, when the layout gives none): shortened ("Bksp", "Num5"), and a number-row key's digit where
    // the layout types a symbol without Shift (French: "3", not '"').
    std::string KeycapLabel(const char* plain, const char* shifted, const std::string& settingName);
}
