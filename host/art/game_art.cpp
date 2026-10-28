// See game_art.h. Where each game keeps its pictures is data: its manifest, games/<id>/art.json, built in
// (g_embeddedArtManifests); this file reads any manifest with the engine's reader. Described in
// docs/runtime/host/game_art.md, with the Giants logo composition.
#include "game_art.h"
#include "alchemy_archive.h"
#include "game_text.h"
#include "lux_images.h"
#include "../dir_list.h"
#include "../collection/catalogue.h"
#include "../collection/datadir.h"
#include "../collection/json.h"
#include "../collection/upgrades.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include "../env.h"

extern const unsigned char g_embeddedArtManifests[];
extern const size_t g_embeddedArtManifestsSize;

namespace art
{
    namespace
    {
        // Bump when a picture is read or composed differently: the cached pictures (not the portraits) are made again.
        constexpr int CacheVersion = 4; // 4: Swap Force's logo from its lossless 32-bit picture

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        std::string Text(const std::filesystem::path& path)
        {
            return datadir::Text(path);
        }

        bool Exists(const std::filesystem::path& path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }

        // Written next to the target first, so that an interrupted run leaves no truncated PNG behind.
        bool SavePng(const Image& image, const std::filesystem::path& path)
        {
            SDL_Surface* surface = SDL_CreateSurfaceFrom(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBA32,
                const_cast<uint32_t*>(image.rgba.data()), int(image.width * 4));
            if (surface == nullptr)
                return false;
            std::string target = Text(path), temporary = target + ".part";
            bool saved = SDL_SavePNG(surface, temporary.c_str()) && SDL_RenamePath(temporary.c_str(), target.c_str());
            SDL_DestroySurface(surface);
            if (!saved)
                fprintf(stderr, "[art] cannot write %s\n", target.c_str());
            return saved;
        }

