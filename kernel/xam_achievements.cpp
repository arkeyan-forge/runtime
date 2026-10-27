// Achievements for the offline profile. Unlocks the title writes (XUserWriteAchievements, a request to the system's
// achievement app 0xFB) are accepted and logged; enumerations list nothing, as no achievement data is kept yet. The
// title only enumerates to check whether every other achievement is unlocked (sub_8264ACF8), so it awards nothing more.
#include "kernel/overlapped.h"
#include "kernel/xam_enumerator.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorInvalidHandle = 6;
    constexpr uint32_t ErrorNoMoreFiles = 18;
    constexpr uint32_t ErrorInsufficientBuffer = 122;
    constexpr uint32_t ErrorNoSuchUser = 0x525;
    constexpr uint32_t Failure = 0x80004005; // E_FAIL, for requests to apps nothing implements

    constexpr uint32_t AchievementApp = 0xFB;
    constexpr uint32_t WriteAchievements = 0x000B0008; // { DWORD count, XUSER_ACHIEVEMENT* { user index, id } }

    constexpr uint32_t AchievementDetailsSize = 0x24; // XACHIEVEMENT_DETAILS
}

// XMsgStartIORequest(app, message, XOVERLAPPED*, buffer, buffer size). Requests to the music player go through the
// Ex variant (kernel/xmp.cpp); this one carries the title's achievement unlocks.
PPC_FUNC(__imp__XMsgStartIORequest)
{
    uint32_t app = ctx.r3.u32;
    uint32_t message = ctx.r4.u32;
    uint32_t overlapped = ctx.r5.u32;
    uint32_t buffer = ctx.r6.u32;

    if (app != AchievementApp || message != WriteAchievements)
    {
        fprintf(stderr, "[xmsg] app 0x%X message 0x%08X not supported\n", app, message);
        ctx.r3.u64 = Failure;
        return;
    }

    uint32_t count = g_memory.Read<uint32_t>(buffer);
    uint32_t achievements = g_memory.Read<uint32_t>(buffer + 4);
    for (uint32_t i = 0; i < count; i++)
    {
        fprintf(stderr, "[achievement] user %u unlocked achievement %u\n", g_memory.Read<uint32_t>(achievements + i * 8),
            g_memory.Read<uint32_t>(achievements + i * 8 + 4));
    }
    ctx.r3.u64 = FinishXamCall(overlapped, ErrorSuccess, 0, ctx.r13.u32);
}

// XamUserCreateAchievementEnumerator(title id, user index, xuid, detail flags, starting index, count, buffer size out,
// handle out)
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator)
{
    uint32_t userIndex = ctx.r4.u32;
    uint32_t count = ctx.r8.u32;
    uint32_t bufferSizePointer = ctx.r9.u32;
    uint32_t handlePointer = ctx.r10.u32;

    if (userIndex != 0 && userIndex != 0xFF) // the signed-in profile, or any user
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }
    if (bufferSizePointer != 0)
        g_memory.Write<uint32_t>(bufferSizePointer, count * AchievementDetailsSize);
    auto enumerator = std::make_shared<XamEnumerator>();
    enumerator->itemSize = AchievementDetailsSize;
    enumerator->itemsPerEnumerate = count;
    g_memory.Write<uint32_t>(handlePointer, CreateHandle(enumerator));
    ctx.r3.u64 = ErrorSuccess;
}

// XamEnumerate(handle, flags, buffer, buffer size, items returned out, XOVERLAPPED*): the next items that fit the
// buffer, ERROR_NO_MORE_FILES once none are left. With an XOVERLAPPED, its length is the item count (as in Xenia, BSD).
PPC_FUNC(__imp__XamEnumerate)
{
    uint32_t buffer = ctx.r5.u32;
    uint32_t bufferSize = ctx.r6.u32;
    uint32_t itemsReturned = ctx.r7.u32;
    uint32_t overlapped = ctx.r8.u32;

    uint32_t result = ErrorInvalidHandle;
    uint32_t count = 0;
    if (auto enumerator = LookupHandleAs<XamEnumerator>(ctx.r3.u32))
    {
        uint32_t total = enumerator->itemSize != 0 ? uint32_t(enumerator->items.size() / enumerator->itemSize) : 0;
        uint32_t left = total - std::min(enumerator->next, total);
        if (left == 0)
        {
            result = ErrorNoMoreFiles;
        }
        else
        {
            count = std::min(left, bufferSize / enumerator->itemSize);
            if (enumerator->itemsPerEnumerate != 0)
                count = std::min(count, enumerator->itemsPerEnumerate);
            if (buffer != 0 && count != 0)
            {
                memcpy(g_memory.Translate(buffer), enumerator->items.data() + size_t(enumerator->next) * enumerator->itemSize,
                    size_t(count) * enumerator->itemSize);
            }
            enumerator->next += count;
            result = count != 0 ? ErrorSuccess : ErrorInsufficientBuffer;
        }
    }
    if (itemsReturned != 0)
        g_memory.Write<uint32_t>(itemsReturned, count);
    ctx.r3.u64 = FinishXamCall(overlapped, result, count, ctx.r13.u32);
}
