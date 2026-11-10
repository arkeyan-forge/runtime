// The Toy Box link's messages (toybox_link.h). Shared word for word with the launcher (src/shared/toybox_link.cpp, tools/sync_collection.sh).
#include "toybox_link.h"
#include <charconv>
#include <random>

namespace toyboxlink
{
    namespace
    {
        bool ToInt(std::string_view text, long long& value)
        {
            if (text.empty())
                return false;
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc() && end == text.data() + text.size();
        }

        bool ToFlag(std::string_view text, bool& value)
        {
            if (text != "0" && text != "1")
                return false;
            value = text == "1";
            return true;
        }
    }

    std::string Format(const Message& message)
    {
        std::string line = message.verb;
        for (const std::string& field : message.fields)
        {
            line += '\t';
            line += field;
        }
        if (message.verb.empty() || line.find_first_of("\r\n") != std::string::npos ||
            message.verb.find('\t') != std::string::npos || line.size() >= MaxLine)
            return {};
        for (const std::string& field : message.fields)
        {
            if (field.find('\t') != std::string::npos)
                return {};
        }
        line += '\n';
        return line;
    }

    bool Parse(std::string_view line, Message& message)
    {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        message = {};
        if (line.empty())
            return false;
        size_t begin = 0;
        bool first = true;
        while (true)
        {
            size_t tab = line.find('\t', begin);
            std::string_view part =
                line.substr(begin, tab == std::string_view::npos ? std::string_view::npos : tab - begin);
            if (first)
                message.verb = std::string(part);
            else
                message.fields.emplace_back(part);
            first = false;
            if (tab == std::string_view::npos)
                break;
            begin = tab + 1;
        }
        return !message.verb.empty();
    }

    void LineBuffer::Append(const char* data, size_t size)
    {
        m_data.append(data, size);
        size_t end = m_data.find('\n');
        if ((end == std::string::npos ? m_data.size() : end) > MaxLine)
            m_overflow = true;
    }

    bool LineBuffer::Next(std::string& line)
    {
        size_t end = m_data.find('\n');
        if (end == std::string::npos || m_overflow)
            return false;
        line.assign(m_data, 0, end);
        m_data.erase(0, end + 1);
        // The next line may be complete and too long already.
        size_t next = m_data.find('\n');
        if ((next == std::string::npos ? m_data.size() : next) > MaxLine)
            m_overflow = true;
        return true;
    }

    Message SeatMessage(int index, const Seat& seat)
    {
        Message message{ "seat", { std::to_string(index) } };
        switch (seat.kind)
        {
        case Seat::Kind::Empty: message.fields.push_back("empty"); break;
        case Seat::Kind::Virtual:
            message.fields.insert(message.fields.end(), { "virtual", seat.path, seat.path2, seat.taken ? "1" : "0" });
            break;
        case Seat::Kind::Real:
            message.fields.insert(message.fields.end(),
                { "real", std::to_string(seat.type), std::to_string(seat.variant), seat.known ? "1" : "0" });
            break;
        }
        return message;
    }

    bool ParseSeat(const Message& message, int& index, Seat& seat)
    {
        const std::vector<std::string>& f = message.fields;
        long long value = 0;
        if (message.verb != "seat" || f.size() < 2 || !ToInt(f[0], value) || value < 0 || value > 64)
            return false;
        index = int(value);
        seat = {};
        if (f[1] == "empty")
            return f.size() == 2;
        if (f[1] == "virtual")
        {
            seat.kind = Seat::Kind::Virtual;
            seat.path = f.size() == 5 ? f[2] : std::string();
            seat.path2 = f.size() == 5 ? f[3] : std::string();
            return f.size() == 5 && !seat.path.empty() && ToFlag(f[4], seat.taken);
        }
        if (f[1] == "real")
        {
            long long type = 0, variant = 0;
            seat.kind = Seat::Kind::Real;
            if (f.size() != 5 || !ToInt(f[2], type) || !ToInt(f[3], variant) || type < 0 || type > 0xFFFF ||
                variant < 0 || variant > 0xFFFF || !ToFlag(f[4], seat.known))
                return false;
            seat.type = uint16_t(type);
            seat.variant = uint16_t(variant);
            return true;
        }
        return false;
    }

    std::string FormatEndpoint(const Endpoint& endpoint)
    {
        return "skytoybox " + std::to_string(endpoint.version) + " " + std::to_string(endpoint.port) + " " +
               endpoint.token + " " + std::to_string(endpoint.pid) + "\n";
    }

    bool ParseEndpoint(std::string_view text, Endpoint& endpoint)
    {
        std::vector<std::string_view> words;
        for (size_t begin = 0; begin < text.size();)
        {
            size_t end = text.find_first_of(" \t\r\n", begin);
            if (end == std::string_view::npos)
                end = text.size();
            if (end > begin)
                words.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        long long version = 0, port = 0, pid = 0;
        if (words.size() != 5 || words[0] != "skytoybox" || !ToInt(words[1], version) || !ToInt(words[2], port) ||
            port <= 0 || port > 65535 || words[3].size() < 16 || !ToInt(words[4], pid) || pid < 0 || pid > 0xFFFFFFFFLL)
            return false;
        endpoint.version = int(version);
        endpoint.port = int(port);
        endpoint.token = std::string(words[3]);
        endpoint.pid = uint32_t(pid);
        return true;
    }

    std::string NewToken()
    {
        std::random_device source;
        static constexpr char Digits[] = "0123456789abcdef";
        std::string token;
        for (int i = 0; i < 8; i++)
        {
            uint32_t word = source();
            for (int nibble = 0; nibble < 4; nibble++)
                token += Digits[word >> (nibble * 4) & 15];
        }
        return token;
    }
}
