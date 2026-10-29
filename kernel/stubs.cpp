#include <atomic>
#include <cstdio>
#include <ppc_config.h>
#include <ppc_context.h>

// Fallback for every kernel/XAM import the game links against: reports the first call and returns 0.
// Real implementations are strong definitions elsewhere and take precedence at link time.
static void ReportUnimplemented(const char* name, std::atomic_flag& reported, const PPCContext& ctx)
{
    if (!reported.test_and_set())
    {
        fprintf(stderr, "[stub] %s (lr=0x%08X r3=0x%08X r4=0x%08X)\n", name, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32);
    }
}

#define IMPORT(name)                                                                                                   \
    PPC_WEAK_FUNC(__imp__##name)                                                                                       \
    {                                                                                                                  \
        static std::atomic_flag reported;                                                                              \
        ReportUnimplemented(#name, reported, ctx);                                                                     \
        ctx.r3.u64 = 0;                                                                                                \
    }
#include "import_list.inc"
