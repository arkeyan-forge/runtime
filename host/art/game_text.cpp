// See game_text.h. The tables and how they were found: docs/runtime/host/figures.md ("Upgrades"), the dump tool
// tools/toy_tables.py (the same reading, in Python).
#include "game_text.h"
#include "alchemy_archive.h"
#include "lux_images.h"
#include "../collection/catalogue.h"
#include "../collection/datadir.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace art
{
    namespace
    {
        // Where each entry of a toy's upgrade list goes [V] (both games' menus): 4 basic upgrades, 3 for each path, the
        // Soul Gem, the Wow Pow (Giants, Series 2; bit 10 of the editors' bits, collection::EditorBits): { bit, path, index
        // in the price list }.
        struct Slot
        {
            int bit, path, price;
        };
        constexpr Slot Slots[] = { { 2, 0, 0 }, { 3, 0, 1 }, { 4, 0, 2 }, { 5, 0, 3 }, { 6, 1, 4 }, { 7, 1, 5 },
            { 8, 1, 6 }, { 6, 2, 4 }, { 7, 2, 5 }, { 8, 2, 6 }, { 9, 3, 7 }, { 10, 4, 8 } };

        uint32_t Be32(const std::vector<uint8_t>& b, size_t at)
        {
            return at + 4 <= b.size() ? uint32_t(b[at]) << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3] : 0;
        }

        std::vector<uint8_t> ReadAll(const Disc& disc, const std::string& path)
        {
            uint64_t size = disc.size(path);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (size == 0 || size > (512ull << 20) || !disc.read(path, 0, bytes.size(), bytes.data()))
                return {};
            return bytes;
        }

        void AppendUtf8(std::string& out, uint32_t c)
        {
            if (c < 0x80)
                out += char(c);
            else if (c < 0x800)
                out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
            else
                out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 0x3F)), out += char(0x80 | (c & 0x3F));
        }

        collection::Json Texts(const std::map<std::string, std::string>& byLanguage)
        {
            collection::Json out = collection::Json::Object();
            for (const auto& [language, text] : byLanguage)
                out.Set(language, text);
            return out;
        }

        collection::Json Entry(int index, int price, collection::Json name, collection::Json description)
        {
            collection::Json item = collection::Json::Object();
            item.Set("bit", Slots[index].bit);
            item.Set("path", Slots[index].path);
            item.Set("price", price);
            item.Set("name", std::move(name));
            item.Set("description", std::move(description));
            return item;
        }

        // --- Giants [Alchemy] -----------------------------------------------------------------------------------------

        // An archive read whole, and its level.bld parsed.
        struct Archive
        {
            std::vector<uint8_t> bytes;
            std::optional<alchemy::IgaArchive> iga;

            bool Open(const Disc& disc, const std::string& path)
            {
                bytes = ReadAll(disc, path);
                iga = alchemy::IgaArchive::Open(
                    [this](uint64_t offset, size_t count, uint8_t* out) {
                        if (offset + count > bytes.size())
                            return false;
                        memcpy(out, bytes.data() + offset, count);
                        return true;
                    },
                    bytes.size());
                return iga.has_value();
            }

            std::optional<alchemy::IgzFile> Entry(const std::string& name) const
            {
                std::optional<size_t> index = iga ? iga->Find(name) : std::nullopt;
                return index ? alchemy::IgzFile::Parse(iga->Read(*index)) : std::nullopt;
            }
        };

        struct Script
        {
            const alchemy::IgzFile& z;
            std::vector<uint64_t> objects;

            explicit Script(const alchemy::IgzFile& file) : z(file), objects(file.ObjectOffsets()) {}

            std::optional<uint64_t> At(uint32_t pointer) const
            {
                return pointer != 0 ? z.Pointer(pointer) : std::nullopt;
            }
            std::string Text(uint32_t pointer) const
            {
                std::optional<uint64_t> at = At(pointer);
                return at ? z.String(*at) : std::string();
            }
            // A ScriptSet's members (file offsets).
            std::vector<uint64_t> Items(uint64_t set) const
            {
                std::vector<uint64_t> out;
                std::optional<uint64_t> list = At(z.Word(set, 6)), data = list ? At(z.Word(*list, 5)) : std::nullopt;
                for (uint32_t i = 0; data && i < z.Word(*list, 2) && i < 4096; i++)
                    if (std::optional<uint64_t> item = At(z.Be32(*data + 4 * i)))
                        out.push_back(*item);
                return out;
            }
            std::optional<uint64_t> Set(const std::string& name) const
            {
                for (uint64_t o : objects)
                    if (z.TypeName(o) == "ScriptSet" && Text(z.Word(o, 2)) == name)
                        return o;
                return std::nullopt;
            }
        };

        // A language pack (ENGLISH.pak...): every string, UTF-16, in the order of its archive's StringInfo objects.
        std::vector<std::string> PakTexts(const alchemy::IgzFile& pak)
        {
            std::vector<std::string> out;
            for (uint64_t o : pak.ObjectOffsets())
            {
                if (pak.TypeName(o) != "LanguagePackInfo")
                    continue;
                std::optional<uint64_t> block = pak.Pointer(pak.Word(o, 7));
                size_t size = pak.Word(o, 6) & 0xFFFFFFF;
                const std::vector<uint8_t>& b = pak.Bytes();
                if (!block || *block + size > b.size())
                    break;
                std::string text;
                for (size_t i = *block; i + 1 < *block + size; i += 2)
                {
                    uint32_t c = uint32_t(b[i]) << 8 | b[i + 1];
                    if (c == 0)
                        out.push_back(std::move(text)), text.clear();
                    else
                        AppendUtf8(text, c);
                }
                out.push_back(std::move(text));
                break;
            }
            return out;
        }

        int GiantsUpgrades(const collection::Json& manifest, const Disc& disc, collection::Json& toys)
        {
            // The prices: global.bld's ScriptSet of ValueInfo (value at word 8).
            std::vector<int> prices;
            {
                Archive global;
                const collection::Json& costs = manifest["costs"];
                std::optional<alchemy::IgzFile> level =
                    global.Open(disc, costs["archive"].AsString()) ? global.Entry("level.bld") : std::nullopt;
                if (!level)
                    return 0;
                Script script(*level);
                if (std::optional<uint64_t> set = script.Set(costs["set"].AsString()))
                    for (uint64_t value : script.Items(*set))
                        prices.push_back(int(level->Word(value, 8)));
            }
            if (prices.size() != 9)
                return 0;

            int count = 0;
            for (const auto& [type, path] : manifest["archives"].Members())
            {
                Archive archive;
                std::optional<alchemy::IgzFile> z =
                    archive.Open(disc, path.AsString()) ? archive.Entry("level.bld") : std::nullopt;
                if (!z)
                    continue;
                Script script(*z);
                std::vector<uint64_t> infos; // StringInfo objects, in file order: the language packs follow it
                for (uint64_t o : script.objects)
                    if (z->TypeName(o) == "StringInfo")
                        infos.push_back(o);
                std::map<std::string, std::vector<std::string>> packs;
                for (const auto& [language, pak] : manifest["languages"].Members())
                    if (std::optional<alchemy::IgzFile> file = archive.Entry(pak.AsString() + ".pak"))
                        packs[language] = PakTexts(*file);
                auto text = [&](uint64_t info) {
                    size_t index = std::lower_bound(infos.begin(), infos.end(), info) - infos.begin();
                    std::map<std::string, std::string> out;
                    for (const auto& [language, texts] : packs)
                        if (index < infos.size() && infos[index] == info && index < texts.size())
                            out[language] = texts[index];
                    return Texts(out);
                };
                // The toy's "Portal - Define Minion" call: its parameters that are references (a string or a set).
                std::map<std::string, uint64_t> params;
                for (uint64_t op : script.objects)
                {
                    if (z->TypeName(op) != "OpMacroParameter" || (z->Word(op, 6) & 0x80000000) != 0)
                        continue;
                    std::optional<uint64_t> macro = script.At(z->Word(op, 14)), value = script.At(z->Word(op, 12));
                    if (!macro || script.Text(z->Word(*macro, 2)) != "Portal - Define Minion" || !value ||
                        z->TypeName(*value) != "RHSReferenceStack")
                        continue;
                    if (std::optional<uint64_t> target = script.At(z->Word(*value, 12)))
                        params[script.Text(z->Word(op, 7))] = *target;
                }
                if (!params.contains("unlock names") || !params.contains("unlock descriptions"))
                    continue;
                std::vector<uint64_t> names = script.Items(params["unlock names"]),
                                      descriptions = script.Items(params["unlock descriptions"]);
                collection::Json entry = collection::Json::Object(), list = collection::Json::Array(),
                                 paths = collection::Json::Array();
                for (size_t i = 0; i < names.size() && i < descriptions.size() && i < std::size(Slots); i++)
                    list.Push(Entry(int(i), prices[size_t(Slots[i].price)], text(names[i]), text(descriptions[i])));
                for (int k = 1; k <= 2; k++)
                {
                    std::string prefix = "upgrade path " + std::to_string(k);
                    if (!params.contains(prefix + " name"))
                        continue;
                    collection::Json one = collection::Json::Object();
                    one.Set("name", text(params[prefix + " name"]));
                    if (params.contains(prefix + " desc"))
                        one.Set("description", text(params[prefix + " desc"]));
                    paths.Push(std::move(one));
                }
                entry.Set("paths", std::move(paths));
                entry.Set("upgrades", std::move(list));
                toys.Set(type, std::move(entry));
                count++;
            }
            return count;
        }

        // --- Spyro's Adventure [Lux] ----------------------------------------------------------------------------------

        // A Lux text file (text/<language>/*.lxb): CRC-32 of the key -> UTF-8 text.
        std::unordered_map<uint32_t, std::string> LxbTexts(const std::vector<uint8_t>& b)
        {
            std::unordered_map<uint32_t, std::string> out;
            uint32_t count = Be32(b, 0x7C);
            for (uint32_t i = 0; i < count && 0x88 + 8 * size_t(i) <= b.size(); i++)
            {
                uint32_t key = Be32(b, 0x80 + 8 * size_t(i));
                size_t at = 0x84 + 8 * size_t(i) + size_t(int32_t(Be32(b, 0x84 + 8 * size_t(i))));
                if (at >= b.size())
                    continue;
                const uint8_t* end = static_cast<const uint8_t*>(memchr(&b[at], 0, b.size() - at));
                if (end != nullptr)
                    out[key] = std::string(reinterpret_cast<const char*>(&b[at]), reinterpret_cast<const char*>(end));
            }
            return out;
        }

        int SsaUpgrades(const collection::Json& manifest, const Disc& disc, collection::Json& toys)
        {
            std::map<std::string, std::unordered_map<uint32_t, std::string>> texts;
            for (const auto& [language, code] : manifest["languages"].Members())
            {
                std::string path = manifest["text"].AsString();
                if (size_t at = path.find("{language}"); at != std::string::npos)
                    path.replace(at, 10, code.AsString());
                texts[language] = LxbTexts(ReadAll(disc, path));
            }
            const auto& english = texts["en"];
            std::vector<uint8_t> stream;
            if (english.empty() || !lux::UnpackStream(ReadAll(disc, manifest["stream"].AsString()), stream))
                return 0;
            std::map<std::string, uint16_t> types; // the toys' English names, as the catalogue gives them
            for (const collection::Toy* toy : collection::Catalogue::Get().Accepted("ssa"))
                types[toy->name] = toy->type;
            auto text = [&](uint32_t key) {
                std::map<std::string, std::string> out;
                for (const auto& [language, table] : texts)
                    if (auto it = table.find(key); it != table.end())
                        out[language] = it->second;
                return Texts(out);
            };
            // A toy's upgrade records: 12 of 0x54 bytes (index, name key, description key, price, ...: index 0-3 basic,
            // 8 "choose your path", 4-6 twice for the two paths, 7 the Soul Gem), after its header (name key, the two
            // paths' name keys) 0x60 bytes before the first. Not 4-aligned in the stream.
            constexpr size_t Record = 0x54;
            int count = 0;
            for (size_t at = 0x60; at + 12 * Record <= stream.size(); at++)
            {
                if (Be32(stream, at) != 0 || Be32(stream, at + 12) != 500 || !english.contains(Be32(stream, at + 4)) ||
                    !english.contains(Be32(stream, at + 8)) || !english.contains(Be32(stream, at - 0x60)))
                    continue;
                auto toy = types.find(english.at(Be32(stream, at - 0x60)));
                std::vector<size_t> basic, paths, soul;
                for (size_t k = 0; k < 12; k++)
                {
                    uint32_t index = Be32(stream, at + k * Record);
                    (index < 4       ? basic
                        : index <= 6 ? paths
                        : index == 7 ? soul
                                     : basic)
                        .push_back(index == 8 ? SIZE_MAX : at + k * Record);
                }
                std::erase(basic, SIZE_MAX);
                if (toy == types.end() || basic.size() != 4 || paths.size() != 6 || soul.size() != 1 ||
                    toys.Has(std::to_string(toy->second)))
                {
                    at += 12 * Record - 1;
                    continue;
                }
                std::vector<size_t> order = basic;
                order.insert(order.end(), paths.begin(), paths.end());
                order.push_back(soul[0]);
                collection::Json entry = collection::Json::Object(), list = collection::Json::Array(),
                                 pathList = collection::Json::Array();
                for (size_t i = 0; i < order.size(); i++)
                    list.Push(Entry(int(i), int(Be32(stream, order[i] + 12)), text(Be32(stream, order[i] + 4)),
                        text(Be32(stream, order[i] + 8))));
                for (size_t k = 0; k < 2; k++)
                {
                    collection::Json one = collection::Json::Object();
                    one.Set("name", text(Be32(stream, at - 0x5C + 4 * k)));
                    pathList.Push(std::move(one));
                }
                entry.Set("paths", std::move(pathList));
                entry.Set("upgrades", std::move(list));
                toys.Set(std::to_string(toy->second), std::move(entry));
                count++;
                at += 12 * Record - 1;
            }
            return count;
        }
    }

    int ExtractUpgrades(
        std::string_view game, const collection::Json& manifest, const Disc& disc, const std::filesystem::path& out)
    {
        collection::Json toys = collection::Json::Object();
        int count = manifest.Has("archives") ? GiantsUpgrades(manifest, disc, toys)
                    : manifest.Has("stream") ? SsaUpgrades(manifest, disc, toys)
                                             : 0;
        if (count == 0)
            return 0;
        collection::Json file = collection::Json::Object();
        file.Set("format", 1);
        file.Set("game", std::string(game));
        file.Set("about",
            "Read from the player's own disc (docs/runtime/host/figures.md, \"Upgrades\"). Game text: never share it.");
        file.Set("toys", std::move(toys));
        std::string text = file.Dump(), target = datadir::Text(out), temporary = target + ".part";
        {
            std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
            stream.write(text.data(), std::streamsize(text.size()));
            if (!stream.good())
                return 0;
        }
        return SDL_RenamePath(temporary.c_str(), target.c_str()) ? count : 0;
    }
}
