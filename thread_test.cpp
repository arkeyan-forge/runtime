// The guest threads without a game (thread.cpp, kernel/objects.cpp, indirect_call_guard.cpp): host calls into the guest,
// thread memory over many create/exit cycles, the dispatcher table. Built with the game's ppc_config.h (its code range);
// no game code runs, the test puts its own functions in the function table.
//   ./SSAGuestThreadTest             Exit code 0 = pass.
#include "kernel/objects.h"
#include "loader.h"
#include "memory.h"
#include "thread.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

LoadedModule g_module;

namespace
{
    int g_checks = 0, g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            g_failures++;                                                                                              \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);                                     \
        }                                                                                                              \
    } while (0)

    constexpr uint32_t TlsMark = 0xC0FFEE01;
    constexpr uint32_t StackSize = 0x40000; // 2000 of them: twice the 256 MB system heap

    std::atomic<int> g_started{ 0 }, g_dirtyStarts{ 0 }, g_apcs{ 0 };

    // A thread start: its TLS must come initialised from the template, its dynamic slots and stack zeroed, even on
    // blocks a previous thread dirtied. Exits with context + 1.
    void Start(PPCContext& ctx, uint8_t*)
    {
        g_started++;
        uint32_t pcr = ctx.r13.u32, tls = g_memory.Read<uint32_t>(pcr);
        uint32_t stackTop = g_memory.Read<uint32_t>(pcr + 0x70);
        if (g_memory.Read<uint32_t>(tls) != TlsMark || g_memory.Read<uint32_t>(tls + g_module.tlsDataSize) != 0 ||
            g_memory.Read<uint32_t>(stackTop - 0x100) != 0 || ctx.r1.u32 != stackTop)
            g_dirtyStarts++;
        g_memory.Write<uint32_t>(tls, 0xBAD);
        g_memory.Write<uint32_t>(tls + g_module.tlsDataSize, 0xBAD);
        g_memory.Write<uint32_t>(stackTop - 0x100, 0xBAD);
        ctx.r3.u64 = ctx.r3.u64 + 1;
    }

    void Exit(PPCContext&, uint8_t*) { ExitGuestThread(7); }

    void Apc(PPCContext& ctx, uint8_t*) { g_apcs += int(ctx.r3.u32); }

    // What `body` writes to stderr.
    template <typename Body>
    std::string Stderr(Body body)
    {
        fflush(stderr);
        FILE* capture = tmpfile();
        int saved = dup(2);
        dup2(fileno(capture), 2);
        body();
        fflush(stderr);
        dup2(saved, 2);
        close(saved);
        std::string text;
        rewind(capture);
        for (int c; (c = fgetc(capture)) != EOF;)
            text += char(c);
        fclose(capture);
        return text;
    }

    size_t Count(const std::string& text, const std::string& what)
    {
        size_t count = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
            count++;
        return count;
    }

    void Wait(const std::shared_ptr<GuestThread>& thread) { WaitForObjects({ thread }, false, 0, 0, nullptr); }
}

namespace
{
    // An object whose destructor looks a handle up, as a file's last reference committing a save does with the table:
    // destroyed under the table's lock, that lookup would deadlock.
    struct LooksUpOnClose : KernelObject
    {
        uint32_t other = 0;
        bool* found = nullptr;
        ~LooksUpOnClose() override { *found = LookupHandle(other) != nullptr; }
    };
}

