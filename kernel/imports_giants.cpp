// Kernel and XAM imports that Skylanders: Giants links against and Skylanders: Spyro's Adventure does not: the pool
// allocator, the I/O manager's device and share-access calls, APCs, SHA-1, and the title loader's launch data (with Swap
// Force's XamLoaderSetLaunchData, which shares its storage).
// Signatures follow Xenia (BSD) where it has them. Calls whose arguments are not documented log their first use.
#include "kernel/objects.h"
#include "memory.h"
#include "thread.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusInvalidDeviceRequest = 0xC0000010;
    constexpr uint32_t ErrorNotFound = 0x490;

    // Logs the first call of each import, with its first argument registers.
#define LOG_FIRST_CALL(name)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        static std::atomic_flag reported;                                                                              \
        if (!reported.test_and_set())                                                                                  \
        {                                                                                                              \
            fprintf(stderr,                                                                                            \
                "[kernel] " name                                                                                       \
                " (lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X)\n",                \
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32); \
        }                                                                                                              \
    } while (0)

    // DEVICE_OBJECT stand-in: zeroed, with room for the device extension.
    constexpr uint32_t DeviceObjectSize = 0x100;

    // SHARE_ACCESS: OpenCount, Readers, Writers, Deleters, SharedRead, SharedWrite, SharedDelete.
    constexpr uint32_t ShareAccessSize = 28;

    // KAPC: u8 type, u8 mode, u8 enqueued, u8 pad, u32 thread, LIST_ENTRY (8), u32 kernel routine, rundown routine,
    // normal routine, normal context, argument 1, argument 2.
    constexpr uint32_t ApcMode = 1;
    constexpr uint32_t ApcEnqueued = 2;
    constexpr uint32_t ApcThread = 4;
    constexpr uint32_t ApcKernelRoutine = 0x10;
    constexpr uint32_t ApcRundownRoutine = 0x14;
    constexpr uint32_t ApcNormalRoutine = 0x18;
    constexpr uint32_t ApcNormalContext = 0x1C;
    constexpr uint32_t ApcArgument1 = 0x20;
    constexpr uint32_t ApcArgument2 = 0x24;
    constexpr uint8_t ApcType = 18;
    constexpr uint8_t UserMode = 1;

    // SHA-1 (FIPS 180-1), written for this file.
    struct Sha1
    {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        uint8_t block[64];
        uint32_t used = 0;
        uint64_t total = 0;

        static uint32_t Rotate(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

        void Process()
        {
            uint32_t w[80];
            for (int i = 0; i < 16; i++)
                w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 |
                       uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
            for (int i = 16; i < 80; i++)
                w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; i++)
            {
                uint32_t f, k;
                if (i < 20)
                {
                    f = (b & c) | (~b & d);
                    k = 0x5A827999;
                }
                else if (i < 40)
                {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                }
                else if (i < 60)
                {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                }
                else
                {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                uint32_t t = Rotate(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = Rotate(b, 30);
                b = a;
                a = t;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
        }

        void Update(const uint8_t* data, uint32_t length)
        {
            total += length;
            while (length > 0)
            {
                uint32_t n = std::min(length, 64 - used);
                memcpy(block + used, data, n);
                used += n;
                data += n;
                length -= n;
                if (used == 64)
                {
                    Process();
                    used = 0;
                }
            }
        }

        void Finish(uint8_t digest[20])
        {
            uint64_t bits = total * 8;
            uint8_t pad = 0x80;
            Update(&pad, 1);
            pad = 0;
            while (used != 56)
                Update(&pad, 1);
            uint8_t size[8];
            for (int i = 0; i < 8; i++)
                size[i] = uint8_t(bits >> (56 - i * 8));
            Update(size, 8);
            for (int i = 0; i < 20; i++)
                digest[i] = uint8_t(h[i / 4] >> (24 - (i % 4) * 8));
        }
    };

    uint32_t AllocateDevice(uint32_t extensionSize) { return g_memory.SystemAlloc(DeviceObjectSize + extensionSize); }
}

// Pool memory

// ExAllocatePoolTypeWithTag(size, tag, pool type) and ExAllocatePoolWithTag(size, tag): zeroed guest memory.
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag) { ctx.r3.u64 = g_virtualHeap4K.Alloc(std::max<uint32_t>(ctx.r3.u32, 1)); }

PPC_FUNC(__imp__ExAllocatePoolWithTag) { ctx.r3.u64 = g_virtualHeap4K.Alloc(std::max<uint32_t>(ctx.r3.u32, 1)); }

PPC_FUNC(__imp__ExFreePool)
{
    if (ctx.r3.u32 != 0)
        g_virtualHeap4K.Free(ctx.r3.u32);
}

// Objects

// Every object the title holds is its own.
PPC_FUNC(__imp__ObReferenceObject) { ctx.r3.u64 = StatusSuccess; }

PPC_FUNC(__imp__ObIsTitleObject) { ctx.r3.u64 = 1; }

// I/O manager. There is no hardware: devices are inert objects.

// IoCreateDevice(driver, extension size, name*, device type, characteristics, exclusive, DEVICE_OBJECT** out): the
// 360 kernel's argument list is not confirmed, so the out pointer is taken from r9 (the NT order) unless r8 holds it.
PPC_FUNC(__imp__IoCreateDevice)
{
    LOG_FIRST_CALL("IoCreateDevice");
    uint32_t out = ctx.r9.u32 >= 0x10000 && ctx.r8.u32 <= 1 ? ctx.r9.u32 : ctx.r8.u32;
    if (out >= 0x10000)
        g_memory.Write<uint32_t>(out, AllocateDevice(ctx.r4.u32));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__IoDeleteDevice)
{
    LOG_FIRST_CALL("IoDeleteDevice");
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__IoInvalidDeviceRequest)
{
    LOG_FIRST_CALL("IoInvalidDeviceRequest");
    ctx.r3.u64 = StatusInvalidDeviceRequest;
}

PPC_FUNC(__imp__IoCompleteRequest) { LOG_FIRST_CALL("IoCompleteRequest"); }

// IoSetShareAccess(desired access, desired share access, file object, SHARE_ACCESS*): one open, nothing shared.
PPC_FUNC(__imp__IoSetShareAccess)
{
    LOG_FIRST_CALL("IoSetShareAccess");
    if (ctx.r6.u32 != 0)
    {
        memset(g_memory.Translate(ctx.r6.u32), 0, ShareAccessSize);
        g_memory.Write<uint32_t>(ctx.r6.u32, 1);
    }
}

// IoCheckShareAccess(desired access, desired share access, file object, SHARE_ACCESS*, update): always compatible.
PPC_FUNC(__imp__IoCheckShareAccess)
{
    LOG_FIRST_CALL("IoCheckShareAccess");
    if (ctx.r7.u32 != 0 && ctx.r6.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r6.u32, g_memory.Read<uint32_t>(ctx.r6.u32) + 1);
    ctx.r3.u64 = StatusSuccess;
}

// IoRemoveShareAccess(file object, SHARE_ACCESS*)
PPC_FUNC(__imp__IoRemoveShareAccess)
{
    LOG_FIRST_CALL("IoRemoveShareAccess");
    if (ctx.r4.u32 != 0 && g_memory.Read<uint32_t>(ctx.r4.u32) != 0)
        g_memory.Write<uint32_t>(ctx.r4.u32, g_memory.Read<uint32_t>(ctx.r4.u32) - 1);
}

// The storage, microphone and remote-control device drivers have no hardware behind them here. StfsCreateDevice's
// arguments are not documented, so it creates nothing and reports success.
PPC_FUNC(__imp__StfsCreateDevice)
{
    LOG_FIRST_CALL("StfsCreateDevice");
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__StfsControlDevice)
{
    LOG_FIRST_CALL("StfsControlDevice");
    ctx.r3.u64 = StatusInvalidDeviceRequest;
}

PPC_FUNC(__imp__MicDeviceRequest)
{
    LOG_FIRST_CALL("MicDeviceRequest");
    ctx.r3.u64 = StatusInvalidDeviceRequest;
}

PPC_FUNC(__imp__RmcDeviceRequest)
{
    LOG_FIRST_CALL("RmcDeviceRequest");
    ctx.r3.u64 = StatusInvalidDeviceRequest;
}

// APCs

// KeInitializeApc(apc, thread, kernel routine, rundown routine, normal routine, processor mode, normal context)
PPC_FUNC(__imp__KeInitializeApc)
{
    uint32_t apc = ctx.r3.u32;
    uint32_t normalRoutine = ctx.r7.u32;
    memset(g_memory.Translate(apc), 0, 0x28);
    g_memory.Write<uint8_t>(apc, ApcType);
    g_memory.Write<uint32_t>(apc + ApcThread, ctx.r4.u32);
    g_memory.Write<uint32_t>(apc + ApcKernelRoutine, ctx.r5.u32);
    g_memory.Write<uint32_t>(apc + ApcRundownRoutine, ctx.r6.u32);
    g_memory.Write<uint32_t>(apc + ApcNormalRoutine, normalRoutine);
    if (normalRoutine != 0)
    {
        g_memory.Write<uint8_t>(apc + ApcMode, uint8_t(ctx.r8.u32));
        g_memory.Write<uint32_t>(apc + ApcNormalContext, ctx.r9.u32);
    }
}

// KeInsertQueueApc(apc, argument 1, argument 2, priority increment): a user-mode APC with a normal routine runs on
// its thread during an alertable wait; the kernel routine is not run. Returns TRUE if it was queued.
PPC_FUNC(__imp__KeInsertQueueApc)
{
    uint32_t apc = ctx.r3.u32;
    uint32_t normalRoutine = g_memory.Read<uint32_t>(apc + ApcNormalRoutine);
    auto thread = std::dynamic_pointer_cast<GuestThread>(LookupDispatcher(g_memory.Read<uint32_t>(apc + ApcThread)));
    if (g_memory.Read<uint8_t>(apc + ApcEnqueued) != 0 || normalRoutine == 0 || thread == nullptr ||
        g_memory.Read<uint8_t>(apc + ApcMode) != UserMode)
    {
        LOG_FIRST_CALL("KeInsertQueueApc: not a queueable user APC");
        ctx.r3.u64 = 0;
        return;
    }

    g_memory.Write<uint32_t>(apc + ApcArgument1, ctx.r4.u32);
    g_memory.Write<uint32_t>(apc + ApcArgument2, ctx.r5.u32);
    g_memory.Write<uint8_t>(apc + ApcEnqueued, 1);
    QueueUserApc(*thread, { normalRoutine, g_memory.Read<uint32_t>(apc + ApcNormalContext), ctx.r4.u32, ctx.r5.u32 });
    ctx.r3.u64 = 1;
}

// Crypto

// XeCryptSha(data 1, length 1, data 2, length 2, data 3, length 3, digest, digest size): SHA-1 of the concatenation
// of the (up to three) buffers, cut to the digest size.
PPC_FUNC(__imp__XeCryptSha)
{
    Sha1 sha;
    uint32_t inputs[3][2] = { { ctx.r3.u32, ctx.r4.u32 }, { ctx.r5.u32, ctx.r6.u32 }, { ctx.r7.u32, ctx.r8.u32 } };
    for (auto& [address, length] : inputs)
    {
        if (address != 0 && length != 0)
            sha.Update(static_cast<const uint8_t*>(g_memory.Translate(address)), length);
    }

    uint8_t digest[20];
    sha.Finish(digest);
    if (ctx.r9.u32 != 0)
        memcpy(g_memory.Translate(ctx.r9.u32), digest, std::min<uint32_t>(ctx.r10.u32, sizeof(digest)));
}

// Title loader

namespace
{
    // Launch data: what XamLoaderSetLaunchData stored (at most MAX_LAUNCH_DATA_SIZE bytes) for the next title. Only
    // this one runs, so a later XamLoaderGetLaunchData in this process reads it back, as in Xenia (BSD).
    constexpr uint32_t MaxLaunchDataSize = 0x400;
    std::mutex g_launchDataLock;
    std::vector<uint8_t> g_launchData;
}

// XamLoaderSetLaunchData(data, size)
PPC_FUNC(__imp__XamLoaderSetLaunchData)
{
    uint32_t size = std::min(ctx.r4.u32, MaxLaunchDataSize);
    std::lock_guard lock(g_launchDataLock);
    if (ctx.r3.u32 == 0)
        size = 0;
    auto* data = size != 0 ? static_cast<const uint8_t*>(g_memory.Translate(ctx.r3.u32)) : nullptr;
    g_launchData.assign(data, data + size);
    ctx.r3.u64 = 0;
}

// XamLoaderGetLaunchDataSize(size out): 0 and ERROR_NOT_FOUND when the title was started from the dashboard, with no
// launch data.
PPC_FUNC(__imp__XamLoaderGetLaunchDataSize)
{
    std::lock_guard lock(g_launchDataLock);
    if (ctx.r3.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r3.u32, uint32_t(g_launchData.size()));
    ctx.r3.u64 = g_launchData.empty() ? ErrorNotFound : 0;
}

// XamLoaderGetLaunchData(buffer, buffer size)
PPC_FUNC(__imp__XamLoaderGetLaunchData)
{
    std::lock_guard lock(g_launchDataLock);
    if (ctx.r3.u32 != 0 && ctx.r4.u32 != 0)
    {
        auto* buffer = static_cast<uint8_t*>(g_memory.Translate(ctx.r3.u32));
        uint32_t copied = std::min<uint32_t>(ctx.r4.u32, uint32_t(g_launchData.size()));
        memset(buffer, 0, ctx.r4.u32);
        if (copied != 0)
            memcpy(buffer, g_launchData.data(), copied);
    }
    ctx.r3.u64 = g_launchData.empty() ? ErrorNotFound : 0;
}
