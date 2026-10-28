#include "host/toybox/model.h"
#include "host/collection/json.h"
#include "host/portal.h"
#include "host/portal_backend.h"
#include "language.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "../env.h"

using namespace collection;

namespace toybox
{
    namespace
    {
        constexpr uint64_t Second = 1'000'000'000;
        constexpr uint64_t LateAfter = 20 * Second;   // "the game has not taken it yet"
        constexpr uint64_t HoldTime = Second;         // reset, delete: A held
        constexpr uint64_t PinHoldTime = 600'000'000; // wheel: X held on a place

        // The Figure page's actions: the main ones, then More (the others) and Details (the file, the toy's numbers).
        enum class Action { PutOn, TakeOff, Unlock, EditStats, ChangeHalf, Rename, Favourite, Wheel, Pin, Duplicate, BackUp, Restore, Reset, Delete, More, Details };

        std::string FileName(const std::string& path)
        {
            return path.substr(path.find_last_of("/\\") + 1);
        }

        bool PlayerSeat(const SeatState& s) { return s.seat.player > 0 && s.seat.id.rfind("p", 0) == 0; }

        bool KeepsProgress(const Figure& f)
        {
            return f.toy == nullptr || f.toy->category <= Category::Mini || f.toy->category == Category::CreationCrystal;
        }

        std::string LockOwner(const LockInfo& lock)
        {
            if (lock.game == "launcher" || lock.game == "skylauncher")
                return "Skylauncher";
            const Game* game = Catalogue::Get().FindGame(lock.game);
            return game != nullptr ? game->name : lock.game;
        }

        // Two digits of an ISO 8601 time, as "today 10:15" or "24/10 10:15" (local time).
        std::string When(const std::string& iso)
        {
            if (iso.size() < 16)
                return {};
            struct tm utc = {};
            utc.tm_year = atoi(iso.substr(0, 4).c_str()) - 1900;
            utc.tm_mon = atoi(iso.substr(5, 2).c_str()) - 1;
            utc.tm_mday = atoi(iso.substr(8, 2).c_str());
            utc.tm_hour = atoi(iso.substr(11, 2).c_str());
            utc.tm_min = atoi(iso.substr(14, 2).c_str());
#ifdef _WIN32
            time_t t = _mkgmtime(&utc);
            struct tm local;
            localtime_s(&local, &t);
            time_t now = time(nullptr);
            struct tm today;
            localtime_s(&today, &now);
#else
            time_t t = timegm(&utc);
            struct tm local;
            localtime_r(&t, &local);
            time_t now = time(nullptr);
            struct tm today;
            localtime_r(&now, &today);
#endif
            char text[48];
            if (local.tm_year == today.tm_year && local.tm_yday == today.tm_yday)
                snprintf(text, sizeof(text), "%s %02d:%02d", L("today", "aujourd'hui"), local.tm_hour, local.tm_min);
            else
                snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d", French() ? local.tm_mday : local.tm_mon + 1,
                    French() ? local.tm_mon + 1 : local.tm_mday, local.tm_year + 1900, local.tm_hour, local.tm_min);
            return text;
        }

