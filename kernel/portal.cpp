// XAM's raw access to "non-controller" devices, which the title uses for the Skylanders Portal of Power. It looks
// both functions up with XexGetProcedureAddress (xam ordinals 0x4A1/0x4A2, see kernel/module.cpp) from
// sub_82615A68 and calls them from its portal thread (sub_8264ABB8 -> sub_826488C0, every 12 ms): one GetRaw per
// poll, then at most one SetRaw with the next command. On the Xbox 360 every report starts with a 2-byte header
// (0B 14 on commands, skipped by the title on input) followed by the portal protocol report. docs/runtime/host/portal.md.
#include "game_hooks.h"
#include "host/portal.h"
#include "memory.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorInvalidParameter = 0x57;
    constexpr uint32_t ErrorDeviceNotConnected = 0x48F;

    constexpr uint8_t Header[2] = { 0x0B, 0x14 };
    constexpr size_t HeaderSize = sizeof(Header);

    // SKY_PORTAL_LOG: one line per command and answer; status reports only when the figures change.
    void LogReport(const char* direction, const uint8_t* report, size_t size)
    {
        static uint8_t lastStatus[5];
        if (size >= 7 && report[0] == 'S')
        {
            uint8_t status[5] = { report[1], report[2], report[3], report[4], report[6] };
            if (memcmp(status, lastStatus, sizeof(status)) == 0)
                return;
            memcpy(lastStatus, status, sizeof(status));
        }

        while (size > 1 && report[size - 1] == 0)
            size--;
        std::string line =
            "[portal] " + std::string(direction) + " " + char(report[0] >= 0x20 && report[0] < 0x7F ? report[0] : '?');
        for (size_t i = 0; i < size; i++)
        {
            char byte[4];
            snprintf(byte, sizeof(byte), " %02X", report[i]);
            line += byte;
        }
        fprintf(stderr, "%s\n", line.c_str());
    }

    // printf for the title's portal log: integer and pointer arguments come from r4..r10.
    std::string FormatGuestString(const char* format, const PPCContext& ctx)
    {
        const uint64_t arguments[] = { ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64,
            ctx.r10.u64 };
        size_t next = 0;
        std::string text;
        for (const char* p = format; *p != '\0'; p++)
        {
            if (*p != '%')
            {
                text += *p;
                continue;
            }

            std::string spec = "%";
            for (p++; *p != '\0' && strchr("-+ #0123456789.", *p) != nullptr; p++)
                spec += *p;
            while (*p == 'l' || *p == 'h')
                p++;
            if (*p == '\0')
                break;

            uint64_t argument = next < std::size(arguments) ? arguments[next] : 0;
            char buffer[256];
            switch (*p)
            {
            case 'd':
            case 'i':
                snprintf(buffer, sizeof(buffer), (spec + 'd').c_str(), int32_t(argument));
                next++;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'c':
                snprintf(buffer, sizeof(buffer), (spec + *p).c_str(), uint32_t(argument));
                next++;
                break;
            case 's':
                snprintf(buffer, sizeof(buffer), (spec + 's').c_str(),
                    uint32_t(argument) != 0 ? static_cast<const char*>(g_memory.Translate(uint32_t(argument)))
                                            : "(null)");
                next++;
                break;
            default: snprintf(buffer, sizeof(buffer), "%c", *p); break;
            }
            text += buffer;
        }
        return text;
    }
}

// XamInputNonControllerGetRaw(DWORD* state, DWORD* length, BYTE* buffer): in, *length is the buffer size (32);
// out, the report and its size. *state is non-zero when the buffer holds a new report (the title reads it as a
// DWORD). Any failure makes the title drop the portal and reset its protocol state.
PPC_FUNC(__imp__XamInputNonControllerGetRaw)
{
    uint32_t state = ctx.r3.u32;
    uint32_t length = ctx.r4.u32;
    uint32_t buffer = ctx.r5.u32;
    uint32_t capacity = length != 0 ? g_memory.Read<uint32_t>(length) : 0;
    if (state == 0 || buffer == 0 || capacity <= HeaderSize || capacity > PortalReportSize)
    {
        ctx.r3.u64 = ErrorInvalidParameter;
        return;
    }
    if (!PortalIsConnected())
    {
        ctx.r3.u64 = ErrorDeviceNotConnected;
        return;
    }

    auto* data = static_cast<uint8_t*>(g_memory.Translate(buffer));
    size_t size = PortalRead(data + HeaderSize, capacity - HeaderSize);
    if (size != 0)
    {
        memcpy(data, Header, HeaderSize);
        if (PortalLogEnabled())
            LogReport("<-", data + HeaderSize, size);
        size += HeaderSize;
    }
    g_memory.Write<uint32_t>(length, uint32_t(size));
    g_memory.Write<uint32_t>(state, size != 0 ? 1 : 0);
    ctx.r3.u64 = ErrorSuccess;
}

// XamInputNonControllerSetRaw(DWORD length, BYTE* buffer): sends one report (header + command).
PPC_FUNC(__imp__XamInputNonControllerSetRaw)
{
    uint32_t size = ctx.r3.u32;
    uint32_t buffer = ctx.r4.u32;
    if (buffer == 0 || size == 0 || size > PortalReportSize)
    {
        ctx.r3.u64 = ErrorInvalidParameter;
        return;
    }

    const auto* data = static_cast<const uint8_t*>(g_memory.Translate(buffer));
    if (size > HeaderSize && PortalLogEnabled())
        LogReport("->", data + HeaderSize, size - HeaderSize);
    bool sent = size <= HeaderSize || PortalWrite(data + HeaderSize, size - HeaderSize);
    ctx.r3.u64 = sent ? ErrorSuccess : ErrorDeviceNotConnected;
}

// The title's portal log (printf-like, format in r3), silent in retail builds: echoed with SKY_PORTAL_LOG.
#ifdef GAME_HOOK_Portal_Log
PPC_FUNC_IMPL(GAME_IMP(Portal_Log));
PPC_FUNC(GAME_HOOK(Portal_Log))
{
    if (PortalLogEnabled())
        fprintf(stderr, "%s", FormatGuestString(static_cast<const char*>(g_memory.Translate(ctx.r3.u32)), ctx).c_str());
    GAME_IMP(Portal_Log)(ctx, base);
}
#endif
