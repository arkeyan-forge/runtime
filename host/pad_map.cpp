#include "host/pad_map.h"
#include <algorithm>

const char* const g_padButtons[PadButtonCount] = { "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back", "ls", "rs",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right" };

uint16_t PadButtonMask(int button)
{
    static const uint16_t masks[PadButtonCount] = { Button::A, Button::B, Button::X, Button::Y, Button::LeftShoulder,
        Button::RightShoulder, 0, 0, Button::Start, Button::Back, Button::LeftThumb, Button::RightThumb, Button::DpadUp,
        Button::DpadDown, Button::DpadLeft, Button::DpadRight };
    return button >= 0 && button < PadButtonCount ? masks[button] : 0;
}

bool PadMap::Identity() const
{
    for (int i = 0; i < PadButtonCount; i++)
        if (target[i] != i)
            return false;
    return true;
}

int PadButtonIndex(std::string_view name)
{
    for (int i = 0; i < PadButtonCount; i++)
        if (name == g_padButtons[i])
            return i;
    return -1;
}

GamepadState RemapPad(const GamepadState& pad, const PadMap& map)
{
    if (map.Identity())
        return pad;
    constexpr int LT = 6, RT = 7;
    constexpr uint8_t TriggerPressed = 30;
    auto trigger = [](GamepadState& state, int button) -> uint8_t& { return button == LT ? state.leftTrigger : state.rightTrigger; };

    GamepadState out = pad;
    for (int source = 0; source < PadButtonCount; source++) // what moved away
    {
        if (map.target[source] == source)
            continue;
        if (source == LT || source == RT)
            trigger(out, source) = 0;
        else
            out.buttons &= uint16_t(~PadButtonMask(source));
    }
    for (int source = 0; source < PadButtonCount; source++) // what arrives
    {
        const int target = map.target[source];
        if (target == source || target < 0)
            continue;
        const bool isTrigger = source == LT || source == RT;
        uint8_t value = isTrigger ? (source == LT ? pad.leftTrigger : pad.rightTrigger) : 0;
        if (isTrigger ? value <= TriggerPressed : (pad.buttons & PadButtonMask(source)) == 0)
            continue;
        if (target == LT || target == RT)
            trigger(out, target) = std::max<uint8_t>(trigger(out, target), isTrigger ? value : 255);
        else
            out.buttons |= PadButtonMask(target);
    }
    return out;
}

uint16_t ScaleRumble(uint16_t motor, std::string_view strength)
{
    if (strength == "off") return 0;
    if (strength == "low") return uint16_t(motor * 2 / 5);
    if (strength == "strong") return uint16_t(std::min<uint32_t>(0xFFFF, uint32_t(motor) * 8 / 5));
    return motor;
}
