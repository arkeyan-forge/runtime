// Achievements for the offline profile. The title's list (names, descriptions, gamerscore) comes from the player's own
// default.xex (host/collection/achievements.h); the unlocks the title writes (XUserWriteAchievements, a request to the
// system's achievement app 0xFB) are kept in the game's data folder (host/collection/progress.h) with their time and shown
// as a notice; the enumerations answer from both, so the title's own checks (sub_8264ACF8 [SSA]: "every other achievement
// is unlocked") see the real state.
#include "kernel/overlapped.h"
#include "kernel/xam_enumerator.h"
#include "host/collection/achievements.h"
#include "host/collection/progress.h"
#include "host/toybox.h"
#include "language.h"
#include "vfs.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <ppc_config.h>
#include <ppc_context.h>
#include <string>

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

    // XACHIEVEMENT_DETAILS: id, label*, description*, unachieved*, image id, gamerscore, FILETIME unlock time, flags.
    constexpr uint32_t AchievementDetailsSize = 0x24;
    // XACHIEVEMENT_DETAILS_LABEL, _DESCRIPTION and _UNACHIEVED: the detail flags that ask for pwszLabel,
    // pwszDescription and pwszUnachieved (Row::text, in that order).
    constexpr uint32_t DetailText[3] = { 0x1, 0x2, 0x4 };
    constexpr uint32_t FlagAchieved = 0x00020000;
    constexpr int64_t FileTimeUnixEpoch = 11644473600ll; // seconds from 1601 to 1970

    // The title's list and what the player unlocked, loaded when first needed (after the settings fixed the language).
    std::mutex g_lock;
    bool g_loaded;
    achievements::List g_list;
    std::vector<progress::Unlock> g_unlocks;
    std::filesystem::path g_folder;

    void LoadLocked()
    {
        if (g_loaded)
            return;
        g_loaded = true;
        g_list = achievements::Read(g_disc.ReadAll("default.xex"), achievements::XdbfLanguage(TextLanguage()), false);
        g_folder = progress::Folder(SSA_GAME_ID);
        if (!g_folder.empty())
            g_unlocks = progress::ReadUnlocks(g_folder);
        uint32_t unlocked = 0, score = 0;
        for (const achievements::Info& info : g_list.items)
        {
            for (const progress::Unlock& unlock : g_unlocks)
                if (unlock.id == info.id)
                {
                    unlocked++;
                    score += info.gamerscore;
                }
        }
        fprintf(stderr, "[achievement] %zu achievements, %u unlocked (gamerscore %u of %u)\n", g_list.items.size(),
            unlocked, score, g_list.TotalGamerscore());
    }

    const progress::Unlock* UnlockOf(uint32_t id)
    {
        for (const progress::Unlock& unlock : g_unlocks)
            if (unlock.id == id)
                return &unlock;
        return nullptr;
    }

    // UTF-8 text as big-endian UTF-16 with its terminator, as the guest reads it.
    std::vector<uint8_t> GuestText(const std::string& text)
    {
        std::vector<uint8_t> out;
        auto put = [&](uint32_t unit) {
            out.push_back(uint8_t(unit >> 8));
            out.push_back(uint8_t(unit));
        };
        for (size_t i = 0; i < text.size();)
        {
            uint8_t lead = uint8_t(text[i]);
            uint32_t code = lead, extra = 0;
            if (lead >= 0xF0)
            {
                code = lead & 0x07;
                extra = 3;
            }
            else if (lead >= 0xE0)
            {
                code = lead & 0x0F;
                extra = 2;
            }
            else if (lead >= 0xC0)
            {
                code = lead & 0x1F;
                extra = 1;
            }
            i++;
            for (; extra != 0 && i < text.size(); extra--, i++)
                code = code << 6 | (uint8_t(text[i]) & 0x3F);
            if (code >= 0x10000)
            {
                code -= 0x10000;
                put(0xD800 | (code >> 10));
                put(0xDC00 | (code & 0x3FF));
            }
            else
                put(code);
        }
        put(0);
        return out;
    }

    // The achievements of a XamUserCreateAchievementEnumerator call: the details, with the three texts of each placed
    // after the array in the title's buffer (the title walks the array by sizeof(XACHIEVEMENT_DETAILS)).
    struct AchievementEnumerator : XamEnumerator
    {
        struct Row
        {
            uint32_t id, imageId, gamerscore, flags;
            int64_t unlockTime; // 0: locked
            std::vector<uint8_t> text[3]; // label, description, unachieved
        };
        std::vector<Row> rows;
        uint32_t detailFlags = 0;

        uint32_t Total() const override { return uint32_t(rows.size()); }

        uint32_t TextBytes(const Row& row) const
        {
            uint32_t bytes = 0;
            for (int i = 0; i < 3; i++)
                if (detailFlags & DetailText[i])
                    bytes += uint32_t(row.text[i].size());
            return bytes;
        }

        uint32_t Fill(uint32_t buffer, uint32_t bufferSize, uint32_t maxItems) override
        {
            uint32_t count = 0, used = 0;
            while (count < maxItems && next + count < rows.size())
            {
                uint32_t need = AchievementDetailsSize + TextBytes(rows[next + count]);
                if (used + need > bufferSize)
                    break;
                used += need;
                count++;
            }
            if (buffer == 0)
                return count;
            uint32_t strings = buffer + count * AchievementDetailsSize;
            for (uint32_t i = 0; i < count; i++)
            {
                const Row& row = rows[next + i];
                uint32_t details = buffer + i * AchievementDetailsSize;
                uint32_t pointers[3] = {};
                for (int t = 0; t < 3; t++)
                {
                    if (!(detailFlags & DetailText[t]))
                        continue;
                    pointers[t] = strings;
                    memcpy(g_memory.Translate(strings), row.text[t].data(), row.text[t].size());
                    strings += uint32_t(row.text[t].size());
                }
                g_memory.Write<uint32_t>(details + 0x00, row.id);
                g_memory.Write<uint32_t>(details + 0x04, pointers[0]);
                g_memory.Write<uint32_t>(details + 0x08, pointers[1]);
                g_memory.Write<uint32_t>(details + 0x0C, pointers[2]);
                g_memory.Write<uint32_t>(details + 0x10, row.imageId);
                g_memory.Write<uint32_t>(details + 0x14, row.gamerscore);
                g_memory.Write<uint64_t>(details + 0x18,
                    row.unlockTime != 0 ? uint64_t(row.unlockTime + FileTimeUnixEpoch) * 10000000ull : 0);
                g_memory.Write<uint32_t>(details + 0x20, row.flags | (row.unlockTime != 0 ? FlagAchieved : 0));
            }
            return count;
        }
    };

    void Announce(const achievements::Info& info)
    {
        std::string english = "Achievement unlocked: " + info.name;
        std::string french = "Succ\xC3\xA8s d\xC3\xA9"
                             "bloqu\xC3\xA9 : " +
                             info.name;
        ToyboxNotice(english.c_str(), french.c_str());
    }
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
        uint32_t user = g_memory.Read<uint32_t>(achievements + i * 8);
        uint32_t id = g_memory.Read<uint32_t>(achievements + i * 8 + 4);
        std::lock_guard lock(g_lock);
        LoadLocked();
        const achievements::Info* info = nullptr;
        for (const achievements::Info& candidate : g_list.items)
            if (candidate.id == id)
                info = &candidate;
        bool added = false;
        if (!g_folder.empty() && !progress::WriteUnlock(g_folder, id, int64_t(std::time(nullptr)), &added))
            fprintf(stderr, "[achievement] cannot write the unlock in %s\n", g_folder.string().c_str());
        if (!g_folder.empty() && added)
            g_unlocks = progress::ReadUnlocks(g_folder);
        fprintf(stderr, "[achievement] user %u unlocked achievement %u%s%s%s\n", user, id, info != nullptr ? " (" : "",
            info != nullptr ? info->name.c_str() : "", info != nullptr ? ")" : "");
        if (added && info != nullptr)
            Announce(*info);
    }
    ctx.r3.u64 = FinishXamCall(overlapped, ErrorSuccess, 0, ctx.r13.u32);
}

