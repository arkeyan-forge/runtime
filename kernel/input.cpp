// XAM input for the controllers of users 0 and 1 (Player 1 and 2). Other user slots, and user 1 without a device, report none.
#include "host/env.h"
#include "host/input.h"
#include "memory.h"
#include <cstdio>
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorDeviceNotConnected = 0x48F;

    constexpr uint8_t DeviceTypeGamepad = 1;
    constexpr uint8_t DeviceSubTypeGamepad = 1;

    void WriteGamepad(uint32_t address, const GamepadState& state)
    {
        g_memory.Write<uint16_t>(address + 0, state.buttons);
        g_memory.Write<uint8_t>(address + 2, state.leftTrigger);
        g_memory.Write<uint8_t>(address + 3, state.rightTrigger);
        g_memory.Write<int16_t>(address + 4, state.thumbLX);
        g_memory.Write<int16_t>(address + 6, state.thumbLY);
        g_memory.Write<int16_t>(address + 8, state.thumbRX);
        g_memory.Write<int16_t>(address + 10, state.thumbRY);
    }
}

// XamInputGetCapabilities(user, flags, X_INPUT_CAPABILITIES*): type, sub type, flags, gamepad, vibration.
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    if (!GamepadConnected(int(ctx.r3.u32)))
    {
        ctx.r3.u64 = ErrorDeviceNotConnected;
        return;
    }

    uint32_t capabilities = ctx.r5.u32;
    memset(g_memory.Translate(capabilities), 0, 0x14);
    g_memory.Write<uint8_t>(capabilities + 0, DeviceTypeGamepad);
    g_memory.Write<uint8_t>(capabilities + 1, DeviceSubTypeGamepad);
    WriteGamepad(capabilities + 4, { 0xF3FF, 0xFF, 0xFF, -1, -1, -1, -1 }); // every input is present
    g_memory.Write<uint16_t>(capabilities + 0x10, 0xFFFF);
    g_memory.Write<uint16_t>(capabilities + 0x12, 0xFFFF);
    ctx.r3.u64 = ErrorSuccess;
}

// XamInputGetState(user, flags, X_INPUT_STATE*): packet number, gamepad.
PPC_FUNC(__imp__XamInputGetState)
{
    if (!GamepadConnected(int(ctx.r3.u32)))
    {
        ctx.r3.u64 = ErrorDeviceNotConnected;
        return;
    }

    uint32_t packetNumber;
    GamepadState state = GetGamepad(packetNumber, int(ctx.r3.u32));
    // SKY_INPUT_LOG=1: what the title reads, when it changes (the pad remapping test, tools/regress/pad_qol.sh).
    static const bool log = skyenv::Get("SKY_INPUT_LOG") != nullptr;
    static GamepadState last[MaxUsers];
    if (log && ctx.r3.u32 < MaxUsers && memcmp(&last[ctx.r3.u32], &state, sizeof(state)) != 0)
    {
        last[ctx.r3.u32] = state;
        fprintf(stderr, "[input] user %u: the title reads buttons 0x%04X, triggers %u/%u\n", ctx.r3.u32, state.buttons,
            state.leftTrigger, state.rightTrigger);
    }
    g_memory.Write<uint32_t>(ctx.r5.u32, packetNumber);
    WriteGamepad(ctx.r5.u32 + 4, state);
    ctx.r3.u64 = ErrorSuccess;
}

// XamInputSetState(user, flags, X_INPUT_VIBRATION*): the two motors, u16 each; the host sends them to the user's pad.
PPC_FUNC(__imp__XamInputSetState)
{
    const int user = int(ctx.r3.u32);
    if (!GamepadConnected(user))
    {
        ctx.r3.u64 = ErrorDeviceNotConnected;
        return;
    }
    if (ctx.r5.u32 != 0)
    {
        const uint16_t left = g_memory.Read<uint16_t>(ctx.r5.u32), right = g_memory.Read<uint16_t>(ctx.r5.u32 + 2);
        static const bool log = skyenv::Get("SKY_INPUT_LOG") != nullptr;
        static uint16_t last[MaxUsers][2];
        if (log && user < MaxUsers && (last[user][0] != left || last[user][1] != right))
        {
            last[user][0] = left;
            last[user][1] = right;
            fprintf(stderr, "[input] user %d: the title asks for vibration %u/%u\n", user, unsigned(left), unsigned(right));
        }
        SetHostVibration(user, left, right);
    }
    ctx.r3.u64 = ErrorSuccess;
}

// XamInputGetKeystrokeEx(user index*, flags, X_KEYSTROKE*): there is no keyboard, so there is never a keystroke.
PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    constexpr uint32_t ErrorEmpty = 0x10D2;
    if (ctx.r5.u32 != 0)
        memset(g_memory.Translate(ctx.r5.u32), 0, 8);
    ctx.r3.u64 = ErrorEmpty;
}
