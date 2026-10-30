#include "json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace collection
{
    namespace
    {
        struct Parser
        {
            const std::string& text;
            size_t at = 0;
            std::string error{};

            bool Fail(const char* what)
            {
                if (error.empty())
                {
                    int line = 1;
                    for (size_t i = 0; i < at && i < text.size(); i++)
                        line += text[i] == '\n';
                    error = "line " + std::to_string(line) + ": " + what;
                }
                return false;
            }

            void Space()
            {
                while (
                    at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
                    at++;
            }

            bool Literal(const char* word)
            {
                size_t length = strlen(word);
                if (text.compare(at, length, word) != 0)
                    return false;
                at += length;
                return true;
            }

            static void AppendUtf8(std::string& out, uint32_t c)
            {
                if (c < 0x80)
                    out += char(c);
                else if (c < 0x800)
                {
                    out += char(0xC0 | c >> 6);
                    out += char(0x80 | (c & 0x3F));
                }
                else if (c < 0x10000)
                {
                    out += char(0xE0 | c >> 12);
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                }
                else
                {
                    out += char(0xF0 | c >> 18);
                    out += char(0x80 | (c >> 12 & 0x3F));
                    out += char(0x80 | (c >> 6 & 0x3F));
                    out += char(0x80 | (c & 0x3F));
                }
            }

            bool Hex4(uint32_t& value)
            {
                if (at + 4 > text.size())
                    return Fail("bad \\u escape");
                value = 0;
                for (int i = 0; i < 4; i++)
                {
                    char c = text[at++];
                    int digit = c >= '0' && c <= '9'   ? c - '0'
                                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                       : -1;
                    if (digit < 0)
                        return Fail("bad \\u escape");
                    value = value << 4 | uint32_t(digit);
                }
                return true;
            }

            bool String(std::string& out)
            {
                at++; // opening quote
                while (at < text.size() && text[at] != '"')
                {
                    char c = text[at++];
                    if (uint8_t(c) < 0x20)
                        return Fail("control character in a string");
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (at >= text.size())
                        break;
                    char escape = text[at++];
                    switch (escape)
                    {
                    case '"':
                    case '\\':
                    case '/': out += escape; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        uint32_t c1;
                        if (!Hex4(c1))
                            return false;
                        if (c1 >= 0xD800 && c1 < 0xDC00 && Literal("\\u"))
                        {
                            uint32_t c2;
                            if (!Hex4(c2))
                                return false;
                            c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                        }
                        AppendUtf8(out, c1);
                        break;
                    }
                    default: return Fail("bad escape");
                    }
                }
                if (at >= text.size())
                    return Fail("unterminated string");
                at++;
                return true;
            }

            bool Value(Json& out, int depth)
            {
                if (depth > 64)
                    return Fail("nested too deeply");
                Space();
                if (at >= text.size())
                    return Fail("value expected");
                char c = text[at];
                if (c == '{')
                {
                    at++;
                    out = Json::Object();
                    Space();
                    if (at < text.size() && text[at] == '}')
                    {
                        at++;
                        return true;
                    }
                    for (;;)
                    {
                        Space();
                        std::string key;
                        if (at >= text.size() || text[at] != '"' || !String(key))
                            return Fail("member name expected");
                        Space();
                        if (at >= text.size() || text[at] != ':')
                            return Fail("':' expected");
                        at++;
                        Json value;
                        if (!Value(value, depth + 1))
                            return false;
                        out.Set(key, std::move(value));
                        Space();
                        if (at < text.size() && text[at] == ',')
                        {
                            at++;
                            continue;
                        }
                        if (at < text.size() && text[at] == '}')
                        {
                            at++;
                            return true;
                        }
                        return Fail("',' or '}' expected");
                    }
                }
                if (c == '[')
                {
                    at++;
                    out = Json::Array();
                    Space();
                    if (at < text.size() && text[at] == ']')
                    {
                        at++;
                        return true;
                    }
                    for (;;)
                    {
                        Json value;
                        if (!Value(value, depth + 1))
                            return false;
                        out.Push(std::move(value));
                        Space();
                        if (at < text.size() && text[at] == ',')
                        {
                            at++;
                            continue;
                        }
                        if (at < text.size() && text[at] == ']')
                        {
                            at++;
                            return true;
                        }
                        return Fail("',' or ']' expected");
                    }
                }
                if (c == '"')
                {
                    std::string value;
                    if (!String(value))
                        return false;
                    out = Json(std::move(value));
                    return true;
                }
                if (Literal("true"))
                    out = Json(true);
                else if (Literal("false"))
                    out = Json(false);
                else if (Literal("null"))
                    out = Json();
                else
                {
                    const char* begin = text.c_str() + at;
                    char* end = nullptr;
                    double number = strtod(begin, &end);
                    if (end == begin || !(c == '-' || (c >= '0' && c <= '9')))
                        return Fail("unexpected character");
                    at += size_t(end - begin);
                    out = Json(number);
                }
                return true;
            }
        };

        void Quote(std::string& out, const std::string& text)
        {
            out += '"';
            for (char c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (uint8_t(c) < 0x20)
                    {
                        char escape[8];
                        snprintf(escape, sizeof(escape), "\\u%04x", c);
                        out += escape;
                    }
                    else
                        out += c;
                }
            }
            out += '"';
        }
    }

    bool Json::Parse(const std::string& text, Json& out, std::string& error)
    {
        Parser parser{ text };
        Json value;
        bool ok = parser.Value(value, 0);
        parser.Space();
        if (ok && parser.at != text.size())
            ok = parser.Fail("text after the value");
        if (!ok)
        {
            error = parser.error;
            return false;
        }
        out = std::move(value);
        return true;
    }

    int64_t Json::AsInt(int64_t fallback) const
    {
        if (m_type == Type::Number)
            return int64_t(std::llround(m_number));
        if (m_type == Type::String && !m_string.empty())
        {
            char* end = nullptr;
            long long value = strtoll(m_string.c_str(), &end, 0);
            if (end != nullptr && *end == '\0')
                return value;
        }
        return fallback;
    }

    const Json& Json::operator[](const std::string& key) const
    {
        static const Json null;
        for (const auto& [name, value] : m_members)
        {
            if (name == key)
                return value;
        }
        return null;
    }

    bool Json::Has(const std::string& key) const
    {
        for (const auto& member : m_members)
        {
            if (member.first == key)
                return true;
        }
        return false;
    }

    void Json::Set(const std::string& key, Json value)
    {
        m_type = Type::Object;
        for (auto& member : m_members)
        {
            if (member.first == key)
            {
                member.second = std::move(value);
                return;
            }
        }
        m_members.emplace_back(key, std::move(value));
    }

    void Json::Erase(const std::string& key)
    {
        for (auto it = m_members.begin(); it != m_members.end(); ++it)
        {
            if (it->first == key)
            {
                m_members.erase(it);
                return;
            }
        }
    }

    std::string Json::Dump() const
    {
        std::string out;
        DumpTo(out, 0);
        out += '\n';
        return out;
    }

    void Json::DumpTo(std::string& out, int indent) const
    {
        std::string pad(size_t(indent + 2), ' ');
        switch (m_type)
        {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += m_bool ? "true" : "false"; break;
        case Type::Number:
        {
            char number[32];
            if (m_number == std::floor(m_number) && std::fabs(m_number) < 1e15)
                snprintf(number, sizeof(number), "%lld", (long long)m_number);
            else
                snprintf(number, sizeof(number), "%.17g", m_number);
            out += number;
            break;
        }
        case Type::String: Quote(out, m_string); break;
        case Type::Array:
        {
            bool flat = true;
            for (const Json& item : m_items)
                flat = flat && (item.m_type != Type::Array && item.m_type != Type::Object);
            out += '[';
            for (size_t i = 0; i < m_items.size(); i++)
            {
                out += i == 0 ? "" : ",";
                if (flat)
                    out += i == 0 ? "" : " ";
                else
                    out += "\n" + pad;
                m_items[i].DumpTo(out, indent + 2);
            }
            if (!flat && !m_items.empty())
                out += "\n" + std::string(size_t(indent), ' ');
            out += ']';
            break;
        }
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < m_members.size(); i++)
            {
                out += i == 0 ? "\n" : ",\n";
                out += pad;
                Quote(out, m_members[i].first);
                out += ": ";
                m_members[i].second.DumpTo(out, indent + 2);
            }
            if (!m_members.empty())
                out += "\n" + std::string(size_t(indent), ' ');
            out += '}';
            break;
        }
    }
}
