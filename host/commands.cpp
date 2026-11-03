#include "host/commands.h"
#include "host/settings.h"
#include <SDL3/SDL.h>
#include <cstdio>

namespace
{
    std::string KeyLine(const char* action) { return std::string("key_") + action; }
    std::string PadLine(int button) { return std::string("pad_") + g_padButtons[button]; }

    const std::string* Find(const LauncherConfig& config, const std::string& section, const std::string& key)
    {
        auto entry = config.find(section);
        if (entry == config.end())
            return nullptr;
        auto value = entry->second.find(key);
        return value == entry->second.end() || value->second.empty() ? nullptr : &value->second;
    }

    const char* DefaultKey(std::string_view action)
    {
        for (const KeyAction& entry : g_keyActions)
            if (action == entry.name)
                return entry.defaultKey;
        return "";
    }

    bool SameKey(const std::string& a, const std::string& b)
    {
        return SDL_GetScancodeFromName(a.c_str()) == SDL_GetScancodeFromName(b.c_str());
    }

    void StorePad(LauncherConfig& config, std::string_view game, int button, int target)
    {
        const std::string section = commands::Section(game);
        if (target == button)
            config[section].erase(PadLine(button));
        else
            config[section][PadLine(button)] = target < 0 ? "none" : g_padButtons[target];
    }

    // Skylauncher's ApplyGameInput (PLAY): the game's keys replace the key_ lines of [input]; an action the game does not
    // list keeps the runtime's default.
    void ShareKeys(LauncherConfig& config, std::string_view game, std::span<const commands::Row> rows)
    {
        const std::string section = commands::Section(game);
        for (const KeyAction& entry : g_keyActions)
        {
            const std::string line = KeyLine(entry.name);
            bool listed = false;
            for (const commands::Row& row : rows)
                listed = listed || std::string_view(row.action) == entry.name;
            if (const std::string* value = Find(config, section, line))
                config["input"][line] = *value;
            else if (listed)
                config["input"][line] = entry.defaultKey;
            else if (auto shared = config.find("input"); shared != config.end())
                shared->second.erase(line);
        }
    }

    constexpr const char* ToyboxDefaults[commands::ToyboxKeyCount] = { "F6", "F7", "F8" };

    size_t ActionIndex(std::string_view action)
    {
        for (size_t i = 0; i < std::size(g_keyActions); i++)
            if (action == g_keyActions[i].name)
                return i;
        return std::size(g_keyActions);
    }

    void StoreSlot(LauncherConfig& config, std::string_view game, std::span<const commands::Row> rows, size_t slot,
        const std::string& key)
    {
        if (slot < rows.size())
            config[commands::Section(game)][KeyLine(rows[slot].action)] = key;
        else
            config["input"][commands::ToyboxLines[slot - rows.size()]] = key;
    }

    commands::Bindings g_published;
    bool g_pending;
}

std::string commands::Section(std::string_view game) { return std::string(game) + ".input"; }

void commands::Seed(LauncherConfig& config, std::string_view game)
{
    const std::string section = Section(game);
    if (config.count(section) != 0 && config[section].count("seeded") != 0)
        return;
    if (auto shared = config.find("input"); shared != config.end())
        for (const KeyAction& entry : g_keyActions)
            if (auto value = shared->second.find(KeyLine(entry.name)); value != shared->second.end())
                config[section].emplace(KeyLine(entry.name), value->second); // a key the game already has stays
    config[section]["seeded"] = "1";
}

std::string commands::Key(const LauncherConfig& config, std::string_view game, const Row& row)
{
    if (const std::string* value = Find(config, Section(game), KeyLine(row.action)))
        return *value;
    return DefaultKey(row.action);
}

int commands::PadTarget(const LauncherConfig& config, std::string_view game, int button)
{
    const std::string* value = Find(config, Section(game), PadLine(button));
    if (value == nullptr)
        return button;
    if (*value == "none")
        return -1;
    int target = PadButtonIndex(*value);
    return target < 0 ? button : target;
}

std::string commands::ToyboxKey(const LauncherConfig& config, int which)
{
    if (const std::string* value = Find(config, "input", ToyboxLines[which]))
        return *value;
    return ToyboxDefaults[which];
}

bool commands::ToyboxTab(const LauncherConfig& config) { return Find(config, "input", ToyboxLines[0]) == nullptr; }

