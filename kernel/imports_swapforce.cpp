// Kernel and XAM imports that Skylanders: Swap Force links against and the earlier games do not, and that have no
// family file of their own: Xbox Live UI (gamer card, player review, marketplace), XAM tasks, Live tokens and XAuth,
// gamer pictures, and the console key's signatures. The console here is offline with one local profile: Live calls
// fail as if it were not signed in to Live, UI calls show nothing and report success. Signatures follow Xenia (BSD)
// where it has them; calls whose arguments are not documented log their first use.
#include "kernel/objects.h"
#include "kernel/overlapped.h"
#include "loader.h"
#include "memory.h"
#include "thread.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorInvalidParameter = 87;
    constexpr uint32_t ErrorNotFound = 0x490;
    constexpr uint32_t ErrorNotLoggedOn = 0x4DD;

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

    // XE_CONSOLE_SIGNATURE: XE_CONSOLE_CERTIFICATE (0x1A8 bytes) then a 0x80-byte signature, as in Xenia (BSD).
    constexpr uint32_t ConsoleSignatureSize = 0x228;
    constexpr uint32_t CertificateSize = 0x1A8;
    constexpr uint32_t CertificateConsoleType = 0x18;
    constexpr uint32_t CertificateManufactureDate = 0x1C;
    constexpr uint32_t RetailConsole = 2;
    constexpr uint32_t HashSize = 20; // the SHA-1 digest the title signs

    // There is no console key: a signature is the console certificate and the signed hash itself, so that this
    // console verifies what it signed (and nothing else).
    void MakeSignature(const uint8_t* hash, uint8_t* signature)
    {
        memset(signature, 0, ConsoleSignatureSize);
        signature[0] = uint8_t(CertificateSize >> 8);
        signature[1] = uint8_t(CertificateSize & 0xFF);
        signature[CertificateConsoleType + 3] = RetailConsole;
        const uint8_t date[8] = { 2, 0, 0, 5, 1, 1, 2, 2 }; // Xenia's
        memcpy(signature + CertificateManufactureDate, date, sizeof(date));
        memcpy(signature + CertificateSize, hash, HashSize);
    }
}

// Xbox Live UI. No guide is drawn; each call returns at once as if the player closed it.

// XamShowGamerCardUIForXUID(user index, xuid, 0)
PPC_FUNC(__imp__XamShowGamerCardUIForXUID)
{
    LOG_FIRST_CALL("XamShowGamerCardUIForXUID");
    ctx.r3.u64 = ErrorSuccess;
}

// XamShowPlayerReviewUI(user index, xuid)
PPC_FUNC(__imp__XamShowPlayerReviewUI)
{
    LOG_FIRST_CALL("XamShowPlayerReviewUI");
    ctx.r3.u64 = ErrorSuccess;
}

// XamShowMarketplaceUI(user index, entry point, offer id, offer type, content categories, title id), as in Xenia (BSD)
PPC_FUNC(__imp__XamShowMarketplaceUI)
{
    LOG_FIRST_CALL("XamShowMarketplaceUI");
    ctx.r3.u64 = ErrorSuccess;
}

// XAM tasks: work XAM runs for the title on a thread of its own.

// XamTaskSchedule(callback, message, task attributes, handle out): runs callback(message) on a new thread at once, as
// Xenia (BSD) does, with the title's stack size. The handle is the thread's; XamTaskCloseHandle closes it.
PPC_FUNC(__imp__XamTaskSchedule)
{
    uint32_t callback = ctx.r3.u32;
    uint32_t message = ctx.r4.u32;
    uint32_t handlePointer = ctx.r6.u32;

    uint32_t stackSize = std::max(0x4000u, (g_module.stackSize + 0xFFF) & ~0xFFFu);
    auto thread = CreateGuestThread(callback, message, 0, stackSize, false, 0);
    fprintf(stderr, "[thread] %u created by XamTaskSchedule: callback 0x%08X message 0x%08X\n", thread->id, callback,
        message);
    if (handlePointer != 0)
        g_memory.Write<uint32_t>(handlePointer, CreateHandle(thread));
    ctx.r3.u64 = 0;
}

// XamTaskCloseHandle(handle): TRUE, or FALSE for a handle that is not open.
PPC_FUNC(__imp__XamTaskCloseHandle) { ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? 1 : 0; }

// XamTaskShouldExit(): the system never asks a task to stop early.
PPC_FUNC(__imp__XamTaskShouldExit) { ctx.r3.u64 = 0; }

// Xbox Live authentication. The title starts XAuth after XHttp, then asks for a token for a URL; offline, XAuth does
// not start and there is never a token.

