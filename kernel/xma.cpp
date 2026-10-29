// The XMA hardware decoder. Like the console, contexts are 64-byte XMA_CONTEXT_DATA blocks in one array in physical
// memory; the title fills them and drives the decoder through little-endian registers at 0x7FEA0000 + 4 * index.
// Those registers are ordinary guest memory here, so the title functions that write them are hooked: each runs
// as is, then the bits it wrote are taken out of the register words and acted upon (apu/xma_context).
#include "apu/xma_context.h"
#include "game_hooks.h"
#include "memory.h"
#include <bitset>
#include <cstdio>
#include <mutex>
#include <ppc_config.h>
#include <ppc_context.h>

PPC_EXTERN_FUNC(__imp__RtlEnterCriticalSection);
PPC_EXTERN_FUNC(__imp__RtlLeaveCriticalSection);

namespace
{
    constexpr uint32_t ContextCount = 320;
    constexpr uint32_t ContextSize = 64;
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorNotEnoughMemory = 8;

    constexpr uint32_t RegisterBase = 0x7FEA0000;
    constexpr uint32_t ContextArrayAddressRegister = RegisterBase + 0x1800;
    // One bit per context, 32 contexts per register word.
    constexpr uint32_t KickRegisters = RegisterBase + 0x1940;
    constexpr uint32_t LockRegisters = RegisterBase + 0x1A40;
    constexpr uint32_t ClearRegisters = RegisterBase + 0x1A80;
    constexpr uint32_t RegisterWords = ContextCount / 32;

    // Held while a hooked writer runs and its register writes are processed, so two threads writing the same
    // register word can't lose each other's bits. Recursive: the create writers call XMACreateContext.
    std::recursive_mutex g_mutex;
    uint32_t g_contexts; // guest address of the array
    std::bitset<ContextCount> g_used;
    XmaContext g_decoders[ContextCount];

    uint32_t ReadRegister(uint32_t address)
    {
        uint32_t value;
        memcpy(&value, g_memory.Translate(address), sizeof(value));
        return value;
    }

    void WriteRegister(uint32_t address, uint32_t value) { memcpy(g_memory.Translate(address), &value, sizeof(value)); }

    // In the 64 KB page window, like the console's (the title's init code reads its physical address back).
    uint32_t ContextArray()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            g_contexts = PhysicalAlloc(ContextCount * ContextSize, 0x10000, 0xA0000000);
            WriteRegister(ContextArrayAddressRegister, g_contexts & 0x1FFFFFFF);
        });
        return g_contexts;
    }

    uint8_t* ContextData(uint32_t index)
    {
        return static_cast<uint8_t*>(g_memory.Translate(ContextArray() + index * ContextSize));
    }

    using ContextSet = std::bitset<ContextCount>;

    // Lock asks the decoder to leave a context alone; kicks decode synchronously, so there is nothing to wait for.
    // `clears` and `kicks`: contexts written to the register whose bit a later write to the same word replaced.
    void ProcessRegisters(const ContextSet& clears = {}, const ContextSet& kicks = {})
    {
        for (uint32_t word = 0; word < RegisterWords; word++)
        {
            uint32_t clear = ReadRegister(ClearRegisters + word * 4);
            uint32_t lock = ReadRegister(LockRegisters + word * 4);
            uint32_t kick = ReadRegister(KickRegisters + word * 4);
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                clear |= uint32_t(clears[word * 32 + bit]) << bit;
                kick |= uint32_t(kicks[word * 32 + bit]) << bit;
            }
            if ((clear | lock | kick) == 0)
                continue;

            WriteRegister(ClearRegisters + word * 4, 0);
            WriteRegister(LockRegisters + word * 4, 0);
            WriteRegister(KickRegisters + word * 4, 0);
            for (uint32_t bit = 0; bit < 32; bit++)
            {
                uint32_t index = word * 32 + bit;
                if (clear & (1u << bit))
                    g_decoders[index].Clear(ContextData(index));
                if (kick & (1u << bit))
                    g_decoders[index].Kick(ContextData(index));
            }
        }
    }

    // A voice of the XDK's XMA voice code (both engines; the writers' argument): the context count at +0, then at +8 a
    // pointer to one 96-byte record per context, holding the hardware context's address at +64 (0 until created) and
    // its index at +80 (u16). Returns the contexts the records name; a record that doesn't match the array is skipped.
    ContextSet VoiceContexts(uint32_t voice)
    {
        constexpr uint32_t RecordSize = 96;
        constexpr uint32_t MaxRecords = 64;
        ContextSet contexts;
        if (voice == 0)
            return contexts;
        uint32_t count = g_memory.Read<uint32_t>(voice);
        uint32_t records = g_memory.Read<uint32_t>(voice + 8);
        if (records == 0 || count > MaxRecords)
            return contexts;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t record = records + i * RecordSize;
            uint32_t context = g_memory.Read<uint32_t>(record + 64);
            uint32_t index = g_memory.Read<uint16_t>(record + 80);
            if (context != 0 && index < ContextCount && context == ContextArray() + index * ContextSize)
                contexts[index] = true;
        }
        return contexts;
    }

    // What a hooked writer writes to the registers. The writers store `1 << (index & 31)` to the word of each context
    // of the voice in turn: on the console every store reaches the decoder, but here the registers are memory read once
    // the writer returns, so of several contexts sharing a word only the last would be seen (a 4-context voice then
    // never decodes 3 of its contexts). The voice's records say which contexts the writer stored for.
    enum class Writes
    {
        Lock,     // nothing to do for a lock
        Kick,     // a kick for every context of the voice
        Create,   // a clear for every context the writer created (its record's address was 0 before)
    };

    template <void (*Original)(PPCContext&, uint8_t*), Writes writes>
    void CallWriter(PPCContext& ctx, uint8_t* base)
    {
        std::lock_guard lock(g_mutex);
        uint32_t voice = ctx.r3.u32;
        ContextSet before;
        if constexpr (writes == Writes::Create)
            before = VoiceContexts(voice);
        Original(ctx, base);
        if constexpr (writes == Writes::Kick)
            ProcessRegisters({}, VoiceContexts(voice));
        else if constexpr (writes == Writes::Create)
            ProcessRegisters(VoiceContexts(voice) & ~before);
        else
            ProcessRegisters();
    }
}