        bool SaveBytes(const std::vector<uint8_t>& bytes, const std::filesystem::path& path)
        {
            std::string target = Text(path), temporary = target + ".part";
            {
                std::ofstream out(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                if (!out.good())
                    return false;
            }
            return SDL_RenamePath(temporary.c_str(), target.c_str());
        }

        std::vector<uint8_t> ReadAll(const Disc& disc, const std::string& path)
        {
            uint64_t size = disc.size(path);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (size == 0 || size > (512ull << 20) || !disc.read(path, 0, bytes.size(), bytes.data()))
                return {};
            return bytes;
        }

        std::optional<alchemy::IgaArchive> OpenArchive(const Disc& disc, const std::string& path)
        {
            uint64_t size = disc.size(path);
            if (size == 0)
                return std::nullopt;
            return alchemy::IgaArchive::Open([&disc, path](uint64_t offset, size_t count, uint8_t* out) { return disc.read(path, offset, count, out); }, size);
        }

        // The IGZ file of archive entry `part` (a name fragment), parsed.
        std::optional<alchemy::IgzFile> OpenScene(const alchemy::IgaArchive& archive, const std::string& part)
        {
            std::optional<size_t> index = archive.Find(part);
            if (!index)
                return std::nullopt;
            return alchemy::IgzFile::Parse(archive.Read(*index));
        }

        // The rows and columns of `image` whose alpha is above `threshold`, in bands of consecutive rows.
        struct Band
        {
            uint32_t top, bottom, left, right; // [top, bottom) x [left, right)
        };

        std::vector<Band> OpaqueBands(const Image& image, uint8_t threshold = 16)
        {
            std::vector<Band> bands;
            bool inside = false;
            for (uint32_t y = 0; y <= image.height; y++)
            {
                uint32_t left = image.width, right = 0;
                if (y < image.height)
                    for (uint32_t x = 0; x < image.width; x++)
                        if ((image.rgba[size_t(y) * image.width + x] >> 24) > threshold)
                            left = std::min(left, x), right = std::max(right, x + 1);
                bool row = left < right;
                if (row && !inside)
                    bands.push_back({ y, y, left, right });
                if (row)
                {
                    bands.back().bottom = y + 1;
                    bands.back().left = std::min(bands.back().left, left);
                    bands.back().right = std::max(bands.back().right, right);
                }
                inside = row;
            }
            return bands;
        }

        // Draws `band` of `from` at (x, y) of `to`; `under`: behind what is there (straight alpha, "over" blending).
        void Blit(const Image& from, const Band& band, Image& to, int x, int y, bool under)
        {
            for (uint32_t sy = band.top; sy < band.bottom; sy++)
                for (uint32_t sx = band.left; sx < band.right; sx++)
                {
                    int dx = x + int(sx - band.left), dy = y + int(sy - band.top);
                    if (dx < 0 || dy < 0 || dx >= int(to.width) || dy >= int(to.height))
                        continue;
                    uint32_t source = from.rgba[size_t(sy) * from.width + sx], &target = to.rgba[size_t(dy) * to.width + dx];
                    uint32_t top = under ? target : source, bottom = under ? source : target;
                    float ta = float(top >> 24) / 255, ba = float(bottom >> 24) / 255, alpha = ta + ba * (1 - ta);
                    if (alpha <= 0)
                        continue;
                    uint32_t out = uint32_t(alpha * 255 + 0.5f) << 24;
                    for (int shift = 0; shift < 24; shift += 8)
                    {
                        float c = (float((top >> shift) & 0xFF) * ta + float((bottom >> shift) & 0xFF) * ba * (1 - ta)) / alpha;
                        out |= uint32_t(std::clamp(c + 0.5f, 0.0f, 255.0f)) << shift;
                    }
                    target = out;
                }
        }

        // The runs of columns of `band` that hold opaque texels, apart by more than `gap` columns.
        std::vector<Band> ColumnRuns(const Image& image, const Band& band, uint32_t gap)
        {
            std::vector<Band> runs;
            uint32_t last = 0;
            for (uint32_t x = band.left; x < band.right; x++)
            {
                uint32_t top = band.bottom, bottom = band.top;
                for (uint32_t y = band.top; y < band.bottom; y++)
                    if ((image.rgba[size_t(y) * image.width + x] >> 24) > 16)
                        top = std::min(top, y), bottom = y + 1;
                if (top >= bottom)
                    continue;
                if (runs.empty() || x - last > gap)
                    runs.push_back({ top, bottom, x, x });
                runs.back().right = x + 1, last = x;
                runs.back().top = std::min(runs.back().top, top), runs.back().bottom = std::max(runs.back().bottom, bottom);
            }
            return runs;
        }

        // The dark blue plate that backs the logo (the title's mesh draws it from title_giants_3d.png): the silhouette of
        // `pieces` grown by `radius`, each row filled from its first to its last texel, with soft edges and a darker rim.
        // `base` is the plate's blue, sampled from the game's own texture.
        Image BackingPlate(const Image& pieces, uint32_t radius, uint32_t base)
        {
            const int w = int(pieces.width), h = int(pieces.height), r = int(radius);
            std::vector<int> count(size_t(h) * (w + 1), 0); // per row: opaque texels before column x
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    count[size_t(y) * (w + 1) + x + 1] = count[size_t(y) * (w + 1) + x] + ((pieces.rgba[size_t(y) * w + x] >> 24) > 16 ? 1 : 0);
            std::vector<float> mask(size_t(w) * h, 0.0f);
            for (int y = 0; y < h; y++)
            {
                int first = w, last = -1;
                for (int x = 0; x < w; x++)
                {
                    bool grown = false;
                    for (int dy = -r; dy <= r && !grown; dy++)
                    {
                        int row = y + dy, half = int(std::sqrt(float(r * r - dy * dy)));
                        if (row < 0 || row >= h)
                            continue;
                        int from = std::max(0, x - half), to = std::min(w, x + half + 1);
                        grown = count[size_t(row) * (w + 1) + to] > count[size_t(row) * (w + 1) + from];
                    }
                    if (grown)
                        first = std::min(first, x), last = x;
                }
                for (int x = first; x <= last; x++)
                    mask[size_t(y) * w + x] = 1.0f;
            }
            // Three box blurs of 3 texels: soft edges.
            std::vector<float> tmp(mask.size());
            for (int pass = 0; pass < 6; pass++)
            {
                const bool horizontal = pass % 2 == 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                    {
                        float sum = 0;
                        for (int d = -3; d <= 3; d++)
                        {
                            int xx = horizontal ? std::clamp(x + d, 0, w - 1) : x, yy = horizontal ? y : std::clamp(y + d, 0, h - 1);
                            sum += mask[size_t(yy) * w + xx];
                        }
                        tmp[size_t(y) * w + x] = sum / 7;
                    }
                mask.swap(tmp);
            }
            Image plate;
            plate.width = pieces.width, plate.height = pieces.height;
            plate.rgba.assign(mask.size(), 0);
            for (size_t i = 0; i < mask.size(); i++)
            {
                float coverage = std::clamp((mask[i] - 0.25f) * 2.0f, 0.0f, 1.0f);       // the edge, antialiased
                float body = std::clamp((mask[i] - 0.55f) / 0.35f, 0.0f, 1.0f);           // 0 on the rim, 1 inside
                uint32_t out = uint32_t(coverage * 255 + 0.5f) << 24;
                for (int shift = 0; shift < 24; shift += 8)
                    out |= uint32_t(float((base >> shift) & 0xFF) * (0.35f + 0.65f * body) + 0.5f) << shift;
                plate.rgba[i] = out;
            }
            return plate;
        }

        // The plate's blue in the game's own flat title picture (title_giants_3d.png): the mean of its blue, dark texels.
        uint32_t PlateBlue(const Image& flat)
        {
            uint64_t r = 0, g = 0, b = 0, n = 0;
            for (uint32_t y = 0; y < flat.height / 2; y++)
                for (uint32_t x = 0; x < flat.width; x++)
                {
                    uint32_t p = flat.rgba[size_t(y) * flat.width + x];
                    uint32_t pr = p & 0xFF, pg = (p >> 8) & 0xFF, pb = (p >> 16) & 0xFF;
                    if (pb > 60 && pb > pr * 2 + 40 && pb > pg && pr + pg + pb < 240)
                        r += pr, g += pg, b += pb, n++;
                }
            if (n == 0)
                return 0xFF642E00; // R 0, G 46, B 100: the same blue, if the texture is not there
            return 0xFF000000 | uint32_t(b / n) << 16 | uint32_t(g / n) << 8 | uint32_t(r / n);
        }

        // [Giants] The manifest's "sprite_sheet_logo": the title's logo is a sprite sheet (spritelogo.png, 1024x1024):
        // "SKYLANDERS" (the arch), "GIANTS", the row of 8 element medallions and the TM mark, apart. The title draws
        // "GIANTS" under the arch's middle, behind it (as in the title's own flat picture, title_giants_3d.png), the
        // medallions under it, the TM at their height on the right, and the dark blue plate behind all but the TM.
        // `flat` is the manifest's "plate" picture (title_giants_3d.png: the plate's blue).
        bool ComposeSpriteSheetLogo(const Image& sheet, const Image* flat, Image& logo)
        {
            std::vector<Band> bands = OpaqueBands(sheet);
            // Top to bottom (once the right way up): the arch, the word, the medallions with the TM.
            if (bands.size() < 3)
                return false;
            const Band& arch = bands[0];
            const Band& word = bands[1];
            std::vector<Band> row = ColumnRuns(sheet, bands[2], 24);
            if (row.size() < 2)
                return false;
            const Band& tm = row.back();
            Band medals = row.front();
            for (size_t i = 1; i + 1 < row.size(); i++)
                medals.left = std::min(medals.left, row[i].left), medals.right = std::max(medals.right, row[i].right),
                medals.top = std::min(medals.top, row[i].top), medals.bottom = std::max(medals.bottom, row[i].bottom);
            const uint32_t archH = arch.bottom - arch.top, archW = arch.right - arch.left, wordH = word.bottom - word.top;
            const uint32_t medalsH = medals.bottom - medals.top, margin = 10; // the plate reaches 10 texels past the pieces
            // "GIANTS" starts 0.48 of the arch's height down, the medallions 0.08 of the word's height under it (both
            // measured on the press logo, scaled to the sheet; title_giants_3d.png says 0.507, drawn in perspective).
            const int wordTop = int(float(archH) * 0.48f + 0.5f), medalsTop = wordTop + int(wordH) + int(float(wordH) * 0.08f + 0.5f);
            const int origin = int(arch.left); // sheet column of the logo's left edge (without the margin)
            Image pieces;
            pieces.width = std::max(archW, tm.right - arch.left) + 2 * margin;
            pieces.height = uint32_t(medalsTop) + medalsH + 2 * margin;
            pieces.rgba.assign(size_t(pieces.width) * pieces.height, 0);
            const int m = int(margin);
            Blit(sheet, arch, pieces, m, m, false);
            Blit(sheet, word, pieces, m + int(word.left) - origin, m + wordTop, true); // as aligned on the sheet: centred
            Blit(sheet, medals, pieces, m + int(medals.left) - origin, m + medalsTop, false);
            Image plate = BackingPlate(pieces, margin, flat != nullptr ? PlateBlue(*flat) : PlateBlue(Image()));
            Blit(sheet, tm, pieces, m + int(tm.left) - origin, m + medalsTop + int(tm.top) - int(medals.top), false); // not on the plate
            logo = plate;
            Band all{ 0, pieces.height, 0, pieces.width };
            Blit(pieces, all, logo, 0, 0, false);
            return true;
        }

        // --- The manifests: where each game keeps its pictures -------------------------------------------------------

        struct Art
        {
            Image logo, background;
        };

        // One picture's place on the disc.
        struct Source
        {
            std::string stream;          // [Lux] the strm file of UI images...
            std::string archive, entry;  // [Alchemy] ...or the IGA archive and its IGZ entry (a fragment of its name)
            std::string picture;         // the picture: its name [Lux], the end of its source path [Alchemy]; empty: the entry's first
            std::string compose, plate;  // a composition step ("sprite_sheet_logo") and the second picture it uses
        };

        // [Alchemy] A folder of material instances in an archive, each naming its texture, an entry of the same archive
        // (Swap Force: "materialinstances\toycollection\collection_zook_normal.igz" names
        // "textures:/JuiceStandard_Collections_Zook_10.igz"). The picture is named after the material.
        struct Materials
        {
            std::string archive, folder, ending; // the entries whose name starts with `folder` (and ends with `ending`)
        };

        struct Manifest
        {
            std::string game;
            Source logo, background;
            // Portraits. [Lux]: the stream, the picture names per toy the catalogue names ("toy_{name}_hud": {name} is
            // the catalogue's internal name, lower case) and, with `images`, the catalogue's other images. [Alchemy]:
            // every square picture of the archive's entry under `folder`, named as on the disc.
            Source portraits;
            std::vector<Materials> portraitMaterials; // [Alchemy] instead of the archive's entry: materials, by folder
            std::vector<std::string> pictures;
            bool images = false;
            std::string folder;
            // Artworks [Alchemy]: the archive whose entries under `artworksFolder` are one large picture each (Giants'
            // versus screen), named after the entry's picture ("ui_pvp_sprites/eruptorseries2.png/0x....igz").
            Source artworks;
            std::string artworksFolder;
            std::vector<Materials> artworkMaterials;
            // The toys' upgrades (game_text.cpp): the manifest's "upgrades" part as it is.
            collection::Json upgrades;
        };

        std::vector<Materials> ReadMaterials(const collection::Json& json)
        {
            std::vector<Materials> out;
            for (const collection::Json& item : json.Items())
                out.push_back({ item["archive"].AsString(), item["folder"].AsString(), item["ending"].AsString() });
            return out;
        }

        Source ReadSource(const collection::Json& json)
        {
            return { json["stream"].AsString(), json["archive"].AsString(), json["entry"].AsString(), json["picture"].AsString(),
                json["compose"].AsString(), json["plate"].AsString() };
        }

        const std::vector<Manifest>& Manifests()
        {
            static const std::vector<Manifest> manifests = [] {
                std::vector<Manifest> out;
                collection::Json all;
                std::string error;
                if (!collection::Json::Parse(std::string(reinterpret_cast<const char*>(g_embeddedArtManifests), g_embeddedArtManifestsSize), all, error))
                {
                    fprintf(stderr, "[art] the built-in manifests do not load: %s\n", error.c_str());
                    return out;
                }
                for (const collection::Json& json : all.Items())
                {
                    Manifest m;
                    m.game = json["game"].AsString();
                    m.logo = ReadSource(json["logo"]);
                    m.background = ReadSource(json["background"]);
                    m.portraits = ReadSource(json["portraits"]);
                    for (const collection::Json& name : json["portraits"]["pictures"].Items())
                        m.pictures.push_back(name.AsString());
                    m.images = json["portraits"]["images"].AsBool();
                    m.folder = json["portraits"]["folder"].AsString();
                    m.portraitMaterials = ReadMaterials(json["portraits"]["materials"]);
                    m.artworks = ReadSource(json["artworks"]);
                    m.artworksFolder = json["artworks"]["folder"].AsString();
                    m.artworkMaterials = ReadMaterials(json["artworks"]["materials"]);
                    m.upgrades = json["upgrades"];
                    if (!m.game.empty())
                        out.push_back(std::move(m));
                }
                return out;
            }();
            return manifests;
        }

        const Manifest* FindManifest(std::string_view game)
        {
            for (const Manifest& manifest : Manifests())
                if (game == manifest.game)
                    return &manifest;
            return nullptr;
        }

        // [Lux] The pictures `names` of a strm stream, decoded in one pass.
        std::map<std::string, Image> LuxPictures(const Disc& disc, const std::string& stream, std::set<std::string> names)
        {
            std::map<std::string, Image> out;
            std::vector<uint8_t> bytes;
            if (names.empty() || !lux::UnpackStream(ReadAll(disc, stream), bytes))
                return out;
            lux::DecodeImages(bytes, [&](const std::string& name) { return names.erase(name) > 0; },
                [&](const std::string& name, const Image& image) { out[name] = image; });
            return out;
        }

        // [Alchemy] A picture of an archive's entry: the one whose source path ends with `picture`, or the first one.
        bool AlchemyPicture(const Disc& disc, const Source& source, const std::string& picture, Image& out)
        {
            auto archive = OpenArchive(disc, source.archive);
            std::optional<alchemy::IgzFile> scene = archive ? OpenScene(*archive, source.entry) : std::nullopt;
            if (!scene)
                return false;
            for (const alchemy::IgzFile::Picture& candidate : scene->Pictures())
                if ((picture.empty() || Lower(candidate.name).ends_with(Lower(picture))) && alchemy::DecodePicture(*scene, candidate, out))
                    return true;
            return false;
        }

        // The logo and the background, as the manifest says.
        void ReadPictures(const Manifest& m, const Disc& disc, Image& logo, Image& background)
        {
            // [Lux] Pictures of one stream are decoded together.
            std::map<std::string, std::set<std::string>> streams;
            for (const Source* source : { &m.logo, &m.background })
                if (!source->stream.empty())
                    streams[source->stream].insert(source->picture);
            std::map<std::string, std::map<std::string, Image>> decoded;
            for (const auto& [stream, names] : streams)
                decoded[stream] = LuxPictures(disc, stream, names);
            auto read = [&](const Source& source, Image& out) {
                if (!source.stream.empty())
                {
                    auto& found = decoded[source.stream];
                    if (auto it = found.find(source.picture); it != found.end())
                        out = it->second;
                    return;
                }
                if (source.archive.empty())
                    return;
                Image picture;
                if (!AlchemyPicture(disc, source, source.picture, picture))
                    return;
                if (source.compose == "sprite_sheet_logo")
                {
                    Image flat;
                    bool haveFlat = !source.plate.empty() && AlchemyPicture(disc, source, source.plate, flat);
                    ComposeSpriteSheetLogo(picture, haveFlat ? &flat : nullptr, out);
                }
                else if (source.compose.empty())
                    out = std::move(picture);
                else
                    fprintf(stderr, "[art] %s: unknown composition \"%s\"\n", m.game.c_str(), source.compose.c_str());
            };
            read(m.logo, logo);
            read(m.background, background);
        }

        int CountPngs(const std::filesystem::path& folder)
        {
            int present = 0;
            std::error_code error;
            for (const auto& entry : ListDirectory(folder, error))
                present += entry.path().extension() == ".png";
            return present;
        }

        // [Lux] The pictures the manifest's patterns give for each toy the catalogue names, and its other images.
        int LuxPortraits(const Manifest& m, const Disc& disc, const std::filesystem::path& folder)
        {
            std::set<std::string> wanted;
            if (const collection::Game* game = collection::Catalogue::Get().FindGame(m.game))
            {
                for (const auto& [type, name] : game->names)
                    for (std::string pattern : m.pictures)
                    {
                        if (size_t at = pattern.find("{name}"); at != std::string::npos)
                            pattern.replace(at, 6, Lower(name));
                        wanted.insert(pattern);
                    }
                if (m.images)
                {
                    for (const auto& [type, name] : game->images)
                        wanted.insert(name);
                    for (const auto& [category, name] : game->kindPictures)
                        wanted.insert(name);
                }
            }
            int present = 0;
            for (auto it = wanted.begin(); it != wanted.end();)
                if (Exists(folder / (*it + ".png")))
                    present++, it = wanted.erase(it);
                else
                    ++it;
            if (wanted.empty())
                return present;
            std::vector<uint8_t> stream;
            if (!lux::UnpackStream(ReadAll(disc, m.portraits.stream), stream))
            {
                fprintf(stderr, "[art] cannot read the UI images (%s) of the disc\n", m.portraits.stream.c_str());
                return present;
            }
            lux::DecodeImages(stream, [&](const std::string& name) { return wanted.erase(name) > 0; },
                [&](const std::string& name, const Image& image) { present += SavePng(image, folder / (name + ".png")); });
            return present;
        }

        // True when every picture of `names` (the catalogue's portraits or artworks) is in `folder`.
        bool Complete(const std::map<uint32_t, std::string>& names, const std::filesystem::path& folder)
        {
            bool complete = !names.empty();
            for (const auto& [key, name] : names)
                complete = complete && Exists(folder / (name + ".png"));
            return complete;
        }

        // [Alchemy] Every material instance under each source's folder: its texture's first picture, named after the
        // material ("collection_zook_normal.png"). Swap Force (IGZ v7): the material names its texture in its
        // dependencies (TDEP), and the texture is an entry of the same archive ("textures\<name>.igz").
        int AlchemyMaterials(const std::vector<Materials>& sources, const Disc& disc, const std::filesystem::path& folder)
        {
            int present = CountPngs(folder);
            for (const Materials& source : sources)
            {
                auto archive = OpenArchive(disc, source.archive);
                if (!archive)
                {
                    fprintf(stderr, "[art] cannot read %s of the disc\n", source.archive.c_str());
                    continue;
                }
                const std::string under = Lower(source.folder), ending = Lower(source.ending);
                for (size_t i = 0; i < archive->Count(); i++)
                {
                    std::string name = Lower(archive->Name(i));
                    if (under.empty() || !name.starts_with(under) || !name.ends_with(".igz") || !name.ends_with(ending))
                        continue;
                    name = name.substr(0, name.size() - 4);
                    name = name.substr(name.find_last_of("\\/") + 1);
                    std::filesystem::path path = folder / (name + ".png");
                    if (Exists(path))
                        continue;
                    std::optional<alchemy::IgzFile> material = alchemy::IgzFile::Parse(archive->Read(i));
                    for (const std::string& dependency : material ? material->Dependencies() : std::vector<std::string>{})
                    {
                        std::string texture = Lower(dependency);
                        if (!texture.starts_with("textures:/"))
                            continue;
                        std::optional<alchemy::IgzFile> scene = OpenScene(*archive, "textures\\" + texture.substr(10));
                        std::vector<alchemy::IgzFile::Picture> pictures = scene ? scene->Pictures() : std::vector<alchemy::IgzFile::Picture>{};
                        Image image;
                        if (!pictures.empty() && alchemy::DecodePicture(*scene, pictures[0], image) && SavePng(image, path))
                            present++;
                        break;
                    }
                }
            }
            return present;
        }

        // [Alchemy] Every square picture of the entry under the manifest's folder (Giants' collection screen: all
        // Skylanders and their decos, sidekicks and items), named as on the disc ("eruptor_lightcore.png"). Done when
        // every portrait the catalogue's "portraits" names is there.
        int AlchemyPortraits(const Manifest& m, const Disc& disc, const std::filesystem::path& folder)
        {
            const collection::Game* game = collection::Catalogue::Get().FindGame(m.game);
            int present = CountPngs(folder);
            if (game != nullptr && Complete(game->portraits, folder))
                return present;
            if (!m.portraitMaterials.empty())
                return AlchemyMaterials(m.portraitMaterials, disc, folder);
            auto archive = OpenArchive(disc, m.portraits.archive);
            std::optional<alchemy::IgzFile> scene = archive ? OpenScene(*archive, m.portraits.entry) : std::nullopt;
            if (!scene)
            {
                fprintf(stderr, "[art] cannot read %s of the disc\n", m.portraits.archive.c_str());
                return present;
            }
            const std::string under = Lower(m.folder);
            for (const alchemy::IgzFile::Picture& picture : scene->Pictures())
            {
                std::string name = Lower(picture.name);
                size_t at = name.find(under);
                if (under.empty() || at == std::string::npos || picture.width != picture.height || !name.ends_with(".png"))
                    continue;
                name = name.substr(at + under.size(), name.size() - at - under.size() - 4);
                std::filesystem::path path = folder / (name + ".png");
                Image image;
                if (!Exists(path) && alchemy::DecodePicture(*scene, picture, image) && SavePng(image, path))
                    present++;
            }
            return present;
        }

        // [Alchemy] Every entry of the artworks archive under the manifest's folder: one picture each, named after the
        // entry ("eruptorseries2.png"). Done when every artwork the catalogue's "artworks" names is there.
        int AlchemyArtworks(const Manifest& m, const Disc& disc, const std::filesystem::path& folder)
        {
            const collection::Game* game = collection::Catalogue::Get().FindGame(m.game);
            int present = CountPngs(folder);
            if (game != nullptr && Complete(game->artworks, folder) && (game->halves.empty() || Complete(game->halves, folder)))
                return present;
            if (!m.artworkMaterials.empty())
                return AlchemyMaterials(m.artworkMaterials, disc, folder);
            if (m.artworks.archive.empty() || m.artworksFolder.empty())
                return present;
            auto archive = OpenArchive(disc, m.artworks.archive);
            if (!archive)
            {
                fprintf(stderr, "[art] cannot read %s of the disc\n", m.artworks.archive.c_str());
                return present;
            }
            const std::string under = Lower(m.artworksFolder);
            for (size_t i = 0; i < archive->Count(); i++)
            {
                std::string name = Lower(archive->Name(i));
                size_t at = name.find(under), end = at == std::string::npos ? at : name.find(".png", at + under.size());
                if (end == std::string::npos)
                    continue;
                std::filesystem::path path = folder / (name.substr(at + under.size(), end - at - under.size()) + ".png");
                if (Exists(path))
                    continue;
                std::optional<alchemy::IgzFile> scene = alchemy::IgzFile::Parse(archive->Read(i));
                std::vector<alchemy::IgzFile::Picture> pictures = scene ? scene->Pictures() : std::vector<alchemy::IgzFile::Picture>{};
                Image image;
                if (!pictures.empty() && alchemy::DecodePicture(*scene, pictures[0], image) && SavePng(image, path))
                    present++;
            }
            return present;
        }

        int ReadPortraits(const Manifest& m, const Disc& disc, const std::filesystem::path& folder)
        {
            if (!m.portraits.stream.empty())
                return LuxPortraits(m, disc, folder);
            if (!m.portraits.archive.empty() || !m.portraitMaterials.empty())
                return AlchemyPortraits(m, disc, folder);
            return CountPngs(folder);
        }

        int ReadVersion(const std::filesystem::path& manifest)
        {
            std::ifstream in(manifest);
            std::string line;
            while (std::getline(in, line))
                if (line.starts_with("version="))
                    return atoi(line.c_str() + 8);
            return 0;
        }
    }

