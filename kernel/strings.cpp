#include "memory.h"
#include <algorithm>
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusBufferOverflow = 0x80000005;

    // ANSI_STRING / UNICODE_STRING: u16 Length (bytes), u16 MaximumLength, u32 Buffer.
    void InitString(uint32_t string, uint32_t buffer, uint16_t length, uint16_t terminator)
    {
        g_memory.Write<uint16_t>(string, buffer != 0 ? length : 0);
        g_memory.Write<uint16_t>(string + 2, buffer != 0 ? length + terminator : 0);
        g_memory.Write<uint32_t>(string + 4, buffer);
    }

    // The console's wide characters are UTF-16 big-endian; non-ASCII becomes '?'.
    char NarrowChar(uint16_t c)
    {
        return c < 0x80 ? char(c) : '?';
    }
}

PPC_FUNC(__imp__RtlInitAnsiString)
{
    uint32_t source = ctx.r4.u32;
    uint16_t length = source != 0 ? uint16_t(strlen(static_cast<const char*>(g_memory.Translate(source)))) : 0;
    InitString(ctx.r3.u32, source, length, 1);
}

PPC_FUNC(__imp__RtlInitUnicodeString)
{
    uint32_t source = ctx.r4.u32;
    uint16_t length = 0;
    if (source != 0)
    {
        while (g_memory.Read<uint16_t>(source + length) != 0)
            length += 2;
    }
    InitString(ctx.r3.u32, source, length, 2);
}

// Strings from RtlUnicodeStringToAnsiString live in the never-reclaimed system heap.
PPC_FUNC(__imp__RtlFreeAnsiString) {}

PPC_FUNC(__imp__RtlUnicodeStringToAnsiString)
{
    uint32_t destination = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    bool allocate = ctx.r5.u32 != 0;

    uint16_t length = g_memory.Read<uint16_t>(source) / 2;
    uint32_t sourceBuffer = g_memory.Read<uint32_t>(source + 4);

    uint32_t buffer;
    uint16_t capacity;
    if (allocate)
    {
        capacity = length + 1;
        buffer = g_memory.SystemAlloc(capacity);
        InitString(destination, buffer, length, 1);
    }
    else
    {
        capacity = g_memory.Read<uint16_t>(destination + 2);
        buffer = g_memory.Read<uint32_t>(destination + 4);
    }

    uint32_t status = StatusSuccess;
    if (length >= capacity)
    {
        length = capacity > 0 ? capacity - 1 : 0;
        status = StatusBufferOverflow;
    }

    for (uint16_t i = 0; i < length; i++)
        g_memory.Write<uint8_t>(buffer + i, NarrowChar(g_memory.Read<uint16_t>(sourceBuffer + i * 2)));
    if (capacity > 0)
        g_memory.Write<uint8_t>(buffer + length, 0);
    g_memory.Write<uint16_t>(destination, length);

    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    uint32_t destination = ctx.r3.u32;
    uint32_t maxBytes = ctx.r4.u32;
    uint32_t bytesWrittenPointer = ctx.r5.u32;
    uint32_t source = ctx.r6.u32;
    uint32_t sourceBytes = ctx.r7.u32;

    uint32_t count = std::min(maxBytes / 2, sourceBytes);
    for (uint32_t i = 0; i < count; i++)
        g_memory.Write<uint16_t>(destination + i * 2, g_memory.Read<uint8_t>(source + i));

    if (bytesWrittenPointer != 0)
        g_memory.Write<uint32_t>(bytesWrittenPointer, count * 2);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    uint32_t destination = ctx.r3.u32;
    uint32_t maxBytes = ctx.r4.u32;
    uint32_t bytesWrittenPointer = ctx.r5.u32;
    uint32_t source = ctx.r6.u32;
    uint32_t sourceBytes = ctx.r7.u32;

    uint32_t count = std::min(maxBytes, sourceBytes / 2);
    for (uint32_t i = 0; i < count; i++)
        g_memory.Write<uint8_t>(destination + i, NarrowChar(g_memory.Read<uint16_t>(source + i * 2)));

    if (bytesWrittenPointer != 0)
        g_memory.Write<uint32_t>(bytesWrittenPointer, count);
    ctx.r3.u64 = StatusSuccess;
}

