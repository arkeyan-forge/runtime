#include "game.h"
#include "host/toybox.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
    std::mutex g_levelMutex;
    std::string g_level; // already retained for LevelStarted's duplicate suppression
}

std::string game::MovieName(std::string_view path)
{
    size_t start = path.find_last_of("\\/");
    std::string name(start == std::string_view::npos ? path : path.substr(start + 1));
    size_t dot = name.rfind('.');
    if (dot != std::string::npos)
        name.resize(dot);
    for (char& c : name)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return name;
}

std::string game::LevelArcOfFile(std::string_view path)
{
    constexpr std::string_view marker = "\\level\\", extension = ".arc";
    std::string lower(path);
    for (char& c : lower)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    if (!lower.starts_with(marker) || !lower.ends_with(extension) || lower.size() <= marker.size() + extension.size())
        return {};
    std::string name = lower.substr(marker.size(), lower.size() - marker.size() - extension.size());
    return name.find('\\') == std::string::npos ? name : std::string();
}

bool game::IsIntroMovie(std::string_view path)
{
    const auto& intro = Current().introMovies;
    return std::find(intro.begin(), intro.end(), MovieName(path)) != intro.end();
}

namespace
{
    std::atomic<bool> g_inMenuLevel{ true }; // no level yet: the start-up logos and the title
}

bool game::InMenuLevel() { return g_inMenuLevel; }

void game::LevelStarted(std::string_view name)
{
    std::lock_guard lock(g_levelMutex);
    if (g_level == name)
        return;
    g_level = name;
    const auto& menus = Current().menuLevels;
    g_inMenuLevel = std::find(menus.begin(), menus.end(), name) != menus.end();
    fprintf(stderr, "[level] %.*s starts\n", int(name.size()), name.data());
    ToyboxLevelStarted();
}

void game::NoteFileOpen(std::string_view path)
{
    if (Current().LevelOfFile == nullptr)
        return;
    if (std::string level = Current().LevelOfFile(path); !level.empty())
        LevelStarted(level);
}

void game::CopyCurrentLevel(char* text, size_t capacity)
{
    if (capacity == 0)
        return;
    std::lock_guard lock(g_levelMutex);
    size_t size = std::min(capacity - 1, g_level.size());
    memcpy(text, g_level.data(), size);
    text[size] = 0;
}
