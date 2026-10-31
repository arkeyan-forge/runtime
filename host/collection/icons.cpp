#include "icons.h"
#include "datadir.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "../env.h"

namespace collection
{
    namespace
    {
        // Shapes are signed distances in a [-1, 1] square (negative inside), y down.
        using Shape = std::function<float(float x, float y)>;
        constexpr float Pi = 3.14159265f;

        struct Point
        {
            float x, y;
        };

        float Circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

        float Box(float x, float y, float cx, float cy, float hw, float hh, float radius)
        {
            float dx = std::fabs(x - cx) - hw + radius, dy = std::fabs(y - cy) - hh + radius;
            return std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) + std::min(std::max(dx, dy), 0.0f) - radius;
        }

        // Any simple polygon: distance to the nearest edge, negative inside (even-odd rule).
        float Polygon(float x, float y, const std::vector<Point>& points)
        {
            float best = 1e9f;
            bool inside = false;
            for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++)
            {
                Point a = points[j], b = points[i];
                float ex = b.x - a.x, ey = b.y - a.y, px = x - a.x, py = y - a.y;
                float t = std::clamp((px * ex + py * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
                best = std::min(best, std::hypot(px - ex * t, py - ey * t));
                if ((a.y > y) != (b.y > y) && x < a.x + (y - a.y) * ex / ey)
                    inside = !inside;
            }
            return inside ? -best : best;
        }

        std::vector<Point> Star(int points, float outer, float inner, float cx = 0, float cy = 0, float turn = 0)
        {
            std::vector<Point> star;
            for (int i = 0; i < points * 2; i++)
            {
                float angle = Pi * (i / float(points) - 0.5f) + turn, r = i % 2 == 0 ? outer : inner;
                star.push_back({ cx + r * std::cos(angle), cy + r * std::sin(angle) });
            }
            return star;
        }

        std::vector<Point> Regular(int sides, float r, float turn = 0)
        {
            std::vector<Point> polygon;
            for (int i = 0; i < sides; i++)
            {
                float angle = 2 * Pi * i / sides + turn;
                polygon.push_back({ r * std::cos(angle), r * std::sin(angle) });
            }
            return polygon;
        }

        // Skylanders are shields: big for Giants, small for minis.
        Shape Shield(float scale, float dy)
        {
            std::vector<Point> shield = { { -0.62f, -0.72f }, { 0, -0.86f }, { 0.62f, -0.72f }, { 0.62f, 0.05f },
                { 0, 0.86f }, { -0.62f, 0.05f } };
            for (Point& point : shield)
                point = { point.x * scale, point.y * scale + dy };
            return [shield](float x, float y) { return Polygon(x, y, shield) - 0.04f; };
        }

        Shape CategoryShape(Category category)
        {
            if (HasKindGlyph(category))
                return [category](float x, float y) { return KindGlyph(category, x, y); };
            switch (category)
            {
            case Category::Core: return Shield(0.8f, 0);
            case Category::Giant: return Shield(1.0f, 0);
            case Category::SwapTop:
                return [](float x, float y) { return std::max(Circle(x, y, 0, 0.1f, 0.72f), y - 0.1f); };
            case Category::SwapBottom:
                return [](float x, float y) { return std::max(Circle(x, y, 0, -0.1f, 0.72f), -0.1f - y); };
            case Category::TrapMaster:
                return [p = Regular(8, 0.76f, Pi / 8)](float x, float y) { return Polygon(x, y, p); };
            case Category::SuperCharger:
            {
                std::vector<Point> bolt = { { 0.15f, -0.85f }, { -0.5f, 0.1f }, { -0.05f, 0.1f }, { -0.2f, 0.85f },
                    { 0.5f, -0.15f }, { 0.05f, -0.15f } };
                return [bolt](float x, float y) { return Polygon(x, y, bolt); };
            }
            case Category::Sensei: return [p = Star(5, 0.85f, 0.4f)](float x, float y) { return Polygon(x, y, p); };
            case Category::Trap:
            {
                std::vector<Point> crystal = { { 0, -0.85f }, { 0.45f, -0.4f }, { 0.45f, 0.4f }, { 0, 0.85f },
                    { -0.45f, 0.4f }, { -0.45f, -0.4f } };
                return [crystal](float x, float y) { return Polygon(x, y, crystal); };
            }
            case Category::Villain:
            {
                std::vector<Point> horns = { { -0.75f, -0.7f }, { -0.3f, -0.35f }, { 0.3f, -0.35f }, { 0.75f, -0.7f },
                    { 0.55f, 0.2f }, { 0, 0.8f }, { -0.55f, 0.2f } };
                return [horns](float x, float y) { return Polygon(x, y, horns); };
            }
            case Category::Vehicle:
                return [](float x, float y) {
                    float ring = std::max(Circle(x, y, 0, 0, 0.78f), -Circle(x, y, 0, 0, 0.5f));
                    float spokes = std::min(Box(x, y, 0, 0, 0.08f, 0.6f, 0.02f), Box(x, y, 0, 0, 0.6f, 0.08f, 0.02f));
                    return std::min({ ring, spokes, Circle(x, y, 0, 0, 0.2f) });
                };
            case Category::CreationCrystal:
            {
                std::vector<Point> gem = { { 0, -0.88f }, { 0.55f, -0.1f }, { 0, 0.88f }, { -0.55f, -0.1f } };
                return [gem](float x, float y) { return Polygon(x, y, gem); };
            }
            case Category::Trophy:
            {
                std::vector<Point> cup = { { -0.7f, -0.75f }, { 0.7f, -0.75f }, { 0.4f, 0.0f }, { 0.12f, 0.15f },
                    { 0.12f, 0.5f }, { 0.45f, 0.55f }, { 0.45f, 0.8f }, { -0.45f, 0.8f }, { -0.45f, 0.55f },
                    { -0.12f, 0.5f }, { -0.12f, 0.15f }, { -0.4f, 0.0f } };
                return [cup](float x, float y) { return Polygon(x, y, cup); };
            }
            default: return [](float x, float y) { return Circle(x, y, 0, 0, 0.66f); };
            }
        }

        // The badge's mark, in the badge's own [-1, 1] square.
        Shape KindMark(VariantKind kind)
        {
            switch (kind)
            {
            case VariantKind::Legendary:
            case VariantKind::EonsElite:
            case VariantKind::Event:
            case VariantKind::Gold: return [p = Star(5, 0.7f, 0.3f)](float x, float y) { return Polygon(x, y, p); };
            case VariantKind::Dark:
                return [](float x, float y) {
                    return std::max(Circle(x, y, 0, 0, 0.6f), -Circle(x, y, 0.3f, -0.25f, 0.5f));
                };
            case VariantKind::Crystal:
            case VariantKind::Metallic:
            case VariantKind::Silver:
            case VariantKind::Chrome:
            {
                std::vector<Point> gem = { { 0, -0.65f }, { 0.5f, 0 }, { 0, 0.65f }, { -0.5f, 0 } };
                return [gem](float x, float y) { return Polygon(x, y, gem); };
            }
            case VariantKind::Glow:
            case VariantKind::LightCore:
                return [](float x, float y) { return std::max(Circle(x, y, 0, 0, 0.62f), -Circle(x, y, 0, 0, 0.32f)); };
            case VariantKind::Series:
                return [](float x, float y) {
                    return std::min(Circle(x, y, -0.3f, 0, 0.22f), Circle(x, y, 0.3f, 0, 0.22f));
                };
            case VariantKind::Flocked:
                return [](float x, float y) {
                    return std::min({ Circle(x, y, -0.32f, 0.2f, 0.2f), Circle(x, y, 0.32f, 0.2f, 0.2f),
                        Circle(x, y, 0, -0.3f, 0.2f) });
                };
            case VariantKind::Shape: return [p = Regular(6, 0.6f)](float x, float y) { return Polygon(x, y, p); };
            default: return [](float x, float y) { return Circle(x, y, 0, 0, 0.3f); };
            }
        }

        struct Rgba
        {
            float r, g, b, a;
        };

        Rgba FromRgb(uint32_t rgb, float alpha = 1)
        {
            return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha };
        }