        size_t Utf8Length(const std::string& text)
        {
            size_t count = 0;
            for (char c : text)
                count += (uint8_t(c) & 0xC0) != 0x80 ? 1 : 0;
            return count;
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
            return text;
        }
    }

    Model& Get()
    {
        static Model model;
        return model;
    }

    bool French() { return TextLanguage() == "fr"; }

    std::string ElementLabel(Element element)
    {
        static const char* const English[] = { "Air", "Earth", "Fire", "Water", "Magic", "Tech", "Life", "Undead", "Light", "Dark", "None" };
        static const char* const France[] = { "Air", "Terre", "Feu", "Eau", "Magie", "Tech", "Vie", "Mort-vivant", "Lumière", "Ténèbres", "Aucun" };
        return element < Element::Count ? (French() ? France : English)[size_t(element)] : "";
    }

    std::string CategoryLabel(Category category)
    {
        static const char* const France[] = { "Skylanders", "Géants", "Swap Force (haut)", "Swap Force (bas)", "Maîtres des pièges",
            "SuperChargers", "Senseis", "Minis et acolytes", "Pièges", "Méchants", "Véhicules", "Cristaux de création",
            "Objets magiques", "Packs d'aventure", "Trophées" };
        static_assert(std::size(France) == size_t(Category::Count));
        if (category >= Category::Count)
            return "";
        if (category == Category::Mini && !French())
            return "Minis and sidekicks";
        return French() ? France[size_t(category)] : CategoryName(category);
    }

    std::string VariantLabel(VariantKind kind)
    {
        static const char* const France[] = { "Standard", "Légendaire", "Sombre", "Cristal", "Phosphorescent", "Métallisé", "Or",
            "Argent", "Chrome", "Floqué", "Séries", "LightCore", "Élite d'Eon", "Autre déco", "Événement", "Forme de piège", "Autre" };
        static_assert(std::size(France) == size_t(VariantKind::Count));
        return kind < VariantKind::Count ? (French() ? France[size_t(kind)] : VariantKindName(kind)) : "";
    }

    // --- Setup ------------------------------------------------------------------------------------------------------------

    void Model::Init(bool real)
    {
        realPortal = real;
        gameInfo = Catalogue::Get().FindGame(game);
        seats.clear();
        if (gameInfo != nullptr)
        {
            for (const Seat& s : gameInfo->seats)
                seats.push_back({ s });
        }
        if (seats.empty())
            seats.push_back({ Seat{ "p1", 1, { Category::Core } } });
        // Once after the update: the never-played figures older builds made on their own go to the trash.
        if (!real && !TidyDone())
            tidied = collection::Tidy();
        if (int purged = PurgeTrash(30))
            fprintf(stderr, "[toybox] %d figure(s) deleted more than 30 days ago removed from the trash\n", purged);
        Reload();
        RefreshSeats(SDL_GetTicksNS());
    }

    void Model::Reload()
    {
        figures.clear();
        for (Figure& f : List())
        {
            if (!UntouchedStarter(f))
                figures.push_back(std::move(f));
        }
    }

    int Model::FindFigure(const std::string& path) const
    {
        for (size_t i = 0; i < figures.size(); i++)
        {
            if (figures[i].path == path)
                return int(i);
        }
        return -1;
    }

    void Model::Open(Screen to, int seatIndex)
    {
        Reload();
        RefreshSeats(SDL_GetTicksNS());
        open = true;
        seat = std::clamp(seatIndex, 0, int(seats.size()) - 1);
        screen = to;
        dialogs.clear();
        search.clear();
        ClearFilters();
        inList = true; // the list, or the empty state's three choices
        column = seat;
        figureMore = figureDetails = false;
        BuildList();
        // The first card that is not on the portal: favourites and recent come first, so "swap to the last one" is A.
        list.cursor = 0;
        for (size_t i = 0; i < list.cells.size(); i++)
        {
            const Cell& cell = list.cells[i];
            if (cell.kind == Cell::Kind::Figure && SeatOf(figures[cell.figure].path) < 0)
            {
                list.cursor = int(i);
                break;
            }
        }
        FixGrid(list, 0);
        if (tidied > 0 && to == Screen::Home)
        {
            int moved = tidied;
            tidied = 0;
            char text[256];
            snprintf(text, sizeof(text), L("%d blank figures moved to the trash.", "%d figurines vierges mises à la corbeille."), moved);
            Ask({ L("After the update", "Après la mise à jour"), text, { "OK", L("Put them back", "Les remettre") }, 0, false, [this](int choice) {
                if (choice != 1)
                    return;
                int back = 0;
                for (const TrashEntry& entry : Trash())
                {
                    if (entry.reason == "tidy" && !Undelete(entry.id).empty())
                        back++;
                }
                Reload();
                BuildList();
                char done[96];
                snprintf(done, sizeof(done), L("%d figures put back", "%d figurines remises"), back);
                Toast(done);
            } });
        }
        fprintf(stderr, "[toybox] open (%d figures, %s)\n", int(figures.size()), realPortal ? "real portal" : "emulated portal");
        fprintf(stderr, "[toybox] %d cards (a swapper is one)\n", int(collection::Cards(figures).size()));
        m_dirty = true;
    }

    void Model::Close()
    {
        if ((screen == Screen::Stats || screen == Screen::Upgrades) && stats.dirty)
            SaveStats();
        open = false;
        screen = Screen::Home;
        dialogs.clear();
        holdStart = 0;
        m_dirty = true;
        fprintf(stderr, "[toybox] closed\n");
    }

    void Model::Toast(const std::string& text, uint64_t seconds)
    {
        toast = text;
        toastUntil = SDL_GetTicksNS() + seconds * Second;
        fprintf(stderr, "[toybox] %s\n", text.c_str());
        m_dirty = true;
    }

    void Model::Ask(Dialog dialog)
    {
        dialogs.push_back(std::move(dialog));
        holdStart = 0;
        holdProgress = 0;
        m_holdArmed = false; // the press that opened it does not count
        m_dirty = true;
    }

    // --- Seats and the portal ----------------------------------------------------------------------------------------

    bool Model::Accepts(const SeatState& s, const Figure& f) const
    {
        if (f.toy == nullptr)
            return true; // unknown toys: any seat, the game decides
        return std::find(s.seat.accepts.begin(), s.seat.accepts.end(), f.toy->category) != s.seat.accepts.end();
    }

    int Model::SeatOf(const std::string& path) const
    {
        for (size_t i = 0; i < seats.size(); i++)
        {
            if (!path.empty() && (seats[i].path == path || seats[i].path2 == path))
                return int(i);
        }
        return -1;
    }

    const Figure* Model::SeatFigure(int index) const
    {
        if (index < 0 || index >= int(seats.size()) || seats[index].path.empty())
            return nullptr;
        int found = FindFigure(seats[index].path);
        return found >= 0 ? &figures[found] : nullptr;
    }

    std::string Model::SeatLabel(int index) const
    {
        const Seat& s = seats[index].seat;
        char text[64];
        if (s.id.rfind("p", 0) == 0)
            snprintf(text, sizeof(text), L("Player %d", "Joueur %d"), s.player);
        else if (s.id.rfind("vehicle", 0) == 0)
            snprintf(text, sizeof(text), L("Vehicle %d", "Véhicule %d"), s.player);
        else if (s.id == "item")
        {
            // Giants has one seat for a magic item or an adventure pack: both together are "too many toys" there.
            if (std::find(s.accepts.begin(), s.accepts.end(), Category::Adventure) != s.accepts.end())
                return L("Item or adventure", "Objet ou aventure");
            return L("Magic item", "Objet magique");
        }
        else if (s.id == "adventure")
            return L("Adventure", "Aventure");
        else if (s.id == "trap")
            return L("Trap", "Piège");
        else
            return s.id;
        return text;
    }

    std::string Model::SeatTag(int index) const
    {
        const Seat& s = seats[index].seat;
        if (s.id.rfind("p", 0) == 0)
            return (French() ? "J" : "P") + std::to_string(s.player);
        if (s.id.rfind("vehicle", 0) == 0)
            return "V" + std::to_string(s.player);
        return SeatLabel(index);
    }

    std::vector<collection::PortalEntry> Model::Overview() const
    {
        if (gameInfo == nullptr)
            return {};
        std::vector<std::string> paths(seats.size()), names(seats.size());
        std::vector<int> slots(seats.size(), -1);
        for (size_t i = 0; i < seats.size(); i++)
        {
            const SeatState& s = seats[i];
            slots[i] = s.slot;
            if (s.physical)
            {
                paths[i] = "physical";
                const Toy* toy = s.physicalKnown ? Catalogue::Get().Find(s.physicalType) : nullptr;
                names[i] = s.physicalKnown ? Catalogue::Get().Describe(toy, s.physicalVariant).name : L("reading...", "lecture...");
            }
            else if (!s.path.empty())
            {
                paths[i] = s.path;
                if (const Figure* f = SeatFigure(int(i)))
                    names[i] = FigureName(*f);
            }
        }
        return PortalOverview(*gameInfo, paths, names, slots, French());
    }

    Card Model::CardFor(const Figure& f) const
    {
        int index = screen == Screen::Half ? -1 : FindFigure(f.path);
        return index >= 0 ? collection::CardOf(figures, size_t(index)) : Card{ &f, nullptr };
    }

    bool Model::CardBusy(const Figure& f) const
    {
        return CardFor(f).Busy();
    }

    std::string Model::FigureName(const Figure& f) const
    {
        if (Card card = CardFor(f); card.Swapper())
            return CardName(card);
        if (f.toy == nullptr && f.label.empty())
        {
            char text[64];
            snprintf(text, sizeof(text), L("Unknown toy %u (variant %04X)", "Jouet inconnu %u (variante %04X)"), f.type, f.variant);
            return text;
        }
        std::string name = collection::NameIn(f, game);
        std::string nickname = figures::NicknameUtf8(f.stats);
        if (f.label.empty() && !nickname.empty() && Lower(nickname) != Lower(name))
            name += " (" + nickname + ")";
        return name;
    }

    std::string Model::LevelLine(const Figure& f) const
    {
        Card card = CardFor(f);
        for (const Figure* half : card.Halves())
        {
            if (half->lock.Busy())
                return std::string(L("in use by ", "utilisée par ")) + LockOwner(half->lock);
        }
        return CardLevelText(card, game, French());
    }

    std::string Model::GameName() const
    {
        std::string name = gameInfo != nullptr ? gameInfo->name : game;
        if (name.rfind("Skylanders: ", 0) == 0)
            name = name.substr(12);
        if (French() && name == "Spyro's Adventure")
            return "Spyro's Adventure";
        return name;
    }

    void Model::RefreshSeats(uint64_t now)
    {
        if (realPortal)
        {
            for (SeatState& s : seats)
            {
                s.physical = false;
                s.slot = -1;
            }
            for (int slot = 0; slot < PortalSlotCount; slot++)
            {
                PortalSlotSeen seen = PortalSeen(slot);
                if (!seen.present)
                    continue;
                bool known = (seen.blocks & 2) != 0;
                uint16_t type = known ? figures::ToyType(seen.data) : 0;
                const Toy* toy = known ? Catalogue::Get().Find(type) : nullptr;
                for (SeatState& s : seats)
                {
                    bool fits = toy == nullptr ? PlayerSeat(s)
                        : std::find(s.seat.accepts.begin(), s.seat.accepts.end(), toy->category) != s.seat.accepts.end();
                    if (s.physical || !fits)
                        continue;
                    s.physical = true;
                    s.slot = slot;
                    s.physicalKnown = known;
                    s.physicalType = type;
                    s.physicalVariant = known ? figures::Variant(seen.data) : 0;
                    figures::FigureStats st;
                    bool progress = (seen.blocks & 0x1FF00) == 0x1FF00 || (seen.blocks & (uint64_t(0x1FF) << 36)) == (uint64_t(0x1FF) << 36);
                    s.physicalLevel = progress && figures::ReadStats(seen.data, st) == figures::AreaState::Valid ? figures::LevelForExperience(figures::TotalExperience(st), LevelCap()) : 0;
                    s.physicalGold = s.physicalLevel > 0 ? st.gold : 0;
                    break;
                }
            }
            return;
        }

        std::string inSlot[PortalSlotCount];
        for (int slot = 0; slot < PortalSlotCount; slot++)
            inSlot[slot] = PortalSlotFigure(slot);
        std::vector<bool> claimed(PortalSlotCount, false);
        for (SeatState& s : seats)
        {
            const std::string was = s.path;
            if (s.slot >= 0 && !inSlot[s.slot].empty() && (s.path == inSlot[s.slot] || s.path.empty()))
            {
                s.path = inSlot[s.slot];
                claimed[s.slot] = true;
            }
            else if (s.slot >= 0 && !inSlot[s.slot].empty())
            {
                // Something else took the slot (F7, F8): seen as placed by hand.
                s.path = inSlot[s.slot];
                s.placedAt = 0;
                s.waiting = false;
                claimed[s.slot] = true;
            }
            else
            {
                s.path.clear();
                s.waiting = false;
            }
            // A swapper's bottom stays with its seat while its slot still holds it and the top did not change.
            if (!s.path2.empty() && s.path == was && s.slot2 >= 0 && inSlot[s.slot2] == s.path2)
                claimed[s.slot2] = true;
            else
            {
                s.path2.clear();
                s.slot2 = -1;
            }
        }
        // Figures placed elsewhere (the last session, F7, SKY_PORTAL_PLACE): the first free seat they fit. A swapper's
        // two tags (linked in the collection) go to one seat; a bottom waits for its top's turn.
        auto slotOf = [&](const std::string& path) {
            for (int slot = 0; slot < PortalSlotCount; slot++)
            {
                if (!path.empty() && inSlot[slot] == path && !claimed[slot])
                    return slot;
            }
            return -1;
        };
        for (int slot = 0; slot < PortalSlotCount; slot++)
        {
            if (inSlot[slot].empty() || claimed[slot])
                continue;
            int found = FindFigure(inSlot[slot]);
            Figure read;
            const Figure* f = found >= 0 ? &figures[found] : (collection::Read(inSlot[slot], read) ? &read : nullptr);
            if (f != nullptr && f->IsSwapBottom() && !f->partner.empty() && slotOf(PathOf(f->partner)) >= 0)
                continue;
            for (SeatState& s : seats)
            {
                if (s.path.empty() && (s.slot < 0 || inSlot[s.slot].empty()) && f != nullptr && Accepts(s, *f))
                {
                    s.slot = slot;
                    s.path = inSlot[slot];
                    s.placedAt = 0;
                    claimed[slot] = true;
                    s.slot2 = f->IsSwapTop() && !f->partner.empty() ? slotOf(PathOf(f->partner)) : -1;
                    s.path2 = s.slot2 >= 0 ? inSlot[s.slot2] : std::string();
                    if (s.slot2 >= 0)
                        claimed[s.slot2] = true;
                    break;
                }
            }
        }
        // Did the title read the new figures?
        for (size_t i = 0; i < seats.size(); i++)
        {
            SeatState& s = seats[i];
            if (!s.waiting || s.slot < 0)
                continue;
            PortalSlotSeen seen = PortalSeen(s.slot);
            if (s.slot2 >= 0) // a swapper is taken when the title read both tags
                seen.lastRead = std::min(seen.lastRead, PortalSeen(s.slot2).lastRead);
            if (seen.lastRead >= s.placedAt)
            {
                s.waiting = false;
                s.late = false;
                const Figure* f = SeatFigure(int(i));
                fprintf(stderr, "[toybox] %s: the game took %s (slot %d, %.1f s after the change)\n", SeatTag(int(i)).c_str(),
                    f != nullptr ? FigureName(*f).c_str() : FileName(s.path).c_str(), s.slot, double(seen.lastRead - s.placedAt) / 1e9);
                m_dirty = true;
            }
            else if (!s.late && now - s.placedAt > LateAfter)
            {
                s.late = true;
                const Figure* f = SeatFigure(int(i));
                char text[160];
                snprintf(text, sizeof(text), L("The game has not taken %s yet", "Le jeu n'a pas encore pris %s"),
                    f != nullptr ? FigureName(*f).c_str() : "?");
                Toast(text, 5);
                fprintf(stderr, "[toybox] %s: not read by the game after 20 s (slot %d)\n", SeatTag(int(i)).c_str(), s.slot);
            }
        }
    }

    int Model::FreeSlot(int seatIndex, int half, int avoid) const
    {
        auto used = [&](int slot) {
            if (slot == avoid || !PortalSlotFigure(slot).empty())
                return true;
            for (size_t i = 0; i < seats.size(); i++)
            {
                if (int(i) != seatIndex && (seats[i].slot == slot || seats[i].slot2 == slot))
                    return true;
            }
            return false;
        };
        // Where the seat's figures sit: the slot it had, else a place by seat order, so that the title, which reads the
        // players in slot order, finds player 1 below player 2 whichever was put on first. A game with swappers gives a
        // player two places (top, bottom): P1 0-1, P2 2-3, then one for each shared seat; the others one per seat.
        bool swappers = std::any_of(seats.begin(), seats.end(), [](const SeatState& s) {
            return std::find(s.seat.accepts.begin(), s.seat.accepts.end(), Category::SwapTop) != s.seat.accepts.end(); });
        int preferred = seatIndex;
        if (swappers)
        {
            preferred = 0;
            for (int i = 0; i < seatIndex; i++)
                preferred += PlayerSeat(seats[i]) ? 2 : 1;
        }
        preferred += half;
        int own = half == 0 ? seats[seatIndex].slot : seats[seatIndex].slot2;
        if (own >= 0 && !used(own))
            return own;
        if (preferred < PortalSlotCount && !used(preferred))
            return preferred;
        for (int slot = 0; slot < PortalSlotCount; slot++)
        {
            if (!used(slot))
                return slot;
        }
        return -1;
    }

    void Model::RemoveFromPortal(int seatIndex)
    {
        SeatState& s = seats[seatIndex];
        if (s.slot >= 0 && !s.path.empty())
            PortalRemoveFigure(s.slot);
        if (s.slot2 >= 0 && !s.path2.empty())
            PortalRemoveFigure(s.slot2);
        s.path2.clear();
        s.slot2 = -1;
    }

    bool Model::PlaceCard(int seatIndex, const std::string& path)
    {
        int index = FindFigure(path);
        if (index < 0)
            return false;
        Card card = collection::CardOf(figures, size_t(index));
        int slot = FreeSlot(seatIndex, 0, -1);
        int placed = slot >= 0 ? PortalPlaceFigureAt(card.first->path, slot) : -1;
        if (placed < 0)
            return false;
        int placed2 = -1;
        if (card.Swapper())
        {
            int slot2 = FreeSlot(seatIndex, 1, placed);
            placed2 = slot2 >= 0 ? PortalPlaceFigureAt(card.second->path, slot2) : -1;
            if (placed2 < 0)
            {
                PortalRemoveFigure(placed);
                return false;
            }
        }
        SeatState& s = seats[seatIndex];
        s.slot = placed;
        s.path = card.first->path;
        s.slot2 = placed2;
        s.path2 = card.Swapper() ? card.second->path : std::string();
        return true;
    }

    bool Model::PutOn(int seatIndex, const std::string& path, bool fromWheel)
    {
        if (realPortal)
        {
            Toast(L("Real portal: use the virtual portal to play it", "Portail réel : utilise le portail virtuel pour la jouer"), 5);
            return false;
        }
        int index = FindFigure(path);
        if (index < 0)
            return false;
        // A swapper is one card, placed by its top: both halves go on the portal.
        const Card card = collection::CardOf(figures, size_t(index));
        const Figure f = *card.first;
        const std::string cardPath = f.path;
        SeatState& target = seats[seatIndex];
        for (const Figure* half : card.Halves())
        {
            if (half->lock.Busy())
            {
                Toast(std::string(L("In use by ", "Utilisée par ")) + LockOwner(half->lock));
                return false;
            }
            if (!Accepts(target, *half) || (half->toy != nullptr && !Catalogue::Get().Accepts(game, half->type)))
            {
                Toast(GameName() + L(" cannot read ", " ne lit pas ") + FigureName(f));
                return false;
            }
        }
        int other = SeatOf(cardPath);
        if (other == seatIndex)
        {
            Toast(FigureName(f) + L(" is already there", " y est déjà"));
            return false;
        }
        if (other >= 0)
        {
            if (PlayerSeat(seats[other]) && PlayerSeat(target) && !target.path.empty())
            {
                if (fromWheel)
                {
                    SwapPlayers();
                    return true;
                }
                std::string question = FigureName(f) + L(" plays as ", " joue en ") + SeatLabel(other) + ".";
                Ask({ L("Swap players?", "Échanger les joueurs ?"), question,
                    { L("Swap ", "Échanger ") + SeatTag(other) + L(" and ", " et ") + SeatTag(seatIndex), L("Cancel", "Annuler") }, 0, false,
                    [this](int choice) { if (choice == 0) SwapPlayers(); } });
                return false;
            }
            RemoveFromPortal(other);
            seats[other].path.clear();
            seats[other].waiting = false;
        }

        std::string before = target.path;
        if (!target.path.empty() && target.slot >= 0)
            RemoveFromPortal(seatIndex);
        else
            before.clear();
        if (!PlaceCard(seatIndex, cardPath))
        {
            Toast(L("The portal is full", "Le portail est plein"));
            return false;
        }
        uint64_t now = SDL_GetTicksNS();
        target.previous = before;
        target.placedAt = now;
        target.waiting = true;
        target.late = false;

        int was = FindFigure(before);
        std::string text = SeatTag(seatIndex) + ": " + (was >= 0 ? FigureName(figures[was]) + " → " : "") + FigureName(f);
        Toast(text);
        fprintf(stderr, "[toybox] %s: %s on slot %d%s\n", SeatTag(seatIndex).c_str(), FigureName(f).c_str(), target.slot,
            target.slot2 >= 0 ? (" and slot " + std::to_string(target.slot2) + " (bottom)").c_str() : "");
        std::string keep = list.cells.empty() || list.cells[list.cursor].kind != Cell::Kind::Figure ? cardPath
            : figures[list.cells[list.cursor].figure].path;
        Reload();
        BuildList();
        Refocus(keep);
        return true;
    }

    void Model::TakeOff(int seatIndex)
    {
        SeatState& s = seats[seatIndex];
        if (realPortal || s.path.empty())
            return;
        const Figure* f = SeatFigure(seatIndex);
        std::string name = f != nullptr ? FigureName(*f) : FileName(s.path);
        RemoveFromPortal(seatIndex);
        s.previous = s.path;
        s.path.clear();
        s.waiting = false;
        Toast(SeatTag(seatIndex) + ": " + name + L(" taken off", " retiré"));
        Reload();
        BuildList();
    }

    void Model::SwapPlayers()
    {
        int a = -1, b = -1;
        for (size_t i = 0; i < seats.size(); i++)
        {
            if (PlayerSeat(seats[i]) && seats[i].seat.player == 1)
                a = int(i);
            if (PlayerSeat(seats[i]) && seats[i].seat.player == 2)
                b = int(i);
        }
        if (a < 0 || b < 0 || realPortal)
            return;
        std::string pa = seats[a].path, pb = seats[b].path;
        RemoveFromPortal(a);
        RemoveFromPortal(b);
        uint64_t now = SDL_GetTicksNS();
        seats[a].path.clear();
        seats[b].path.clear();
        if (!pb.empty() && PlaceCard(a, pb))
        {
            seats[a].previous = pa;
            seats[a].placedAt = now;
            seats[a].waiting = true;
        }
        if (!pa.empty() && PlaceCard(b, pa))
        {
            seats[b].previous = pb;
            seats[b].placedAt = now;
            seats[b].waiting = true;
        }
        Toast(L("Players swapped: ", "Joueurs échangés : ") + SeatTag(a) + L(" and ", " et ") + SeatTag(b));
        Reload();
        BuildList();
    }

    void Model::ClearPortal()
    {
        PortalRemoveAllFigures();
        for (SeatState& s : seats)
        {
            s.path.clear();
            s.path2.clear();
            s.slot2 = -1;
            s.waiting = false;
        }
        Toast(L("Portal cleared", "Portail vidé"));
        Reload();
        BuildList();
    }

    void Model::SeatOptions(int seatIndex)
    {
        SeatState& s = seats[seatIndex];
        std::vector<std::string> buttons;
        std::vector<int> ids; // 0 take off, 1 swap, 2 details, 3 undo, 4 clear, 5 save copy, 6 manage
        if (realPortal)
        {
            if (s.physical)
            {
                buttons.push_back(L("Save a copy to My Skylanders", "Garder une copie dans Mes Skylanders"));
                ids.push_back(5);
            }
        }
        else
        {
            if (!s.path.empty())
            {
                buttons.push_back(L("Take off", "Retirer"));
                ids.push_back(0);
            }
            if (PlayerSeat(s))
            {
                buttons.push_back(L("Swap players (P1 and P2)", "Échanger les joueurs (J1 et J2)"));
                ids.push_back(1);
            }
            if (!s.path.empty() && FindFigure(s.path) >= 0)
            {
                buttons.push_back(L("Figure details", "Fiche de la figurine"));
                ids.push_back(2);
            }
            if (s.late && FindFigure(s.previous) >= 0)
            {
                buttons.push_back(L("Undo", "Annuler le changement"));
                ids.push_back(3);
            }
            buttons.push_back(L("Clear the portal", "Vider le portail"));
            ids.push_back(4);
        }
        buttons.push_back(L("Manage the collection", "Gérer la collection"));
        ids.push_back(6);
        Ask({ SeatLabel(seatIndex), {}, buttons, 0, false, [this, ids, seatIndex](int choice) {
            if (choice < 0)
                return;
            switch (ids[choice])
            {
            case 0: TakeOff(seatIndex); break;
            case 1: SwapPlayers(); break;
            case 2: OpenFigure(FindFigure(seats[seatIndex].path)); break;
            case 3: PutOn(seatIndex, seats[seatIndex].previous); break;
            case 4:
                Ask({ L("Clear the portal", "Vider le portail"), L("Take every figure off the portal?", "Retirer toutes les figurines du portail ?"),
                    { L("Clear", "Vider"), L("Cancel", "Annuler") }, 1, false, [this](int c) { if (c == 0) ClearPortal(); } });
                break;
            case 5: SaveCopy(seatIndex); break;
            case 6: OpenManage(); break;
            }
        } });
    }

    void Model::SaveCopy(int seatIndex)
    {
        // The title reads blocks 0-1 (serial, toy, variant), both progress areas (8-16, 36-44) and the sector
        // trailers, not the blocks later games use [V: mask 88889FF88889FF8B on placement]. The copy keeps what was
        // read; the other blocks stay blank and the trailers get the keys of the toy's serial.
        PortalSlotSeen seen = PortalSeen(seats[seatIndex].slot);
        constexpr uint64_t AreaA = uint64_t(0x1FF) << 8, AreaB = uint64_t(0x1FF) << 36;
        fprintf(stderr, "[toybox] real portal copy: blocks read %016llX\n", (unsigned long long)seen.blocks);
        if ((seen.blocks & 3) != 3 || ((seen.blocks & AreaA) != AreaA && (seen.blocks & AreaB) != AreaB))
        {
            Toast(L("Not read by the game yet", "Pas encore lu par le jeu"), 5);
            return;
        }
        FigureData tag = figures::CreateFigure(figures::ToyType(seen.data), figures::Variant(seen.data));
        for (size_t block = 0; block < FigureBlockCount; block++)
        {
            if (block % 4 != 3 && (seen.blocks >> block & 1) != 0)
                memcpy(tag.data() + block * FigureBlockSize, seen.data.data() + block * FigureBlockSize, FigureBlockSize);
        }
        figures::WriteSectorKeys(tag);
        std::string error;
        std::string path = ImportTag(tag, &error);
        if (path.empty())
            Toast(L("Could not save a copy: ", "Copie impossible : ") + error, 5);
        else if (FindFigure(path) >= 0)
            Toast(L("Already in My Skylanders: ", "Déjà dans Mes Skylanders : ") + FileName(path), 5);
        else
        {
            Toast(L("Copy saved in My Skylanders: ", "Copie gardée dans Mes Skylanders : ") + FileName(path), 5);
            Reload();
            BuildList();
        }
    }

    bool Model::Tick(uint64_t now)
    {
        if (now - m_lastScan > 250'000'000)
        {
            m_lastScan = now;
            std::vector<std::string> before;
            for (const SeatState& s : seats)
                before.push_back(s.path + (s.waiting ? "*" : "") + (s.physical ? std::to_string(s.physicalType) : ""));
            RefreshSeats(now);
            for (size_t i = 0; i < seats.size(); i++)
            {
                if (before[i] != seats[i].path + (seats[i].waiting ? "*" : "") + (seats[i].physical ? std::to_string(seats[i].physicalType) : ""))
                    m_dirty = true;
            }
        }
        // The game fades the portal's light all the time: only a visible step redraws.
        PortalLightState light = PortalLight();
        auto step = [](uint8_t v) { return uint32_t(std::min(255, (v + 32) / 64 * 64)); };
        uint32_t rgb = PortalLightOn(light) ? step(light.r) << 16 | step(light.g) << 8 | step(light.b) : 0;
        if (rgb != lightRgb)
        {
            lightRgb = rgb;
            m_dirty = true;
        }
        if (!toast.empty() && now >= toastUntil)
        {
            toast.clear();
            m_dirty = true;
        }
        if (holdStart != 0 && !dialogs.empty() && dialogs.back().hold)
        {
            holdProgress = std::min(float(now - holdStart) / float(HoldTime), 1.0f);
            m_dirty = true;
            if (now - holdStart >= HoldTime)
            {
                holdStart = 0;
                holdProgress = 0;
                Dialog dialog = std::move(dialogs.back());
                dialogs.pop_back();
                if (dialog.done)
                    dialog.done(0);
            }
        }
        return m_dirty;
    }

    void Model::Hold(bool held, uint64_t now)
    {
        if (dialogs.empty() || !dialogs.back().hold || dialogs.back().focused != 0)
        {
            holdStart = 0;
            return;
        }
        if (!held)
            m_holdArmed = true;
        if (held && holdStart == 0 && m_holdArmed)
            holdStart = now;
        else if (!held && holdStart != 0)
        {
            holdStart = 0;
            holdProgress = 0;
            m_dirty = true;
        }
    }

    // --- My Skylanders -------------------------------------------------------------------------------------------------

    void Model::BuildList()
    {
        const SeatState& s = seats[std::clamp(seat, 0, int(seats.size()) - 1)];
        // A search or a filter narrows the shelf: no "+ Add a figure" card among the results.
        const bool narrowed = !search.empty() || filterElement >= 0 || !filterGame.empty() || filterCategory >= 0;
        std::vector<Figure> shown;
        std::vector<int> map;
        // A swapper is one card: a half that passes brings the other half along.
        auto passes = [&](const Figure& f) {
            const Toy* toy = f.toy;
            return (filterElement < 0 || (toy != nullptr && int(toy->element) == filterElement)) &&
                (filterCategory < 0 || (toy != nullptr && int(toy->category) == filterCategory)) &&
                (filterGame.empty() || (toy != nullptr && toy->game == filterGame));
        };
        for (size_t i = 0; i < figures.size(); i++)
        {
            const Figure& f = figures[i];
            // A toy this game cannot read is not on its shelf (Skylauncher keeps the whole collection).
            if (!Accepts(s, f) || (f.toy != nullptr && !Catalogue::Get().Accepts(game, f.type)))
                continue;
            int other = f.partner.empty() ? -1 : FindFigure(PathOf(f.partner));
            if (!passes(f) && (other < 0 || !passes(figures[other])))
                continue;
            if (!search.empty() && !SearchMatches(search, collection::CardOf(figures, i), game))
                continue;
            shown.push_back(f);
            map.push_back(int(i));
        }
        // The shelf, as in Skylauncher: every figure once; favourites first, then the ones this game took, newest first
        // (Recent), or in the player's sort.
        std::vector<Section> sections = Shelf(shown, sort, game);
        shownCount = 0;
        for (const Section& section : sections)
            shownCount += int(section.items.size());
        if (FiltersActive())
            fprintf(stderr, "[toybox] shelf: %d found for \"%s\" (element %d, game %s, type %d, sort %s)\n", shownCount, search.c_str(),
                filterElement, filterGame.c_str(), filterCategory, SortId(sort));

        std::string keep = !list.cells.empty() && list.cursor < int(list.cells.size()) && list.cells[list.cursor].kind == Cell::Kind::Figure
            ? figures.size() > size_t(list.cells[list.cursor].figure) ? figures[list.cells[list.cursor].figure].path : "" : "";
        int oldCursor = list.cursor;

        Grid grid;
        auto addSection = [&](ListSection section, const std::vector<int>& items, bool addCard) {
            int index = int(grid.sections.size());
            section.firstRow = grid.rows;
            section.count = int(items.size());
            grid.sections.push_back(section);
            int column = 0;
            auto push = [&](Cell cell) {
                if (column == ListColumns)
                {
                    column = 0;
                    grid.rows++;
                }
                cell.section = index;
                cell.row = grid.rows;
                cell.column = column++;
                grid.cells.push_back(cell);
            };
            if (section.kind == Section::Kind::Foreign)
            {
                push({ Cell::Kind::Fold });
                column = ListColumns; // the fold is a whole row: its figures start below it
            }
            if (!section.folded)
            {
                for (int item : items)
                    push({ Cell::Kind::Figure, item });
            }
            if (addCard)
                push({ Cell::Kind::Add, -1, 0, 0, 0, Element::Count });
            grid.rows++;
        };
        bool shelf = false;
        for (const Section& section : sections)
        {
            std::vector<int> items;
            for (size_t i : section.items)
                items.push_back(map[i]);
            ListSection header{ section.kind, section.value, section.game };
            if (section.kind != Section::Kind::Shelf && !shelf && !narrowed && !realPortal)
            {
                // Nothing for this seat yet: the + card first ("Not owned yet" is one line, drawn beside it).
                shelf = true;
                addSection({ Section::Kind::Shelf, 0, {}, 0, false, false, false }, {}, true);
            }
            if (section.kind == Section::Kind::Shelf)
            {
                shelf = true;
                header.titled = false;
            }
            if (section.kind == Section::Kind::Foreign)
            {
                header.foreign = true;
                header.folded = !foreignOpen;
            }
            addSection(header, items, section.kind == Section::Kind::Shelf && !narrowed && !realPortal);
        }
        if (!shelf && !narrowed && !realPortal)
            addSection({ Section::Kind::Shelf, 0, {}, 0, false, false, false }, {}, true);
        grid.rowHeight = 150;
        grid.headerHeight = 40;
        grid.visibleHeight = 420;
        list = std::move(grid);
        list.cursor = 0;
        if (!keep.empty())
            Refocus(keep);
        else
            list.cursor = std::clamp(oldCursor, 0, std::max(int(list.cells.size()) - 1, 0));
        FixGrid(list, 0);
        m_dirty = true;
    }

    void Model::Refocus(const std::string& path)
    {
        // Each figure has one card.
        int found = -1;
        for (size_t i = 0; i < list.cells.size() && found < 0; i++)
        {
            const Cell& cell = list.cells[i];
            if (cell.kind == Cell::Kind::Figure && figures[cell.figure].path == path)
                found = int(i);
        }
        if (found >= 0)
        {
            list.cursor = found;
            FixGrid(list, 0);
            return;
        }
        list.cursor = std::clamp(list.cursor, 0, std::max(int(list.cells.size()) - 1, 0));
        FixGrid(list, 0);
    }

    int Grid::RowY(int row) const
    {
        int headers = 0;
        for (const ListSection& section : sections)
        {
            if (section.firstRow <= row && section.titled)
                headers++;
        }
        return row * rowHeight + headers * headerHeight;
    }

    int Grid::HeaderY(int section) const
    {
        const ListSection& s = sections[section];
        return RowY(s.firstRow) - (s.titled ? headerHeight : 0);
    }

    void Model::FixGrid(Grid& grid, int)
    {
        if (grid.cells.empty())
        {
            grid.cursor = 0;
            grid.scroll = 0;
            return;
        }
        grid.cursor = std::clamp(grid.cursor, 0, int(grid.cells.size()) - 1);
        const Cell& cell = grid.cells[grid.cursor];
        int top = grid.RowY(cell.row), bottom = grid.RowY(cell.row) + grid.rowHeight;
        // The section's header stays in view with its first row.
        if (cell.row == grid.sections[cell.section].firstRow)
            top = grid.HeaderY(cell.section);
        grid.scroll = std::clamp(grid.scroll, bottom - grid.visibleHeight, top);
        int total = grid.RowY(grid.rows - 1) + grid.rowHeight;
        grid.scroll = std::clamp(grid.scroll, 0, std::max(total - grid.visibleHeight, 0));
    }

    void Model::MoveGrid(Grid& grid, Command command)
    {
        if (grid.cells.empty())
            return;
        const Cell current = grid.cells[grid.cursor];
        auto inRow = [&](int row) {
            int best = -1;
            for (size_t i = 0; i < grid.cells.size(); i++)
            {
                if (grid.cells[i].row == row && (best < 0 || std::abs(grid.cells[i].column - current.column) < std::abs(grid.cells[best].column - current.column)))
                    best = int(i);
            }
            return best;
        };
        switch (command)
        {
        case Command::Up:
            for (int row = current.row - 1; row >= 0; row--)
            {
                if (int found = inRow(row); found >= 0)
                {
                    grid.cursor = found;
                    break;
                }
            }
            break;
        case Command::Down:
            for (int row = current.row + 1; row < grid.rows; row++)
            {
                if (int found = inRow(row); found >= 0)
                {
                    grid.cursor = found;
                    break;
                }
            }
            break;
        case Command::Left: grid.cursor = std::max(grid.cursor - 1, 0); break;
        case Command::Right: grid.cursor = std::min(grid.cursor + 1, int(grid.cells.size()) - 1); break;
        default: break;
        }
        FixGrid(grid, 0);
        m_dirty = true;
    }

    void Model::JumpSection(Grid& grid, int step)
    {
        if (grid.cells.empty())
            return;
        int section = std::clamp(grid.cells[grid.cursor].section + step, 0, int(grid.sections.size()) - 1);
        for (size_t i = 0; i < grid.cells.size(); i++)
        {
            if (grid.cells[i].section == section)
            {
                grid.cursor = int(i);
                break;
            }
        }
        grid.scroll = grid.HeaderY(section);
        FixGrid(grid, 0);
        m_dirty = true;
    }

    std::string Model::SectionLabel(const ListSection& section) const
    {
        char text[96];
        switch (section.kind)
        {
        case Section::Kind::Shelf: return L("Your figures", "Tes figurines");
        case Section::Kind::Element: return ElementLabel(Element(section.value));
        case Section::Kind::Category:
            if (Category(section.value) == Category::MagicItem)
                return L("My magic items", "Mes objets magiques");
            if (Category(section.value) == Category::Adventure)
                return L("My adventure packs", "Mes packs d'aventure");
            return CategoryLabel(Category(section.value));
        case Section::Kind::Game:
        {
            const Game* g = Catalogue::Get().FindGame(section.game);
            return g != nullptr ? g->name : section.game;
        }
        case Section::Kind::Series: return VariantKind(section.value) == VariantKind::Base ? L("Series 1 and standard", "Série 1 et standard") : VariantLabel(VariantKind(section.value));
        case Section::Kind::Level: snprintf(text, sizeof(text), L("Level %d", "Niveau %d"), section.value); return text;
        case Section::Kind::Letter: return std::string(1, char(section.value));
        case Section::Kind::Unknown: return L("Unknown toys", "Jouets inconnus");
        case Section::Kind::Foreign: snprintf(text, sizeof(text), L("Not for this game (%d)", "Pas pour ce jeu (%d)"), section.count); return text;
        }
        return {};
    }

    // --- Commands ---------------------------------------------------------------------------------------------------------

    void Model::Run(Command command)
    {
        if (command == Command::None)
            return;
        m_dirty = true;
        if (!dialogs.empty())
        {
            RunDialog(command);
            return;
        }
        switch (screen)
        {
        case Screen::Home: RunHome(command); break;
        case Screen::WheelList: RunList(command, true); break;
        case Screen::Figure: RunFigure(command); break;
        case Screen::Pin: RunPin(command); break;
        case Screen::Half: RunHalf(command); break;
        case Screen::Backups: RunBackups(command); break;
        case Screen::Stats: RunStats(command); break;
        case Screen::Upgrades: RunUpgrades(command); break;
        case Screen::Add: RunAdd(command); break;
        case Screen::Name: RunName(command); break;
        case Screen::Manage: RunManage(command); break;
        case Screen::Wheel:
            if (command == Command::Back)
            {
                AimWheel(-1);
                ConfirmWheel();
            }
            else if (command == Command::Accept)
                ConfirmWheel();
            else if (command == Command::View)
                WheelList();
            else if (command == Command::PreviousSection || command == Command::NextSection)
                WheelSeat(command == Command::NextSection ? 1 : -1);
            break;
        }
    }

    void Model::RunDialog(Command command)
    {
        Dialog& dialog = dialogs.back();
        int count = int(dialog.buttons.size());
        switch (command)
        {
        case Command::Up:
        case Command::Left:
            dialog.focused = (dialog.focused + count - 1) % std::max(count, 1);
            holdStart = 0;
            break;
        case Command::Down:
        case Command::Right:
            dialog.focused = (dialog.focused + 1) % std::max(count, 1);
            holdStart = 0;
            break;
        case Command::Accept:
        case Command::Back:
        {
            if (command == Command::Accept && dialog.hold && dialog.focused == 0)
                return; // held: Hold() and Tick() finish it
            int choice = command == Command::Back ? -1 : dialog.focused;
            Dialog done = std::move(dialog);
            dialogs.pop_back();
            if (done.done)
                done.done(choice);
            break;
        }
        default: break;
        }
    }

    std::vector<std::string> Model::ColumnRows() const
    {
        if (realPortal)
            return { L("Manage", "Gérer") };
        return { L("Add a figure", "Ajouter une figurine"), L("Manage", "Gérer") };
    }

    void Model::FocusSeat(int index)
    {
        if (index < 0 || index >= int(seats.size()))
            return;
        bool changed = index != seat;
        seat = index;
        column = index;
        if (changed)
        {
            BuildList();
            list.cursor = 0;
            FixGrid(list, 0);
        }
        m_dirty = true;
    }

    void Model::FocusColumnRow(int row)
    {
        inList = false;
        column = row;
        if (row < int(seats.size()))
            FocusSeat(row);
        m_dirty = true;
    }

    void Model::RunHome(Command command)
    {
        int columnCount = int(seats.size() + ColumnRows().size());
        if (filtersOpen)
        {
            RunFilters(command);
            return;
        }
        if (command == Command::View)
        {
            if (OwnsAnything())
                OpenSearch();
            return;
        }
        if (command == Command::Start)
        {
            if (OwnsAnything())
                OpenFilters();
            return;
        }
        if (inList && OwnsAnything())
        {
            RunList(command, false);
            return;
        }
        if (inList && !OwnsAnything())
        {
            // The empty state's three choices (list.cursor indexes them); up from the first goes to the seats.
            switch (command)
            {
            case Command::Up:
                if (list.cursor == 0)
                    inList = false;
                list.cursor = std::max(list.cursor - 1, 0);
                return;
            case Command::Down: list.cursor = std::min(list.cursor + 1, 2); return;
            case Command::Back: Close(); return;
            case Command::Accept:
                if (list.cursor == 0)
                {
                    BuildAdd();
                    screen = Screen::Add;
                }
                else if (list.cursor == 1)
                {
                    OpenManage();
                    manage.section = 2;
                    manage.inPanel = true;
                }
                else
                {
                    // SSA's starter box.
                    for (uint16_t type : { uint16_t(16), uint16_t(14), uint16_t(19) })
                        Create(type);
                    Reload();
                    BuildList();
                    inList = true;
                    Toast(L("Spyro, Gill Grunt and Trigger Happy are in My Skylanders", "Spyro, Gill Grunt et Trigger Happy sont dans Mes Skylanders"));
                }
                return;
            default: return;
            }
        }
        // The top row: the seats, then Add a figure and Manage. Down goes into the figures for the chosen seat.
        switch (command)
        {
        case Command::Left: FocusColumnRow((column + columnCount - 1) % columnCount); break;
        case Command::Right: FocusColumnRow((column + 1) % columnCount); break;
        case Command::PreviousSection: FocusColumnRow(std::max(std::min(column, int(seats.size()) - 1) - 1, 0)); break;
        case Command::NextSection: FocusColumnRow(std::min(column + 1, int(seats.size()) - 1)); break;
        case Command::Down:
            inList = true;
            if (!OwnsAnything())
                list.cursor = 0;
            break;
        case Command::Accept:
            if (column < int(seats.size()))
            {
                inList = true;
                if (!OwnsAnything())
                    list.cursor = 0;
            }
            else
            {
                std::string row = ColumnRows()[column - seats.size()];
                if (row == L("Manage", "Gérer"))
                    OpenManage();
                else
                {
                    BuildAdd();
                    screen = Screen::Add;
                }
            }
            break;
        case Command::Options:
            if (column < int(seats.size()))
                SeatOptions(column);
            break;
        case Command::Delete:
            if (column < int(seats.size()))
                TakeOff(column);
            break;
        case Command::Back: Close(); break;
        default: break;
        }
    }

    void Model::RunList(Command command, bool wheelList)
    {
        Grid& grid = list;
        const Cell* cell = grid.cells.empty() ? nullptr : &grid.cells[grid.cursor];
        switch (command)
        {
        case Command::Up:
            // From the first row, up to the seats.
            if (!wheelList && (cell == nullptr || cell->row == grid.cells.front().row))
            {
                inList = false;
                column = seat;
            }
            else
                MoveGrid(grid, command);
            break;
        case Command::Down:
        case Command::Left:
        case Command::Right:
            MoveGrid(grid, command);
            break;
        case Command::PreviousSection:
        case Command::NextSection:
            // LB / RB: the seat before or after, still in the grid (the wheel's list keeps its seat).
            if (wheelList)
                JumpSection(grid, command == Command::NextSection ? 1 : -1);
            else
            {
                FocusSeat(std::clamp(seat + (command == Command::NextSection ? 1 : -1), 0, int(seats.size()) - 1));
                inList = true;
            }
            break;
        case Command::Accept:
            if (cell == nullptr)
                break;
            if (cell->kind == Cell::Kind::Fold)
            {
                foreignOpen = !foreignOpen;
                BuildList();
                for (size_t i = 0; i < list.cells.size(); i++)
                {
                    if (list.cells[i].kind == Cell::Kind::Fold)
                        list.cursor = int(i);
                }
                FixGrid(list, 0);
                break;
            }
            if (cell->kind == Cell::Kind::Add)
            {
                BuildAdd(cell->element);
                screen = Screen::Add;
                break;
            }
            if (grid.sections[cell->section].foreign)
            {
                Toast(GameName() + L(" cannot read ", " ne lit pas ") + FigureName(figures[cell->figure]));
                break;
            }
            if (PutOn(wheelList ? wheel.seat : seat, figures[cell->figure].path, wheelList) && wheelList)
                Close();
            break;
        case Command::Options:
            if (!wheelList && cell != nullptr && cell->kind == Cell::Kind::Figure)
                OpenFigure(cell->figure);
            break;
        case Command::Delete:
            // Delete on a figure that sits on a seat takes it off (the seat column's Delete does the same).
            if (!wheelList && cell != nullptr && cell->kind == Cell::Kind::Figure && SeatOf(figures[cell->figure].path) >= 0)
            {
                std::string path = figures[cell->figure].path;
                TakeOff(SeatOf(path));
                Refocus(path);
            }
            break;
        case Command::Back:
            if (!search.empty() && !wheelList)
            {
                search.clear();
                BuildList();
            }
            else
                Close();
            break;
        default: break;
        }
    }

    void Model::FocusCell(int index)
    {
        if (index < 0 || index >= int(list.cells.size()))
            return;
        inList = true;
        list.cursor = index;
        FixGrid(list, 0);
        m_dirty = true;
    }

    void Model::ActivateCell(int index)
    {
        FocusCell(index);
        Run(Command::Accept);
    }

    void Model::OptionsCell(int index)
    {
        FocusCell(index);
        Run(Command::Options);
    }

    void Model::ScrollList(int rows)
    {
        Grid& grid = screen == Screen::Add ? add : list;
        for (int i = 0; i < std::abs(rows); i++)
            MoveGrid(grid, rows > 0 ? Command::Down : Command::Up);
    }

    void Model::Type(const std::string& utf8)
    {
        if (!dialogs.empty())
            return;
        if (screen == Screen::Name)
        {
            for (size_t i = 0; i < utf8.size();)
            {
                size_t length = 1;
                uint8_t lead = uint8_t(utf8[i]);
                length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                std::string character = utf8.substr(i, length);
                i += length;
                if (Utf8Length(name.text) >= name.limit)
                    break;
                name.text.insert(name.cursor, character);
                name.cursor += character.size();
            }
            m_dirty = true;
            return;
        }
        if (screen == Screen::Home && OwnsAnything() && !utf8.empty() && !(utf8 == " " && search.empty()))
        {
            filtersOpen = false;
            search += utf8;
            inList = true;
            BuildList();
            list.cursor = 0;
            FixGrid(list, 0);
        }
    }

    void Model::Backspace()
    {
        if (screen == Screen::Name)
        {
            if (name.cursor == 0)
                return;
            size_t at = name.cursor - 1;
            while (at > 0 && (uint8_t(name.text[at]) & 0xC0) == 0x80)
                at--;
            name.text.erase(at, name.cursor - at);
            name.cursor = at;
            m_dirty = true;
            return;
        }
        if (screen == Screen::Home && !search.empty())
        {
            size_t at = search.size() - 1;
            while (at > 0 && (uint8_t(search[at]) & 0xC0) == 0x80)
                at--;
            search.erase(at);
            BuildList();
        }
    }

    // --- Filters and sort (the Filters panel and the tags above the shelf) -----------------------------------------------

    namespace
    {
        // The elements in the order the games' menus show them.
        constexpr Element MenuElements[] = { Element::Magic, Element::Earth, Element::Water, Element::Air, Element::Fire, Element::Life,
            Element::Undead, Element::Tech, Element::Light, Element::Dark };

        std::string ShortGame(const std::string& id)
        {
            const Game* found = Catalogue::Get().FindGame(id);
            std::string name = found != nullptr ? found->name : id;
            return name.rfind("Skylanders: ", 0) == 0 ? name.substr(12) : name;
        }
    }

    std::string Model::FilterRowLabel(int row) const
    {
        switch (row)
        {
        case 0: return L("Element", "Élément");
        case 1: return L("Game", "Jeu");
        case 2: return L("Type", "Type");
        case 3: return L("Sort by", "Trier par");
        default: return L("Clear all", "Tout effacer");
        }
    }

    std::string Model::FilterValueLabel(int row) const
    {
        switch (row)
        {
        case 0: return filterElement < 0 ? L("All elements", "Tous les éléments") : ElementLabel(Element(filterElement));
        case 1: return filterGame.empty() ? L("All games", "Tous les jeux") : ShortGame(filterGame);
        case 2: return filterCategory < 0 ? L("All types", "Tous les types") : CategoryLabel(Category(filterCategory));
        case 3: return SortLabel(sort, French());
        default: return {};
        }
    }

    void Model::CycleFilter(int row, int step)
    {
        // The values of a row, "All" first; the types are the ones the player has.
        std::vector<int> values;
        int current = 0;
        if (row == 0)
        {
            values.push_back(-1);
            for (Element element : MenuElements)
                values.push_back(int(element));
            current = filterElement;
        }
        else if (row == 1)
        {
            values.push_back(-1);
            for (const Game& g : Catalogue::Get().Games())
                values.push_back(int(&g - Catalogue::Get().Games().data()));
            current = -1;
            for (size_t i = 0; i < Catalogue::Get().Games().size(); i++)
                if (Catalogue::Get().Games()[i].id == filterGame)
                    current = int(i);
        }
        else if (row == 2)
        {
            values.push_back(-1);
            for (int category = 0; category < int(Category::Count); category++)
                if (category == filterCategory || std::any_of(figures.begin(), figures.end(), [&](const Figure& f) { return f.toy != nullptr && int(f.toy->category) == category; }))
                    values.push_back(category);
            current = filterCategory;
        }
        else if (row == 3)
        {
            for (int i = 0; i < int(Sort::Count); i++)
                values.push_back(i);
            current = int(sort);
        }
        else
            return;
        size_t at = size_t(std::find(values.begin(), values.end(), current) - values.begin());
        if (at == values.size())
            at = 0;
        int next = values[(at + (step < 0 ? values.size() - 1 : 1)) % values.size()];
        if (row == 0)
            filterElement = next;
        else if (row == 1)
            filterGame = next < 0 ? std::string() : Catalogue::Get().Games()[size_t(next)].id;
        else if (row == 2)
            filterCategory = next;
        else
            sort = Sort(next);
        BuildList();
        list.cursor = 0;
        FixGrid(list, 0);
        m_dirty = true;
    }

    bool Model::FiltersActive() const
    {
        return !search.empty() || filterElement >= 0 || !filterGame.empty() || filterCategory >= 0 || sort != Sort::Recent;
    }

    std::vector<Model::Tag> Model::Tags() const
    {
        std::vector<Tag> tags;
        if (filterElement >= 0)
            tags.push_back({ FilterValueLabel(0), 0 });
        if (!filterGame.empty())
            tags.push_back({ FilterValueLabel(1), 1 });
        if (filterCategory >= 0)
            tags.push_back({ FilterValueLabel(2), 2 });
        if (sort != Sort::Recent)
            tags.push_back({ std::string(L("Sort: ", "Tri : ")) + FilterValueLabel(3), 3 });
        return tags;
    }

    void Model::RemoveTag(int index)
    {
        std::vector<Tag> tags = Tags();
        if (index < 0 || index >= int(tags.size()))
            return;
        switch (tags[size_t(index)].row)
        {
        case 0: filterElement = -1; break;
        case 1: filterGame.clear(); break;
        case 2: filterCategory = -1; break;
        default: sort = Sort::Recent; break;
        }
        BuildList();
        list.cursor = 0;
        FixGrid(list, 0);
        m_dirty = true;
    }

    void Model::ClearFilters()
    {
        bool changed = FiltersActive();
        search.clear();
        filterElement = -1;
        filterGame.clear();
        filterCategory = -1;
        sort = Sort::Recent;
        filtersOpen = false;
        filterRow = 0;
        if (changed && screen == Screen::Home)
        {
            BuildList();
            list.cursor = 0;
            FixGrid(list, 0);
        }
        m_dirty = true;
    }

    void Model::ClearSearch()
    {
        search.clear();
        BuildList();
        list.cursor = 0;
        FixGrid(list, 0);
        m_dirty = true;
    }

    void Model::OpenFilters()
    {
        filtersOpen = true;
        m_dirty = true;
    }

    void Model::CloseFilters()
    {
        filtersOpen = false;
        m_dirty = true;
    }

    void Model::RunFilters(Command command)
    {
        switch (command)
        {
        case Command::Up: filterRow = (filterRow + FilterRows) % (FilterRows + 1); break;
        case Command::Down: filterRow = (filterRow + 1) % (FilterRows + 1); break;
        case Command::Left: CycleFilter(filterRow, -1); break;
        case Command::Right: CycleFilter(filterRow, 1); break;
        case Command::Accept:
            if (filterRow == FilterRows)
            {
                ClearFilters();
                filtersOpen = true; // stays open: the player sees it empty
            }
            else
                CycleFilter(filterRow, 1);
            break;
        case Command::View:
            CloseFilters();
            OpenSearch();
            break;
        case Command::Back:
        case Command::Start: CloseFilters(); break;
        default: break;
        }
        m_dirty = true;
    }

    // --- Search (Y): our keyboard, or a physical one typing straight into the grid ---------------------------------------

    void Model::OpenSearch()
    {
        NameEntry entry;
        entry.purpose = NameEntry::Purpose::Search;
        entry.title = L("SEARCH", "RECHERCHE");
        entry.text = search;
        entry.done = [this](bool keep, const std::string& text) {
            if (keep)
                search = text;
            screen = Screen::Home;
            inList = true;
            BuildList();
            list.cursor = 0;
            FixGrid(list, 0);
        };
        OpenName(entry);
    }

    // --- Figure page -----------------------------------------------------------------------------------------------------

    namespace
    {
        // Three main actions (put on, edit, favourite), as Skylauncher's figure sheet; the rest behind More.
        std::vector<Action> ActionsFor(const Model& model, const Figure& f)
        {
            std::vector<Action> actions;
            if (model.figureMore)
            {
                if (KeepsProgress(f))
                    actions.push_back(Action::Rename);
                for (Action a : { Action::Pin, Action::Duplicate, Action::BackUp, Action::Restore, Action::Reset, Action::Delete })
                    actions.push_back(a);
                if (!KeepsProgress(f))
                    std::erase(actions, Action::Reset);
                return actions;
            }
            if (!model.realPortal)
                actions.push_back(Action::PutOn);
            if (!model.realPortal && model.SeatOf(f.path) >= 0)
                actions.push_back(Action::TakeOff);
            if (f.lock.Busy())
                actions.push_back(Action::Unlock);
            actions.push_back(KeepsProgress(f) ? Action::EditStats : Action::Rename);
            if (f.IsSwapTop() || f.IsSwapBottom())
                actions.push_back(Action::ChangeHalf);
            if (model.WheelPossible(f))
                actions.push_back(Action::Wheel);
            for (Action a : { Action::Favourite, Action::More, Action::Details })
                actions.push_back(a);
            return actions;
        }
    }

    void Model::OpenFigure(int index)
    {
        if (index < 0 || index >= int(figures.size()))
            return;
        figure = index;
        figureAction = 0;
        figureMore = false;
        screen = Screen::Figure;
        m_dirty = true;
    }

    std::vector<std::string> Model::FigureActions() const
    {
        std::vector<std::string> lines;
        if (figure < 0 || figure >= int(figures.size()))
            return lines;
        const Figure& f = figures[figure];
        std::string busy = f.lock.Busy() ? std::string(L("in use by ", "utilisée par ")) + LockOwner(f.lock) : std::string();
        for (Action a : ActionsFor(*this, f))
        {
            switch (a)
            {
            case Action::PutOn:
            {
                const Figure* on = SeatFigure(seat);
                std::string line = L("Put on ", "Poser en ") + SeatLabel(seat) + "\t";
                if (on != nullptr && on->path != f.path)
                    line += L("replaces ", "remplace ") + FigureName(*on);
                lines.push_back(line);
                break;
            }
            case Action::TakeOff: lines.push_back(std::string(L("Take off ", "Retirer de ")) + SeatLabel(SeatOf(f.path))); break;
            case Action::Unlock: lines.push_back(std::string(L("It is not in use any more", "Elle n'est plus utilisée")) + "\t" + busy); break;
            case Action::EditStats: lines.push_back(std::string(L("Edit", "Modifier")) + "\t" + (busy.empty() ? L("level, gold, nickname", "niveau, or, surnom") : busy)); break;
            case Action::ChangeHalf:
                lines.push_back(std::string(f.IsSwapTop() ? L("Change the bottom", "Changer le bas") : L("Change the top", "Changer le haut")) + "\t" +
                    (busy.empty() ? (f.partner.empty() ? L("none yet", "aucun pour l'instant") : L("mix halves", "mélanger les moitiés")) : busy));
                break;
            case Action::Rename: lines.push_back(std::string(L("Rename", "Renommer")) + "\t" + (busy.empty() ? L("the name in the lists", "le nom dans les listes") : busy)); break;
            case Action::Favourite:
                lines.push_back(f.favourite ? std::string(L("Favourite", "Favori")) + "\t" + L("on: shown first", "oui : en premier")
                                            : std::string(L("Add to favourites", "Ajouter aux favoris")));
                break;
            case Action::Wheel:
                lines.push_back(collection::OnWheel(game, f.file) ? std::string(L("On the wheel", "Sur la roue")) + "\t" + L("on: A takes it off", "oui : A la retire")
                                                                  : std::string(L("Add to the wheel", "Ajouter à la roue")));
                break;
            case Action::More: lines.push_back(std::string(L("More…", "Plus…")) + "\t" + L("other actions", "autres actions")); break;
            case Action::Details: lines.push_back(figureDetails ? L("Hide the details", "Masquer les détails") : L("Details", "Détails")); break;
            case Action::Pin: lines.push_back(L("Pin to the wheel…", "Épingler sur la roue…")); break;
            case Action::Duplicate: lines.push_back(L("Duplicate", "Dupliquer")); break;
            case Action::BackUp: lines.push_back(L("Back up now", "Sauvegarder maintenant")); break;
            case Action::Restore: lines.push_back(std::string(L("Restore a backup…", "Restaurer une sauvegarde…")) + "\t" + busy); break;
            case Action::Reset: lines.push_back(std::string(L("Reset to new", "Remettre à neuf")) + "\t" + (busy.empty() ? L("hold A", "maintenir A") : busy)); break;
            case Action::Delete: lines.push_back(std::string(L("Delete", "Supprimer")) + "\t" + (busy.empty() ? L("to the trash", "à la corbeille") : busy)); break;
            }
        }
        return lines;
    }

    void Model::FocusAction(int index)
    {
        figureAction = std::max(index, 0);
        m_dirty = true;
    }

    void Model::RunFigure(Command command)
    {
        int count = int(FigureActions().size());
        switch (command)
        {
        case Command::Up: figureAction = (figureAction + count - 1) % count; break;
        case Command::Down: figureAction = (figureAction + 1) % count; break;
        case Command::Accept: DoFigureAction(figureAction); break;
        case Command::PreviousSection:
        case Command::NextSection:
        {
            // The previous / next figure of the list, each once.
            std::vector<int> order;
            for (const Cell& cell : list.cells)
            {
                if (cell.kind == Cell::Kind::Figure && std::find(order.begin(), order.end(), cell.figure) == order.end())
                    order.push_back(cell.figure);
            }
            auto at = std::find(order.begin(), order.end(), figure);
            if (order.empty())
                break;
            int index = at == order.end() ? 0 : int(at - order.begin());
            index = (index + (command == Command::NextSection ? 1 : int(order.size()) - 1)) % int(order.size());
            figure = order[index];
            figureAction = std::min(figureAction, int(FigureActions().size()) - 1);
            break;
        }
        case Command::Back:
            if (figureMore)
            {
                // Out of More: back on its line.
                figureMore = false;
                std::vector<Action> main = ActionsFor(*this, figures[figure]);
                figureAction = int(std::find(main.begin(), main.end(), Action::More) - main.begin());
                break;
            }
            screen = Screen::Home;
            Refocus(figures[figure].path);
            break;
        default: break;
        }
    }

    void Model::DoFigureAction(int index)
    {
        Figure f = figures[figure];
        std::vector<Action> actions = ActionsFor(*this, f);
        if (index < 0 || index >= int(actions.size()))
            return;
        Action action = actions[index];
        bool writes = action == Action::EditStats || action == Action::Rename || action == Action::Restore || action == Action::Reset
            || action == Action::Delete || action == Action::ChangeHalf;
        if (writes && CardBusy(f))
        {
            for (const Figure* half : CardFor(f).Halves())
            {
                if (half->lock.Busy())
                {
                    Toast(std::string(L("In use by ", "Utilisée par ")) + LockOwner(half->lock));
                    return;
                }
            }
        }
        std::string path = f.path;
        std::vector<std::string> halves;
        switch (action)
        {
        case Action::More:
            figureMore = true;
            figureAction = 0;
            break;
        case Action::Details: figureDetails = !figureDetails; break;
        case Action::PutOn:
            if (PutOn(seat, path))
            {
                screen = Screen::Home;
                Refocus(path);
            }
            break;
        case Action::TakeOff:
            TakeOff(SeatOf(path));
            screen = Screen::Home;
            Refocus(path);
            break;
        case Action::Unlock:
            Ask({ L("It is not in use any more", "Elle n'est plus utilisée"),
                std::string(L("Only if ", "Seulement si ")) + LockOwner(f.lock) + L(" has quit.", " est fermé."),
                { L("Free it", "La libérer"), L("Cancel", "Annuler") }, 1, false, [this, path](int c) {
                    if (c != 0)
                        return;
                    BreakLock(path);
                    Reload();
                    BuildList();
                    figure = FindFigure(path);
                    Toast(L("Free again", "Libérée"));
                } });
            break;
        case Action::EditStats: OpenStats(figure); break;
        case Action::ChangeHalf: OpenHalf(); break;
        case Action::Rename:
        {
            NameEntry entry;
            entry.purpose = NameEntry::Purpose::Rename;
            entry.title = L("RENAME", "RENOMMER");
            entry.text = f.label;
            std::string base = f.toy != nullptr ? Catalogue::Get().Describe(f.toy, f.variant).name : FigureName(f);
            entry.suggestions = { base, base + " 2", L("Player 1's ", "") + base + L("", " du Joueur 1"),
                L("Player 2's ", "") + base + L("", " du Joueur 2") };
            std::string nickname = figures::NicknameUtf8(f.stats);
            if (!nickname.empty())
                entry.suggestions.insert(entry.suggestions.begin() + 1, nickname);
            entry.done = [this, path](bool keep, const std::string& text) {
                screen = Screen::Figure;
                if (!keep)
                    return;
                SetLabel(path, text);
                Reload();
                figure = FindFigure(path);
                BuildList();
                Toast(L("Renamed", "Renommée"));
            };
            OpenName(entry);
            break;
        }
        case Action::Favourite:
            SetFavourite(path, !f.favourite);
            Reload();
            figure = FindFigure(path);
            BuildList();
            break;
        case Action::Wheel:
        {
            bool on = !collection::OnWheel(game, f.file);
            if (collection::SetOnWheel(game, f.file, on))
                Toast(FigureName(f) + (on ? L(" is on the wheel", " est sur la roue") : L(" is off the wheel", " n'est plus sur la roue")));
            else
                Toast(L("The wheel is full: take a figure off it first", "La roue est pleine : retire d'abord une figurine"), 4);
            break;
        }
        case Action::Pin:
            pinPlace = 0;
            screen = Screen::Pin;
            break;
        case Action::Duplicate:
        {
            std::string error;
            std::string copy = collection::Duplicate(path, &error);
            if (copy.empty())
            {
                Toast(L("Could not duplicate: ", "Copie impossible : ") + error);
                break;
            }
            Reload();
            BuildList();
            figure = FindFigure(copy);
            Toast(L("Copy made: ", "Copie faite : ") + FileName(copy));
            break;
        }
        case Action::BackUp:
            Toast(Backup(path).empty() ? L("Could not back up", "Sauvegarde impossible") : L("Backed up", "Sauvegardée"));
            PruneBackups(path);
            break;
        case Action::Restore:
            backups = Backups(path);
            backup = 0;
            if (backups.empty())
                Toast(L("No backup yet", "Pas encore de sauvegarde"));
            else
                screen = Screen::Backups;
            break;
        case Action::Reset:
            Ask({ L("Reset to new", "Remettre à neuf"),
                FigureName(f) + L(" loses its level, gold and upgrades.", " perd son niveau, son or et ses améliorations."),
                { L("Hold A to reset", "Maintiens A pour remettre à neuf"), L("Cancel", "Annuler") }, 0, true, [this, path](int c) {
                    if (c != 0)
                        return;
                    int s = SeatOf(path);
                    int slot = s >= 0 ? seats[s].slot : -1;
                    if (slot >= 0)
                        PortalRemoveFigure(slot);
                    std::string error;
                    bool done = ResetToNew(path, &error);
                    if (slot >= 0)
                        PortalPlaceFigureAt(path, slot);
                    Reload();
                    figure = FindFigure(path);
                    BuildList();
                    Toast(done ? L("Figure reset to new", "Figurine remise à neuf") : L("Could not reset: ", "Remise à neuf impossible : ") + error);
                } });
            break;
        case Action::Delete:
            for (const Figure* half : CardFor(f).Halves())
                halves.push_back(half->path);
            Ask({ L("Delete", "Supprimer"),
                FigureName(f) + (halves.size() > 1 ? L(" goes to the trash, both halves.", " part à la corbeille, les deux moitiés.")
                                                   : L(" goes to the trash.", " part à la corbeille.")),
                { L("Hold A to delete", "Maintiens A pour supprimer"), L("Cancel", "Annuler") }, 0, true, [this, path, halves](int c) {
                    if (c != 0)
                        return;
                    int s = SeatOf(path);
                    if (s >= 0)
                    {
                        RemoveFromPortal(s);
                        seats[s].path.clear();
                    }
                    // A swapper is one card: both halves go to the trash.
                    std::string error;
                    bool done = true;
                    for (const std::string& half : halves)
                        done = collection::Delete(half, "deleted", &error) && done;
                    Reload();
                    BuildList();
                    screen = Screen::Home;
                    figure = -1;
                    Toast(done ? L("Moved to the trash", "Mise à la corbeille") : L("Could not delete: ", "Suppression impossible : ") + error);
                } });
            break;
        }
    }

    // --- Pin to the wheel, backups ----------------------------------------------------------------------------------------

    // Change the bottom / the top: the card keeps its half and takes another dump of the other category. Every dump of
    // that category is a card of its own here (CardFor), the ones on a seat or in use left out.
    void Model::OpenHalf()
    {
        const Figure& keep = figures[figure];
        bool wantBottom = keep.IsSwapTop();
        Grid grid;
        grid.sections.push_back({ Section::Kind::Shelf, 0, {}, 0, false, false });
        screen = Screen::Half; // before the cells: CardFor shows each dump alone
        int column = 0;
        for (size_t i = 0; i < figures.size(); i++)
        {
            const Figure& f = figures[i];
            if ((wantBottom ? !f.IsSwapBottom() : !f.IsSwapTop()) || f.file == keep.partner || f.lock.Busy() || SeatOf(f.path) >= 0)
                continue;
            if (column == ListColumns)
            {
                column = 0;
                grid.rows++;
            }
            grid.cells.push_back({ Cell::Kind::Figure, int(i), 0, grid.rows, column++ });
        }
        grid.rows += grid.cells.empty() ? 0 : 1;
        grid.sections[0].count = int(grid.cells.size());
        grid.rowHeight = 150;
        grid.visibleHeight = 420;
        list = std::move(grid);
        FixGrid(list, 0);
        m_dirty = true;
    }

    void Model::RunHalf(Command command)
    {
        switch (command)
        {
        case Command::Up:
        case Command::Down:
        case Command::Left:
        case Command::Right: MoveGrid(list, command); break;
        case Command::Accept:
            if (!list.cells.empty())
            {
                std::string keep = figures[figure].path, chosen = figures[list.cells[list.cursor].figure].path;
                ChangeHalfTo(keep, chosen);
            }
            break;
        case Command::Back:
            screen = Screen::Figure;
            BuildList();
            break;
        default: break;
        }
    }

    void Model::ChangeHalfTo(const std::string& keep, const std::string& replacement)
    {
        // A card on the portal is lifted, changed and put back, so that both tags are the new ones.
        screen = Screen::Figure;
        int on = SeatOf(keep);
        std::string played = on >= 0 ? seats[on].path : std::string();
        if (on >= 0)
        {
            RemoveFromPortal(on);
            seats[on].path.clear();
        }
        std::string error;
        bool done = collection::ChangeHalf(keep, replacement, &error);
        Reload();
        figure = FindFigure(keep);
        if (figure >= 0 && figures[figure].IsSwapBottom() && !figures[figure].partner.empty())
            figure = FindFigure(PathOf(figures[figure].partner)); // a card shows as its top
        BuildList();
        if (on >= 0 && !played.empty())
            PutOn(on, played);
        Toast(done ? L("Halves changed", "Moitiés changées") : L("Could not change: ", "Changement impossible : ") + error, 4);
        if (figure < 0)
            screen = Screen::Home;
    }

    void Model::RunPin(Command command)
    {
        switch (command)
        {
        case Command::Left:
        case Command::Up: pinPlace = (pinPlace + WheelPlaces - 1) % WheelPlaces; break;
        case Command::Right:
        case Command::Down: pinPlace = (pinPlace + 1) % WheelPlaces; break;
        case Command::Accept:
        {
            int playerSeat = PlayerSeat(seats[seat]) ? seat : 0;
            std::vector<WheelPlace> places = GetWheel(game, seats[playerSeat].seat.id);
            std::string file = figures[figure].file;
            for (WheelPlace& place : places)
            {
                if (place.file == file)
                    place = {};
            }
            places[pinPlace] = { file, true };
            SetWheel(game, seats[playerSeat].seat.id, places);
            char text[96];
            snprintf(text, sizeof(text), L("Pinned to place %d of %s's wheel", "Épinglée à la place %d de la roue du %s"), pinPlace + 1,
                SeatLabel(playerSeat).c_str());
            Toast(text);
            screen = Screen::Figure;
            break;
        }
        case Command::Back: screen = Screen::Figure; break;
        default: break;
        }
    }

    void Model::RunBackups(Command command)
    {
        int count = int(backups.size());
        switch (command)
        {
        case Command::Up: backup = std::max(backup - 1, 0); break;
        case Command::Down: backup = std::min(backup + 1, count - 1); break;
        case Command::Accept:
        {
            std::string path = figures[figure].path, chosen = backups[backup];
            Ask({ L("Restore a backup", "Restaurer une sauvegarde"), L("Put this copy back?", "Remettre cette copie ?"),
                { L("Restore", "Restaurer"), L("Cancel", "Annuler") }, 0, false, [this, path, chosen](int c) {
                    if (c != 0)
                        return;
                    int s = SeatOf(path);
                    int slot = s >= 0 ? seats[s].slot : -1;
                    if (slot >= 0)
                        PortalRemoveFigure(slot);
                    std::string error;
                    bool done = Restore(path, chosen, &error);
                    if (slot >= 0)
                        PortalPlaceFigureAt(path, slot);
                    Reload();
                    figure = FindFigure(path);
                    BuildList();
                    screen = figure >= 0 ? Screen::Figure : Screen::Home;
                    Toast(done ? L("Backup restored", "Sauvegarde restaurée") : L("Could not restore: ", "Restauration impossible : ") + error);
                } });
            break;
        }
        case Command::Back: screen = manage.inPanel && manage.section == 1 ? Screen::Manage : Screen::Figure; break;
        default: break;
        }
    }

    // --- Edit stats (the old sheet) ---------------------------------------------------------------------------------------

    void Model::OpenStats(int index)
    {
        stats = {};
        stats.figure = index;
        if (!LoadFigure(figures[index].path, stats.tag))
        {
            Toast(L("Cannot read the figure's file", "Fichier de la figurine illisible"));
            return;
        }
        figures::ReadStats(stats.tag, stats.stats);
        screen = Screen::Stats;
    }

    void Model::FocusStatsRow(int row)
    {
        if (stats.row == row && row >= 0)
            RunStats(Command::Accept);
        else
            stats.row = row;
        m_dirty = true;
    }

    // --- Upgrades: the toy's upgrades by name (collection::Upgrades), the paths side by side -----------------------------

    void Model::OpenUpgrades()
    {
        const Figure& f = figures[stats.figure];
        if (f.toy == nullptr)
            return;
        upgrades = {};
        upgrades.table = collection::Upgrades(*f.toy, game, French() ? "fr" : "en");
        int basic = 0, first = 0, second = 0, soul = 0;
        for (size_t i = 0; i < upgrades.table.upgrades.size(); i++)
        {
            using collection::UpgradePath;
            UpgradeItem item{ UpgradeItem::Kind::Upgrade, int(i), 0, 0 };
            switch (upgrades.table.upgrades[i].path)
            {
            case UpgradePath::Basic: item.row = basic++; break;
            case UpgradePath::First: item.column = 1; item.row = first++; break;
            case UpgradePath::Second: item.column = 2; item.row = second++; break;
            default: item.row = 4 + soul++; break; // Soul Gem, then the Wow Pow, under the basic ones
            }
            upgrades.items.push_back(item);
        }
        upgrades.items.push_back({ UpgradeItem::Kind::All, 0, 0, 6 });
        upgrades.items.push_back({ UpgradeItem::Kind::None, 0, 1, 6 });
        upgrades.items.push_back({ UpgradeItem::Kind::Done, 0, 2, 6 });
        screen = Screen::Upgrades;
        m_dirty = true;
    }

    void Model::FocusUpgrade(int item)
    {
        if (item < 0 || item >= int(upgrades.items.size()))
            return;
        upgrades.cursor = item;
        RunUpgrades(Command::Accept);
    }

    void Model::RunUpgrades(Command command)
    {
        figures::FigureStats& s = stats.stats;
        std::vector<UpgradeItem>& items = upgrades.items;
        if (items.empty())
        {
            screen = Screen::Stats;
            return;
        }
        auto move = [&](int dc, int dr) {
            const UpgradeItem& from = items[upgrades.cursor];
            int best = -1, score = 1 << 30;
            for (size_t i = 0; i < items.size(); i++)
            {
                int c = items[i].column - from.column, r = items[i].row - from.row;
                if ((dc != 0 && c * dc <= 0) || (dr != 0 && r * dr <= 0))
                    continue;
                int along = dc != 0 ? std::abs(c) : std::abs(r), across = dc != 0 ? std::abs(r) : std::abs(c);
                if (along * 10 + across < score)
                    score = along * 10 + across, best = int(i);
            }
            if (best >= 0)
                upgrades.cursor = best;
        };
        switch (command)
        {
        case Command::Up: move(0, -1); break;
        case Command::Down: move(0, 1); break;
        case Command::Left: move(-1, 0); break;
        case Command::Right: move(1, 0); break;
        case Command::Back: screen = Screen::Stats; break;
        case Command::Accept:
        {
            const UpgradeItem& item = items[upgrades.cursor];
            uint16_t before = collection::EditorBits(s.upgrades, s.wowPow), bits = before;
            if (item.kind == UpgradeItem::Kind::Done)
            {
                screen = Screen::Stats;
                break;
            }
            if (item.kind == UpgradeItem::Kind::All)
                bits = collection::AllUpgrades(bits, upgrades.table);
            else if (item.kind == UpgradeItem::Kind::None)
                bits = collection::NoUpgrades(bits);
            else
                bits = collection::ToggleUpgrade(bits, upgrades.table.upgrades[item.index]);
            collection::StoreEditorBits(bits, s.upgrades, s.wowPow); // the Wow Pow goes to its own place
            stats.dirty = stats.dirty || bits != before;
            break;
        }
        default: break;
        }
        m_dirty = true;
    }

    void Model::SaveStats()
    {
        if (!stats.dirty || stats.figure < 0)
            return;
        std::string path = figures[stats.figure].path;
        // A figure on the portal is lifted and put back so that the title reads the new values (it keeps what it read
        // when the figure arrived and writes it back later).
        int s = SeatOf(path);
        int slot = s >= 0 ? seats[s].slot : -1;
        if (slot >= 0)
            PortalRemoveFigure(slot);
        std::string error;
        bool saved = WriteStats(path, stats.stats, &error);
        if (slot >= 0)
            PortalPlaceFigureAt(path, slot);
        stats.dirty = false;
        Toast(saved ? L("Saved", "Enregistrée") : L("Could not save: ", "Échec de l'enregistrement : ") + error);
        Reload();
        stats.figure = FindFigure(path);
        figure = stats.figure;
        BuildList();
    }

    namespace
    {
        int SetBits(uint32_t value)
        {
            int count = 0;
            for (; value != 0; value &= value - 1)
                count++;
            return count;
        }
    }

    std::vector<uint16_t> Model::Hats() const
    {
        std::vector<uint16_t> hats;
        if (gameInfo == nullptr || gameInfo->hats.empty())
            return hats;
        hats.push_back(0);
        for (const auto& [number, name] : gameInfo->hats)
            if (number != 0)
                hats.push_back(number);
        return hats;
    }

    std::string Model::HatName(uint16_t hat) const
    {
        if (hat == 0)
            return L("No hat", "Aucun chapeau");
        if (gameInfo != nullptr)
            if (auto found = gameInfo->hats.find(hat); found != gameInfo->hats.end())
                return found->second;
        return (French() ? "Chapeau " : "Hat ") + std::to_string(hat);
    }

    void Model::RunStats(Command command)
    {
        // Rows: 0 nickname, 1 hat, 2 level, 3 experience, 4 gold, 5 play time, 6 hero points, 7 heroic, 8 upgrades,
        // 9 cancel, 10 save.
        constexpr int Rows = 11;
        figures::FigureStats& s = stats.stats;
        int step = command == Command::Left || command == Command::PreviousSection ? -1 : command == Command::Right || command == Command::NextSection ? 1 : 0;
        bool big = command == Command::PreviousSection || command == Command::NextSection;
        auto clamp = [](int64_t value, int64_t high) { return uint32_t(std::clamp<int64_t>(value, 0, high)); };
        switch (command)
        {
        case Command::Up: stats.row = (stats.row + Rows - 1) % Rows; return;
        case Command::Down: stats.row = (stats.row + 1) % Rows; return;
        case Command::Back:
            SaveStats();
            screen = Screen::Figure;
            return;
        case Command::Accept:
            if (stats.row == 0)
            {
                NameEntry entry;
                entry.purpose = NameEntry::Purpose::Nickname;
                entry.title = L("NICKNAME IN THE GAME", "SURNOM DANS LE JEU");
                entry.text = figures::NicknameUtf8(s);
                entry.limit = 15;
                const Figure& f = figures[stats.figure];
                entry.suggestions = { f.toy != nullptr ? f.toy->name : FigureName(f) };
                if (!f.label.empty())
                    entry.suggestions.push_back(f.label);
                entry.done = [this](bool keep, const std::string& text) {
                    screen = Screen::Stats;
                    if (!keep)
                        return;
                    figures::SetNicknameUtf8(stats.stats, text);
                    stats.dirty = true;
                };
                OpenName(entry);
            }
            else if (stats.row == 8)
                OpenUpgrades();
            else if (stats.row == 9)
            {
                stats.dirty = false;
                screen = Screen::Figure;
                Toast(L("Changes dropped", "Changements annulés"));
            }
            else if (stats.row == 10)
            {
                if (!stats.dirty)
                    Toast(L("Nothing to save", "Aucun changement à enregistrer"));
                SaveStats();
            }
            return;
        default: break;
        }
        if (step == 0)
            return;
        // Experience as the running game counts it: Spyro's Adventure changes only its own part (up to level 10), so a
        // figure Giants took to level 15 keeps those levels (docs/runtime/host/figures.md, "Data rules").
        int cap = LevelCap();
        uint32_t maxExperience = figures::LevelExperience[cap - 1];
        uint32_t total = figures::ExperienceFor(s, cap);
        switch (stats.row)
        {
        case 1:
        {
            std::vector<uint16_t> hats = Hats();
            if (!hats.empty())
            {
                // The game's hats by name, in its order (0: none).
                auto at = std::find(hats.begin(), hats.end(), s.hat);
                int index = at == hats.end() ? 0 : int(at - hats.begin());
                s.hat = hats[std::clamp(index + step * (big ? 10 : 1), 0, int(hats.size()) - 1)];
            }
            else if (cap > figures::MaxLevel)
                s.hat = uint16_t(clamp(int64_t(s.hat) + step * (big ? 10 : 1), 0xFF));
            else
            {
                // Spyro's Adventure's own hat, in the first area; Giants shows it too unless it has one of its own.
                uint16_t hat = uint16_t(clamp(int64_t(s.ssaHat) + step * (big ? 10 : 1), 0xFF));
                if (s.hat == s.ssaHat)
                    s.hat = hat;
                s.ssaHat = hat;
            }
            break;
        }
        case 2:
        {
            int level = std::clamp(figures::LevelForExperience(total, cap) + step, 1, cap);
            figures::SetExperienceFor(s, cap, figures::LevelExperience[level - 1]);
            break;
        }
        case 3: figures::SetExperienceFor(s, cap, clamp(int64_t(total) + step * (big ? 1000 : 100), maxExperience)); break;
        case 4: s.gold = uint16_t(clamp(int64_t(s.gold) + step * (big ? 1000 : 100), figures::MaxGold)); break;
        case 5: s.playSeconds = clamp(int64_t(s.playSeconds) + step * (big ? 3600 : 60), 0xFFFFFFFFll); break;
        case 6: s.heroPoints = uint16_t(clamp(int64_t(s.heroPoints) + step * (big ? 10 : 1), 0xFFFF)); break;
        case 7:
        {
            int count = std::clamp(SetBits(s.heroicChallenges) + step * (big ? 8 : 1), 0, 32);
            s.heroicChallenges = count == 32 ? 0xFFFFFFFFu : (1u << count) - 1;
            break;
        }
        default: return;
        }
        stats.dirty = true;
    }

    // --- Add a figure --------------------------------------------------------------------------------------------------

    int Model::OwnedCount(uint16_t type, uint16_t variant, const std::string& edition) const
    {
        int count = 0;
        for (const Figure& f : figures)
            count += f.type == type && f.variant == variant && f.edition == edition ? 1 : 0;
        return count;
    }

    // A swapper is added as one card (its top): the bottom of the same swapper is made with it.
    bool Model::HalfMadeWithTop(const Toy& toy) const
    {
        return toy.category == Category::SwapBottom && Catalogue::Get().Accepts(game, MatchingHalf(toy.type));
    }

    std::string Model::AddName(const Toy& toy)
    {
        std::string name = toy.name;
        if (toy.category == Category::SwapTop && name.size() > 6 && name.compare(name.size() - 6, 6, " (top)") == 0)
            name.erase(name.size() - 6);
        return name;
    }

    void Model::BuildAdd(Element element)
    {
        const Catalogue& catalogue = Catalogue::Get();
        std::vector<const Toy*> toys = catalogue.Accepted(game);
        Grid grid;
        addToys.clear();
        struct Group
        {
            Section::Kind kind;
            int value;
            std::vector<const Toy*> toys;
        };
        std::vector<Group> groups;
        static const Element Elements[] = { Element::Magic, Element::Earth, Element::Water, Element::Air, Element::Fire,
            Element::Life, Element::Undead, Element::Tech, Element::Light, Element::Dark };
        for (Element e : Elements)
        {
            Group group{ Section::Kind::Element, int(e) };
            for (const Toy* toy : toys)
            {
                if (toy->element == e && toy->category != Category::Mini && toy->category <= Category::Sensei && !HalfMadeWithTop(*toy))
                    group.toys.push_back(toy);
            }
            groups.push_back(group);
        }
        for (Category c : { Category::Mini, Category::Trap, Category::Vehicle, Category::CreationCrystal, Category::MagicItem,
                 Category::Adventure, Category::Trophy })
        {
            Group group{ Section::Kind::Category, int(c) };
            for (const Toy* toy : toys)
            {
                if (toy->category == c)
                    group.toys.push_back(toy);
            }
            groups.push_back(group);
        }
        int jump = -1;
        for (const Group& group : groups)
        {
            if (group.toys.empty())
                continue;
            int index = int(grid.sections.size());
            ListSection section{ group.kind, group.value };
            section.firstRow = grid.rows;
            section.count = int(group.toys.size());
            grid.sections.push_back(section);
            int col = 0;
            for (const Toy* toy : group.toys)
            {
                if (col == AddColumns)
                {
                    col = 0;
                    grid.rows++;
                }
                Cell cell{ Cell::Kind::Figure, int(addToys.size()), index, grid.rows, col++ };
                addToys.push_back(toy);
                grid.cells.push_back(cell);
            }
            grid.rows++;
            bool wanted = element == Element::Count ? false
                : element == Element::None ? group.kind == Section::Kind::Category
                    && std::find(seats[seat].seat.accepts.begin(), seats[seat].seat.accepts.end(), Category(group.value)) != seats[seat].seat.accepts.end()
                : group.kind == Section::Kind::Element && Element(group.value) == element;
            if (wanted && jump < 0)
                jump = int(grid.cells.size()) - section.count;
        }
        grid.rowHeight = 72;
        grid.headerHeight = 40;
        grid.visibleHeight = 520;
        add = std::move(grid);
        add.cursor = std::max(jump, 0);
        FixGrid(add, 0);
        addVersions = false;
        addVersion = 0;
        BuildVersions();
        m_dirty = true;
    }

    void Model::BuildVersions()
    {
        versions.clear();
        if (add.cells.empty())
            return;
        const Toy* toy = addToys[add.cells[add.cursor].figure];
        // The plain toy is one row, named as the toy is: made under the id it is sold with when the catalogue has one (its
        // "base" entry, which is never a second row of its own). Then only the versions this game tells apart: the others
        // play as the plain toy in it.
        uint16_t sold = StandardVariant(*toy);
        Version plain{ toy->type, sold, AddName(*toy), {}, toy->edition };
        plain.owned = OwnedCount(toy->type, 0, {}) + (sold != 0 ? OwnedCount(toy->type, sold, {}) : 0);
        versions.push_back(plain);
        for (const Variant* v : Catalogue::Get().VersionsFor(game, *toy))
        {
            Version version{ toy->type, uint16_t(v->sameTag ? sold : v->id), v->name, v->sameTag ? v->name : std::string(), v->kind };
            version.owned = OwnedCount(version.type, version.variant, version.edition);
            versions.push_back(version);
        }
    }

    void Model::FocusAddCell(int index)
    {
        if (index < 0 || index >= int(add.cells.size()))
            return;
        if (add.cursor == index && !addVersions)
        {
            addVersions = true;
            addVersion = 0;
        }
        else
        {
            add.cursor = index;
            addVersions = false;
            BuildVersions();
        }
        FixGrid(add, 0);
        m_dirty = true;
    }

    void Model::FocusVersion(int index)
    {
        if (index < 0 || index >= int(versions.size()))
            return;
        if (addVersions && addVersion == index)
            CreateFrom(versions[index]);
        else
        {
            addVersions = true;
            addVersion = index;
        }
        m_dirty = true;
    }

    void Model::RunAdd(Command command)
    {
        if (addVersions)
        {
            int count = int(versions.size());
            switch (command)
            {
            case Command::Up: addVersion = (addVersion + count - 1) % count; break;
            case Command::Down: addVersion = (addVersion + 1) % count; break;
            case Command::Accept: CreateFrom(versions[addVersion]); break;
            case Command::Back:
            case Command::Left: addVersions = false; break;
            default: break;
            }
            return;
        }
        switch (command)
        {
        case Command::Up:
        case Command::Down:
        case Command::Left:
        case Command::Right:
        {
            int before = add.cursor;
            const Cell& cell = add.cells[add.cursor];
            if (command == Command::Right && cell.column == AddColumns - 1)
            {
                addVersions = true;
                addVersion = 0;
                break;
            }
            MoveGrid(add, command);
            if (add.cursor != before)
                BuildVersions();
            break;
        }
        case Command::PreviousSection:
        case Command::NextSection:
            JumpSection(add, command == Command::NextSection ? 1 : -1);
            BuildVersions();
            break;
        case Command::Accept:
            addVersions = true;
            addVersion = 0;
            break;
        case Command::Back:
            screen = Screen::Home;
            BuildList();
            break;
        default: break;
        }
    }

    void Model::CreateFrom(const Version& version)
    {
        // A swapper: both halves, linked (docs/runtime/host/collection.md).
        const Toy* made = Catalogue::Get().Find(version.type);
        std::string path = made != nullptr && made->category == Category::SwapTop && MatchingHalf(version.type) != 0
            ? CreateSwapper(version.type, MatchingHalf(version.type), version.variant).top
            : Create(version.type, version.variant);
        if (path.empty())
        {
            Toast(L("Could not create the figure", "Création impossible"));
            return;
        }
        if (!version.edition.empty())
            SetEdition(path, version.edition, version.kind);
        const Toy* toy = Catalogue::Get().Find(version.type);
        std::string base = version.edition.empty() ? Catalogue::Get().Describe(toy, version.variant).name : version.edition;
        if (made != nullptr && made->category == Category::SwapTop && base.size() > 6 && base.compare(base.size() - 6, 6, " (top)") == 0)
            base.erase(base.size() - 6); // a swapper's name, not its top's
        int owned = OwnedCount(version.type, version.variant, version.edition);
        NameEntry entry;
        entry.purpose = NameEntry::Purpose::NewFigure;
        entry.title = std::string(L("NAME THIS FIGURE", "NOMME CETTE FIGURINE"));
        entry.text = {};
        entry.suggestions = { base, L("Player 1's ", "") + base + L("", " du Joueur 1"), L("Player 2's ", "") + base + L("", " du Joueur 2"),
            L("(no name)", "(sans nom)") };
        if (owned > 0)
            entry.suggestions.insert(entry.suggestions.begin() + 1, base + " " + std::to_string(owned + 1));
        entry.done = [this, path, base](bool keep, const std::string& text) {
            if (keep && !text.empty() && text != base && text != L("(no name)", "(sans nom)"))
                SetLabel(path, text);
            Reload();
            BuildList();
            screen = Screen::Add;
            BuildVersions();
            int index = FindFigure(path);
            if (index < 0)
                return;
            Toast(FigureName(figures[index]) + L(" is in My Skylanders", " est dans Mes Skylanders"));
            // Offer the seat it fits: the focused one, else the first that takes it.
            int target = Accepts(seats[seat], figures[index]) ? seat : -1;
            for (size_t i = 0; target < 0 && i < seats.size(); i++)
            {
                if (Accepts(seats[i], figures[index]))
                    target = int(i);
            }
            if (target < 0 || !Catalogue::Get().Accepts(game, figures[index].type))
                return;
            Ask({ FigureName(figures[index]), {},
                { L("Put it on ", "La poser en ") + SeatLabel(target) + L(" now", " maintenant"), L("Not now", "Pas maintenant") }, 0, false,
                [this, path, target](int c) {
                    if (c != 0)
                        return;
                    seat = target;
                    if (PutOn(target, path))
                    {
                        screen = Screen::Home;
                        inList = true;
                        BuildList();
                        Refocus(path);
                    }
                } });
        };
        OpenName(entry);
    }

    // --- Names: suggestions and the on-screen keyboard ------------------------------------------------------------------

    const std::vector<std::vector<std::string>>& Model::Keys(bool french, bool symbols, bool shift)
    {
        auto make = [](std::initializer_list<const char*> rows, bool upper) {
            std::vector<std::vector<std::string>> keys;
            for (const char* row : rows)
            {
                std::vector<std::string> line;
                for (const char* p = row; *p != '\0';)
                {
                    size_t length = uint8_t(*p) >= 0xE0 ? 3 : uint8_t(*p) >= 0xC0 ? 2 : 1;
                    std::string key(p, length);
                    p += length;
                    if (upper && length == 1)
                        key[0] = char(toupper(uint8_t(key[0])));
                    else if (upper && key == "é")
                        key = "É";
                    else if (upper && key == "è")
                        key = "È";
                    line.push_back(key);
                }
                keys.push_back(line);
            }
            keys.push_back({ "\x01" "shift", "\x01" "symbols", "\x01" "space", "\x01" "delete", "\x01" "done" });
            return keys;
        };
        static const auto EnglishLower = make({ "qwertyuiop", "asdfghjkl'", "zxcvbnm-.!" }, false);
        static const auto EnglishUpper = make({ "qwertyuiop", "asdfghjkl'", "zxcvbnm-.!" }, true);
        static const auto FrenchLower = make({ "azertyuiop", "qsdfghjklm", "wxcvbné\xC3\xA8'-" }, false);
        static const auto FrenchUpper = make({ "azertyuiop", "qsdfghjklm", "wxcvbné\xC3\xA8'-" }, true);
        static const auto Symbols = make({ "1234567890", "&@#()!?.,:", "àçùâêîôû_/" }, false);
        if (symbols)
            return Symbols;
        if (french)
            return shift ? FrenchUpper : FrenchLower;
        return shift ? EnglishUpper : EnglishLower;
    }

    void Model::OpenName(NameEntry entry)
    {
        name = std::move(entry);
        name.cursor = name.text.size();
        name.focus = 0;
        name.onKeys = name.suggestions.empty();
        name.keyRow = name.keyColumn = 0;
        name.shift = name.text.empty();
        name.symbols = false;
        screen = Screen::Name;
        m_dirty = true;
    }

    void Model::FocusKey(int row, int column)
    {
        name.onKeys = true;
        name.keyRow = row;
        name.keyColumn = column;
        RunName(Command::Accept);
        m_dirty = true;
    }

    void Model::FocusSuggestion(int index)
    {
        name.onKeys = false;
        name.focus = index;
        RunName(Command::Accept);
    }

    void Model::RunName(Command command)
    {
        const auto& keys = Keys(French(), name.symbols, name.shift);
        auto finish = [this](bool keep) {
            NameEntry entry = name;
            std::string text = entry.text;
            if (entry.done)
                entry.done(keep, text);
        };
        switch (command)
        {
        case Command::Up:
            if (name.onKeys && name.keyRow == 0 && !name.suggestions.empty())
                name.onKeys = false;
            else if (name.onKeys)
                name.keyRow--;
            return;
        case Command::Down:
            if (!name.onKeys)
                name.onKeys = true;
            else
                name.keyRow = std::min(name.keyRow + 1, int(keys.size()) - 1);
            name.keyColumn = std::min(name.keyColumn, int(keys[name.keyRow].size()) - 1);
            return;
        case Command::Left:
            if (name.onKeys)
                name.keyColumn = (name.keyColumn + int(keys[name.keyRow].size()) - 1) % int(keys[name.keyRow].size());
            else
                name.focus = (name.focus + int(name.suggestions.size()) - 1) % int(name.suggestions.size());
            return;
        case Command::Right:
            if (name.onKeys)
                name.keyColumn = (name.keyColumn + 1) % int(keys[name.keyRow].size());
            else
                name.focus = (name.focus + 1) % int(name.suggestions.size());
            return;
        case Command::PreviousSection:
            while (name.cursor > 0)
            {
                name.cursor--;
                if ((uint8_t(name.text[name.cursor]) & 0xC0) != 0x80)
                    break;
            }
            return;
        case Command::NextSection:
            while (name.cursor < name.text.size())
            {
                name.cursor++;
                if (name.cursor == name.text.size() || (uint8_t(name.text[name.cursor]) & 0xC0) != 0x80)
                    break;
            }
            return;
        case Command::Options: Backspace(); return;
        case Command::View:
        case Command::Start: finish(true); return;
        case Command::Back: finish(false); return;
        case Command::Accept:
            if (!name.onKeys)
            {
                std::string chosen = name.suggestions[name.focus];
                if (chosen == L("(no name)", "(sans nom)"))
                    chosen.clear();
                name.text = chosen;
                name.cursor = chosen.size();
                finish(true);
                return;
            }
            name.keyColumn = std::min(name.keyColumn, int(keys[name.keyRow].size()) - 1);
            {
                const std::string& key = keys[name.keyRow][name.keyColumn];
                if (key == "\x01" "shift")
                    name.shift = !name.shift;
                else if (key == "\x01" "symbols")
                    name.symbols = !name.symbols;
                else if (key == "\x01" "space")
                    Type(" ");
                else if (key == "\x01" "delete")
                    Backspace();
                else if (key == "\x01" "done")
                    finish(true);
                else
                {
                    Type(key);
                    if (name.shift && !name.symbols)
                        name.shift = false;
                }
            }
            return;
        default: return;
        }
    }

    // --- Manage ----------------------------------------------------------------------------------------------------------

    void Model::OpenManage()
    {
        manage = {};
        RefreshManage();
        screen = Screen::Manage;
        m_dirty = true;
    }

    void Model::RefreshManage()
    {
        Reload();
        manage.trash = Trash();
        manage.withBackups.clear();
        manage.unknown.clear();
        for (size_t i = 0; i < figures.size(); i++)
        {
            if (!Backups(figures[i].path).empty())
                manage.withBackups.push_back(int(i));
            if (figures[i].toy == nullptr)
                manage.unknown.push_back(int(i));
        }
        manage.starters = UntouchedStarters();
        const char* import = skyenv::Get("SKY_IMPORT_FOLDER");
        manage.importFolder = import != nullptr ? import : collection::Folder() + "/Import";
        manage.exportFolder = collection::Folder() + "/Export";
        m_dirty = true;
    }

    void Model::FocusManage(int section, int item, bool panel)
    {
        bool again = manage.section == section && manage.inPanel == panel && manage.item == item;
        manage.section = section;
        manage.inPanel = panel;
        manage.item = item;
        if (again && panel)
            ManageAction();
        m_dirty = true;
    }

    void Model::RunManage(Command command)
    {
        int items = 1;
        switch (manage.section)
        {
        case 0: items = std::max(int(manage.trash.size()), 1); break;
        case 1: items = std::max(int(manage.withBackups.size()), 1); break;
        case 4: items = std::max(int(manage.unknown.size()), 1); break;
        default: items = 1; break;
        }
        if (!manage.inPanel)
        {
            switch (command)
            {
            case Command::Up: manage.section = (manage.section + 5) % 6; manage.item = 0; break;
            case Command::Down: manage.section = (manage.section + 1) % 6; manage.item = 0; break;
            case Command::Right:
            case Command::Accept: manage.inPanel = true; manage.item = 0; break;
            case Command::Back: screen = Screen::Home; BuildList(); break;
            default: break;
            }
            return;
        }
        switch (command)
        {
        case Command::Up: manage.item = std::max(manage.item - 1, 0); break;
        case Command::Down: manage.item = std::min(manage.item + 1, items - 1); break;
        case Command::Left:
        case Command::Back: manage.inPanel = false; break;
        case Command::Accept: ManageAction(); break;
        case Command::Options:
            if (manage.section == 0 && manage.item < int(manage.trash.size()))
            {
                TrashEntry entry = manage.trash[manage.item];
                Ask({ L("Delete for good", "Supprimer pour de bon"), entry.file + L(" cannot be brought back after this.", " ne pourra plus être récupérée."),
                    { L("Hold A to delete for good", "Maintiens A pour supprimer"), L("Cancel", "Annuler") }, 0, true, [this, entry](int c) {
                        if (c != 0)
                            return;
                        DeleteForGood(entry.id);
                        RefreshManage();
                        manage.item = std::min(manage.item, std::max(int(manage.trash.size()) - 1, 0));
                        Toast(L("Deleted for good", "Supprimée pour de bon"));
                    } });
            }
            break;
        default: break;
        }
    }

    void Model::ManageAction()
    {
        char text[256];
        switch (manage.section)
        {
        case 0: // trash: put back
            if (manage.item < int(manage.trash.size()))
            {
                std::string back = Undelete(manage.trash[manage.item].id);
                RefreshManage();
                manage.item = std::min(manage.item, std::max(int(manage.trash.size()) - 1, 0));
                Toast(back.empty() ? L("Could not put it back", "Impossible de la remettre") : L("Put back: ", "Remise : ") + FileName(back));
                BuildList();
            }
            break;
        case 1: // backups of a figure
            if (manage.item < int(manage.withBackups.size()))
            {
                figure = manage.withBackups[manage.item];
                backups = Backups(figures[figure].path);
                backup = 0;
                screen = Screen::Backups;
            }
            break;
        case 2: // import
        {
            SDL_CreateDirectory(manage.importFolder.c_str());
            ImportReport report = Import({ manage.importFolder });
            snprintf(text, sizeof(text), L("Import: %d added, %d already there, %d not figure files, %d unknown toys",
                "Import : %d ajoutées, %d déjà là, %d fichiers non valides, %d jouets inconnus"),
                report.added, report.duplicates, report.invalid, int(report.unknown.size()));
            Toast(text, 5);
            RefreshManage();
            BuildList();
            break;
        }
        case 3: // export
        {
            time_t now = time(nullptr);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M", localtime(&now));
            std::vector<std::string> paths;
            for (const Figure& f : figures)
                paths.push_back(f.path);
            std::string folder = manage.exportFolder + "/" + stamp;
            SDL_CreateDirectory(manage.exportFolder.c_str());
            int copied = Export(paths, folder);
            snprintf(text, sizeof(text), L("%d figures copied to %s", "%d figurines copiées dans %s"), copied, folder.c_str());
            Toast(text, 6);
            break;
        }
        case 4: // name an unknown toy
            if (manage.item < int(manage.unknown.size()))
            {
                uint16_t type = figures[manage.unknown[manage.item]].type;
                NameEntry entry;
                entry.purpose = NameEntry::Purpose::ToyName;
                entry.title = L("NAME THIS TOY", "NOMME CE JOUET");
                entry.done = [this, type](bool keep, const std::string& text) {
                    screen = Screen::Manage;
                    if (!keep || text.empty())
                        return;
                    // The collection's catalogue.json: added to the built-in catalogue at the next start.
                    std::string path = collection::Folder() + "/catalogue.json";
                    Json root = Json::Object();
                    size_t size = 0;
                    if (char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size)))
                    {
                        std::string error;
                        Json parsed;
                        if (Json::Parse(std::string(data, size), parsed, error) && parsed.IsObject())
                            root = parsed;
                        SDL_free(data);
                    }
                    Json toys = Json::Array();
                    for (const Json& toy : root["toys"].Items())
                    {
                        if (toy["type"].AsInt(-1) != type)
                            toys.Push(toy);
                    }
                    Json toy = Json::Object();
                    toy.Set("type", int(type));
                    toy.Set("name", text);
                    toy.Set("category", "core");
                    toy.Set("game", game);
                    toys.Push(toy);
                    root.Set("format", 1);
                    root.Set("toys", toys);
                    std::string dump = root.Dump();
                    bool saved = SDL_SaveFile(path.c_str(), dump.data(), dump.size());
                    Toast(saved ? L("Named: shown at the next start", "Nommé : affiché au prochain lancement")
                                : L("Couldn't save the name", "Nom non enregistré"), 5);
                };
                OpenName(entry);
            }
            break;
        case 5: // tidy
        {
            int moved = collection::Tidy();
            snprintf(text, sizeof(text), L("%d blank figures moved to the trash", "%d figurines vierges mises à la corbeille"), moved);
            Toast(text);
            RefreshManage();
            BuildList();
            break;
        }
        }
    }

    // --- The quick swap wheel ------------------------------------------------------------------------------------------

    void Model::OpenWheel(int seatIndex)
    {
        Reload();
        RefreshSeats(SDL_GetTicksNS());
        wheel = {};
        wheel.seat = std::clamp(seatIndex, 0, int(seats.size()) - 1);
        const SeatState& s = seats[wheel.seat];
        wheel.places = GetWheel(game, s.seat.id);
        wheel.places.resize(WheelPlaces);
        // Places keep their figure; empty ones (and figures gone) fill from this game's recents for the seat.
        bool changed = false;
        for (WheelPlace& place : wheel.places)
        {
            if (!place.file.empty() && FindFigure(collection::PathOf(place.file)) < 0)
            {
                place = {};
                changed = true;
            }
        }
        const std::vector<std::string> skipped = GetWheelSkipped(game, s.seat.id);
        for (const std::string& path : Recent(game, 32))
        {
            int index = FindFigure(path);
            if (index >= 0 && figures[index].IsSwapBottom() && !figures[index].partner.empty())
                index = FindFigure(PathOf(figures[index].partner)); // a swapper's card is its top
            if (index < 0 || !Accepts(s, figures[index]) || figures[index].path == s.path ||
                std::find(skipped.begin(), skipped.end(), figures[index].file) != skipped.end())
                continue;
            if (std::any_of(wheel.places.begin(), wheel.places.end(), [&](const WheelPlace& p) { return p.file == figures[index].file; }))
                continue;
            auto empty = std::find_if(wheel.places.begin(), wheel.places.end(), [](const WheelPlace& p) { return p.file.empty(); });
            if (empty == wheel.places.end())
                break;
            empty->file = figures[index].file;
            changed = true;
        }
        if (changed)
            SetWheel(game, s.seat.id, wheel.places);
        for (const WheelPlace& place : wheel.places)
            wheel.figures.push_back(place.file.empty() ? -1 : FindFigure(collection::PathOf(place.file)));
        wheel.aimed = -1;
        screen = Screen::Wheel;
        open = true;
        dialogs.clear();
        fprintf(stderr, "[toybox] wheel for %s\n", SeatTag(wheel.seat).c_str());
        m_dirty = true;
    }

    bool Model::WheelPossible(const Figure& f) const
    {
        if (f.toy == nullptr || !Catalogue::Get().Accepts(game, f.type))
            return false;
        return std::any_of(seats.begin(), seats.end(), [&](const SeatState& s) { return PlayerSeat(s) && Accepts(s, f); });
    }

    void Model::AimWheel(int place)
    {
        if (place != wheel.aimed)
        {
            wheel.aimed = place;
            wheel.pinHoldStart = 0;
            m_dirty = true;
        }
    }

    void Model::ConfirmWheel()
    {
        int aimed = wheel.aimed;
        int index = aimed >= 0 && aimed < int(wheel.figures.size()) ? wheel.figures[aimed] : -1;
        std::string path = index >= 0 ? figures[index].path : std::string();
        Close();
        if (!path.empty())
            PutOn(wheel.seat, path, true);
    }

    void Model::WheelSeat(int step)
    {
        int next = (wheel.seat + step + int(seats.size())) % int(seats.size());
        OpenWheel(next);
    }

    void Model::WheelList()
    {
        seat = wheel.seat;
        search.clear();
        ClearFilters();
        BuildList();
        list.cursor = 0;
        FixGrid(list, 0);
        screen = Screen::WheelList;
        m_dirty = true;
    }

    void Model::PinFromWheel(uint64_t now, bool held)
    {
        if (!held || wheel.aimed < 0)
        {
            wheel.pinHoldStart = 0;
            return;
        }
        if (wheel.pinHoldStart == 0)
        {
            wheel.pinHoldStart = now;
            return;
        }
        if (now - wheel.pinHoldStart < PinHoldTime)
            return;
        wheel.pinHoldStart = 0;
        const SeatState& s = seats[wheel.seat];
        int current = FindFigure(s.path);
        if (current < 0)
            return;
        for (WheelPlace& place : wheel.places)
        {
            if (place.file == figures[current].file)
                place = {};
        }
        wheel.places[wheel.aimed] = { figures[current].file, true };
        SetWheel(game, s.seat.id, wheel.places);
        for (size_t i = 0; i < wheel.places.size(); i++)
            wheel.figures[i] = wheel.places[i].file.empty() ? -1 : FindFigure(collection::PathOf(wheel.places[i].file));
        char text[96];
        snprintf(text, sizeof(text), L("%s pinned to place %d", "%s épinglé à la place %d"), FigureName(figures[current]).c_str(), wheel.aimed + 1);
        Toast(text);
    }

    void Model::FocusDialogButton(int index)
    {
        if (dialogs.empty())
            return;
        Dialog& dialog = dialogs.back();
        if (index < 0 || index >= int(dialog.buttons.size()))
            return;
        dialog.focused = index;
        if (!(dialog.hold && index == 0))
            RunDialog(Command::Accept);
        m_dirty = true;
    }
}
