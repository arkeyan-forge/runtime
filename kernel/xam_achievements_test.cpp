// The kernel side of the achievements (kernel/xam_achievements.cpp) without a game: the calls a title makes, run on a fake
// guest context against the player's disc. A title writes an unlock, the Toy Box's toast is raised (name, points, picture), the enumeration shows
// it; a second process, "after a restart", enumerates it again from the data folder. Scratch folders only (SKY_SAVE_DIR).
//   ArkeyanAchievementsKernelTest <game disc .iso>             Exit code 0 = pass.
#include "kernel/objects.h"
#include "host/toybox.h"
#include "memory.h"
#include "thread.h"
#include "vfs.h"
#include "host/collection/achievements.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ppc_context.h>
#include <string>
#include <unistd.h>

void __imp__XMsgStartIORequest(PPCContext& ctx, uint8_t* base);
void __imp__XamUserCreateAchievementEnumerator(PPCContext& ctx, uint8_t* base);
void __imp__XamEnumerate(PPCContext& ctx, uint8_t* base);

namespace fs = std::filesystem;

std::string g_lastNotice;
size_t g_lastPicture;
uint32_t g_lastPoints;
void ToyboxNotice(const char* english, const char*) { g_lastNotice = english; }
void ToyboxAchievement(const std::string& name, uint32_t points, std::vector<uint8_t> png)
{
    g_lastNotice = name;
    g_lastPoints = points;
    g_lastPicture = png.size();
}
void ToyboxSetAchievementPreview(void (*)()) {}
void ToyboxSetAchievementList(std::vector<ToyboxAchievementRow> (*)()) {}
std::string_view TextLanguage() { return "en"; }
// The test passes no XOVERLAPPED, so no call completes asynchronously.
std::shared_ptr<GuestThread> CurrentGuestThread(uint32_t) { return nullptr; }
void QueueUserApc(GuestThread&, const UserApc&) {}

namespace
{
    int g_checks, g_failures;
    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    PPCContext g_context;
    uint8_t* g_base;

    // Guest memory layout of the test.
    constexpr uint32_t Request = 0x10000, Entries = 0x10100, SizeOut = 0x10200, HandleOut = 0x10204, Returned = 0x10208,
                       Buffer = 0x20000;

    void WriteUnlock(uint32_t id)
    {
        g_memory.Write<uint32_t>(Request, 1);
        g_memory.Write<uint32_t>(Request + 4, Entries);
        g_memory.Write<uint32_t>(Entries, 0);
        g_memory.Write<uint32_t>(Entries + 4, id);
        g_context.r3.u64 = 0xFB;
        g_context.r4.u64 = 0x000B0008;
        g_context.r5.u64 = 0;
        g_context.r6.u64 = Request;
        __imp__XMsgStartIORequest(g_context, g_base);
        CHECK(g_context.r3.u32 == 0);
    }

