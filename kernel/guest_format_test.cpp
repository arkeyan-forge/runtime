// Test of the guest's printf (kernel/guest_format.h): flags, width, precision and the Microsoft conversions, on a small
// fake guest memory. Run with no arguments; exit code 0 = pass.
#include "kernel/guest_format.h"
#include "memory.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

GuestMemory g_memory;

namespace
{
    int g_checks;
    int g_failures;

    uint64_t Bits(double value)
    {
        uint64_t bits;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    void Expect(int line, const char* expected, const char* format, std::initializer_list<uint64_t> arguments = {})
    {
        std::vector<uint64_t> slots(arguments);
        size_t next = 0;
        std::string text = FormatGuest(format, [&] { return next < slots.size() ? slots[next++] : 0; });
        g_checks++;
        if (text != expected)
        {
            g_failures++;
            fprintf(
                stderr, "FAIL line %d: \"%s\" gave \"%s\", expected \"%s\"\n", line, format, text.c_str(), expected);
        }
    }
#define EXPECT(...) Expect(__LINE__, __VA_ARGS__) // EXPECT(expected, format, { arguments })

    uint8_t g_guest[0x400];
    constexpr uint64_t Narrow = 0x100; // "Skylanders"
    constexpr uint64_t Wide = 0x200; // u"Slot", big-endian
    constexpr uint64_t Short = 0x300; // "ab"

    void TestIntegers()
    {
        EXPECT("Skylanders - Slot 1", "Skylanders - Slot %d", { 1 }); // the game's save package names
        EXPECT("   42|42   |00042|+42| 42", "%5d|%-5d|%05d|%+d|% d", { 42, 42, 42, 42, 42 });
        EXPECT("007|     007||5", "%.3d|%08.3d|%.0d|%+u", { 7, 7, 0, 5 });
        EXPECT("0xff 0XFF 010 0 0", "%#x %#X %#o %#o %#x", { 255, 255, 8, 0, 0 });
        EXPECT("0x0000ff|0xff    |", "%#08x|%-#8x|", { 255, 255 });
        EXPECT("-1 4294967295 -32768 32768", "%d %u %hd %hu", { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFF8000, 0xFFFF8000 });
        EXPECT("-1 -9223372036854775808 123456789ABC -1", "%I64d %lld %I64X %I32d",
            { ~0ull, 0x8000000000000000ull, 0x123456789ABCull, 0x1FFFFFFFFull });
    }

    void TestStarArguments()
    {
        EXPECT("[   5][5   ][5   ]", "[%*d][%-*d][%*d]", { 4, 5, 4, 5, uint32_t(-4), 5 });
        // A negative precision is no precision (it used to reach the host's printf as "%.-4", read as a width).
        EXPECT("[5][ab][0.500000]", "[%.*d][%.*s][%.*f]",
            { uint32_t(-4), 5, uint32_t(-1), Short, uint32_t(-1), Bits(0.5) });
        EXPECT("[   005]", "[%*.*d]", { 6, 3, 5 });
    }

    void TestStrings()
    {
        EXPECT(
            "Skylanders|Sky|  Skylanders|Slot    |(null)", "%s|%.3s|%12s|%-8S|%s", { Narrow, Narrow, Narrow, Wide, 0 });
        EXPECT("Slot|ab|w|000ab|x   |OK", "%ls|%hS|%wc|%05s|%-4c|%c%c", { Wide, Short, 'w', Short, 'x', 'O', 'K' });
    }

    void TestDoubles()
    {
        EXPECT("3.14|1.500000e+00|0.0001|-00002.500|+1.0| 2", "%.2f|%e|%g|%010.3f|%+.1f|% .0f",
            { Bits(3.14159), Bits(1.5), Bits(0.0001), Bits(-2.5), Bits(1.0), Bits(2.0) });
        EXPECT("3.|1.00000|1E-10|1.234500E+03", "%#.0f|%#g|%G|%E", { Bits(3.0), Bits(1.0), Bits(1e-10), Bits(1234.5) });
        // %a's digits are the host CRT's: UCRT, as the console's CRT, writes all 13 without a precision.
        char hex[128];
        snprintf(hex, sizeof(hex), "%a|%A|%010a|%-8.1a|", 1.0, -1.0, 1.0, 1.0);
        EXPECT(hex, "%a|%A|%010a|%-8.1a|", { Bits(1.0), Bits(-1.0), Bits(1.0), Bits(1.0) });
        EXPECT("     inf|-INF  |+inf", "%08f|%-6F|%+f", { Bits(INFINITY), Bits(-INFINITY), Bits(INFINITY) });
    }

    void TestOther()
    {
        EXPECT("100% 00001234 %q %y", "100%% %p %n%q %y", { 0x1234, 0 });
        EXPECT("end %5", "end %5");
        EXPECT("1.000000 3 4", "%Lf %zu %td", { Bits(1.0), 3, 4 });
    }
}

int main()
{
    g_memory.base = g_guest;
    strcpy(reinterpret_cast<char*>(g_guest + Narrow), "Skylanders");
    for (int i = 0; i < 4; i++)
        g_memory.Write<uint16_t>(uint32_t(Wide) + 2 * i, uint16_t("Slot"[i]));
    strcpy(reinterpret_cast<char*>(g_guest + Short), "ab");

    TestIntegers();
    TestStarArguments();
    TestStrings();
    TestDoubles();
    TestOther();
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
