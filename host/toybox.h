#pragma once

// The Toy Box (docs/runtime/host/toybox_ux.md): the portal's seats, My Skylanders, figure management, and the quick swap wheel
// over the game. Gamepad: hold View for the wheel (a tap still reaches the game), View + Y for the Toy Box; keyboard:
// hold Tab for the wheel, tap Tab or F6 for the Toy Box. While either is open the title gets no input from that pad.
// Drawn with Dear ImGui on the CPU (host/imgui_software.h) into one image that gpu::SetOverlay blends over the game.
#include "host/input.h"

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

// [portal] toybox_auto_open (default on): when the game asks for a figure (its "place a Skylander on the portal" screen),
// the Toy Box opens by itself on that player's seat; it closes again when the prompt goes away. Emulated portal only.
void ToyboxSetAutoOpen(bool enabled);