int main()
{
    g_memory.Reserve();
    {
        bool found = false;
        auto keep = std::make_shared<LooksUpOnClose>();
        const uint32_t other = CreateHandle(std::make_shared<KernelObject>());
        auto closing = std::make_shared<LooksUpOnClose>();
        closing->other = other;
        closing->found = &found;
        keep->found = &found;
        keep->other = other;
        const uint32_t handle = CreateHandle(std::move(closing));
        CHECK(CloseHandle(handle) && found); // the destructor ran after the lock was released
        CHECK(!CloseHandle(handle) && CloseHandle(other));
    }
    const uint32_t start = PPC_CODE_BASE, exit = PPC_CODE_BASE + 4, apc = PPC_CODE_BASE + 8;
    PPC_LOOKUP_FUNC(g_memory.base, start) = Start;
    PPC_LOOKUP_FUNC(g_memory.base, exit) = Exit;
    PPC_LOOKUP_FUNC(g_memory.base, apc) = Apc;
    g_module.tlsDataSize = 0x10;
    g_module.tlsSlotCount = 4;
    g_module.tlsRawDataSize = 4;
    g_module.tlsRawData = g_memory.SystemAlloc(4);
    g_memory.Write<uint32_t>(g_module.tlsRawData, TlsMark);

    // Host calls: a target in the code runs; one outside it (a corrupt callback, misaligned, past the end) is refused,
    // r3 = 0, logged once per target. Unchecked, the table read there gives a garbage host pc.
    PPCContext ctx{};
    ctx.r3.u64 = 41;
    CHECK(CallGuest(ctx, apc) && g_apcs == 41);
    std::string log = Stderr([&] {
        ctx.r3.u64 = 5;
        CHECK(!CallGuest(ctx, 0x12345678) && ctx.r3.u64 == 0);
        CHECK(!CallGuest(ctx, 0x12345678));
        CHECK(!CallGuest(ctx, apc + 2));
        CHECK(!CallGuest(ctx, uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE)));
        CHECK(!CallGuest(ctx, 0));
    });
    CHECK(Count(log, "refused") == 4 && log.find("0x12345678") != std::string::npos);
    CHECK(GuestCodeAddress(start) && GuestCodeAddress(uint32_t(PPC_CODE_BASE + PPC_CODE_SIZE - 4)) &&
          !GuestCodeAddress(uint32_t(PPC_CODE_BASE - 4)));

    // A thread started at an address outside the code, and an APC whose routine is: refused, no crash.
    log = Stderr([&] {
        auto bad = CreateGuestThread(0x00001000, 5, 0, StackSize, false, 0);
        Wait(bad);
        CHECK(bad->exited && bad->exitCode == 0);
        GuestThread target;
        QueueUserApc(target, { 0xDEAD0000, 1, 0, 0 });
        QueueUserApc(target, { apc, 1, 0, 0 });
        DeliverUserApcs(ctx, target);
    });
    CHECK(Count(log, "refused") == 2 && g_apcs == 42);

    // 2000 threads created and ended, one after the other (returning, or through ExitGuestThread): their blocks are
    // reused, the 256 MB system heap ("System heap exhausted" after about 1000) is never full.
    std::weak_ptr<GuestThread> first;
    int good = 0;
    for (uint32_t i = 0; i < 2000; i++)
    {
        auto thread = CreateGuestThread(i % 2 ? exit : start, i, 0, StackSize, false, 0);
        if (i == 0)
            first = thread;
        Wait(thread);
        good += thread->exitCode == (i % 2 ? 7 : i + 1);
    }
    CHECK(good == 2000 && g_started == 1000 && g_dirtyStarts == 0);
    // The first thread's KTHREAD went to a later thread, which replaced it in the dispatcher table.
    CHECK(first.expired());

    // A dispatcher header initialised again (the title's inlined KeInitializeEvent) gets a new host object, which
    // replaces the old one: the table kept one more object at each re-initialisation.
    uint32_t header = g_memory.SystemAlloc(0x10);
    g_memory.Write<uint8_t>(header, 1); // synchronization event
    std::weak_ptr<KernelObject> event = LookupDispatcher(header);
    CHECK(!event.expired() && LookupDispatcher(header) == event.lock());
    memset(g_memory.Translate(header), 0, 0x10);
    auto again = LookupDispatcher(header);
    CHECK(again != nullptr && event.expired());

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
