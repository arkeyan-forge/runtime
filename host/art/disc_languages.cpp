#include "disc_languages.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace art
{
    namespace
    {
        // Every settings code, in the order the lists keep.
        constexpr const char* Order[] = { "en", "fr", "de", "es", "es-419", "it", "nl", "pt", "sv", "no", "da", "fi" };

        // SSA's folder names (the disc's own) and the settings code of each.
        struct Folder { const char* name; const char* code; };
        constexpr Folder LuxFolders[] = { { "eng", "en" }, { "fre", "fr" }, { "ger", "de" }, { "spa", "es" }, { "ita", "it" },
            { "dut", "nl" }, { "swe", "sv" }, { "nor", "no" }, { "dan", "da" }, { "fin", "fi" } };

        std::string Normal(std::string path)
        {
            for (char& c : path)
                c = c == '\\' ? '/' : char(std::tolower((unsigned char)c));
            while (!path.empty() && path.front() == '/')
                path.erase(path.begin());
            return path;
        }

        std::vector<std::string> InOrder(const std::set<std::string>& codes)
        {
            std::vector<std::string> out;
            for (const char* code : Order)
                if (codes.count(code))
                    out.push_back(code);
            return out;
        }

        bool StartsWith(const std::string& text, std::string_view prefix) { return text.compare(0, prefix.size(), prefix) == 0; }
    }

    DiscLanguages LanguagesOnDisc(std::string_view game, const std::vector<std::string>& files)
    {
        DiscLanguages result;
        if (game == "giants")
        {
            result.text.assign(std::begin(Order), std::end(Order));
            result.voice = result.text;
            return result;
        }
        std::set<std::string> text, voice;
        for (const std::string& file : files)
        {
            std::string path = Normal(file);
            if (game == "ssa")
            {
                for (const Folder& folder : LuxFolders)
                {
                    std::string name = folder.name;
                    if (StartsWith(path, "text/" + name + "/"))
                        text.insert(folder.code);
                    else if (path == "audio/" + name + "/voice.bnk")
                        voice.insert(folder.code);
                }
            }
            else if (game == "swapforce")
            {
                // archives/<name>_<code>.pak
                if (!StartsWith(path, "archives/") || path.size() < 4 || path.compare(path.size() - 4, 4, ".pak") != 0)
                    continue;
                size_t underscore = path.rfind('_');
                if (underscore == std::string::npos || path.size() - 4 - underscore - 1 != 2)
                    continue;
                std::string code = path.substr(underscore + 1, 2);
                if (std::find_if(std::begin(Order), std::end(Order), [&](const char* known) { return code == known; }) != std::end(Order))
                    text.insert(code), voice.insert(code);
            }
        }
        result.text = InOrder(text);
        result.voice = InOrder(voice);
        return result;
    }

    std::string JoinLanguages(const std::vector<std::string>& codes)
    {
        std::string out;
        for (const std::string& code : codes)
            out += (out.empty() ? "" : ",") + code;
        return out;
    }

    std::vector<std::string> SplitLanguages(std::string_view text)
    {
        std::vector<std::string> out;
        while (!text.empty())
        {
            size_t comma = text.find(',');
            std::string_view part = text.substr(0, comma);
            if (!part.empty())
                out.emplace_back(part);
            text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
        }
        return out;
    }
}
