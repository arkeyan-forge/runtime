// Console configuration the title reads: XConfig settings, AV pack, regions, language and dashboard version. The
// emulated console is a North American retail unit on an HD output, in the language the host picks (English by
// default, see language.h).
#include "language.h"
#include "memory.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>
#include <string_view>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusBufferTooSmall = 0xC0000023;
    constexpr uint32_t StatusInvalidParameter1 = 0xC00000EF;
    constexpr uint32_t StatusInvalidParameter2 = 0xC00000F0;

    constexpr uint16_t SecuredCategory = 2;
    constexpr uint16_t SecuredAvRegion = 2;
    constexpr uint16_t SecuredGameRegion = 3;

    constexpr uint16_t UserCategory = 3;
    constexpr uint16_t UserTimeZoneBias = 1;
    constexpr uint16_t UserTimeZoneStandardName = 2;
    constexpr uint16_t UserTimeZoneDaylightName = 3;
    constexpr uint16_t UserTimeZoneStandardDate = 4;
    constexpr uint16_t UserTimeZoneDaylightDate = 5;
    constexpr uint16_t UserTimeZoneStandardBias = 6;
    constexpr uint16_t UserTimeZoneDaylightBias = 7;
    constexpr uint16_t UserRetailFlags = 12;
    constexpr uint16_t UserLanguage = 9;
    constexpr uint16_t UserVideoFlags = 10;
    constexpr uint16_t UserCountry = 14;

    // The title only tests the AV region for PAL ((value & 0xFF00) == 0x0300, sub_822DF218).
    constexpr uint32_t AvRegionNorthAmerica = 0x00001000;
    constexpr uint32_t GameRegionNorthAmerica = 0x000000FF;
    constexpr uint32_t VideoFlagsWidescreen720p = 0x00030000;
    constexpr uint8_t CountryUnitedStates = 103; // XC_COUNTRY_US; the title maps codes 5..109 to its territories
    // sub_822DF218 warns about the video output unless the AV pack is one of 3, 4, 6 or 8; 6 is an HD pack (VGA).
    constexpr uint32_t AvPackHd = 6;
    // Dashboard 2.0.17559.0, the last one: (major << 28) | (minor << 24) | (build << 8) | qfe.
    constexpr uint32_t SystemVersion = (2u << 28) | (0u << 24) | (17559u << 8) | 0u;

    // The time zone XAPI's GetTimeZoneInformation assembles from settings 3/1-3/7 (sub_822E4A18): the host's current
    // offset as the bias (minutes to add to local time to get UTC), daylight saving included, and no transition dates
    // (month 0), so local time is right today. The title stamps its saves with it.
    int32_t TimeZoneBias()
    {
        time_t now = time(nullptr);
        std::tm utc = *gmtime(&now);
        utc.tm_isdst = -1; // read the UTC fields back as local time
        return int32_t(difftime(mktime(&utc), now) / 60);
    }

    // The console language (XC_LANGUAGE_*) and country (XC_COUNTRY_*) behind each language code. Dutch, Swedish,
    // Norwegian, Danish and Finnish have no console language: the title takes them from the country (sub_82808190 via
    // sub_822DECD0, which maps these five countries to them), whatever the console language says.
    struct ConsoleLanguage
    {
        std::string_view code;
        uint32_t language;
        uint8_t country;
    };

    constexpr ConsoleLanguage ConsoleLanguages[] = {
        { "en", 1, CountryUnitedStates },
        { "ja", 2, CountryUnitedStates },
        { "de", 3, CountryUnitedStates },
        { "fr", 4, CountryUnitedStates },
        { "es", 5, CountryUnitedStates },
        { "it", 6, CountryUnitedStates },
        { "ko", 7, CountryUnitedStates },
        { "zh-tw", 8, CountryUnitedStates },
        { "pt", 9, CountryUnitedStates },
        { "zh-cn", 10, CountryUnitedStates },
        { "pl", 11, CountryUnitedStates },
        { "ru", 12, CountryUnitedStates },
        { "nl", 1, 74 /* Netherlands */ },
        { "sv", 1, 90 /* Sweden */ },
        { "no", 1, 75 /* Norway */ },
        { "da", 1, 25 /* Denmark */ },
        { "fi", 1, 32 /* Finland */ },
    };

    const ConsoleLanguage* g_language = &ConsoleLanguages[0]; // English

    uint32_t Language() { return g_language->language; }

    void ReportUnknownSetting(uint16_t category, uint16_t setting)
    {
        static std::mutex mutex;
        static std::set<uint32_t> reported;
        std::lock_guard lock(mutex);
        if (reported.insert((uint32_t(category) << 16) | setting).second)
            fprintf(stderr, "[xconfig] setting %u/%u not available\n", category, setting);
    }
}

