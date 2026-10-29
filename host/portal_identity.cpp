#include "host/portal_identity.h"
#include "host/portal_backend.h"
#include <cstdio>
#include <cstring>
#include <iterator>

namespace
{
    // Ids from the community table of `R` replies (tsapZ/SwapForceRecomp, docs/portal-compatibility.md, MIT).
    const PortalModel Models[] = {
        { { 0x01, 0x29 }, 2, "Spyro's Adventure portal (Wii, PS3)" },
        { { 0x01, 0x28 }, 2, "Spyro's Adventure portal (Xbox 360)" },
        { { 0x01, 0x2C }, 2, "Spyro's Adventure portal (PC, Mac)" },
        { { 0x01, 0x34 }, 2, "Spyro's Adventure / Giants portal (3DS)" },
        { { 0x01, 0x3D }, 2, "Giants portal" },
        { { 0x01, 0x3C }, 2, "Giants portal" },
        { { 0x01, 0x48 }, 2, "Giants portal (Xbox 360)" },
        { { 0x02, 0x03 }, 2, "Swap Force portal" },
        { { 0x02, 0x02 }, 2, "Swap Force portal (3DS)" },
        { { 0x02, 0x18 }, 2, "Traptanium portal" },
        { { 0x02, 0x1B }, 2, "Traptanium portal (Xbox 360)" },
        { { 0x02, 0x19 }, 2, "Trap Team portal (mobile)" },
        { { 0x02, 0x27 }, 2, "Traptanium portal (Xbox One)" },
        { { 0x02, 0x0A, 0x03, 0x02 }, 4, "SuperChargers portal" },
        { { 0x02, 0x14, 0x03, 0x02 }, 4, "SuperChargers portal (Xbox One)" },
        { { 0x02, 0x0A, 0x05, 0x08 }, 4, "Imaginators portal" },
    };

    constexpr uint16_t SsaMinimumVersion = 0x011F; // the title's own check (docs/runtime/host/portal.md)

    uint16_t Version(const uint8_t* id, size_t length) { return length >= 2 ? uint16_t(id[0] << 8 | id[1]) : 0; }

    const char* GameName(const char* game)
    {
        if (strcmp(game, "ssa") == 0)
            return "Spyro's Adventure";
        if (strcmp(game, "giants") == 0)
            return "Giants";
        if (strcmp(game, "swapforce") == 0)
            return "Swap Force";
        return game;
    }
}

const PortalModel* FindPortalModel(const uint8_t* id, size_t length)
{
    for (const PortalModel& model : Models)
    {
        if (model.idLength == length && memcmp(model.id, id, length) == 0)
            return &model;
    }
    return nullptr;
}

std::string PortalIdText(const uint8_t* id, size_t length)
{
    std::string text;
    for (size_t i = 0; i < length; i++)
    {
        char byte[4];
        snprintf(byte, sizeof(byte), i == 0 ? "%02X" : " %02X", id[i]);
        text += byte;
    }
    return text;
}

PortalVerdict PortalVerdictFor(const char* game, const uint8_t* id, size_t length, std::string* reason)
{
    auto say = [&](const char* text) {
        if (reason != nullptr)
            *reason = text;
    };
    if (length < 2)
    {
        say("the portal sent no id");
        return PortalVerdict::Unknown;
    }
    if (strcmp(game, "ssa") == 0)
    {
        // The title accepts a portal whose version, (b1 << 8 | b2), is at least 0x011F and calls older ones out of date.
        // Every model in the table is above that, so any real portal passes; a portal of a later generation too.
        if (Version(id, length) < SsaMinimumVersion)
        {
            say("the game calls a portal with a version under 01 1F out of date");
            return PortalVerdict::Rejected;
        }
        return PortalVerdict::Works;
    }
    if (strcmp(game, "giants") == 0)
    {
        // Public reports: any portal of the console family works (not checked against the disc).
        say("not checked against the game");
        return PortalVerdict::Unknown;
    }
    if (strcmp(game, "swapforce") == 0)
    {
        // The title's portal driver has SSA's check (a version under 0x011F is "out of date"); then the game sorts the
        // portal by its first id byte and wants the Swap Force kind: 1 (Spyro's Adventure and Giants portals) shows
        // "WRONG PORTAL VERSION: An old Portal of Power is connected to the console" and the game waits there; 2 and
        // up (Swap Force and every later portal) plays. Seen in runs with 01 28, 01 48 and 02 03 (games/swapforce/docs/status.md).
        if (Version(id, length) < SsaMinimumVersion)
        {
            say("the game calls a portal with a version under 01 1F out of date");
            return PortalVerdict::Rejected;
        }
        if (id[0] == 1)
        {
            say("\"Wrong Portal Version\": it wants a Swap Force portal or a later one");
            return PortalVerdict::Rejected;
        }
        return PortalVerdict::Works;
    }
    say("nothing is known about this game");
    return PortalVerdict::Unknown;
}

const PortalModel& EmulatedPortalModel(const char* game)
{
    uint8_t id[2] = { 0x01, 0x28 }; // SSA
    if (strcmp(game, "giants") == 0)
        id[1] = 0x48;
    else if (strcmp(game, "swapforce") == 0)
        id[0] = 0x02, id[1] = 0x03;
    else if (strcmp(game, "ssa") != 0)
        id[0] = 0x02, id[1] = 0x1B;
    return *FindPortalModel(id, 2);
}

std::string DescribePortal(const char* game, const uint8_t* id, size_t length)
{
    const PortalModel* model = FindPortalModel(id, length);
    std::string text = PortalIdText(id, length) + " " + (model != nullptr ? model->name : "unknown portal");
    if (game == nullptr)
        return text;

    std::string reason;
    PortalVerdict verdict = PortalVerdictFor(game, id, length, &reason);
    text += std::string("; ") + GameName(game) + ": ";
    switch (verdict)
    {
    case PortalVerdict::Works: text += "works"; break;
    case PortalVerdict::Warns: text += "the game warns (" + reason + ")"; break;
    case PortalVerdict::Rejected: text += "the game rejects it (" + reason + ")"; break;
    case PortalVerdict::Unknown: text += "unknown (" + reason + ")"; break;
    }
    return text;
}

bool PortalIdFromReset(const uint8_t* report, size_t size, uint8_t id[4], size_t& length)
{
    if (size < 3 || report[0] != 'R')
        return false;
    length = size >= 5 && FindPortalModel(report + 1, 4) != nullptr ? 4 : 2;
    memcpy(id, report + 1, length);
    return true;
}

void NameProbe(PortalProbe& probe, const uint8_t* report, size_t size)
{
    uint8_t id[4];
    size_t length;
    if (!PortalIdFromReset(report, size, id, length))
        return;
    const PortalModel* model = FindPortalModel(id, length);
    probe.id = PortalIdText(id, length);
    probe.model = model != nullptr ? model->name : "unknown portal";
    PortalVerdict verdict = PortalVerdictFor("ssa", id, length, &probe.verdictReason);
    probe.verdict = verdict == PortalVerdict::Works      ? "works"
                    : verdict == PortalVerdict::Warns    ? "warns"
                    : verdict == PortalVerdict::Rejected ? "rejected"
                                                         : "unknown";
}
