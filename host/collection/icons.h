#pragma once

// A picture for any toy: the one the player's own disc has (cached as PNG by the game, docs/runtime/host/figures.md), else one drawn
// here: a shape per category in the element's colour. Variants get a badge (Legendary star, Dark crescent...).
// Nothing is shipped or downloaded: the shapes are code.
#include "catalogue.h"
#include <cstdint>
#include <string>
#include <vector>

namespace collection
{
    struct Image
    {
        int width = 0, height = 0;
        std::vector<uint32_t> rgba; // R in the low byte, straight (not premultiplied) alpha

        bool Empty() const { return rgba.empty(); }
    };

    // The mark Spyro's Adventure's menus give a toy family (magic items: a potion; adventure packs: a compass; minis and
    // sidekicks: a paw): a signed distance (negative inside) in a [-1, 1] square, y down. Skylauncher draws it over its
    // own icon shapes, so both apps show the same mark. Other families have none (KindGlyph returns 1).
    bool HasKindGlyph(Category category);
    float KindGlyph(Category category, float x, float y);

    // The category's shape on a transparent square (on an opaque rounded tile with `tile`).
    Image GeneratedIcon(const Toy* toy, int size, bool tile = false);
    // A round badge for the variant kind in the bottom right corner; nothing for Base.
    void AddBadge(Image& image, VariantKind kind);

    // A toy's picture from one game's disc, as cached by art::Extract (art/game_art.h): the picture of its variant
    // (`variant`: the variant id, 0 for the plain toy) if the game has one, else the toy's. `large`: the game's biggest
    // (SSA: the 512 px "versus" picture; Giants has one size, 256 px). Empty when that game has none cached.
    // Folders: SKY_PORTRAITS, else "portraits" in the game's data folder.
    std::string GamePortraitPath(const std::string& gameId, const Toy& toy, uint16_t variant = 0, bool large = false);

    // A toy family's picture from one game's disc (Game::kindPictures: SSA's magic item, location and sidekick icons),
    // empty when that game has none cached.
    std::string GameKindPicturePath(const std::string& gameId, Category category);

    // The rule for every picture of a toy (docs/runtime/host/game_art.md): the toy's art from the game being viewed
    // (`gameId`), else from another game (catalogue order), else its family's picture from those games in the same
    // order, else empty, and the caller draws the generated icon.
    std::string PortraitPath(const std::string& gameId, const Toy& toy, uint16_t variant = 0, bool large = false);
    // The best picture of a toy or variant from the discs (docs/runtime/host/game_art.md): the variant's own picture
    // before the plain toy's, and for each the largest: the game's artwork (Giants' versus art, about 1000x960, not
    // square; <portraits>/artworks), then its portrait. The game being viewed first, then the others in catalogue
    // order. Empty: the caller draws the generated icon. PortraitPath stays the square picture for icons and cards.
    std::string BestPicturePath(const std::string& gameId, const Toy& toy, uint16_t variant = 0);
    // When only the kind is known (the Toy Box, the collection's cards): the toy's one variant of that kind, else 0.
    uint16_t VariantFor(const Toy& toy, VariantKind kind);
    // One half of a swapper from the disc (Swap Force's swap screen pictures, Game::halves): a top above its waist or
    // a bottom's legs, the variant's own before the plain toy's, from the first game that has one. Empty when none.
    std::string HalfPicturePath(const Toy& toy, uint16_t variant = 0);
    // `image` cut to its visible pixels (alpha above `minAlpha`: its bounding box); unchanged when none is visible.
    Image CropToVisible(const Image& image, uint8_t minAlpha = 8);
    // The two halves of a swapper joined, as the game's swap screen shows them: each half is drawn in place on one shared
    // square frame (the sizes differ between toys: both frames are brought to the larger one's size first), the top
    // composited over the bottom (straight alpha), the result cut to its visible box. No stacking, no rescaling of a half
    // on its own. Empty when either half has no visible pixel.
    Image JoinHalves(const Image& top, const Image& bottom);
    // A swapper's picture: its two halves' pictures from the discs (HalfPicturePath) joined. Empty when either half has
    // none (the caller falls back to the top's picture with the bottom small in a corner). The same picture in the
    // launcher, the figure sheet, Add a figure, the seats and the Toy Box.
    Image SwapperPicture(const Toy& top, uint16_t topVariant, const Toy& bottom, uint16_t bottomVariant);
    // PortraitPath with no game being viewed: the first game, in catalogue order, that has one.
    std::string DiscImagePath(const Toy& toy);

    // The picture PortraitPath finds, else the generated one, `size` pixels square, with the badge of `kind`.
    Image Portrait(const std::string& gameId, const Toy* toy, VariantKind kind, int size, uint16_t variant = 0, bool* fromDisc = nullptr);
    // Portrait with no game being viewed.
    Image Icon(const Toy* toy, VariantKind kind, int size, bool* fromDisc = nullptr);

    Image LoadPng(const std::string& path);
    bool SavePng(const Image& image, const std::string& path);
    Image Resize(const Image& image, int width, int height);
}
