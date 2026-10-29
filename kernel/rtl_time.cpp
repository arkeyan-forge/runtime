// Conversions between the kernel's time (100 ns intervals since 1601-01-01) and TIME_FIELDS (CSHORT year, month, day,
// hour, minute, second, milliseconds, weekday). XAPI's FileTimeToSystemTime/SystemTimeToFileTime use them; the title
// stamps its save games with them.
#include "memory.h"
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr int64_t TicksPerMillisecond = 10000;
    constexpr int64_t TicksPerDay = 24ll * 60 * 60 * 1000 * TicksPerMillisecond;

    // Days since 1601-01-01 of a proleptic Gregorian date (years counted from March, so leap days come last).
    int64_t DaysFromCivil(int64_t year, int64_t month, int64_t day)
    {
        year -= month <= 2;
        int64_t era = (year >= 0 ? year : year - 399) / 400;
        int64_t yearOfEra = year - era * 400;
        int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        constexpr int64_t DaysTo1601 = 584694; // DaysFromCivil(1601, 1, 1) counted from 0000-03-01
        return era * 146097 + dayOfEra - DaysTo1601;
    }

    void CivilFromDays(int64_t days, int64_t& year, int64_t& month, int64_t& day)
    {
        days += 584694;
        int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        int64_t dayOfEra = days - era * 146097;
        int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
        int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
        int64_t monthIndex = (5 * dayOfYear + 2) / 153;
        day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
        month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
        year = yearOfEra + era * 400 + (month <= 2);
    }
}

// RtlTimeToTimeFields(const LARGE_INTEGER* time, TIME_FIELDS* fields)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    int64_t time = g_memory.Read<int64_t>(ctx.r3.u32);
    uint32_t fields = ctx.r4.u32;

    int64_t days = time / TicksPerDay;
    int64_t milliseconds = (time % TicksPerDay) / TicksPerMillisecond;
    int64_t year, month, day;
    CivilFromDays(days, year, month, day);

    g_memory.Write<int16_t>(fields + 0, int16_t(year));
    g_memory.Write<int16_t>(fields + 2, int16_t(month));
    g_memory.Write<int16_t>(fields + 4, int16_t(day));
    g_memory.Write<int16_t>(fields + 6, int16_t(milliseconds / 3600000));
    g_memory.Write<int16_t>(fields + 8, int16_t(milliseconds / 60000 % 60));
    g_memory.Write<int16_t>(fields + 10, int16_t(milliseconds / 1000 % 60));
    g_memory.Write<int16_t>(fields + 12, int16_t(milliseconds % 1000));
    g_memory.Write<int16_t>(fields + 14, int16_t((days + 1) % 7)); // 1601-01-01 was a Monday; Sunday is 0
}

// RtlTimeFieldsToTime(const TIME_FIELDS* fields, LARGE_INTEGER* time): FALSE for an invalid date.
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    uint32_t fields = ctx.r3.u32;
    int64_t year = g_memory.Read<int16_t>(fields + 0);
    int64_t month = g_memory.Read<int16_t>(fields + 2);
    int64_t day = g_memory.Read<int16_t>(fields + 4);
    int64_t hour = g_memory.Read<int16_t>(fields + 6);
    int64_t minute = g_memory.Read<int16_t>(fields + 8);
    int64_t second = g_memory.Read<int16_t>(fields + 10);
    int64_t millisecond = g_memory.Read<int16_t>(fields + 12);

    static constexpr int DaysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    bool valid = year >= 1601 && month >= 1 && month <= 12 && day >= 1 &&
                 day <= DaysInMonth[month - 1] + (month == 2 && leap) && hour >= 0 && hour < 24 && minute >= 0 &&
                 minute < 60 && second >= 0 && second < 60 && millisecond >= 0 && millisecond < 1000;
    if (!valid)
    {
        ctx.r3.u64 = 0;
        return;
    }

    int64_t milliseconds = ((hour * 60 + minute) * 60 + second) * 1000 + millisecond;
    g_memory.Write<int64_t>(
        ctx.r4.u32, DaysFromCivil(year, month, day) * TicksPerDay + milliseconds * TicksPerMillisecond);
    ctx.r3.u64 = 1;
}