std::string commands::SlotKey(
    const LauncherConfig& config, std::string_view game, std::span<const Row> rows, size_t slot)
{
    return slot < rows.size() ? Key(config, game, rows[slot]) : ToyboxKey(config, int(slot - rows.size()));
}

int commands::SetKey(
    LauncherConfig& config, std::string_view game, std::span<const Row> rows, size_t slot, const std::string& key)
{
    Seed(config, game);
    const std::string old = SlotKey(config, game, rows, slot);
    if (SameKey(old, key))
    {
        ShareKeys(config, game, rows);
        return -1;
    }
    int other = -1;
    for (size_t i = 0; i < rows.size() + ToyboxKeyCount && other < 0; i++)
        if (i != slot && SameKey(SlotKey(config, game, rows, i), key))
            other = int(i);
    StoreSlot(config, game, rows, slot, key);
    if (other >= 0)
        StoreSlot(config, game, rows, size_t(other), old);
    ShareKeys(config, game, rows);
    return other;
}

std::string commands::SlotLabel(std::span<const Row> rows, size_t slot, bool french)
{
    if (slot < rows.size())
    {
        const Row& row = rows[slot];
        const std::string_view action = row.action;
        static constexpr const char* directions[] = { "right_up", "right_down", "right_left", "right_right" };
        static constexpr const char* english[] = { "R stick up", "R stick down", "R stick left", "R stick right" };
        static constexpr const char* translated[] = { "stick droit haut", "stick droit bas", "stick droit gauche",
            "stick droit droite" };
        for (size_t i = 0; i < std::size(directions); i++)
            if (action == directions[i])
                return french ? translated[i] : english[i];
        return french ? row.french : row.label;
    }
    static constexpr const char* english[] = { "Open the Toy Box", "Add a figure", "Clear the figures" };
    static constexpr const char* translated[] = { "Ouvrir la Toy Box", "Ajouter une figurine",
        "Retirer les figurines" };
    slot -= rows.size();
    return slot < ToyboxKeyCount ? (french ? translated[slot] : english[slot]) : "";
}

bool commands::MouseOn(const LauncherConfig& config, std::string_view game)
{
    const std::string* value = Find(config, Section(game), "mouse");
    return value != nullptr && (*value == "1" || *value == "true" || *value == "on" || *value == "yes");
}

void commands::SetMouseOn(LauncherConfig& config, std::string_view game, std::span<const Row> rows, bool on)
{
    Seed(config, game);
    config[Section(game)]["mouse"] = on ? "1" : "0";
    ShareKeys(config, game, rows);
}

mouse::Bindings commands::MouseLines(const LauncherConfig& config, std::string_view game)
{
    mouse::Bindings bindings;
    bindings.enabled = MouseOn(config, game);
    auto section = config.find(Section(game));
    if (section == config.end())
        return bindings;
    for (const auto& [key, value] : section->second)
    {
        mouse::Button button;
        size_t action =
            key.starts_with("mouse_") ? ActionIndex(std::string_view(key).substr(6)) : std::size(g_keyActions);
        if (action < std::size(g_keyActions) && mouse::FromName(value, button))
        {
            bindings.stored[action] = button;
            bindings.set[action] = true;
        }
    }
    return bindings;
}

int commands::SetMouse(LauncherConfig& config, std::string_view game, std::span<const Row> rows,
    std::span<const mouse::Default> defaults, size_t action, mouse::Button button)
{
    Seed(config, game);
    mouse::Button now[mouse::ActionCount];
    mouse::Effective(MouseLines(config, game), defaults, now);
    int other = -1;
    for (size_t i = 0; i < mouse::ActionCount && other < 0; i++)
        if (i != action && button != mouse::None && now[i] == button)
            other = int(i);
    const std::string section = Section(game);
    config[section][std::string("mouse_") + g_keyActions[action].name] =
        button == mouse::None ? "none" : mouse::Name(button);
    if (other >= 0)
        config[section][std::string("mouse_") + g_keyActions[other].name] = "none";
    ShareKeys(config, game, rows);
    return other;
}

int commands::SetPad(LauncherConfig& config, std::string_view game, std::span<const Row> rows, int job, int button)
{
    Seed(config, game);
    const int old = PadTarget(config, game, button);
    int other = -1;
    if (old != job)
    {
        for (int b = 0; b < PadButtonCount && other < 0; b++)
            if (b != button && PadTarget(config, game, b) == job)
                other = b;
        StorePad(config, game, button, job);
        if (other >= 0)
            StorePad(config, game, other, old);
    }
    ShareKeys(config, game, rows);
    return other;
}

