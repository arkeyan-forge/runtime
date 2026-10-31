#include "disc_languages.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace art
{
    namespace
    {
        // Every settings code, in the order the lists keep: the twelve of Giants' disc, then the other languages of the
        // engines' lists (Swap Force's and Trap Team's IG_CORE_LANGUAGE: Polish, Russian, Czech, Hungarian, Turkish,
        // Greek, Japanese, Korean, Chinese).
        constexpr const char* Order[] = { "en", "fr", "de", "es", "es-419", "it", "nl", "pt", "sv", "no", "da", "fi",
            "pl", "ru", "cs", "hu", "tr", "el", "ja", "ko", "zh" };

        // SSA's folder names (the disc's own) and the settings code of each.
        struct Folder
        {
            const char* name;
            const char* code;
        };
        constexpr Folder LuxFolders[] = { { "eng", "en" }, { "fre", "fr" }, { "ger", "de" }, { "spa", "es" },
            { "ita", "it" }, { "dut", "nl" }, { "swe", "sv" }, { "nor", "no" }, { "dan", "da" }, { "fin", "fi" } };

        // Trap Team's names of its languages, in lower case: the packs' "<NAME>.pak" and the voice folders (the title's
        // IG_CORE_LANGUAGE_<NAME> enum).
        constexpr Folder TrapTeamNames[] = { { "english", "en" }, { "french", "fr" }, { "german", "de" },
            { "spanish", "es" }, { "spanish_mexican", "es-419" }, { "italian", "it" }, { "dutch", "nl" },
            { "portuguese", "pt" }, { "swedish", "sv" }, { "norwegian", "no" }, { "danish", "da" }, { "finnish", "fi" },
            { "polish", "pl" }, { "russian", "ru" }, { "czech", "cs" }, { "hungarian", "hu" }, { "turkish", "tr" },
            { "greek", "el" }, { "japanese", "ja" }, { "korean", "ko" }, { "chinese", "zh" } };

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

        bool StartsWith(const std::string& text, std::string_view prefix)
        {
            return text.compare(0, prefix.size(), prefix) == 0;
        }

        bool EndsWith(const std::string& text, std::string_view suffix)
        {
            return text.size() >= suffix.size() &&
                   text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        const char* TrapTeamCode(std::string_view name)
        {
            for (const Folder& known : TrapTeamNames)
                if (name == known.name)
                    return known.code;
            return nullptr;
        }

        // The text languages of Trap Team's packs: the archive's entries "<LANGUAGE>.pak".
        std::set<std::string> TrapTeamText(const std::vector<std::string>& entries)
        {
            std::set<std::string> codes;
            for (const std::string& entry : entries)
            {
                std::string name = Normal(entry);
                if (!EndsWith(name, ".pak"))
                    continue;
                if (const char* code = TrapTeamCode(std::string_view(name).substr(0, name.size() - 4)))
                    codes.insert(code);
            }
            return codes;
        }

        // The voice lines of a character archive, entries named "<path>/<language folder>/<line>.wav/<hash>.wav...".
        // `localized` gets the codes of the language folders; the result says whether the archive has a voice line at
        // all (a ".wav" component starting with "vo_", in a language folder or not).
        bool TrapTeamVoices(const std::vector<std::string>& entries, std::set<std::string>& localized)
        {
            bool any = false;
            for (const std::string& entry : entries)
            {
                std::vector<std::string> parts;
                std::string name = Normal(entry);
                for (size_t from = 0;;)
                {
                    size_t slash = name.find('/', from);
                    parts.push_back(name.substr(from, slash == std::string::npos ? slash : slash - from));
                    if (slash == std::string::npos)
                        break;
                    from = slash + 1;
                }
                for (size_t i = 1; i + 1 < parts.size(); i++)
                {
                    if (!EndsWith(parts[i], ".wav"))
                        continue;
                    if (const char* code = TrapTeamCode(parts[i - 1]))
                        localized.insert(code), any = true;
                    else if (StartsWith(parts[i], "vo_"))
                        any = true;
                    break;
                }
            }
            return any;
        }

        // Trap Team: see the header. At most this many character archives are read for the voices.
        constexpr size_t VoiceArchives = 6;

        DiscLanguages TrapTeam(const std::vector<std::string>& files, const ArchiveNames& archive)
        {
            DiscLanguages result;
            if (!archive)
                return result;
            std::set<std::string> text;
            for (const char* pack : { "character/init_setup.bld", "permanent/global.bld" })
                if (text.empty())
                    text = TrapTeamText(archive(pack));
            if (text.empty())
                return result;

            std::vector<std::string> arcs;
            for (const std::string& file : files)
            {
                std::string path = Normal(file);
                if (StartsWith(path, "character/") && EndsWith(path, ".arc"))
                    arcs.push_back(path);
            }
            std::sort(arcs.begin(), arcs.end());
            std::set<std::string> voice;
            bool lines = false;
            for (size_t i = 0; i < arcs.size() && i < VoiceArchives; i++)
            {
                lines |= TrapTeamVoices(archive(arcs[i]), voice);
                if (!voice.empty())
                    break;
            }
            // English has no folder: its lines are the ones outside every language folder, so it is there when the text
            // has it (a Dutch and French disc keeps a few unmarked lines, sounds that need no translation, not English).
            if (lines && text.count("en"))
                voice.insert("en");
            result.text = InOrder(text);
            result.voice = InOrder(voice);
            return result;
        }
    }

    DiscLanguages LanguagesOnDisc(
        std::string_view game, const std::vector<std::string>& files, const ArchiveNames& archive)
    {
        DiscLanguages result;
        if (game == "giants")
        {
            result.text.assign(std::begin(Order), std::begin(Order) + 12);
            result.voice = result.text;
            return result;
        }
        if (game == "trapteam")
            return TrapTeam(files, archive);
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
                if (!StartsWith(path, "archives/") || !EndsWith(path, ".pak"))
                    continue;
                size_t underscore = path.rfind('_');
                if (underscore == std::string::npos)
                    continue;
                std::string code = path.substr(underscore + 1, path.size() - 4 - underscore - 1);
                if (std::find_if(std::begin(Order), std::end(Order),
                        [&](const char* known) { return code == known; }) != std::end(Order))
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