    const char* RoleFile(Role role)
    {
        switch (role)
        {
        case Role::Logo: return "logo.png";
        case Role::Background: return "background.png";
        case Role::TitleIcon: return "title_icon.png";
        case Role::Count: break;
        }
        return "";
    }

    std::filesystem::path CacheFolder(std::string_view game)
    {
        std::filesystem::path data = datadir::Game(game);
        return data.empty() ? std::filesystem::path() : data / "launcher-cache";
    }

    std::filesystem::path PortraitFolder(std::string_view game)
    {
        if (const char* folder = skyenv::Get("SKY_PORTRAITS"))
            return datadir::FromText(folder);
        std::filesystem::path data = datadir::Game(game);
        return data.empty() ? std::filesystem::path() : data / "portraits";
    }

    std::filesystem::path ArtworkFolder(std::string_view game)
    {
        std::filesystem::path portraits = PortraitFolder(game);
        return portraits.empty() ? portraits : portraits / "artworks";
    }

    std::string CachedPath(std::string_view game, Role role)
    {
        std::filesystem::path folder = CacheFolder(game);
        if (folder.empty() || !Exists(folder / RoleFile(role)))
            return {};
        return Text(folder / RoleFile(role));
    }

    bool Known(std::string_view game)
    {
        return FindManifest(game) != nullptr;
    }

