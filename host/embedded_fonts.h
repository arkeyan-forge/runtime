#pragma once

// The Toy Box's fonts, built into the executable by cmake/embed_fonts.cmake (all SIL OFL 1.1): Outfit SemiBold and Bold
// (static instances of resources/launcher/fonts/Outfit-Variable.ttf, in resources/toybox/fonts) and Bungee.
#include <cstddef>

extern const unsigned char g_embeddedOutfitBold[];
extern const size_t g_embeddedOutfitBoldSize;
extern const unsigned char g_embeddedOutfitSemiBold[];
extern const size_t g_embeddedOutfitSemiBoldSize;
extern const unsigned char g_embeddedBungee[];
extern const size_t g_embeddedBungeeSize;