// XamUserCreateAchievementEnumerator(title id, user index, xuid, detail flags, starting index, count, buffer size out,
// handle out)
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator)
{
    uint32_t userIndex = ctx.r4.u32;
    uint32_t detailFlags = ctx.r6.u32;
    uint32_t start = ctx.r7.u32;
    uint32_t count = ctx.r8.u32;
    uint32_t bufferSizePointer = ctx.r9.u32;
    uint32_t handlePointer = ctx.r10.u32;

    if (userIndex != 0 && userIndex != 0xFF) // the signed-in profile, or any user
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }
    auto enumerator = std::make_shared<AchievementEnumerator>();
    enumerator->itemSize = AchievementDetailsSize;
    enumerator->itemsPerEnumerate = count;
    enumerator->detailFlags = detailFlags;
    {
        std::lock_guard lock(g_lock);
        LoadLocked();
        for (size_t i = start; i < g_list.items.size() && enumerator->rows.size() < count; i++)
        {
            const achievements::Info& info = g_list.items[i];
            const progress::Unlock* unlock = UnlockOf(info.id);
            AchievementEnumerator::Row row = { info.id, info.imageId, info.gamerscore, info.flags & 0xF,
                unlock != nullptr ? std::max<int64_t>(unlock->time, 1) : 0,
                { GuestText(info.name), GuestText(info.unlockedDescription), GuestText(info.lockedDescription) } };
            enumerator->rows.push_back(std::move(row));
        }
    }
    uint32_t bufferSize = 0;
    for (const AchievementEnumerator::Row& row : enumerator->rows)
        bufferSize += AchievementDetailsSize + enumerator->TextBytes(row);
    if (bufferSizePointer != 0)
        g_memory.Write<uint32_t>(bufferSizePointer, bufferSize);
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
        uint32_t total = enumerator->Total();
        uint32_t left = total - std::min(enumerator->next, total);
        if (left == 0)
        {
            result = ErrorNoMoreFiles;
        }
        else
        {
            uint32_t most = enumerator->itemsPerEnumerate != 0 ? std::min(left, enumerator->itemsPerEnumerate) : left;
            count = enumerator->Fill(buffer, bufferSize, most);
            enumerator->next += count;
            result = count != 0 ? ErrorSuccess : ErrorInsufficientBuffer;
        }
    }
    if (itemsReturned != 0)
        g_memory.Write<uint32_t>(itemsReturned, count);
    ctx.r3.u64 = FinishXamCall(overlapped, result, count, ctx.r13.u32);
}
