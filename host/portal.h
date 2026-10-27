#pragma once

#include "host/figures/figure_data.h"
#include <cstddef>
#include <cstdint>
#include <string>

// The Skylanders "Portal of Power". Every portal model speaks the same command protocol: reports of at most
// 32 bytes whose first byte is a command letter (R reset, A activate, S status, Q query block, W write block,
// C colour...). The Xbox 360's 2-byte report header is added and removed by the XAM layer (kernel/portal.cpp).
// See docs/runtime/host/portal.md.

constexpr size_t PortalReportSize = 32;

// Picks the portal (SSA_PORTAL=auto|real|hid|usb|emulated|none, default auto: a real portal plugged in at launch,
// otherwise the emulated one; real waits for a real portal to be plugged in) and installs the figure hotkeys. The
// emulated portal starts as the last session left it (portal.txt in the save folder; empty the first time), or with
// the figures listed in SSA_PORTAL_PLACE instead. Call after SDL_Init.
void PortalInit();

// False when no portal is attached; the title then asks the player to connect one.
bool PortalIsConnected();

// Sends one command. Returns false if the portal went away.
bool PortalWrite(const uint8_t* command, size_t size);

// Copies the next report from the portal and returns its size, 0 if none is pending. Never blocks.
size_t PortalRead(uint8_t* report, size_t capacity);

// Emulated portal only: puts the figure dump at `path` on the portal (written back when the title saves
// progress to it). Returns the slot, or -1.
int PortalPlaceFigure(const std::string& path);
// The same in `slot` if it is free (a swap: remove, then place in the same slot, so the title sees the toy change in
// one status report), else in the first free slot.
int PortalPlaceFigureAt(const std::string& path, int slot);
void PortalRemoveFigure(int slot);
// Takes every figure off (F8, the Toy Box's "Clear portal").
void PortalRemoveAllFigures();

// Emulated portal only (false / empty otherwise).
constexpr int PortalSlotCount = 16;
bool PortalIsEmulated();
// Path of the dump in `slot`, empty if the slot is free.
std::string PortalSlotFigure(int slot);
// Where figure dumps live: the shared collection (collection::Folder(): SSA_COLLECTION, SSA_FIGURES, else
// <user data>/Skylanders/Collection; docs/runtime/host/collection.md).
std::string PortalFigureFolder();

// What the title saw of each slot, from the traffic (any portal, real or emulated): the status reports' presence and
// the blocks it read or wrote since the toy arrived. The Toy Box uses it to tell when the title took a figure, and to
// show a real portal's toys.
struct PortalSlotSeen
{
    bool present = false;
    uint64_t arrived = 0;   // SDL_GetTicksNS() of the status report that showed it (0: unknown)
    uint64_t lastRead = 0;  // of the last block read from it, 0 if none yet
    uint64_t blocks = 0;    // bit n: block n of `data` is known
    FigureData data{};
};
PortalSlotSeen PortalSeen(int slot);

// The colour of the portal's light as the title last set it with `C r g b` (any portal, real or emulated). Giants
// fades it through the colours of the wheel; it is the only light command seen so far (docs/runtime/host/portal.md).
struct PortalLightState
{
    bool set = false;       // the title has sent at least one colour
    uint8_t r = 0, g = 0, b = 0;
    uint64_t changed = 0;   // SDL_GetTicksNS() of the last colour command
    uint64_t commands = 0;  // how many colour commands since the start
};
PortalLightState PortalLight();
// True if the colour is not black.
inline bool PortalLightOn(const PortalLightState& light) { return light.set && (light.r | light.g | light.b) != 0; }

// --portal-probe: looks for real portals on every transport (HID, USB), opens and queries each one, prints one JSON
// object per line on stdout, then {"done":true,"count":N}. Returns the exit code (0). Contract: docs/runtime/host/portal_probe.md.
int PortalProbeMain();

// SSA_PORTAL_LOG=1 traces portal traffic and the title's own portal log.
bool PortalLogEnabled();
