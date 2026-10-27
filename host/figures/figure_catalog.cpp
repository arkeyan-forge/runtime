#include "figure_catalog.h"
#include "../collection/catalogue.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace figures
{
    namespace
    {
        // The catalogue's toys that a game accepts, in its order; built once per game, never changed afterwards.
        const std::vector<ToyInfo>& Toys(const std::string& id)
        {
            static std::mutex mutex;
            static std::map<std::string, std::vector<ToyInfo>> games;
            std::lock_guard lock(mutex);
            if (auto found = games.find(id); found != games.end())
                return found->second;
            {
                static std::deque<std::string> strings; // the ToyInfo fields point into these
                auto keep = [](const std::string& text) { return text.empty() ? nullptr : strings.emplace_back(text).c_str(); };
                const collection::Catalogue& catalogue = collection::Catalogue::Get();
                const collection::Game* game = catalogue.FindGame(id);
                std::vector<ToyInfo> list;
                for (const collection::Toy* toy : catalogue.Accepted(id))
                {
                    ToyInfo info{ toy->type, keep(toy->name), nullptr, Element::None, ToyKind::Character, nullptr };
                    // The names and pictures are the game's own; a game that lists none (Giants') takes SSA's for the toys
                    // it shares with it.
                    const collection::Game* named = game != nullptr && !game->names.empty() ? game : catalogue.FindGame("ssa");
                    if (named != nullptr)
                    {
                        if (auto name = named->names.find(toy->type); name != named->names.end())
                            info.portrait = keep(name->second);
                        if (auto image = named->images.find(toy->type); image != named->images.end())
                            info.image = keep(image->second);
                    }
                    // The title's eight elements; light, dark and none are None here.
                    info.element = toy->element <= collection::Element::Undead ? Element(toy->element) : Element::None;
                    info.kind = toy->category == collection::Category::MagicItem ? ToyKind::MagicItem
                        : toy->category == collection::Category::Adventure       ? ToyKind::Adventure
                        : toy->category == collection::Category::Mini            ? ToyKind::Sidekick
                        : toy->edition == collection::VariantKind::Legendary     ? ToyKind::Legendary
                                                                                 : ToyKind::Character;
                    list.push_back(info);
                }
                return games.emplace(id, std::move(list)).first->second;
            }
        }
    }

    std::span<const ToyInfo> Catalog(const std::string& game)
    {
        return Toys(game);
    }

    const ToyInfo* FindToy(uint16_t toyType, const std::string& game)
    {
        for (const ToyInfo& toy : Toys(game))
        {
            if (toy.toyType == toyType)
                return &toy;
        }
        return nullptr;
    }

    const char* ElementName(Element element)
    {
        static const char* const Names[] = { "Air", "Earth", "Fire", "Water", "Magic", "Tech", "Life", "Undead", "" };
        return Names[size_t(element)];
    }

    uint32_t ElementColor(Element element)
    {
        // 0xAABBGGRR
        static const uint32_t Colors[] = { 0xFFE8C46A, 0xFF2E6C9A, 0xFF2A4FE0, 0xFFD08A28, 0xFFC04AA8, 0xFF20B0E8,
            0xFF3CB04C, 0xFF7A5A6A, 0xFF707070 };
        return Colors[size_t(element)];
    }

    std::string PortraitImageName(const ToyInfo& toy)
    {
        if (toy.portrait == nullptr)
            return toy.image != nullptr ? toy.image : std::string();
        std::string name = std::string("toy_") + toy.portrait + "_hud";
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(tolower(c)); });
        return name;
    }

    std::string StarterFileName(const ToyInfo& toy)
    {
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "%03u ", toy.toyType);
        return prefix + std::string(toy.name) + ".sky";
    }
}
