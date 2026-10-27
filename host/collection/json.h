#pragma once

// A small JSON reader and writer for the collection's files (the catalogue and the collection index). RFC 8259
// values; numbers are doubles; object members keep their order.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace collection
{
    class Json
    {
    public:
        enum class Type { Null, Bool, Number, String, Array, Object };

        Json() = default;
        Json(bool value) : m_type(Type::Bool), m_bool(value) {}
        Json(double value) : m_type(Type::Number), m_number(value) {}
        Json(int value) : Json(double(value)) {}
        Json(int64_t value) : Json(double(value)) {}
        Json(const char* value) : m_type(Type::String), m_string(value) {}
        Json(std::string value) : m_type(Type::String), m_string(std::move(value)) {}
        static Json Array() { Json json; json.m_type = Type::Array; return json; }
        static Json Object() { Json json; json.m_type = Type::Object; return json; }

        // False (and `error` set to "line N: ...") if `text` is not one JSON value.
        static bool Parse(const std::string& text, Json& out, std::string& error);
        // Two-space indentation; arrays of numbers or strings on one line.
        std::string Dump() const;

        Type type() const { return m_type; }
        bool IsNull() const { return m_type == Type::Null; }
        bool IsObject() const { return m_type == Type::Object; }
        bool IsArray() const { return m_type == Type::Array; }
        bool IsString() const { return m_type == Type::String; }
        bool IsNumber() const { return m_type == Type::Number; }

        bool AsBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
        double AsNumber(double fallback = 0) const { return m_type == Type::Number ? m_number : fallback; }
        // Strings too: "0x1801" and "6145" are numbers (hexadecimal ids read better in the catalogue).
        int64_t AsInt(int64_t fallback = 0) const;
        const std::string& AsString() const { return m_string; }

        // Arrays.
        const std::vector<Json>& Items() const { return m_items; }
        void Push(Json value) { m_items.push_back(std::move(value)); }

        // Objects. A missing member reads as null.
        const std::vector<std::pair<std::string, Json>>& Members() const { return m_members; }
        const Json& operator[](const std::string& key) const;
        bool Has(const std::string& key) const;
        // Adds the member or replaces its value.
        void Set(const std::string& key, Json value);
        void Erase(const std::string& key);

    private:
        void DumpTo(std::string& out, int indent) const;

        Type m_type = Type::Null;
        bool m_bool = false;
        double m_number = 0;
        std::string m_string;
        std::vector<Json> m_items;
        std::vector<std::pair<std::string, Json>> m_members;
    };
}