    // XamUserCreateAchievementEnumerator(0, 0, xuid, flags 7, start, count) then XamEnumerate into Buffer.
    struct Seen
    {
        uint32_t id, gamerscore, flags;
        uint64_t time;
        std::u16string name;
    };
    std::vector<Seen> Enumerate(uint32_t start, uint32_t count, uint32_t* sizeOut = nullptr)
    {
        g_context.r3.u64 = 0;
        g_context.r4.u64 = 0;
        g_context.r5.u64 = 0;
        g_context.r6.u64 = 7;
        g_context.r7.u64 = start;
        g_context.r8.u64 = count;
        g_context.r9.u64 = SizeOut;
        g_context.r10.u64 = HandleOut;
        __imp__XamUserCreateAchievementEnumerator(g_context, g_base);
        CHECK(g_context.r3.u32 == 0);
        uint32_t size = g_memory.Read<uint32_t>(SizeOut);
        if (sizeOut != nullptr)
            *sizeOut = size;
        std::vector<Seen> seen;
        g_context.r3.u64 = g_memory.Read<uint32_t>(HandleOut);
        g_context.r4.u64 = 0;
        g_context.r5.u64 = Buffer;
        g_context.r6.u64 = size;
        g_context.r7.u64 = Returned;
        g_context.r8.u64 = 0;
        __imp__XamEnumerate(g_context, g_base);
        uint32_t got = g_memory.Read<uint32_t>(Returned);
        CHECK(g_context.r3.u32 == (got != 0 ? 0u : 18u));
        for (uint32_t i = 0; i < got; i++)
        {
            uint32_t item = Buffer + i * 0x24;
            Seen s{ g_memory.Read<uint32_t>(item), g_memory.Read<uint32_t>(item + 0x14),
                g_memory.Read<uint32_t>(item + 0x20), g_memory.Read<uint64_t>(item + 0x18), {} };
            for (uint32_t label = g_memory.Read<uint32_t>(item + 4); label != 0; label += 2)
            {
                char16_t unit = char16_t(g_memory.Read<uint16_t>(label));
                if (unit == 0)
                    break;
                s.name += unit;
            }
            seen.push_back(s);
        }
        // Everything fits: the next call has nothing.
        g_context.r3.u64 = g_memory.Read<uint32_t>(HandleOut);
        __imp__XamEnumerate(g_context, g_base);
        CHECK(g_context.r3.u32 == 18 && (count == 0 || got <= count));
        return seen;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <game disc .iso> [read]\n", argv[0]);
        return 2;
    }
    bool second = argc > 2; // the process "after a restart"
    fs::path scratch =
        fs::temp_directory_path() / ("ArkeyanAchievementsKernelTest-" + std::to_string(second ? getppid() : getpid()));
    fs::create_directories(scratch);
    setenv("SKY_SAVE_DIR", scratch.string().c_str(), 1);

    CHECK(g_disc.Open(argv[1]));
    g_memory.Reserve();
    g_base = g_memory.base;
    achievements::List list = achievements::Read(g_disc.ReadAll("default.xex"), 1, false);
    CHECK(list.items.size() > 3);
    uint32_t first = list.items[0].id, third = list.items[2].id;

    if (!second)
    {
        std::vector<Seen> all = Enumerate(0, 255);
        CHECK(all.size() == list.items.size());
        for (const Seen& s : all)
            CHECK(s.time == 0 && !(s.flags & 0x20000) && !s.name.empty());
        uint32_t sizeOut = 0;
        CHECK(Enumerate(1, 2, &sizeOut).size() == 2 &&
              Enumerate(1, 2).front().id == list.items[1].id); // start index and count

        WriteUnlock(third);
        // The toast: the name, the points and the picture of the title's list.
        CHECK(g_lastNotice == list.items[2].name && g_lastPoints == list.items[2].gamerscore && g_lastPicture > 0);
        g_lastNotice.clear();
        WriteUnlock(third); // again: kept once, no second notice
        CHECK(g_lastNotice.empty());
        printf("  unlocked achievement %u (%s)\n", third, list.items[2].name.c_str());
        all = Enumerate(0, 255);
        int unlocked = 0;
        for (const Seen& s : all)
        {
            if (s.id == third)
            {
                unlocked++;
                CHECK((s.flags & 0x20000) && s.time > 116444736000000000ull);
            }
            else
                CHECK(!(s.flags & 0x20000));
        }
        CHECK(unlocked == 1);

        // "After a restart": this program again, which reads only the data folder.
        std::string command = std::string("\"") + argv[0] + "\" \"" + argv[1] + "\" read";
        CHECK(system(command.c_str()) == 0);
        fs::remove_all(scratch);
    }
    else
    {
        std::vector<Seen> all = Enumerate(0, 255);
        CHECK(all.size() == list.items.size());
        int unlocked = 0;
        for (const Seen& s : all)
            unlocked += (s.flags & 0x20000) != 0 ? (s.id == third ? 1 : 100) : 0;
        CHECK(unlocked == 1);
        CHECK(first != third);
        printf("  after a restart: achievement %u still unlocked\n", third);
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