// Returns how many leading bytes match the repeated 32-bit pattern.
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    uint32_t source = ctx.r3.u32;
    uint32_t length = ctx.r4.u32 & ~3u;
    uint32_t pattern = ctx.r5.u32;

    uint32_t matched = 0;
    while (matched < length && g_memory.Read<uint32_t>(source + matched) == pattern)
        matched += 4;
    ctx.r3.u64 = matched;
}

PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    uint32_t destination = ctx.r3.u32;
    uint32_t length = ctx.r4.u32 & ~3u;
    for (uint32_t offset = 0; offset < length; offset += 4)
        g_memory.Write<uint32_t>(destination + offset, ctx.r5.u32);
}

PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    uint32_t status = ctx.r3.u32;
    uint32_t error;
    switch (status)
    {
    case 0x00000000: error = 0; break;    // ERROR_SUCCESS
    case 0x00000102: error = 258; break;  // WAIT_TIMEOUT
    case 0xC0000008: error = 6; break;    // ERROR_INVALID_HANDLE
    case 0xC000000D: error = 87; break;   // ERROR_INVALID_PARAMETER
    case 0xC000000F: error = 2; break;    // ERROR_FILE_NOT_FOUND
    case 0xC0000011: error = 38; break;   // ERROR_HANDLE_EOF
    case 0xC0000017: error = 8; break;    // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022: error = 5; break;    // ERROR_ACCESS_DENIED
    case 0xC0000034: error = 2; break;    // ERROR_FILE_NOT_FOUND
    case 0xC000003A: error = 3; break;    // ERROR_PATH_NOT_FOUND
    case 0xC0000262: error = 182; break;  // ERROR_INVALID_ORDINAL
    default:
        // Win32 errors wrapped as HRESULTs carry their code; anything else has no DOS equivalent.
        error = (status & 0xFFFF0000) == 0x80070000 ? status & 0xFFFF : 317; // ERROR_MR_MID_NOT_FOUND
        break;
    }
    ctx.r3.u64 = error;
}

// RtlCompareStringN(string 1, length 1, string 2, length 2, case insensitive), as Xenia declares it: raw byte
// strings with their lengths. Returns the difference of the first bytes that differ (ASCII upper-cased when
// insensitive), else the difference of the lengths.
PPC_FUNC(__imp__RtlCompareStringN)
{
    const auto* first = static_cast<const uint8_t*>(g_memory.Translate(ctx.r3.u32));
    uint32_t firstLength = ctx.r4.u32;
    const auto* second = static_cast<const uint8_t*>(g_memory.Translate(ctx.r5.u32));
    uint32_t secondLength = ctx.r6.u32;
    bool insensitive = ctx.r7.u32 != 0;

    auto fold = [&](uint8_t c) -> int { return insensitive && c >= 'a' && c <= 'z' ? c - 0x20 : c; };
    int32_t result = int32_t(firstLength) - int32_t(secondLength);
    for (uint32_t i = 0; i < std::min(firstLength, secondLength); i++)
    {
        if (fold(first[i]) != fold(second[i]))
        {
            result = fold(first[i]) - fold(second[i]);
            break;
        }
    }
    ctx.r3.s64 = result;
}

// RtlUpcaseUnicodeChar(wchar): ASCII and Latin-1 (plus U+00FF -> U+0178); other characters are returned unchanged.
PPC_FUNC(__imp__RtlUpcaseUnicodeChar)
{
    uint16_t c = ctx.r3.u16;
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        c -= 0x20;
    else if (c == 0xFF)
        c = 0x178;
    ctx.r3.u64 = c;
}