        Rgba Mix(Rgba a, Rgba b, float t)
        {
            return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
        }

        Rgba Unpack(uint32_t p)
        {
            return { (p & 0xFF) / 255.0f, ((p >> 8) & 0xFF) / 255.0f, ((p >> 16) & 0xFF) / 255.0f, (p >> 24) / 255.0f };
        }

        uint32_t Pack(Rgba c)
        {
            auto byte = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            return byte(c.r) | byte(c.g) << 8 | byte(c.b) << 16 | byte(c.a) << 24;
        }

        // `top` over `bottom`, straight alpha.
        Rgba Over(Rgba top, Rgba bottom)
        {
            float a = top.a + bottom.a * (1 - top.a);
            if (a <= 0)
                return { 0, 0, 0, 0 };
            auto channel = [&](float t, float b) { return (t * top.a + b * bottom.a * (1 - top.a)) / a; };
            return { channel(top.r, bottom.r), channel(top.g, bottom.g), channel(top.b, bottom.b), a };
        }

        // Coverage of a pixel by a shape whose distance is `d` (in pixels).
        float Coverage(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }

        // Paints `shape` (unit square mapped onto the square at x0, y0 of `extent` pixels): fill, then an outline.
        void Paint(Image& image, float x0, float y0, float extent, const Shape& shape, Rgba fill, Rgba light,
            Rgba outline, float outlineWidth)
        {
            float scale = extent / 2; // pixels per unit
            int left = std::max(int(x0), 0), top = std::max(int(y0), 0);
            int right = std::min(int(std::ceil(x0 + extent)), image.width),
                bottom = std::min(int(std::ceil(y0 + extent)), image.height);
            for (int py = top; py < bottom; py++)
            {
                for (int px = left; px < right; px++)
                {
                    float ux = (px + 0.5f - x0) / scale - 1, uy = (py + 0.5f - y0) / scale - 1;
                    float d = shape(ux, uy) * scale;
                    if (d > 1)
                        continue;
                    // A soft light from the top left, then the outline band.
                    Rgba color = Mix(light, fill, std::clamp((ux + uy + 1) / 2.5f, 0.0f, 1.0f));
                    float edge = Coverage(d + outlineWidth);
                    color = Mix(outline, color, edge);
                    color.a *= Coverage(d);
                    uint32_t& pixel = image.rgba[size_t(py) * image.width + px];
                    pixel = Pack(Over(color, Unpack(pixel)));
                }
            }
        }

