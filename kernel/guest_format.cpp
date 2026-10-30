// The guest's printf (kernel/guest_format.h). Flags, field width and precision are applied here: the host's snprintf
// only ever gets one of the literal formats below, never a format string from the guest.
#include "kernel/guest_format.h"
#include "memory.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace
{
    struct Spec
    {
        bool left = false; // '-'
        bool plus = false; // '+'
        bool space = false; // ' '
        bool alternate = false; // '#'
        bool zero = false; // '0'
        int width = 0;
        int precision = -1; // none
    };

    __attribute__((format(printf, 1, 2))) std::string Print(const char* format, ...)
    {
        va_list args, copy;
        va_start(args, format);
        va_copy(copy, args);
        int size = vsnprintf(nullptr, 0, format, copy);
        va_end(copy);
        std::string text;
        if (size > 0)
        {
            text.resize(size_t(size) + 1);
            vsnprintf(text.data(), text.size(), format, args);
            text.resize(size_t(size));
        }
        va_end(args);
        return text;
    }

    // `prefix` (sign, 0x) then `body`, padded to the field width with spaces, or with zeros between the two (the '0'
    // flag; strings take it too in the Microsoft CRT and on the hosts; `zeros` false where printf ignores it).
    void AppendField(std::string& out, const Spec& spec, std::string_view prefix, std::string_view body, bool zeros)
    {
        size_t length = prefix.size() + body.size();
        size_t fill = size_t(spec.width) > length ? size_t(spec.width) - length : 0;
        bool zeroFill = zeros && spec.zero && !spec.left;
        if (!spec.left && !zeroFill)
            out.append(fill, ' ');
        out += prefix;
        if (zeroFill)
            out.append(fill, '0');
        out += body;
        if (spec.left)
            out.append(fill, ' ');
    }

    // d u o x X; a negative d comes as its magnitude.
    void AppendInteger(std::string& out, const Spec& spec, char conversion, unsigned long long magnitude, bool negative)
    {
        std::string digits = conversion == 'o'   ? Print("%.*llo", spec.precision, magnitude)
                             : conversion == 'x' ? Print("%.*llx", spec.precision, magnitude)
                             : conversion == 'X' ? Print("%.*llX", spec.precision, magnitude)
                                                 : Print("%.*llu", spec.precision, magnitude);
        std::string prefix = negative                          ? "-"
                             : conversion == 'd' && spec.plus  ? "+"
                             : conversion == 'd' && spec.space ? " "
                                                               : "";
        if (spec.alternate && conversion == 'o' && !digits.starts_with('0'))
            digits.insert(0, 1, '0');
        if (spec.alternate && (conversion == 'x' || conversion == 'X') && magnitude != 0)
            prefix = conversion == 'x' ? "0x" : "0X";
        AppendField(out, spec, prefix, digits, spec.precision < 0); // a precision turns the '0' flag off
    }

    // e E f F g G a A: the uppercase ones are the lowercase text in uppercase (INF, 0X1P+0).
    void AppendDouble(std::string& out, const Spec& spec, char conversion, double value)
    {
        const int precision = spec.precision;
        std::string text;
        switch (tolower(conversion))
        {
        case 'e': text = spec.alternate ? Print("%#.*e", precision, value) : Print("%.*e", precision, value); break;
        case 'f': text = spec.alternate ? Print("%#.*f", precision, value) : Print("%.*f", precision, value); break;
        case 'g': text = spec.alternate ? Print("%#.*g", precision, value) : Print("%.*g", precision, value); break;
        default: text = spec.alternate ? Print("%#.*a", precision, value) : Print("%.*a", precision, value); break;
        }
        if (isupper(static_cast<unsigned char>(conversion)))
            for (char& c : text)
                c = char(toupper(static_cast<unsigned char>(c)));
        // The sign and 0x go before the zeros of a zero-padded field.
        size_t split = text.starts_with('-') ? 1 : 0;
        std::string prefix = split != 0 ? "-" : spec.plus ? "+" : spec.space ? " " : "";
        if (text.compare(split, 2, "0x") == 0 || text.compare(split, 2, "0X") == 0)
        {
            prefix += text.substr(split, 2);
            split += 2;
        }
        AppendField(out, spec, prefix, std::string_view(text).substr(split), std::isfinite(value));
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
}

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
        Spec spec;
        for (; *p != '\0' && strchr("-+ #0", *p) != nullptr; p++)
        {
            switch (*p)
            {
            case '-': spec.left = true; break;
            case '+': spec.plus = true; break;
            case ' ': spec.space = true; break;
            case '#': spec.alternate = true; break;
            default: spec.zero = true; break;
            }
        }
        for (int part = 0; part < 2; part++) // width, then precision
        {
            if (part == 1)
            {
                if (*p != '.')
                    break;
                p++;
            }
            int64_t number = 0;
            if (*p == '*')
            {
                number = int32_t(next());
                p++;
            }
            else
            {
                while (isdigit(static_cast<unsigned char>(*p)))
                    number = std::min<int64_t>(number * 10 + (*p++ - '0'), INT32_MAX);
            }
            if (part == 0)
            {
                spec.left = spec.left || number < 0; // a negative '*' width is the '-' flag
                spec.width = int(std::min<int64_t>(std::abs(number), INT32_MAX));
            }
            else
            {
                spec.precision = number < 0 ? -1 : int(number); // a negative '*' precision is none
            }
        }

        enum class Size
        {
            Default,
            Short,
            Long,
            LongLong
        } size = Size::Default;
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
                             : size == Size::Short  ? int16_t(value)
                                                    : int32_t(value);
            AppendInteger(out, spec, 'd', number < 0 ? 0 - uint64_t(number) : uint64_t(number), number < 0);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        {
            uint64_t value = next();
            uint64_t number = size == Size::LongLong ? value : size == Size::Short ? uint16_t(value) : uint32_t(value);
            AppendInteger(out, spec, conversion, number, false);
            break;
        }
        case 'p': out += Print("%08X", uint32_t(next())); break;
        case 'c':
        case 'C':
        {
            const char c = char(uint8_t(next()));
            AppendField(out, spec, "", std::string_view(&c, 1), true);
            break;
        }
        case 's':
        case 'S':
        {
            bool wide = conversion == 'S' ? size != Size::Short : size == Size::Long;
            uint32_t address = uint32_t(next());
            std::string text = wide ? GuestWideString(address) : GuestNarrowString(address);
            std::string_view body = text;
            AppendField(out, spec, "", spec.precision >= 0 ? body.substr(0, size_t(spec.precision)) : body, true);
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
            AppendDouble(out, spec, conversion, value);
            break;
        }
        case 'n':
            next(); // the count is not written back, as with the Microsoft CRT's default
            break;
        case '%': out += '%'; break;
        case '\0': out.append(start); return out;
        default: out.append(start, p + 1); break;
        }
    }
    return out;
}
