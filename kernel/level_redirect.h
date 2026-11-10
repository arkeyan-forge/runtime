#pragma once

#include <string>
#include <string_view>

// SKY_LEVEL's file redirect (kernel/files.cpp RedirectLevel; the games whose levels are files: SSA, Giants, Trap Team).
// `first` is the disc path of the redirected level's files up to the end of its name (game::Adapter::firstLevel, or
// that path with the name SKY_LEVEL_FROM gives: a Battle Mode arena the menus start), `follows` the characters that may
// follow the name ('\0': the path ends there). Every place of `path` where `first` appears, followed by one of them, has
// the name replaced by `level`.
namespace levelredirect
{
    // `first` with its level name (after the last backslash) replaced by `from`; `first` itself when either is empty
    // (a game with no file redirect keeps none).
    inline std::string FirstWithName(std::string_view first, std::string_view from)
    {
        if (from.empty() || first.empty())
            return std::string(first);
        return std::string(first.substr(0, first.rfind('\\') + 1)) + std::string(from);
    }

    inline std::string Apply(std::string path, std::string_view first, std::string_view follows, std::string_view level)
    {
        if (level.empty() || first.empty())
            return path;
        const size_t nameAt = first.rfind('\\') + 1;
        for (size_t at = path.find(first); at != std::string::npos; at = path.find(first, at + 1))
        {
            const char next = at + first.size() < path.size() ? path[at + first.size()] : '\0';
            if (std::string_view(follows.data(), follows.size()).find(next) != std::string_view::npos)
                path.replace(at + nameAt, first.size() - nameAt, level);
        }
        return path;
    }
}
