// Protocol test of the emulated portal: the command sequence Spyro's Adventure sends (docs/runtime/host/portal.md), figure
// placement and removal, block reads and writes, and figure write-back. Run with no arguments; exit code 0 = pass.
#include "host/portal_backend.h"
#include "host/portal_identity.h"
#include "host/portal_prompt.h"
#include "giants_portal_prompt.h"
#include "ssa_portal_prompt.h"
#include "swapforce_portal_prompt.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
    int g_checks;
    int g_failures;

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

    std::vector<uint8_t> Send(EmulatedPortal& portal, std::initializer_list<uint8_t> command)
    {
        std::vector<uint8_t> bytes(command);
        portal.Write(bytes.data(), bytes.size());
        return bytes;
    }

    std::vector<uint8_t> Receive(EmulatedPortal& portal)
    {
        uint8_t report[PortalReportSize];
        size_t size = portal.Read(report, sizeof(report));
        return std::vector<uint8_t>(report, report + size);
    }

    uint32_t Status(const std::vector<uint8_t>& report)
    {
        return report[1] | report[2] << 8 | report[3] << 16 | uint32_t(report[4]) << 24;
    }
}

int main()
{
    // CRC-16/CCITT-FALSE check value.
    CHECK(FigureChecksum(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x29B1);

    // Blank figure: serial check byte, toy type, header checksum, access bits.
    FigureData figure = CreateBlankFigure(14);
    CHECK(figure[4] == (figure[0] ^ figure[1] ^ figure[2] ^ figure[3]));
    CHECK(figure[0x10] == 14 && figure[0x11] == 0);
    CHECK((figure[0x1E] | figure[0x1F] << 8) == FigureChecksum(figure.data(), 0x1E));
    CHECK(memcmp(&figure[3 * 16 + 6], "\x0F\x0F\x0F\x69", 4) == 0);
    CHECK(memcmp(&figure[7 * 16 + 6], "\x7F\x0F\x08\x69", 4) == 0);

    // A dump on disk, so that writes can be checked for persistence.
    // In the system's temporary folder, never in the user's application data.
    std::string path = (std::filesystem::temp_directory_path() / "SSAPortalTest_figure.sky").string();
    CHECK(SDL_SaveFile(path.c_str(), figure.data(), figure.size()));
    FigureData loaded{};
    CHECK(LoadFigure(path, loaded) && loaded == figure);

    EmulatedPortal portal;
    CHECK(portal.Place(loaded, path) == 0);

    // An inactive portal only answers commands.
    CHECK(Receive(portal).empty());

    // Reset: firmware version, which the title requires to be at least 0x011F.
    Send(portal, { 'R', 0x00 });
    auto reply = Receive(portal);
    CHECK(reply.size() == PortalReportSize && reply[0] == 'R' && (reply[1] << 8 | reply[2]) >= 0x011F);
    CHECK(Receive(portal).empty());

    // The title deactivates the portal first, then activates it.
    Send(portal, { 'A', 0x00 });
    reply = Receive(portal);
    CHECK(reply[0] == 'A' && reply[1] == 0 && reply[2] == 0xFF);
    CHECK(Receive(portal).empty());
    Send(portal, { 'A', 0x01 });
    reply = Receive(portal);
    CHECK(reply[0] == 'A' && reply[1] == 1 && reply[2] > 164); // battery above the title's "too low" level

    // Explicit status, then unprompted ones: the new figure is "present + changed" once, then "present".
    Send(portal, { 'S' });
    reply = Receive(portal);
    CHECK(reply[0] == 'S' && (Status(reply) & 3) == 3 && reply[6] == 1);
    uint8_t counter = reply[5];
    reply = Receive(portal);
    CHECK(reply[0] == 'S' && (Status(reply) & 3) == 1 && reply[5] == uint8_t(counter + 1));

    // Block reads: answer carries one block, slot in the low nibble.
    Send(portal, { 'Q', 0x10, 0x01 });
    reply = Receive(portal);
    CHECK(reply[0] == 'Q' && reply[1] == 0x10 && reply[2] == 0x01 && memcmp(&reply[3], &figure[16], 16) == 0);
    Send(portal, { 'Q', 0x31, 0x01 }); // empty slot 1 (the title asks for several blocks in the high nibble)
    reply = Receive(portal);
    CHECK(reply[0] == 'Q' && reply[1] == 0x01);
    Send(portal, { 'Q', 0x10, 0x40 });
    CHECK(Receive(portal)[1] == 0x01);

    // Block write, verified by reading back, and saved to the dump.
    std::vector<uint8_t> write = { 'W', 0x10, 0x08 };
    for (int i = 0; i < 16; i++)
        write.push_back(uint8_t(0xA0 + i));
    portal.Write(write.data(), write.size());
    reply = Receive(portal);
    CHECK(reply[0] == 'W' && reply[1] == 0x10 && reply[2] == 0x08);
    Send(portal, { 'Q', 0x10, 0x08 });
    reply = Receive(portal);
    CHECK(memcmp(&reply[3], &write[3], 16) == 0);
    CHECK(LoadFigure(path, loaded) && memcmp(&loaded[8 * 16], &write[3], 16) == 0);

    // The serial block and sector trailers are not writable.
    write[2] = 0x00;
    portal.Write(write.data(), write.size());
    CHECK(Receive(portal)[1] == 0x01);
    write[2] = 0x07;
    portal.Write(write.data(), write.size());
    CHECK(Receive(portal)[1] == 0x01);

    // Colour has no answer: the next report is a status.
    Send(portal, { 'C', 0xFF, 0x00, 0x00 });
    CHECK(Receive(portal)[0] == 'S');
    // The portal's light (Giants fades it with C all the time): the last colour and the count are kept, a short report
    // is zero-filled, and a command no game here is known to send is ignored without an answer.
    CHECK(portal.LightCommands() == 1 && portal.LightColor().r == 0xFF && portal.LightColor().g == 0 && portal.LightColor().b == 0);
    Send(portal, { 'C', 0x00, 0x80 });
    Send(portal, { 'C', 0x10, 0x20, 0x30 });
    Send(portal, { 'C' });
    CHECK(portal.LightCommands() == 4 && portal.LightColor().r == 0 && portal.LightColor().g == 0 && portal.LightColor().b == 0);
    Send(portal, { 'C', 0x00, 0x80 });
    CHECK(portal.LightColor().g == 0x80);
    Send(portal, { 'J', 0x01, 0x02 });
    CHECK(Receive(portal)[0] == 'S' && portal.LightCommands() == 5);

    // Removal: "changed" once, then absent. A new figure avoids the slot whose removal was not reported yet.
    portal.Remove(0);
    CHECK(portal.Place(CreateBlankFigure(16), "") == 1);
    reply = Receive(portal);
    CHECK((Status(reply) & 0xF) == 0b1110);
    reply = Receive(portal);
    CHECK((Status(reply) & 0xF) == 0b0100);

    // Reset deactivates and drops pending answers.
    Send(portal, { 'Q', 0x11, 0x00 });
    Send(portal, { 'R' });
    CHECK(Receive(portal)[0] == 'R');
    CHECK(Receive(portal).empty() && !portal.IsActive());

    // Portal identity: the emulated portal answers with the id of the portal its game was made for.
    {
        EmulatedPortal ssa;
        Send(ssa, { 'R', 0x00 });
        std::vector<uint8_t> id = Receive(ssa);
        CHECK(id.size() == PortalReportSize && id[0] == 'R' && id[1] == 0x01 && id[2] == 0x28 && id[3] == 0);
        CHECK(FindPortalModel(&id[1], 2) != nullptr && strcmp(FindPortalModel(&id[1], 2)->name, "Spyro's Adventure portal (Xbox 360)") == 0);

        // A four-byte id (SuperChargers) goes through whole.
        EmulatedPortal four;
        four.SetModel(*FindPortalModel(reinterpret_cast<const uint8_t*>("\x02\x0A\x03\x02"), 4));
        Send(four, { 'R' });
        id = Receive(four);
        CHECK(id[1] == 0x02 && id[2] == 0x0A && id[3] == 0x03 && id[4] == 0x02);
        uint8_t parsed[4];
        size_t length = 0;
        CHECK(PortalIdFromReset(id.data(), id.size(), parsed, length) && length == 4 && PortalIdText(parsed, length) == "02 0A 03 02");
    }
    {
        // The table: names, and what Spyro's Adventure makes of each id (its check is version >= 0x011F).
        const uint8_t wiiPs3[] = { 0x01, 0x29 }, xbox360[] = { 0x01, 0x28 }, pc[] = { 0x01, 0x2C }, giants[] = { 0x01, 0x3D },
            swap[] = { 0x02, 0x03 }, trap[] = { 0x02, 0x1B }, superChargers[] = { 0x02, 0x0A, 0x03, 0x02 }, imaginators[] = { 0x02, 0x0A, 0x05, 0x08 },
            unknown[] = { 0x02, 0x7F }, old[] = { 0x00, 0x90 }, wiiBug[] = { 0x90, 0x00 };
        CHECK(strcmp(FindPortalModel(wiiPs3, 2)->name, "Spyro's Adventure portal (Wii, PS3)") == 0);
        CHECK(strcmp(FindPortalModel(pc, 2)->name, "Spyro's Adventure portal (PC, Mac)") == 0);
        CHECK(strcmp(FindPortalModel(giants, 2)->name, "Giants portal") == 0);
        CHECK(strcmp(FindPortalModel(swap, 2)->name, "Swap Force portal") == 0);
        CHECK(strcmp(FindPortalModel(trap, 2)->name, "Traptanium portal (Xbox 360)") == 0);
        CHECK(strcmp(FindPortalModel(superChargers, 4)->name, "SuperChargers portal") == 0);
        CHECK(strcmp(FindPortalModel(imaginators, 4)->name, "Imaginators portal") == 0);
        CHECK(FindPortalModel(superChargers, 2) == nullptr); // a four-byte id is not its first two bytes
        CHECK(FindPortalModel(unknown, 2) == nullptr);
        for (const uint8_t* id : { wiiPs3, xbox360, pc, giants, swap, trap })
            CHECK(PortalVerdictFor("ssa", id, 2) == PortalVerdict::Works);
        CHECK(PortalVerdictFor("ssa", superChargers, 4) == PortalVerdict::Works);
        CHECK(PortalVerdictFor("ssa", unknown, 2) == PortalVerdict::Works); // newer than any it knows: version only
        std::string reason;
        CHECK(PortalVerdictFor("ssa", old, 2, &reason) == PortalVerdict::Rejected && !reason.empty());
        CHECK(PortalVerdictFor("ssa", wiiBug, 1) == PortalVerdict::Unknown);
        CHECK(PortalVerdictFor("trapteam", xbox360, 2) == PortalVerdict::Unknown);
        CHECK(DescribePortal("ssa", xbox360, 2) == "01 28 Spyro's Adventure portal (Xbox 360); Spyro's Adventure: works");
        CHECK(DescribePortal(nullptr, unknown, 2) == "02 7F unknown portal");
        CHECK(DescribePortal("ssa", old, 2).find("rejects it") != std::string::npos);
        // The emulated id per game is in the table, and Spyro's Adventure accepts it.
        for (const char* game : { "ssa", "giants", "swapforce" })
        {
            const PortalModel& model = EmulatedPortalModel(game);
            CHECK(FindPortalModel(model.id, model.idLength) == &model);
        }
        CHECK(EmulatedPortalModel("ssa").id[1] == 0x28 && EmulatedPortalModel("giants").id[1] == 0x48);
        // Swap Force: older portals (first byte 1) get "Wrong Portal Version"; its own and every later one plays.
        const uint8_t giants360[] = { 0x01, 0x48 };
        CHECK(EmulatedPortalModel("swapforce").id[0] == 0x02 && EmulatedPortalModel("swapforce").id[1] == 0x03);
        for (const uint8_t* id : { wiiPs3, xbox360, giants, giants360 })
            CHECK(PortalVerdictFor("swapforce", id, 2) == PortalVerdict::Rejected);
        for (const uint8_t* id : { swap, trap, unknown })
            CHECK(PortalVerdictFor("swapforce", id, 2) == PortalVerdict::Works);
        CHECK(PortalVerdictFor("swapforce", superChargers, 4) == PortalVerdict::Works);
        CHECK(PortalVerdictFor("swapforce", old, 2) == PortalVerdict::Rejected);
        CHECK(DescribePortal("swapforce", swap, 2) == "02 03 Swap Force portal; Swap Force: works");
        CHECK(DescribePortal("swapforce", xbox360, 2).find("Wrong Portal Version") != std::string::npos);
        // The probe names a real portal from its answer to 'R'.
        PortalProbe probe{ "hid", 0x1430, 0x0150, "Spyro Porta" };
        const uint8_t answer[PortalReportSize] = { 'R', 0x01, 0x29 };
        NameProbe(probe, answer, sizeof(answer));
        CHECK(probe.id == "01 29" && probe.model == "Spyro's Adventure portal (Wii, PS3)" && probe.verdict == "works");
        PortalProbe silent{ "hid", 0x1430, 0x0150, "" };
        const uint8_t status[] = { 'S', 0, 0, 0, 0 };
        NameProbe(silent, status, sizeof(status));
        CHECK(silent.id.empty());
    }

    {
        // The prompt: Spyro's Adventure's screen is up when the mode is 2; the text's hash says who is asked.
        using namespace portalprompt;
        CHECK(ClassifySsa(0, SsaPlayer1Hash) == Prompt{});
        CHECK(ClassifySsa(4, SsaPlayer1Hash) == Prompt{});
        CHECK(ClassifySsa(2, SsaPlayer1Hash) == (Prompt{ Kind::Player, 1 }));
        CHECK(ClassifySsa(2, SsaPlayer2Hash) == (Prompt{ Kind::Player, 2 }));
        CHECK(ClassifySsa(2, 0xCE3BDD24).kind == Kind::TooMany); // "too many Skylanders"
        CHECK(ClassifySsa(2, 0).kind == Kind::TooMany);

        // Giants: the message's key and the state say who is asked.
        CHECK(ClassifyGiants("", "Portal Control - In Game - 0 - Idle") == Prompt{});
        CHECK(ClassifyGiants("203_PortalError_TagTypeNotOnPortal", "Portal Control - Portal - 0 - Idle P1 Only") == (Prompt{ Kind::Player, 1 }));
        CHECK(ClassifyGiants("203_PortalError_TagTypeNotOnPortal", "Portal Control - Portal - 0 - Idle P2 Only") == (Prompt{ Kind::Player, 2 }));
        CHECK(ClassifyGiants("301_PortalError_ToyMissing", "") == (Prompt{ Kind::Player, 1 }));
        CHECK(ClassifyGiants("304_PortalError_NeedsRemoval_Minion", "Portal Control - Portal - 0 - Idle P2 Only") == (Prompt{ Kind::Player, 2 }));
        CHECK(ClassifyGiants("102_PortalError_TooManyMinions", "Portal Control - Portal - 0 - Idle P1 Only").kind == Kind::TooMany);
        CHECK(ClassifyGiants("103_PortalError_TooManyItems", "").kind == Kind::TooMany);
        CHECK(ClassifyGiants("306_PortalError_ConfirmPVP", "") == Prompt{}); // a question the game's own buttons answer

        // Swap Force: the CMagicMoment state and the player index.
        CHECK(ClassifySwapForce(0, ~0u) == Prompt{});                      // Inactive: in play
        CHECK(ClassifySwapForce(3, 0) == Prompt{});                        // the zoom out to the portal
        CHECK(ClassifySwapForce(5, 0) == (Prompt{ Kind::Player, 1 }));     // Idle: the prompt is up
        CHECK(ClassifySwapForce(5, 1) == (Prompt{ Kind::Player, 2 }));
        CHECK(ClassifySwapForce(13, 0) == (Prompt{ Kind::Player, 1, true })); // Dead: the Skylander died, the game waits for the next
        CHECK(ClassifySwapForce(13, 1) == (Prompt{ Kind::Player, 2, true }) && !ClassifySwapForce(5, 0).defeated); // Idle is not a death
        CHECK(!(ClassifySwapForce(13, 0) == ClassifySwapForce(5, 0)));       // so a death after an Idle prompt is a new episode
        CHECK(ClassifySwapForce(5, ~0u) == Prompt{});                      // nobody asked
        CHECK(ClassifySwapForce(7, 0) == Prompt{});                        // IdleOutro: a figure came
        CHECK(ClassifySwapForce(16, ~0u).kind == Kind::TooMany);           // Error: a "too many" screen

        // One event per episode, after the prompt held steady for Stable polls.
        Tracker tracker;
        Prompt got;
        auto poll = [&](const Prompt& p, int times) {
            Tracker::Event last = Tracker::Event::None;
            for (int i = 0; i < times; i++)
            {
                Tracker::Event e = tracker.Update(p, got);
                if (e != Tracker::Event::None)
                    last = e;
            }
            return last;
        };
        const Prompt p1{ Kind::Player, 1 }, p2{ Kind::Player, 2 }, many{ Kind::TooMany, 0 };
        CHECK(poll({}, 10) == Tracker::Event::None);
        CHECK(poll(p1, Tracker::Stable - 1) == Tracker::Event::None); // not steady yet
        CHECK(poll(p1, 1) == Tracker::Event::Appeared && got == p1);
        CHECK(poll(p1, 50) == Tracker::Event::None);                  // the menu is not opened again
        CHECK(poll({}, Tracker::Stable) == Tracker::Event::Gone);
        CHECK(poll(p2, Tracker::Stable) == Tracker::Event::Appeared && got == p2);
        CHECK(poll(p1, 1) == Tracker::Event::None && poll({}, 1) == Tracker::Event::None); // a flicker is ignored
        CHECK(poll(p2, Tracker::Stable) == Tracker::Event::None && tracker.Current() == p2);
        CHECK(poll(p1, Tracker::Stable) == Tracker::Event::Appeared && got == p1); // straight from one player to the other
        CHECK(poll(many, Tracker::Stable) == Tracker::Event::Gone);                // "too many" is not a request
        CHECK(poll(many, 20) == Tracker::Event::None);
        CHECK(poll({}, Tracker::Stable) == Tracker::Event::None);
        // A death (Swap Force's Dead state) arrives as its own episode with the flag, straight after the Idle prompt too.
        const Prompt dead{ Kind::Player, 1, true };
        CHECK(poll(dead, Tracker::Stable) == Tracker::Event::Appeared && got.defeated && got.player == 1);
        CHECK(poll(p1, Tracker::Stable) == Tracker::Event::Appeared && !got.defeated);
        CHECK(poll(dead, Tracker::Stable) == Tracker::Event::Appeared && got.defeated);
        CHECK(poll({}, Tracker::Stable) == Tracker::Event::Gone);
    }

    // A variant figure (Series 2 Eruptor, variant 0x1801 at block 1 0x1C): the portal hands the title the tag as it
    // is. Spyro's Adventure reads the toy type (0x10), Giants also the variant (docs/runtime/host/figures.md).
    {
        FigureData series = CreateBlankFigure(9);
        series[0x1C] = 0x01;
        series[0x1D] = 0x18;
        uint16_t crc = FigureChecksum(series.data(), 0x1E);
        series[0x1E] = uint8_t(crc);
        series[0x1F] = uint8_t(crc >> 8);
        EmulatedPortal other;
        CHECK(other.Place(series, "") == 0);
        Send(other, { 'R' });
        Send(other, { 'A', 0x01 });
        Send(other, { 'Q', 0x10, 0x01 });
        std::vector<uint8_t> block;
        for (int i = 0; i < 8 && (block.empty() || block[0] != 'Q'); i++)
            block = Receive(other);
        CHECK(block.size() > 19 && block[0] == 'Q' && block[3] == 9 && block[3 + 0x0C] == 0x01 && block[3 + 0x0D] == 0x18);
    }

    SDL_RemovePath(path.c_str());
    printf("portal test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
