#pragma once

#include <cstdint>

// XINPUT_GAMEPAD as the title sees it.
struct GamepadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
};

namespace Button
{
    constexpr uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
    constexpr uint16_t Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080;
    constexpr uint16_t LeftShoulder = 0x0100, RightShoulder = 0x0200;
    constexpr uint16_t A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
}

// Two controllers: user 0 (the keyboard, the first gamepad) and user 1 (the second gamepad, Player 2). User 1 is
// reported to the title only once it has a device (SetHostGamepad for it, or a SSA_PRESS2 script).
constexpr int MaxUsers = 2;

// Called on the host's main thread with the latest physical state (gamepad, keyboard) of a user. `connected` false removes
// the device of user 1 (user 0 is always there).
void SetHostGamepad(const GamepadState& state, int user = 0, bool connected = true);

// Whether the title sees a controller for this user.
bool GamepadConnected(int user);

// The state the title reads for a controller, and a counter that changes whenever it does.
GamepadState GetGamepad(uint32_t& packetNumber, int user = 0);

// SSA_PRESS="start:6,a:9,lup:20:3" presses a button for 150 ms at each time (seconds since launch), for unattended
// runs; a third field holds it that many seconds. lup/ldown/lleft/lright push the left stick all the way. SSA_PRESS2 is
// the same script for the second controller (user 1), which then exists.
void LoadInputScript(const char* script, int user = 0);
