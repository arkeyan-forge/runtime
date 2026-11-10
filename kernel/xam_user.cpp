// XAM user/profile services. The players' profiles come from the launcher (host/profiles.h,
// docs/runtime/host/profiles.md): one is always signed in on the first controller, as on a console where the player
// picked a profile; the second controller has one only when the launcher gives Player 2 a profile.
#include "game.h"
#include "host/collection/datadir.h"
#include "host/profiles.h"
#include "kernel/overlapped.h"
#include "memory.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ppc_config.h>
#include <ppc_context.h>

const std::array<profiles::Profile, 2>& profiles::Current()
{
    static const std::array<Profile, 2> current = [] {
        std::array<Profile, 2> read =
            Parse([](const char* variable) { return std::getenv(variable); }, game::Current().nameAccents);
        // The name and the picture's path stay out of the log (problem reports carry it).
        for (size_t i = 0; i < read.size(); i++)
            if (read[i].signedIn)
                fprintf(stderr, "[profile] user %zu signed in: a name of %zu bytes, xuid %016llX, %s\n", i,
                    read[i].name.size(), static_cast<unsigned long long>(read[i].xuid),
                    read[i].picture.empty() ? "no picture" : "a picture");
        return read;
    }();
    return current;
}

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorNotFound = 0x490;
    constexpr uint32_t ErrorNoSuchUser = 0x525;

    constexpr uint32_t SignedInLocally = 1;

    bool IsSignedIn(uint32_t userIndex) { return userIndex < 2 && profiles::Current()[userIndex].signedIn; }

    // The name into a guest buffer of `capacity` bytes, ended by a zero, never cut inside a UTF-8 character.
    void CopyName(uint32_t buffer, uint32_t capacity, const std::string& name)
    {
        size_t length = profiles::FittingBytes(name, capacity - 1);
        auto* out = static_cast<char*>(g_memory.Translate(buffer));
        memcpy(out, name.data(), length);
        out[length] = '\0';
    }

    // The first call of each name import, for the log: which game code asks, for which user.
    void LogFirstCall(const char* name, uint32_t userIndex, const PPCContext& ctx)
    {
        static std::atomic_flag reportedName, reportedInfo;
        std::atomic_flag& reported = strcmp(name, "XamUserGetName") == 0 ? reportedName : reportedInfo;
        if (!reported.test_and_set())
            fprintf(stderr, "[kernel] %s (user %u, lr=0x%08X)\n", name, userIndex, uint32_t(ctx.lr));
    }

    // The profile's picture, decoded on first use; false when it has none or it is not an image.
    bool ProfilePicture(uint32_t userIndex, const profiles::Picture*& picture)
    {
        static std::mutex lock;
        static profiles::Picture pictures[2];
        static bool read[2] = {}, decoded[2] = {};
        std::lock_guard guard(lock);
        if (!read[userIndex])
        {
            read[userIndex] = true;
            const std::string& path = profiles::Current()[userIndex].picture;
            if (!path.empty())
            {
                std::ifstream file(datadir::FromText(path), std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                decoded[userIndex] = profiles::DecodePicture(bytes.data(), bytes.size(), pictures[userIndex]);
                if (decoded[userIndex])
                    fprintf(stderr, "[profile] user %u picture: %dx%d\n", userIndex, pictures[userIndex].width,
                        pictures[userIndex].height);
                else
                    fprintf(
                        stderr, "[profile] user %u picture: not an image (%zu bytes read)\n", userIndex, bytes.size());
            }
        }
        picture = &pictures[userIndex];
        return decoded[userIndex];
    }
}

PPC_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? SignedInLocally : 0; }

PPC_FUNC(__imp__XamUserGetXUID)
{
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }
    g_memory.Write<uint64_t>(ctx.r5.u32, profiles::Current()[ctx.r3.u32].xuid);
    ctx.r3.u64 = ErrorSuccess;
}

PPC_FUNC(__imp__XamUserGetName)
{
    uint32_t userIndex = ctx.r3.u32;
    uint32_t buffer = ctx.r4.u32;
    uint32_t length = ctx.r5.u32;
    if (!IsSignedIn(userIndex))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }

    LogFirstCall("XamUserGetName", userIndex, ctx);
    if (length != 0)
        CopyName(buffer, length, profiles::Current()[userIndex].name);
    ctx.r3.u64 = ErrorSuccess;
}

// X_USER_SIGNIN_INFO: u64 xuid, u32 flags, u32 signin state, u32 guest number, u32 sponsor, char name[16].
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    uint32_t userIndex = ctx.r3.u32;
    uint32_t info = ctx.r5.u32;
    if (!IsSignedIn(userIndex))
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }

    LogFirstCall("XamUserGetSigninInfo", userIndex, ctx);
    memset(g_memory.Translate(info), 0, 0x28);
    const profiles::Profile& profile = profiles::Current()[userIndex];
    g_memory.Write<uint64_t>(info + 0x00, profile.xuid);
    g_memory.Write<uint32_t>(info + 0x0C, SignedInLocally);
    CopyName(info + 0x18, 16, profile.name);
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

// XamReadTileToTexture(tile type, title id, tile id (64-bit), user index, buffer, pitch, height, overlapped), signature
// as in Xenia (BSD). The gamer picture of a signed-in user (tile types 2 to 5: the gamer tile and the local one, large
// and small) is the profile's picture, square, as wide as the buffer is high. Anything else, or no picture: the texture
// is cleared (transparent) and the call fails with ERROR_NOT_FOUND, as a console without the tile answers.
PPC_FUNC(__imp__XamReadTileToTexture)
{
    uint32_t type = ctx.r3.u32, userIndex = ctx.r6.u32, buffer = ctx.r7.u32, pitch = ctx.r8.u32, height = ctx.r9.u32;
    static std::atomic_flag reported;
    if (!reported.test_and_set())
        fprintf(stderr, "[kernel] XamReadTileToTexture (type %u user %u buffer 0x%08X pitch %u height %u)\n", type,
            userIndex, buffer, pitch, height);
    uint32_t result = ErrorNotFound;
    const profiles::Picture* picture = nullptr;
    if (buffer != 0)
    {
        auto* out = static_cast<uint8_t*>(g_memory.Translate(buffer));
        if (type >= 2 && type <= 5 && IsSignedIn(userIndex) && ProfilePicture(userIndex, picture))
        {
            profiles::WriteTile(*picture, height, height, pitch, out);
            result = ErrorSuccess;
        }
        else
            memset(out, 0, size_t(pitch) * height);
    }
    ctx.r3.u64 = FinishXamCall(ctx.r10.u32, result, 0, ctx.r13.u32);
}
