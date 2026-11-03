#pragma once

// Allocation-free presentation logic, independent of the renderer and the host clock.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string_view>

namespace debugscreen
{
    constexpr size_t MaxFrames = 2048, GraphBars = 60;
    constexpr uint64_t RefreshNs = 500'000'000;

    struct Rate
    {
        uint64_t time = 0, count = 0;
        double fps = 0;
        bool started = false, ready = false;
        bool Update(uint64_t now, uint64_t frames)
        {
            if (!started || now < time || frames < count)
            {
                time = now;
                count = frames;
                started = true;
                fps = 0;
                ready = false;
                return true;
            }
            if (now - time < RefreshNs)
                return false;
            fps = double(frames - count) * 1e9 / double(now - time);
            time = now;
            count = frames;
            ready = true;
            return true;
        }
    };

    // Reciprocal of the mean duration of the slowest ceil(1%) frames, not a percentile of FPS.
    inline double LowOnePercent(std::span<const uint64_t> frames)
    {
        std::array<uint64_t, MaxFrames> sorted{};
        size_t count = 0;
        for (uint64_t ns : frames.last(std::min(frames.size(), MaxFrames)))
            if (ns != 0)
                sorted[count++] = ns;
        if (count == 0)
            return 0;
        std::sort(sorted.begin(), sorted.begin() + count, std::greater<uint64_t>());
        size_t slow = (count + 99) / 100;
        double sum = 0;
        for (size_t i = 0; i < slow; i++)
            sum += double(sorted[i]);
        return double(slow) * 1e9 / sum;
    }

    inline void Bytes(char* out, size_t capacity, uint64_t bytes, bool known = true)
    {
        if (!known)
            snprintf(out, capacity, "n/a");
        else if (bytes >= (1ull << 30))
            snprintf(out, capacity, "%.2f GiB", double(bytes) / double(1ull << 30));
        else
            snprintf(out, capacity, "%.0f MiB", double(bytes) / double(1ull << 20));
    }

    // Only display labels supplied by the runtime, never a path. Reject control characters too.
    inline void Label(char* out, size_t capacity, std::string_view value)
    {
        if (capacity == 0)
            return;
        if (value.find_first_of("/\\@") != std::string_view::npos)
            value = "n/a";
        size_t length = 0;
        for (unsigned char c : value)
        {
            if (length + 1 >= capacity)
                break;
            out[length++] = c >= 32 && c != 127 ? char(c) : ' ';
        }
        // Do not leave the leading bytes of a truncated UTF-8 character at the buffer boundary.
        if (length < value.size())
            while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
                length--;
        out[length] = 0;
    }

    enum class Group
    {
        Game,
        Frame,
        Machine,
        Renderer,
        Devices
    };
    struct Row
    {
        Group group{};
        char label[32]{};
        char value[192]{};
    };
    struct Panel
    {
        std::array<Row, 20> rows{};
        size_t count = 0;
        void Add(Group group, const char* label, const char* value)
        {
            if (count == rows.size())
                return;
            Row& row = rows[count++];
            row.group = group;
            Label(row.label, sizeof(row.label), label);
            Label(row.value, sizeof(row.value), value);
        }
        bool StartsGroup(size_t row) const { return row == 0 || rows[row].group != rows[row - 1].group; }
    };

    struct Screen
    {
        Panel left, right;
        std::array<float, GraphBars> graphMs{};
        size_t graphCount = 0;
        Rate rate;
    };
}