void commands::Reset(LauncherConfig& config, std::string_view game, std::span<const Row> rows)
{
    Seed(config, game);
    std::map<std::string, std::string>& section = config[Section(game)];
    for (const KeyAction& entry : g_keyActions)
        section.erase(KeyLine(entry.name));
    for (int button = 0; button < PadButtonCount; button++)
        section.erase(PadLine(button));
    for (const KeyAction& entry : g_keyActions)
        section.erase(std::string("mouse_") + entry.name);
    for (const char* line : ToyboxLines)
        config["input"].erase(line);
    ShareKeys(config, game, rows);
}

commands::Bindings commands::Read(const LauncherConfig& config, std::string_view game)
{
    Bindings bindings;
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        bindings.keys[i] = g_keyActions[i].defaultKey;
    for (const std::string& name : { std::string("input"), Section(game) })
    {
        auto section = config.find(name);
        if (section == config.end())
            continue;
        for (const auto& [key, value] : section->second)
        {
            if (key.starts_with("key_") && SDL_GetScancodeFromName(value.c_str()) != SDL_SCANCODE_UNKNOWN)
            {
                for (size_t i = 0; i < std::size(g_keyActions); i++)
                    if (key.compare(4, std::string::npos, g_keyActions[i].name) == 0)
                        bindings.keys[i] = value;
            }
            else if (key.starts_with("pad_"))
            {
                const int from = PadButtonIndex(std::string_view(key).substr(4)), to = PadButtonIndex(value);
                if (from >= 0 && (to >= 0 || value == "none"))
                    bindings.padMap.target[from] = int8_t(to);
            }
        }
    }
    bindings.mouse = MouseLines(config, game);
    for (int i = 0; i < ToyboxKeyCount; i++)
        if (const std::string* value = Find(config, "input", ToyboxLines[i]);
            value != nullptr && SDL_GetScancodeFromName(value->c_str()) != SDL_SCANCODE_UNKNOWN)
        {
            bindings.toyboxKeys[i] = *value;
            bindings.toyboxTab = bindings.toyboxTab && i != 0;
        }
    return bindings;
}

void commands::Publish(const Bindings& bindings)
{
    g_published = bindings;
    g_pending = true;
}

bool commands::Take(RuntimeSettings& settings)
{
    if (!g_pending)
        return false;
    g_pending = false;
    std::string changed;
    for (size_t i = 0; i < std::size(g_keyActions); i++)
    {
        if (settings.keys[i] != g_published.keys[i])
            changed +=
                std::string(changed.empty() ? "" : ", ") + "key_" + g_keyActions[i].name + "=" + g_published.keys[i];
        settings.keys[i] = g_published.keys[i];
    }
    for (int i = 0; i < PadButtonCount; i++)
    {
        int8_t target = g_published.padMap.target[i];
        if (settings.padMap.target[i] != target)
            changed += std::string(changed.empty() ? "" : ", ") + "pad_" + g_padButtons[i] + "=" +
                       (target < 0 ? "none" : g_padButtons[target]);
    }
    settings.padMap = g_published.padMap;
    if (settings.mouse.enabled != g_published.mouse.enabled)
        changed += std::string(changed.empty() ? "" : ", ") + "mouse=" + (g_published.mouse.enabled ? "1" : "0");
    for (size_t i = 0; i < mouse::ActionCount; i++)
        if (g_published.mouse.set[i] &&
            (!settings.mouse.set[i] || settings.mouse.stored[i] != g_published.mouse.stored[i]))
            changed += std::string(changed.empty() ? "" : ", ") + "mouse_" + g_keyActions[i].name + "=" +
                       (g_published.mouse.stored[i] == mouse::None ? "none" : mouse::Name(g_published.mouse.stored[i]));
    settings.mouse = g_published.mouse;
    for (int i = 0; i < ToyboxKeyCount; i++)
    {
        if (settings.toyboxKeys[i] != g_published.toyboxKeys[i])
            changed += std::string(changed.empty() ? "" : ", ") + ToyboxLines[i] + "=" + g_published.toyboxKeys[i];
        settings.toyboxKeys[i] = g_published.toyboxKeys[i];
    }
    settings.toyboxTab = g_published.toyboxTab;
    fprintf(stderr, "[input] bindings in use now: %s\n", changed.empty() ? "unchanged" : changed.c_str());
    return true;
}
