// XAM user/profile services. One local (offline) profile is signed in on the first controller, as on a console
// where the player picked a profile; the other user slots are empty.
#include "kernel/overlapped.h"
#include "memory.h"
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorNoSuchUser = 0x525;

    constexpr uint32_t SignedInLocally = 1;
    constexpr uint64_t LocalXuid = 0xE000000000000001ull; // offline XUIDs live in the 0xE... range
    constexpr const char* ProfileName = "Player";

    bool IsSignedIn(uint32_t userIndex)
    {
        return userIndex == 0;
    }
}

PPC_FUNC(__imp__XamUserGetSigninState)
{
    ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? SignedInLocally : 0;
}

PPC_FUNC(__imp__XamUserGetXUID)
{
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }
    g_memory.Write<uint64_t>(ctx.r5.u32, LocalXuid);
    ctx.r3.u64 = ErrorSuccess;
}

PPC_FUNC(__imp__XamUserGetName)
{
    uint32_t buffer = ctx.r4.u32;
    uint32_t length = ctx.r5.u32;
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }

    if (length != 0)
    {
        auto* name = static_cast<char*>(g_memory.Translate(buffer));
        strncpy(name, ProfileName, length);
        name[length - 1] = '\0';
    }
    ctx.r3.u64 = ErrorSuccess;
}

// X_USER_SIGNIN_INFO: u64 xuid, u32 flags, u32 signin state, u32 guest number, u32 sponsor, char name[16].
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    uint32_t info = ctx.r5.u32;
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }

    memset(g_memory.Translate(info), 0, 0x28);
    g_memory.Write<uint64_t>(info + 0x00, LocalXuid);
    g_memory.Write<uint32_t>(info + 0x0C, SignedInLocally);
    strncpy(static_cast<char*>(g_memory.Translate(info + 0x18)), ProfileName, 15);
    ctx.r3.u64 = ErrorSuccess;
}

// Parental controls and Live privileges: everything is allowed for the local profile.
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }
    g_memory.Write<uint32_t>(ctx.r5.u32, 1);
    ctx.r3.u64 = ErrorSuccess;
}

PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    if (ctx.r6.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r6.u32, 0);
    ctx.r3.u64 = ErrorSuccess;
}

// XamUserWriteProfileSettings(title id, user index, setting count, XUSER_PROFILE_SETTING*, overlapped), signature as
// in Xenia (BSD): the settings are accepted for the signed-in profile but not kept (XamUserReadProfileSettings has no
// store to read them back from yet).
PPC_FUNC(__imp__XamUserWriteProfileSettings)
{
    constexpr uint32_t ErrorInvalidParameter = 87;
    uint32_t result = ErrorSuccess;
    if (ctx.r5.u32 == 0 || ctx.r6.u32 == 0)
        result = ErrorInvalidParameter;
    else if (!IsSignedIn(ctx.r4.u32))
        result = ErrorNoSuchUser;
    ctx.r3.u64 = FinishXamCall(ctx.r7.u32, result, 0, ctx.r13.u32);
}
