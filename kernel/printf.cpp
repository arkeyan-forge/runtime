// The kernel's C runtime formatting (kernel/guest_format.h). The game's only vsprintf caller names its save packages
// ("Skylanders - Slot 1").
#include "kernel/guest_format.h"
#include "memory.h"
#include <cstring>
#include <string>
#include <ppc_config.h>
#include <ppc_context.h>

// vsprintf(buffer, format, va_list)
PPC_FUNC(__imp__vsprintf)
{
    // A guest va_list points at the slots.
    uint32_t args = ctx.r5.u32;
    std::string text = FormatGuest(static_cast<const char*>(g_memory.Translate(ctx.r4.u32)), [&] {
        uint64_t value = g_memory.Read<uint64_t>(args);
        args += 8;
        return value;
    });
    memcpy(g_memory.Translate(ctx.r3.u32), text.c_str(), text.size() + 1);
    ctx.r3.u64 = text.size();
}

// _snprintf(buffer, count, format, ...): the variable arguments are r6..r10, then the 8-byte stack slots from r1+0x50
// (the ninth argument's low word is at 0x54). Doubles are read from the same slots (unverified). Like the Microsoft
// CRT: no terminator when the text fills `count`, and -1 when it does not fit.
PPC_FUNC(__imp___snprintf)
{
    uint32_t buffer = ctx.r3.u32;
    uint32_t count = ctx.r4.u32;
    uint64_t registers[] = { ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64 };
    uint32_t index = 0;
    uint32_t stack = ctx.r1.u32 + 0x50;
    std::string text = FormatGuest(static_cast<const char*>(g_memory.Translate(ctx.r5.u32)), [&] {
        if (index < 5)
            return registers[index++];
        uint64_t value = g_memory.Read<uint64_t>(stack);
        stack += 8;
        return value;
    });

    if (text.size() <= count)
    {
        memcpy(g_memory.Translate(buffer), text.c_str(), text.size() < count ? text.size() + 1 : text.size());
        ctx.r3.u64 = text.size();
    }
    else
    {
        memcpy(g_memory.Translate(buffer), text.c_str(), count);
        ctx.r3.u64 = uint64_t(int64_t(-1));
    }
}
