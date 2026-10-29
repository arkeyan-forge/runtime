#include "kernel/objects.h"
#include "thread.h"
#include <cstdio>
#include <mutex>
#include <unordered_set>
#ifndef _WIN32
#include <pthread.h>
#endif

// Called by PPC_CALL_INDIRECT_FUNC (SSARecompLib/indirect_call_guard.h) for a target that is not a recompiled
// function. Logs it once per (caller, target) and skips the call: r3 = 0, as if the callee returned null/false.
extern "C" void SsaBadIndirectCall(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    static std::mutex lock;
    static std::unordered_set<uint64_t> seen;
    static uint64_t count;

    uint32_t lr = uint32_t(ctx.lr);
    uint32_t object = ctx.r3.u32;
    bool first;
    uint64_t total;
    {
        std::lock_guard guard(lock);
        first = seen.insert((uint64_t(lr) << 32) | target).second;
        total = ++count;
    }
    if (first)
    {
        // r3 is usually the callee's `this` and r31 the caller's: their first word is the vtable of a C++ object.
        auto firstWord = [](uint32_t address) {
            return address >= 0x10000 && address < 0xFFFF0000 ? g_memory.Read<uint32_t>(address) : 0;
        };
        uint32_t caller = ctx.r31.u32;
        uint32_t threadId = g_memory.Read<uint32_t>(CurrentKThread(ctx.r13.u32) + 0x14C);
        char name[64] = "";
#ifndef _WIN32
        pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
        fprintf(stderr,
            "[guard] bad indirect call to 0x%08X skipped: lr 0x%08X r3 0x%08X (*r3 0x%08X) r31 0x%08X (*r31 0x%08X) "
            "thread %u \"%s\" (%llu so far)\n",
            target, lr, object, firstWord(object), caller, firstWord(caller), threadId, name,
            (unsigned long long)total);
    }
    ctx.r3.u64 = 0;
}
