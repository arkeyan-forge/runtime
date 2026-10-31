#pragma once
#include "host/env.h"
#include "thread.h"
#include <cstdint>
#include <cstdio>
#include <ppc_context.h>

// SKY_ALLOC_LOG=1 (test aid, docs/porting/debugging.md): every call of the allocation imports and of the imports the
// title is new to logs its arguments, thread and result. Nothing is read or logged when the variable is not set.
inline bool AllocLogEnabled()
{
    static const bool enabled = skyenv::Get("SKY_ALLOC_LOG") != nullptr;
    return enabled;
}

// `what` names the import; a..d are its meaningful arguments, `result` the value it returns (r3).
inline void AllocLog(const PPCContext& ctx, const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t result)
{
    if (!AllocLogEnabled())
        return;
    auto thread = CurrentGuestThread(ctx.r13.u32);
    fprintf(stderr, "[alloc] %s thread %u: 0x%08X 0x%08X 0x%08X -> 0x%08X%s\n", what, thread ? thread->id : 0, a, b, c,
        result, result == 0 ? "  (ZERO)" : "");
}
