#pragma once
#include <cstdlib>
#include <string>

// Environment variables of the games, the builder and the launcher. Ask for the current name; the older names are read
// when it is not set, so every variable of earlier versions keeps working (test runs and scripts included):
//   "SKY_X"          then "SSA_X"                  (games, runtime, SkyBuild)
//   "SKY_LAUNCHER_X" then "SKYLAUNCHER_X", then "SSA_LAUNCHER_X"   (launcher; the tail of the chain is the older spellings)
// A variable that is set, even to an empty text, wins over the older names. Names that never changed (SKYLANDERS_DATA,
// SDL_*, GIANTS_ISO, ...) are read with plain getenv. The same file is in the launcher's src/shared/ (tools/sync_skylauncher.sh).
namespace skyenv
{
    // The older spellings of a current name, in reading order (at most two; "" for none).
    inline void OlderNames(const std::string& name, std::string& first, std::string& second)
    {
        first.clear();
        second.clear();
        if (name.rfind("SKY_LAUNCHER_", 0) == 0)
        {
            first = "SKYLAUNCHER_" + name.substr(13);
            second = "SSA_LAUNCHER_" + name.substr(13);
        }
        else if (name.rfind("SKY_", 0) == 0)
            first = "SSA_" + name.substr(4);
    }

    // The value through any lookup (getenv, a test's table, ...): it takes a name and returns its text or nullptr.
    template <typename Lookup>
    inline const char* Get(const char* name, const Lookup& lookup)
    {
        if (const char* value = lookup(name))
            return value;
        std::string first, second;
        OlderNames(name, first, second);
        if (!first.empty())
            if (const char* value = lookup(first.c_str()))
                return value;
        if (!second.empty())
            if (const char* value = lookup(second.c_str()))
                return value;
        return nullptr;
    }

    inline const char* Get(const char* name)
    {
        return Get(name, [](const char* variable) { return std::getenv(variable); });
    }
}
