#pragma once

// The Toy Box (docs/runtime/host/toybox_ux.md): the portal's seats, My Skylanders, figure management, and the quick swap wheel
// over the game. Gamepad: hold View for the wheel (a tap still reaches the game), View + Y for the Toy Box; keyboard:
// hold Tab for the wheel, tap Tab or F6 for the Toy Box. While either is open the title gets no input from that pad.
// F3 (View + Start on a pad) opens the game menu: volume, FPS counter, the settings a player changes in play, Report a problem
// (host/gamemenu.h); it takes the input as the Toy Box does.
// Drawn with Dear ImGui on the CPU (host/imgui_software.h) into one image that gpu::SetOverlay blends over the game.
#include "host/input.h"
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event; // no SDL header: the UWP host includes this file and SDL's headers refuse its API family

// After PortalInit and the window. With a real portal the seats are read-only (what the title reads from the toys).
void ToyboxInit(SDL_Window* window);

// Main thread, for every event: true when the menu used it (the title must not see it).
bool ToyboxHandleEvent(const SDL_Event& event);

// Main thread, with the physical gamepad state of each frame: takes what the Toy Box and the wheel use out of `state`
// (everything while one is open, View while it may become the wheel) and gives back a tap of View.
void ToyboxFilterGamepad(GamepadState& state);

// Main thread, once per frame: runs SKY_TOYBOX_SCRIPT and redraws the menu when it changed.
void ToyboxUpdate();

// The F3 menu's Commands page waits for a pad button: the main loop then gives ToyboxFilterGamepad the pad as it is (no
// remapping, no keyboard), so the button pressed is the physical one.
bool ToyboxWaitsForPadButton();

// The F3 menu, the Toy Box or its wheel is open (including while the Commands page waits for a key or a button): the
// game does not hold the pointer and gets no click.
bool ToyboxOverlayOpen();
// The Commands page waits for a mouse button or a wheel tick: the next click or tick is the page's.
bool ToyboxWaitsForMouseButton();
// The Toy Box's keys (SDL scancode names, RuntimeSettings::toyboxKeys: open it, add the next figure, clear the portal) and
// whether Tab opens it too (toyboxTab). Main thread; the Commands page sets them again when it changes them.
void ToyboxSetKeys(const std::string (&keys)[3], bool tab);
// The current wheel hold key, as printed on the player's keyboard (also used by the Toy Box's hints).
std::string ToyboxWheelKeyName();

// The keyboard as the game's input reads it: SDL_GetKeyboardState, plus the keys SKY_TOYBOX_SCRIPT holds ("hold=K").
const bool* ToyboxKeyboardState();

// [portal] toybox_auto_open (default on): when the game asks for a figure (its "place a Skylander on the portal" screen),
// the Toy Box opens by itself on that player's seat; it closes again when the prompt goes away. Emulated portal only.
void ToyboxSetAutoOpen(bool enabled);

// Any thread: a short message over the game (the Toy Box's toast, 4 s), in English or French by the game's text language.
// Nothing is shown when the Toy Box is unavailable (no window, or its fonts did not load).
void ToyboxNotice(const char* english, const char* french);

// Any thread: an achievement unlocked, as a toast with its picture for 6 s: its name (in the game's text language), its
// points and its picture as the title's PNG (empty: none). Decoded on the main thread, once.
void ToyboxAchievement(const std::string& name, uint32_t points, std::vector<uint8_t> png);

// SKY_TOYBOX_SCRIPT's "achievement" (test aid) calls `preview`: the achievements' side (kernel/xam_achievements.cpp)
// announces the title's first achievement as if unlocked, without saving it.
void ToyboxSetAchievementPreview(void (*preview)());

// One achievement as the overlay's Achievements page lists it: the title's own texts in its text language.
struct ToyboxAchievementRow
{
    std::string name, description; // the description unlocked, or the one the title shows while locked
    uint32_t points = 0;
    int64_t unlocked = 0; // unix time of the unlock; 0: locked
    bool secret = false;  // locked and hidden by the title until unlocked: no name or description is shown
};
// The achievements' side (kernel/xam_achievements.cpp) gives the page the title's list and the player's unlocks;
// main thread, when the page opens (and when the F3 menu shows the count).
void ToyboxSetAchievementList(std::vector<ToyboxAchievementRow> (*list)());
std::vector<ToyboxAchievementRow> ToyboxAchievements(); // empty when there is no list (no source, or none in the title)

// A level started (game.cpp NoteLevel): the figures that ran out of health can play again.
void ToyboxLevelStarted();
