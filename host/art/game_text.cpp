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
            if (size == 0 || size > (512ull << 20)) // before allocating: the size comes from the disc's directory
                return {};
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!disc.read(path, 0, bytes.size(), bytes.data()))
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

        // Writes `file` to `out` (a temporary file renamed over).
        bool WriteFile(const collection::Json& file, const std::filesystem::path& out)
        {
            std::string text = file.Dump(), target = datadir::Text(out), temporary = target + ".part";
            {
                std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
                stream.write(text.data(), std::streamsize(text.size()));
                if (!stream.good())
                    return false;
            }
            return SDL_RenamePath(temporary.c_str(), target.c_str());
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

        // --- Imaginators [Alchemy] ------------------------------------------------------------------------------------

        // A list object's members (file offsets): igObjectList, igNameList... keep their count at word 2 and their data at
        // word 5; an igNameList's members are { name pointer, hash } pairs (`stride` 8).
        std::vector<uint64_t> ListItems(const alchemy::IgzFile& z, uint64_t list, uint32_t stride = 4)
        {
            std::vector<uint64_t> out;
            std::optional<uint64_t> data = z.Pointer(z.Word(list, 5));
            for (uint32_t i = 0; data && i < z.Word(list, 2) && i < 0x10000; i++)
                out.push_back(*data + uint64_t(stride) * i);
            return out;
        }

        // The creation parts' names by part id (docs/runtime/host/figures.md, "The parts' names"). The trackers' file
        // (LooseData/CollectibleTrackers.igz) has a root list of objects and a name list (ONAM) naming each of them, in the
        // same order; a CCYOSLootCollectibleTracker keeps its part id (the id a crystal's recipe stores) at word 36. Each
        // language's file (CollectibleTrackers_<code>.lng) is one igLocalizedStringDataList of { text (word 2), object
        // (word 3), field (word 4) }: the object and the field are external names, 0x80000000 | an index in the EXNM
        // pairs { file, name } (TSTR indices) whose file word has the top bit (objects) or not (fields). The tracker's
        // "CCYOSLootCollectibleTracker::_name" text is the part's name.
        int ImaginatorsParts(const collection::Json& manifest, const Disc& disc, collection::Json& parts)
        {
            Archive archive;
            if (!archive.Open(disc, manifest["archive"].AsString()))
                return 0;
            std::optional<alchemy::IgzFile> trackers = archive.Entry(manifest["trackers"].AsString());
            if (!trackers)
                return 0;
            const alchemy::IgzFile& z = *trackers;
            std::vector<uint32_t> root = z.RecordWords("ROOT"), names = z.RecordWords("ONAM");
            std::optional<uint64_t> rootList = root.empty() ? std::nullopt : z.Pointer(root[0]);
            std::optional<uint64_t> nameList = names.empty() ? std::nullopt : z.Pointer(names[0]);
            if (!rootList || !nameList)
                return 0;
            std::vector<uint64_t> objects = ListItems(z, *rootList), named = ListItems(z, *nameList, 8);
            std::map<std::string, uint32_t> idByName;
            for (size_t i = 0; i < objects.size() && i < named.size(); i++)
            {
                std::optional<uint64_t> object = z.Pointer(z.Be32(objects[i])), name = z.Pointer(z.Be32(named[i]));
                if (object && name && z.TypeName(*object) == "CCYOSLootCollectibleTracker")
                    idByName[z.String(*name)] = z.Word(*object, 36);
            }

            const std::string prefix = manifest["texts"].AsString(), field = "CCYOSLootCollectibleTracker::_name";
            std::map<uint32_t, std::map<std::string, std::string>> texts;
            for (size_t entry = 0; archive.iga && entry < archive.iga->Count(); entry++)
            {
                // "<prefix><code>.lng", any separator and case.
                std::string path = archive.iga->Name(entry);
                std::replace(path.begin(), path.end(), '\\', '/');
                size_t at = path.find(prefix);
                if (at == std::string::npos || path.size() < at + prefix.size() + 4 ||
                    path.compare(path.size() - 4, 4, ".lng") != 0)
                    continue;
                const std::string language = path.substr(at + prefix.size(), path.size() - 4 - at - prefix.size());
                std::optional<alchemy::IgzFile> lng = alchemy::IgzFile::Parse(archive.iga->Read(entry));
                if (!lng)
                    continue;
                std::vector<uint32_t> pairs = lng->RecordWords("EXNM"), lngRoot = lng->RecordWords("ROOT");
                const std::vector<std::string>& strings = lng->StringTable();
                std::vector<std::string> objectNames, fieldNames;
                for (size_t i = 0; i + 1 < pairs.size(); i += 2)
                    (pairs[i] & 0x80000000 ? objectNames : fieldNames)
                        .push_back(pairs[i + 1] < strings.size() ? strings[pairs[i + 1]] : std::string());
                std::optional<uint64_t> top = lngRoot.empty() ? std::nullopt : lng->Pointer(lngRoot[0]);
                std::vector<uint64_t> lists = top ? ListItems(*lng, *top) : std::vector<uint64_t>();
                std::optional<uint64_t> list = lists.empty() ? std::nullopt : lng->Pointer(lng->Be32(lists[0]));
                for (uint64_t item : list ? ListItems(*lng, *list) : std::vector<uint64_t>())
                {
                    std::optional<uint64_t> data = lng->Pointer(lng->Be32(item));
                    if (!data || lng->TypeName(*data) != "igLocalizedStringData")
                        continue;
                    uint32_t objectIndex = lng->Word(*data, 3) & 0xFFFFFF, fieldIndex = lng->Word(*data, 4) & 0xFFFFFF;
                    std::optional<uint64_t> text = lng->Pointer(lng->Word(*data, 2));
                    if (!text || objectIndex >= objectNames.size() || fieldIndex >= fieldNames.size() ||
                        fieldNames[fieldIndex] != field)
                        continue;
                    if (auto id = idByName.find(objectNames[objectIndex]); id != idByName.end())
                        texts[id->second][language] = lng->String(*text);
                }
            }
            for (const auto& [id, byLanguage] : texts)
                parts.Set(std::to_string(id), Texts(byLanguage));
            return int(texts.size());
        }
    }

    int ExtractPartNames(
        std::string_view game, const collection::Json& manifest, const Disc& disc, const std::filesystem::path& out)
    {
        collection::Json parts = collection::Json::Object();
        int count = ImaginatorsParts(manifest, disc, parts);
        if (count == 0)
            return 0;
        collection::Json file = collection::Json::Object();
        file.Set("format", 1);
        file.Set("game", std::string(game));
        file.Set("about", "Read from the player's own disc (docs/runtime/host/figures.md, \"The parts' names\"). Game "
                          "text: never share "
                          "it.");
        file.Set("parts", std::move(parts));
        return WriteFile(file, out) ? count : 0;
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
        return WriteFile(file, out) ? count : 0;
    }
}
