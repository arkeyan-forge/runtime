// The kernel's C runtime formatting, with Microsoft's conversions (%S is a wide string, I64 a 64-bit length). The
// game's only vsprintf caller names its save packages ("Skylanders - Slot 1").
#include "memory.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    template<typename T>
    void AppendFormatted(std::string& out, const std::string& spec, T value)
    {
        int size = snprintf(nullptr, 0, spec.c_str(), value);
        if (size <= 0)
            return;
        size_t at = out.size();
        out.resize(at + size + 1);
        snprintf(out.data() + at, size + 1, spec.c_str(), value);
        out.resize(at + size);
    }

    std::string GuestNarrowString(uint32_t address)
    {
        return address != 0 ? static_cast<const char*>(g_memory.Translate(address)) : "(null)";
    }

    // UTF-16 (big-endian in guest memory), narrowed to Latin-1.
    std::string GuestWideString(uint32_t address)
    {
        if (address == 0)
            return "(null)";
        std::string text;
        for (uint16_t c; (c = g_memory.Read<uint16_t>(address)) != 0; address += 2)
            text += c < 0x100 ? char(c) : '?';
        return text;
    }

    // `next` yields the 8-byte argument slots in order: integers and pointers in the low word, doubles in the whole slot.
    std::string FormatGuest(const char* format, const std::function<uint64_t()>& next)
    {
        std::string out;
        for (const char* p = format; *p != '\0'; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }

            const char* start = p++;
            std::string spec = "%";
            while (*p != '\0' && strchr("-+ #0", *p) != nullptr)
                spec += *p++;
            for (int part = 0; part < 2; part++) // width, then precision
            {
                if (part == 1)
                {
                    if (*p != '.')
                        break;
                    spec += *p++;
                }
                if (*p == '*')
                {
                    spec += std::to_string(int32_t(next()));
                    p++;
                }
                while (isdigit(static_cast<unsigned char>(*p)))
                    spec += *p++;
            }

            enum class Size { Default, Short, Long, LongLong } size = Size::Default;
            if (*p == 'h')
            {
                size = Size::Short;
                p += p[1] == 'h' ? 2 : 1;
            }
            else if (*p == 'l')
            {
                size = p[1] == 'l' ? Size::LongLong : Size::Long;
                p += p[1] == 'l' ? 2 : 1;
            }
            else if (strncmp(p, "I64", 3) == 0)
            {
                size = Size::LongLong;
                p += 3;
            }
            else if (strncmp(p, "I32", 3) == 0)
            {
                p += 3;
            }
            else if (*p == 'w')
            {
                size = Size::Long;
                p++;
            }
            else if (*p == 'L' || *p == 'z' || *p == 't')
            {
                p++;
            }

            switch (char conversion = *p)
            {
            case 'd':
            case 'i':
            {
                uint64_t value = next();
                int64_t number = size == Size::LongLong ? int64_t(value)
                    : size == Size::Short ? int16_t(value) : int32_t(value);
                AppendFormatted(out, spec + "lld", (long long)number);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            {
                uint64_t value = next();
                uint64_t number = size == Size::LongLong ? value : size == Size::Short ? uint16_t(value) : uint32_t(value);
                AppendFormatted(out, spec + "ll" + conversion, (unsigned long long)number);
                break;
            }
            case 'p':
                AppendFormatted(out, std::string("%08X"), uint32_t(next()));
                break;
            case 'c':
            case 'C':
                AppendFormatted(out, spec + "c", int(uint8_t(next())));
                break;
            case 's':
            case 'S':
            {
                bool wide = conversion == 'S' ? size != Size::Short : size == Size::Long;
                uint32_t address = uint32_t(next());
                std::string text = wide ? GuestWideString(address) : GuestNarrowString(address);
                AppendFormatted(out, spec + "s", text.c_str());
                break;
            }
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                uint64_t bits = next();
                double value;
                memcpy(&value, &bits, sizeof(value));
                AppendFormatted(out, spec + conversion, value);
                break;
            }
            case 'n':
                next(); // the count is not written back, as with the Microsoft CRT's default
                break;
            case '%':
                out += '%';
                break;
            case '\0':
                out.append(start);
                return out;
            default:
                out.append(start, p + 1);
                break;
            }
        }
        return out;
    }
}

// vsprintf(buffer, format, va_list)
PPC_FUNC(__imp__vsprintf)
{
    // A guest va_list points at the slots.
    uint32_t args = ctx.r5.u32;
    std::string text = FormatGuest(static_cast<const char*>(g_memory.Translate(ctx.r4.u32)), [&]
    {
        uint64_t value = g_memory.Read<uint64_t>(args);
        args += 8;
        return value;
    });
    memcpy(g_memory.Translate(ctx.r3.u32), text.c_str(), text.size() + 1);
    ctx.r3.u64 = text.size();
}

// _snprintf(buffer, count, format, ...): the variable arguments are r6..r10, then the 8-byte stack slots from r1+0x50
// (the ninth argument's low word is at 0x54). Doubles are read from the same slots (unverified). Like the Microsoft
// CRT: no terminator when the text fills `count`, and -1 when it does not fit.
PPC_FUNC(__imp___snprintf)
{
    uint32_t buffer = ctx.r3.u32;
    uint32_t count = ctx.r4.u32;
    uint64_t registers[] = { ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64 };
    uint32_t index = 0;
    uint32_t stack = ctx.r1.u32 + 0x50;
    std::string text = FormatGuest(static_cast<const char*>(g_memory.Translate(ctx.r5.u32)), [&]
    {
        if (index < 5)
            return registers[index++];
        uint64_t value = g_memory.Read<uint64_t>(stack);
        stack += 8;
        return value;
    });

    if (text.size() <= count)
    {
        memcpy(g_memory.Translate(buffer), text.c_str(), text.size() < count ? text.size() + 1 : text.size());
        ctx.r3.u64 = text.size();
    }
    else
    {
        memcpy(g_memory.Translate(buffer), text.c_str(), count);
        ctx.r3.u64 = uint64_t(int64_t(-1));
    }
}