    bool ReadLogo(std::string_view game, const Disc& disc, Image& logo)
    {
        const Manifest* where = FindManifest(game);
        if (where == nullptr)
            return false;
        Manifest logoOnly = *where;
        logoOnly.background = {}; // not read
        Image background;
        ReadPictures(logoOnly, disc, logo, background);
        return !logo.rgba.empty();
    }

    Extraction Extract(std::string_view game, const Disc& disc, const std::vector<uint8_t>& titleIconPng, int parts)
    {
        Extraction result;
        const Manifest* where = FindManifest(game); // the game's art.json
        std::filesystem::path cache = CacheFolder(game), portraits = PortraitFolder(game);
        std::error_code error;
        if ((parts & Pictures) && !cache.empty())
        {
            std::filesystem::create_directories(cache, error);
            // Before 0.4: the cache was <data>/SSA/launcher-cache/<title id>/ for every game.
            for (const auto& entry : ListDirectory(cache, error))
            {
                std::string name = entry.path().filename().string();
                if (entry.is_directory(error) && name.size() == 8 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); }))
                    std::filesystem::remove_all(entry.path(), error);
            }
            std::filesystem::path manifest = cache / "manifest.ini";
            bool current = ReadVersion(manifest) == CacheVersion;
            if (!titleIconPng.empty())
                SaveBytes(titleIconPng, cache / RoleFile(Role::TitleIcon));
            if (where != nullptr && (!current || !Exists(cache / RoleFile(Role::Logo)) || !Exists(cache / RoleFile(Role::Background))))
            {
                Art pictures;
                ReadPictures(*where, disc, pictures.logo, pictures.background);
                if (!pictures.logo.rgba.empty())
                    SavePng(pictures.logo, cache / RoleFile(Role::Logo));
                if (!pictures.background.rgba.empty())
                    SavePng(pictures.background, cache / RoleFile(Role::Background));
                std::ofstream out(manifest, std::ios::trunc);
                out << "# Pictures read from the player's own disc (docs/runtime/host/game_art.md). Local only; deleting\n"
                       "# this folder is always safe: the launcher or the game reads them again.\n"
                    << "version=" << CacheVersion << "\ngame=" << game << "\nlogo=" << (pictures.logo.rgba.empty() ? "missing" : "ok")
                    << "\nbackground=" << (pictures.background.rgba.empty() ? "missing" : "ok") << "\n";
            }
            result.logo = Exists(cache / RoleFile(Role::Logo));
            result.background = Exists(cache / RoleFile(Role::Background));
            result.titleIcon = Exists(cache / RoleFile(Role::TitleIcon));
        }
        if ((parts & Portraits) && where != nullptr && !portraits.empty())
        {
            std::filesystem::create_directories(portraits, error);
            result.portraits = ReadPortraits(*where, disc, portraits);
        }
        if ((parts & Artworks) && where != nullptr && (!where->artworks.archive.empty() || !where->artworkMaterials.empty()) && !portraits.empty())
        {
            std::filesystem::path artworks = ArtworkFolder(game);
            std::filesystem::create_directories(artworks, error);
            result.artworks = AlchemyArtworks(*where, disc, artworks);
        }
        if ((parts & Upgrades) && where != nullptr && where->upgrades.IsObject())
        {
            // Read once: a later Extract keeps the file (delete it to read the disc again).
            std::filesystem::path file = collection::UpgradeFile(game);
            std::error_code exists;
            if (!file.empty() && !std::filesystem::is_regular_file(file, exists))
            {
                std::filesystem::create_directories(file.parent_path(), error);
                ExtractUpgrades(game, where->upgrades, disc, file);
            }
            result.upgrades = std::filesystem::is_regular_file(file, exists);
        }
        return result;
    }
}