        // Colour of toys without an element: the Toy Box's item and adventure colours.
        uint32_t ToyRgb(const Toy* toy)
        {
            if (toy == nullptr)
                return 0x6E7180;
            if (toy->element != Element::None)
                return ElementRgb(toy->element);
            switch (toy->category)
            {
            case Category::MagicItem: return 0x1E8F82;
            case Category::Adventure: return 0xA8406F;
            case Category::Trophy: return 0xC79A2A;
            default: return ElementRgb(Element::None);
            }
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
            return text;
        }
    }

    // A few families get the mark Spyro's Adventure's menus use for them (a potion for magic items, a compass for
    // locations, a paw for sidekicks), so a drawn icon reads like the disc's.
    bool HasKindGlyph(Category category)
    {
        return category == Category::MagicItem || category == Category::Adventure || category == Category::Mini;
    }

    float KindGlyph(Category category, float x, float y)
    {
        switch (category)
        {
        case Category::MagicItem:
        {
            // A round flask with a neck and a lip, and a spark at its shoulder.
            float flask = Circle(x, y, 0, 0.28f, 0.52f);
            float neck = Box(x, y, 0, -0.34f, 0.16f, 0.24f, 0.04f);
            float lip = Box(x, y, 0, -0.62f, 0.27f, 0.09f, 0.06f);
            static const std::vector<Point> spark = Star(4, 0.26f, 0.07f, 0.6f, -0.52f);
            return std::min({ flask, neck, lip, Polygon(x, y, spark) });
        }
        case Category::Adventure:
        {
            // A compass: a ring and a four-pointed rose.
            static const std::vector<Point> rose = Star(4, 0.6f, 0.17f);
            float ring = std::max(Circle(x, y, 0, 0, 0.86f), -Circle(x, y, 0, 0, 0.7f));
            return std::min(ring, Polygon(x, y, rose));
        }
        case Category::Mini:
        {
            // A paw: the pad and four toes.
            float pad = std::min(Circle(x, y, 0, 0.36f, 0.36f), Box(x, y, 0, 0.44f, 0.46f, 0.26f, 0.24f));
            return std::min({ pad, Circle(x, y, -0.62f, -0.12f, 0.18f), Circle(x, y, -0.25f, -0.5f, 0.2f),
                Circle(x, y, 0.25f, -0.5f, 0.2f), Circle(x, y, 0.62f, -0.12f, 0.18f) });
        }
        default: return 1;
        }
    }

    Image GeneratedIcon(const Toy* toy, int size, bool tile)
    {
        Image image{ size, size, std::vector<uint32_t>(size_t(size) * size) };
        Rgba base = FromRgb(ToyRgb(toy));
        Rgba white = { 1, 1, 1, 1 }, black = { 0, 0, 0, 1 };
        if (tile)
        {
            Rgba dark = Mix(base, black, 0.55f);
            Paint(
                image, 0, 0, float(size), [](float x, float y) { return Box(x, y, 0, 0, 1, 1, 0.22f); }, dark,
                Mix(dark, base, 0.4f), dark, 0);
        }
        // A pale fill shows on the element-coloured disc the menus draw under it; the outline is the element darkened.
        Category category = toy != nullptr ? toy->category : Category::Core;
        float margin = size * 0.1f;
        Paint(image, margin, margin, size - 2 * margin, CategoryShape(category), Mix(base, white, 0.55f),
            Mix(base, white, 0.85f), Mix(base, black, 0.45f), std::max(1.5f, size / 40.0f));
        return image;
    }

    void AddBadge(Image& image, VariantKind kind)
    {
        if (kind == VariantKind::Base || image.Empty())
            return;
        float extent = image.width * 0.42f, x0 = image.width - extent - image.width * 0.02f,
              y0 = image.height - extent - image.height * 0.02f;
        Rgba color = FromRgb(VariantKindRgb(kind)), black = { 0, 0, 0, 1 }, white = { 1, 1, 1, 1 };
        float outline = std::max(1.5f, image.width / 48.0f);
        Paint(
            image, x0, y0, extent, [](float x, float y) { return Circle(x, y, 0, 0, 0.92f); }, color,
            Mix(color, white, 0.35f), Mix(color, black, 0.6f), outline);
        // The mark: white on dark badges, dark on pale ones.
        float luma = 0.3f * color.r + 0.59f * color.g + 0.11f * color.b;
        Rgba ink = luma > 0.6f ? Mix(color, black, 0.7f) : white;
        Paint(image, x0, y0, extent, KindMark(kind), ink, ink, ink, 0);
    }

    namespace
    {
        // The first of `names` cached in the game's portraits folder (SKY_PORTRAITS, else <data>/<Game>/portraits).
        std::string FirstCached(const Game& game, const std::vector<std::string>& names)
        {
            std::string folder;
            if (const char* portraits = skyenv::Get("SKY_PORTRAITS"))
                folder = std::string(portraits) + "/";
            else if (std::filesystem::path data = datadir::Game(game.id); !data.empty())
                folder = datadir::Text(data / "portraits") + "/";
            else
                return {};
            for (const std::string& name : names)
            {
                std::string path = folder + name + ".png";
                if (SDL_GetPathInfo(path.c_str(), nullptr))
                    return path;
            }
            return {};
        }
    }

    std::string GamePortraitPath(const std::string& gameId, const Toy& toy, uint16_t variant, bool large)
    {
        const Game* game = Catalogue::Get().FindGame(gameId);
        if (game == nullptr)
            return {};
        // The game's names for its pictures: Giants lists a file per toy and deco; Spyro's Adventure names its characters
        // ("toy_<name>_hud", 128 px; "<name>_vs", 512 px) and a few other pictures (items).
        std::vector<std::string> names;
        if (auto it = game->portraits.find(uint32_t(toy.type) << 16 | variant);
            variant != 0 && it != game->portraits.end())
            names.push_back(it->second);
        if (auto it = game->portraits.find(uint32_t(toy.type) << 16); it != game->portraits.end())
            names.push_back(it->second);
        if (auto it = game->names.find(toy.type); it != game->names.end())
        {
            if (large)
                names.push_back(Lower(it->second) + "_vs");
            names.push_back("toy_" + Lower(it->second) + "_hud");
        }
        else if (auto image = game->images.find(toy.type); image != game->images.end())
            names.push_back(image->second);
        if (names.empty())
            return {};
        return FirstCached(*game, names);
    }

    std::string VillainPicturePath(const std::string& gameId, const Villain& villain, bool captured)
    {
        const Game* game = Catalogue::Get().FindGame(gameId);
        if (game == nullptr)
            return {};
        return FirstCached(*game, { std::to_string(villain.id) + (captured ? "_villaincaptured" : "_villainescaped") });
    }

    std::string GameKindPicturePath(const std::string& gameId, Category category)
    {
        const Game* game = Catalogue::Get().FindGame(gameId);
        if (game == nullptr)
            return {};
        auto found = game->kindPictures.find(category);
        return found != game->kindPictures.end() ? FirstCached(*game, { found->second }) : std::string();
    }

    std::string PortraitPath(const std::string& gameId, const Toy& toy, uint16_t variant, bool large)
    {
        if (std::string path = GamePortraitPath(gameId, toy, variant, large); !path.empty())
            return path;
        for (const Game& game : Catalogue::Get().Games())
            if (game.id != gameId)
                if (std::string path = GamePortraitPath(game.id, toy, variant, large); !path.empty())
                    return path;
        if (std::string path = GameKindPicturePath(gameId, toy.category); !path.empty())
            return path;
        for (const Game& game : Catalogue::Get().Games())
            if (game.id != gameId)
                if (std::string path = GameKindPicturePath(game.id, toy.category); !path.empty())
                    return path;
        return {};
    }

    std::string BestPicturePath(const std::string& gameId, const Toy& toy, uint16_t variant)
    {
        std::vector<const Game*> games;
        if (const Game* viewed = Catalogue::Get().FindGame(gameId))
            games.push_back(viewed);
        for (const Game& game : Catalogue::Get().Games())
            if (game.id != gameId)
                games.push_back(&game);
        // A file the game names for `key` (type << 16 | variant) in `pictures`, under its portraits folder.
        auto find = [](const Game& game, const std::map<uint32_t, std::string>& pictures, uint32_t key,
                        const char* subfolder) -> std::string {
            auto it = pictures.find(key);
            if (it == pictures.end())
                return {};
            std::string folder;
            if (const char* portraits = skyenv::Get("SKY_PORTRAITS"))
                folder = std::string(portraits) + "/";
            else if (std::filesystem::path data = datadir::Game(game.id); !data.empty())
                folder = datadir::Text(data / "portraits") + "/";
            else
                return {};
            std::string path = folder + subfolder + it->second + ".png";
            return SDL_GetPathInfo(path.c_str(), nullptr) ? path : std::string();
        };
        uint32_t plain = uint32_t(toy.type) << 16;
        if (variant != 0)
            for (const Game* game : games)
                for (auto [pictures, subfolder] :
                    { std::pair{ &game->artworks, "artworks/" }, std::pair{ &game->portraits, "" } })
                    if (std::string path = find(*game, *pictures, plain | variant, subfolder); !path.empty())
                        return path;
        for (const Game* game : games)
            if (std::string path = find(*game, game->artworks, plain, "artworks/"); !path.empty())
                return path;
        return PortraitPath(gameId, toy, variant, true);
    }

    std::string HalfPicturePath(const Toy& toy, uint16_t variant)
    {
        for (const Game& game : Catalogue::Get().Games())
        {
            std::filesystem::path data = datadir::Game(game.id);
            std::string folder;
            if (const char* portraits = skyenv::Get("SKY_PORTRAITS"))
                folder = std::string(portraits) + "/artworks/";
            else if (!data.empty())
                folder = datadir::Text(data / "portraits") + "/artworks/";
            else
                continue;
            for (uint32_t key :
                { uint32_t(toy.type) << 16 | variant, uint32_t(toy.type) << 16 }) // for variant 0 the same twice
            {
                auto it = game.halves.find(key);
                if (it == game.halves.end())
                    continue;
                std::string path = folder + it->second + ".png";
                if (SDL_GetPathInfo(path.c_str(), nullptr))
                    return path;
            }
        }
        return {};
    }

    namespace
    {
        // The bounding box (x0, y0, x1, y1: end exclusive) of the pixels whose alpha is above `minAlpha`.
        bool VisibleBox(const Image& image, uint8_t minAlpha, int& x0, int& y0, int& x1, int& y1)
        {
            x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
            for (int y = 0; y < image.height; y++)
                for (int x = 0; x < image.width; x++)
                    if ((image.rgba[size_t(y) * image.width + x] >> 24) > minAlpha)
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            return x0 < x1 && y0 < y1;
        }

        Image Cut(const Image& image, int x0, int y0, int x1, int y1)
        {
            Image out{ x1 - x0, y1 - y0, std::vector<uint32_t>(size_t(x1 - x0) * size_t(y1 - y0)) };
            for (int y = y0; y < y1; y++)
                std::copy_n(image.rgba.begin() + size_t(y) * image.width + x0, x1 - x0,
                    out.rgba.begin() + size_t(y - y0) * out.width);
            return out;
        }
    }

    Image CropToVisible(const Image& image, uint8_t minAlpha)
    {
        int x0, y0, x1, y1;
        if (!VisibleBox(image, minAlpha, x0, y0, x1, y1))
            return image;
        return Cut(image, x0, y0, x1, y1);
    }

    Image JoinHalves(const Image& top, const Image& bottom)
    {
        int box[4];
        if (!VisibleBox(top, 8, box[0], box[1], box[2], box[3]) ||
            !VisibleBox(bottom, 8, box[0], box[1], box[2], box[3]))
            return {};
        int width = std::max(top.width, bottom.width), height = std::max(top.height, bottom.height);
        Image upper = Resize(top, width, height), lower = Resize(bottom, width, height);
        for (size_t i = 0; i < lower.rgba.size(); i++)
        {
            uint32_t t = upper.rgba[i], b = lower.rgba[i];
            uint32_t at = t >> 24, ab = b >> 24;
            uint32_t a = at * 255 + ab * (255 - at); // alpha x 255
            if (a == 0)
            {
                lower.rgba[i] = 0;
                continue;
            }
            uint32_t out = ((a + 127) / 255) << 24;
            for (int shift = 0; shift < 24; shift += 8)
                out |= ((((t >> shift) & 255) * at * 255 + ((b >> shift) & 255) * ab * (255 - at)) / a) << shift;
            lower.rgba[i] = out;
        }
        if (!VisibleBox(lower, 0, box[0], box[1], box[2], box[3]))
            return {};
        return Cut(lower, box[0], box[1], box[2], box[3]);
    }

    Image SwapperPicture(const Toy& top, uint16_t topVariant, const Toy& bottom, uint16_t bottomVariant)
    {
        std::string upper = HalfPicturePath(top, topVariant), lower = HalfPicturePath(bottom, bottomVariant);
        if (upper.empty() || lower.empty())
            return {};
        return JoinHalves(LoadPng(upper), LoadPng(lower));
    }

    uint16_t VariantFor(const Toy& toy, VariantKind kind)
    {
        if (kind == VariantKind::Base || kind == toy.edition)
            return 0;
        uint16_t variant = 0;
        int count = 0;
        for (const Variant& candidate : toy.variants)
            if (candidate.kind == kind && candidate.id != 0)
                variant = candidate.id, count++;
        return count == 1 ? variant : 0;
    }

    std::string DiscImagePath(const Toy& toy) { return PortraitPath({}, toy); }

    Image Portrait(
        const std::string& gameId, const Toy* toy, VariantKind kind, int size, uint16_t variant, bool* fromDisc)
    {
        Image image;
        if (toy != nullptr)
        {
            if (variant == 0)
                variant = VariantFor(*toy, kind);
            std::string path = PortraitPath(gameId, *toy, variant, size > 128);
            if (!path.empty())
                image = LoadPng(path);
        }
        if (fromDisc != nullptr)
            *fromDisc = !image.Empty();
        image = image.Empty() ? GeneratedIcon(toy, size) : Resize(image, size, size);
        AddBadge(image, kind);
        return image;
    }

    Image Icon(const Toy* toy, VariantKind kind, int size, bool* fromDisc)
    {
        return Portrait({}, toy, kind, size, 0, fromDisc);
    }

    Image LoadPng(const std::string& path)
    {
        Image image;
        SDL_Surface* loaded = SDL_LoadPNG(path.c_str());
        if (loaded == nullptr)
            return image;
        if (SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32))
        {
            image.width = rgba->w;
            image.height = rgba->h;
            image.rgba.resize(size_t(rgba->w) * rgba->h);
            for (int y = 0; y < rgba->h; y++)
                memcpy(&image.rgba[size_t(y) * rgba->w],
                    static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch, size_t(rgba->w) * 4);
            SDL_DestroySurface(rgba);
        }
        SDL_DestroySurface(loaded);
        return image;
    }

    bool SavePng(const Image& image, const std::string& path)
    {
        SDL_Surface* surface = SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA32,
            const_cast<uint32_t*>(image.rgba.data()), image.width * 4);
        bool saved = surface != nullptr && SDL_SavePNG(surface, path.c_str());
        SDL_DestroySurface(surface);
        return saved;
    }

    Image Resize(const Image& image, int width, int height)
    {
        if (image.width == width && image.height == height)
            return image;
        // Box filter when shrinking, bilinear when growing; colours weighted by alpha.
        Image out{ width, height, std::vector<uint32_t>(size_t(width) * height) };
        float sx = float(image.width) / width, sy = float(image.height) / height;
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                float u0 = x * sx, v0 = y * sy, u1 = u0 + std::max(sx, 1.0f), v1 = v0 + std::max(sy, 1.0f);
                if (sx < 1)
                {
                    u0 = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
                    u1 = u0 + 1;
                }
                if (sy < 1)
                {
                    v0 = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
                    v1 = v0 + 1;
                }
                float sum[4] = {}, weight = 0;
                for (int v = int(v0); v < int(std::ceil(v1)) && v < image.height; v++)
                {
                    float wy = std::min(v1, v + 1.0f) - std::max(v0, float(v));
                    for (int u = int(u0); u < int(std::ceil(u1)) && u < image.width; u++)
                    {
                        float w = wy * (std::min(u1, u + 1.0f) - std::max(u0, float(u)));
                        Rgba c = Unpack(image.rgba[size_t(v) * image.width + u]);
                        sum[0] += c.r * c.a * w;
                        sum[1] += c.g * c.a * w;
                        sum[2] += c.b * c.a * w;
                        sum[3] += c.a * w;
                        weight += w;
                    }
                }
                Rgba c = { 0, 0, 0, 0 };
                if (sum[3] > 0)
                    c = { sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3], sum[3] / weight };
                out.rgba[size_t(y) * width + x] = Pack(c);
            }
        }
        return out;
    }
}