// ExGetXConfigSetting(category, setting, buffer, buffer size, required size out): NTSTATUS.
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    uint16_t category = uint16_t(ctx.r3.u32);
    uint16_t setting = uint16_t(ctx.r4.u32);
    uint32_t buffer = ctx.r5.u32;
    uint16_t bufferSize = uint16_t(ctx.r6.u32);
    uint32_t requiredSize = ctx.r7.u32;

    uint8_t value[4];
    uint16_t size = 4;
    auto store32 = [&](uint32_t v) {
        v = ByteSwap(v);
        memcpy(value, &v, 4);
    };

    if (category == UserCategory && setting == UserTimeZoneBias)
        store32(uint32_t(TimeZoneBias()));
    else if (category == UserCategory && (setting == UserTimeZoneStandardName || setting == UserTimeZoneDaylightName))
    {
        // Four characters, not terminated: the host zone's abbreviation ("CEST").
        char name[8] = {};
        time_t now = time(nullptr);
        strftime(name, sizeof(name), "%Z", localtime(&now));
        memcpy(value, name, 4);
    }
    else if (category == UserCategory && (setting == UserTimeZoneStandardDate || setting == UserTimeZoneDaylightDate ||
                                             setting == UserTimeZoneStandardBias ||
                                             setting == UserTimeZoneDaylightBias || setting == UserRetailFlags))
        store32(0);
    else if (category == SecuredCategory && setting == SecuredAvRegion)
        store32(AvRegionNorthAmerica);
    else if (category == SecuredCategory && setting == SecuredGameRegion)
        store32(GameRegionNorthAmerica);
    else if (category == UserCategory && setting == UserLanguage)
        store32(Language());
    else if (category == UserCategory && setting == UserVideoFlags)
        store32(VideoFlagsWidescreen720p);
    else if (category == UserCategory && setting == UserCountry)
    {
        value[0] = g_language->country;
        size = 1;
    }
    else
    {
        ReportUnknownSetting(category, setting);
        ctx.r3.u64 =
            category == SecuredCategory || category == UserCategory ? StatusInvalidParameter2 : StatusInvalidParameter1;
        return;
    }

    if (requiredSize != 0)
        g_memory.Write<uint16_t>(requiredSize, size);
    if (bufferSize < size)
    {
        ctx.r3.u64 = StatusBufferTooSmall;
        return;
    }
    if (buffer != 0)
        memcpy(g_memory.Translate(buffer), value, size);
    ctx.r3.u64 = StatusSuccess;
}

bool SetTextLanguage(std::string_view code)
{
    for (const ConsoleLanguage& language : ConsoleLanguages)
    {
        if (language.code == code)
        {
            g_language = &language;
            return true;
        }
    }
    return false;
}

std::string_view TextLanguage() { return g_language->code; }

PPC_FUNC(__imp__XGetAVPack) { ctx.r3.u64 = AvPackHd; }

PPC_FUNC(__imp__XGetGameRegion) { ctx.r3.u64 = GameRegionNorthAmerica; }

PPC_FUNC(__imp__XGetLanguage) { ctx.r3.u64 = Language(); }

PPC_FUNC(__imp__XamGetSystemVersion) { ctx.r3.u64 = SystemVersion; }