// XMACreateContext(XMA_CONTEXT_DATA** context)
PPC_FUNC(__imp__XMACreateContext)
{
    std::lock_guard lock(g_mutex);
    uint32_t array = ContextArray();
    for (uint32_t i = 0; i < ContextCount; i++)
    {
        if (g_used[i])
            continue;

        g_used[i] = true;
        memset(ContextData(i), 0, ContextSize);
        g_decoders[i].Reset();
        g_memory.Write<uint32_t>(ctx.r3.u32, array + i * ContextSize);
        ctx.r3.u64 = ErrorSuccess;
        return;
    }

    fprintf(stderr, "[xma] out of contexts\n");
    ctx.r3.u64 = ErrorNotEnoughMemory;
}

PPC_FUNC(__imp__XMAReleaseContext)
{
    std::lock_guard lock(g_mutex);
    uint32_t index = (ctx.r3.u32 - ContextArray()) / ContextSize;
    if (index < ContextCount)
    {
        g_used[index] = false;
        g_decoders[index].Reset();
    }
    ctx.r3.u64 = ErrorSuccess;
}

// The title has two copies of the XMA voice code: engine 1 (0x825xxxxx, used by its XAudio2) and engine 2
// (0x826xxxxx, used by Wwise). Each reads ContextArrayAddress once at init, so the array must exist before.
#ifdef GAME_HOOK_Xma1_Init
PPC_FUNC_IMPL(GAME_IMP(Xma1_Init));
PPC_FUNC(GAME_HOOK(Xma1_Init))
{
    ContextArray();
    GAME_IMP(Xma1_Init)(ctx, base);
}
#endif

#ifdef GAME_HOOK_Xma2_Init
PPC_FUNC_IMPL(GAME_IMP(Xma2_Init));
PPC_FUNC(GAME_HOOK(Xma2_Init))
{
    ContextArray();
    GAME_IMP(Xma2_Init)(ctx, base);
}
#endif

// The register writers: voice group create (Clear), lock and kick, for each engine.
#ifdef GAME_HOOK_Xma1_VoiceClear
PPC_FUNC_IMPL(GAME_IMP(Xma1_VoiceClear));
PPC_FUNC(GAME_HOOK(Xma1_VoiceClear)) { CallWriter<GAME_IMP(Xma1_VoiceClear), Writes::Create>(ctx, base); }
#endif

#ifdef GAME_HOOK_Xma1_VoiceKick
PPC_FUNC_IMPL(GAME_IMP(Xma1_VoiceKick));
PPC_FUNC(GAME_HOOK(Xma1_VoiceKick)) { CallWriter<GAME_IMP(Xma1_VoiceKick), Writes::Kick>(ctx, base); }
#endif

#ifdef GAME_HOOK_Xma2_VoiceClear
PPC_FUNC_IMPL(GAME_IMP(Xma2_VoiceClear));
PPC_FUNC(GAME_HOOK(Xma2_VoiceClear)) { CallWriter<GAME_IMP(Xma2_VoiceClear), Writes::Create>(ctx, base); }
#endif

#ifdef GAME_HOOK_Xma2_VoiceLock
PPC_FUNC_IMPL(GAME_IMP(Xma2_VoiceLock));
PPC_FUNC(GAME_HOOK(Xma2_VoiceLock)) { CallWriter<GAME_IMP(Xma2_VoiceLock), Writes::Lock>(ctx, base); }
#endif

#ifdef GAME_HOOK_Xma2_VoiceKick
PPC_FUNC_IMPL(GAME_IMP(Xma2_VoiceKick));
PPC_FUNC(GAME_HOOK(Xma2_VoiceKick)) { CallWriter<GAME_IMP(Xma2_VoiceKick), Writes::Kick>(ctx, base); }
#endif

// Engine 1 locks all its voices inside its critical section. Take that first, so the order is always the guest's
// lock then ours: a thread holding the critical section can then call the other writers without deadlocking.
#ifdef GAME_HOOK_Xma1_LockAll
PPC_FUNC_IMPL(GAME_IMP(Xma1_LockAll));
PPC_FUNC(GAME_HOOK(Xma1_LockAll))
{
    PPCRegister r3 = ctx.r3;
    ctx.r3.u64 = game::Engine1CriticalSection;
    __imp__RtlEnterCriticalSection(ctx, base);
    ctx.r3 = r3;

    CallWriter<GAME_IMP(Xma1_LockAll), Writes::Lock>(ctx, base);

    r3 = ctx.r3;
    ctx.r3.u64 = game::Engine1CriticalSection;
    __imp__RtlLeaveCriticalSection(ctx, base);
    ctx.r3 = r3;
}
#endif