// XampXAuthStartup(XAUTH_SETTINGS*): the failure codes Xenia (BSD) returns: 0x80158401 for settings of the wrong
// size, 0x80158406 otherwise. The title logs the failure and leaves its online service off.
PPC_FUNC(__imp__XampXAuthStartup)
{
    LOG_FIRST_CALL("XampXAuthStartup");
    bool valid = ctx.r3.u32 != 0 && g_memory.Read<uint32_t>(ctx.r3.u32) == 8;
    ctx.r3.u64 = valid ? 0x80158406 : 0x80158401;
}

// XampXAuthShutdown(count out): no XAuth user is left.
PPC_FUNC(__imp__XampXAuthShutdown)
{
    if (ctx.r3.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r3.u32, 0);
}

// XampXAuthGetTitleBuffer(): the buffer XampXAuthStartup would have kept; none.
PPC_FUNC(__imp__XampXAuthGetTitleBuffer) { ctx.r3.u64 = 0; }

// XamGetToken(xauth context, url, url length, token out, overlapped?): not in Xenia; read from the title's call
// (sub_82C8D488), which turns a positive result into an HRESULT and frees the token with XamFreeToken. The token out
// is cleared and the call fails at once with ERROR_NOT_LOGGED_ON, without touching the fifth argument (likely an
// XOVERLAPPED, unconfirmed: a synchronous failure leaves it alone on the console too).
PPC_FUNC(__imp__XamGetToken)
{
    LOG_FIRST_CALL("XamGetToken");
    if (ctx.r6.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r6.u32, 0);
    ctx.r3.u64 = ErrorNotLoggedOn;
}

// XamFreeToken(token)
PPC_FUNC(__imp__XamFreeToken) { LOG_FIRST_CALL("XamFreeToken"); }

// Gamer pictures. The local profile has none.

// XamParseGamerTileKey(XUSER_DATA* key, title id out, big tile id out, small tile id out), signature as in Xenia (BSD):
// no key names a picture here, so every id is 0 and the call fails.
PPC_FUNC(__imp__XamParseGamerTileKey)
{
    for (uint32_t out : { ctx.r4.u32, ctx.r5.u32, ctx.r6.u32 })
    {
        if (out != 0)
            g_memory.Write<uint32_t>(out, 0);
    }
    ctx.r3.u64 = ErrorInvalidParameter;
}

// XamReadTileToTexture(tile type, title id, tile id (64-bit), user index, buffer, stride, height, overlapped),
// signature as in Xenia (BSD): the texture is cleared (transparent) and the call fails with ERROR_NOT_FOUND.
PPC_FUNC(__imp__XamReadTileToTexture)
{
    LOG_FIRST_CALL("XamReadTileToTexture");
    uint32_t buffer = ctx.r7.u32;
    if (buffer != 0)
        memset(g_memory.Translate(buffer), 0, size_t(ctx.r8.u32) * ctx.r9.u32);
    ctx.r3.u64 = FinishXamCall(ctx.r10.u32, ErrorNotFound, 0, ctx.r13.u32);
}

// The console's private key. XAPI signs and checks a small file with it (sub_82F42CB0 verifies, sub_82F42D30 signs a
// SHA-1 of 88 bytes and writes certificate and signature in front of them).

// XeKeysConsolePrivateKeySign(hash, XE_CONSOLE_SIGNATURE* out): TRUE. The certificate is Xenia's (BSD) fixed retail one.
PPC_FUNC(__imp__XeKeysConsolePrivateKeySign)
{
    if (ctx.r3.u32 == 0 || ctx.r4.u32 == 0)
    {
        ctx.r3.u64 = 0;
        return;
    }
    MakeSignature(static_cast<const uint8_t*>(g_memory.Translate(ctx.r3.u32)),
        static_cast<uint8_t*>(g_memory.Translate(ctx.r4.u32)));
    ctx.r3.u64 = 1;
}

// XeKeysConsoleSignatureVerification(hash, XE_CONSOLE_SIGNATURE*, compare result out): TRUE, with a non-zero result
// when the signature is the one this console makes for the hash (the title reads a non-zero result as valid).
PPC_FUNC(__imp__XeKeysConsoleSignatureVerification)
{
    uint32_t match = 0;
    if (ctx.r3.u32 != 0 && ctx.r4.u32 != 0)
    {
        uint8_t expected[ConsoleSignatureSize];
        MakeSignature(static_cast<const uint8_t*>(g_memory.Translate(ctx.r3.u32)), expected);
        match = memcmp(expected, g_memory.Translate(ctx.r4.u32), ConsoleSignatureSize) == 0 ? 1 : 0;
    }
    if (ctx.r5.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r5.u32, match);
    ctx.r3.u64 = 1;
}
