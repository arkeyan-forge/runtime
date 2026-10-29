#pragma once

#include "host/input.h"
#include <cstdint>
#include <string_view>

// Pad remapping and rumble strength (docs/runtime/xbox360/input.md, "Remapping and rumble"). A game's [<game>.input]
// holds `pad_<button>=<button>` (the button that does the job of the first; `none` turns the first off) and
// `rumble_p1` / `rumble_p2` (off, low, normal, strong). Applied to the real pads only, never to the keyboard.
constexpr int PadButtonCount = 16;

// The names of the settings, in the order of PadMap::target.
extern const char* const g_padButtons[PadButtonCount];

// The XINPUT_GAMEPAD mask of a button (0 for the triggers: they are analog).
uint16_t PadButtonMask(int button);

struct PadMap
{
    int8_t target
        [PadButtonCount]; // what each physical button does: its own job, another button's (index), or -1 (nothing)
    PadMap()
    {
        for (int i = 0; i < PadButtonCount; i++)
            target[i] = int8_t(i);
    }
    bool Identity() const;
};

// The index of a button name in g_padButtons, -1 when it is none.
int PadButtonIndex(std::string_view name);

// The state the title sees for a pad's physical state. A trigger counts as pressed past 30 (an analog value that is not
// remapped stays analog; a button that becomes a trigger pulls it all the way).
GamepadState RemapPad(const GamepadState& pad, const PadMap& map);

// Rumble strength: "off", "low", "normal" (what the game asks) or "strong". Anything else is normal.
uint16_t ScaleRumble(uint16_t motor, std::string_view strength);
