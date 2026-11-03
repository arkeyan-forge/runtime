#include "host/report.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    bool WordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

    bool MatchesAt(const std::string& text, size_t at, const std::string& needle)
    {
        if (at + needle.size() > text.size())
            return false;
        for (size_t i = 0; i < needle.size(); i++)
            if (std::tolower(static_cast<unsigned char>(text[at + i])) !=
                std::tolower(static_cast<unsigned char>(needle[i])))
                return false;
        return true;
    }

    struct Rule
    {
        std::string needle, replacement;
        bool word; // a name: whole words only
    };

    void AddRule(std::vector<Rule>& rules, const std::string& needle, const std::string& replacement, bool word)
    {
        if (needle.empty())
            return;
        for (const Rule& rule : rules)
            if (rule.needle == needle)
                return;
        rules.push_back({ needle, replacement, word });
    }

    // A path with its two separator spellings, without a trailing separator.
    void AddPath(std::vector<Rule>& rules, std::string path, const std::string& replacement)
    {
        while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
            path.pop_back();
        if (path.size() < 2)
            return;
        std::string forward = path, back = path;
        std::replace(forward.begin(), forward.end(), '\\', '/');
        std::replace(back.begin(), back.end(), '/', '\\');
        for (const std::string& spelling : { path, forward, back })
            AddRule(rules, spelling, replacement, false);
    }

    std::string ReadTail(const fs::path& file, size_t maxBytes, bool& cut)
    {
        cut = false;
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        if (!stream)
            return {};
        std::streamoff size = stream.tellg();
        if (size < 0)
            return {};
        std::streamoff start = size > std::streamoff(maxBytes) ? size - std::streamoff(maxBytes) : 0;
        cut = start > 0;
        stream.seekg(start);
        std::string text(size_t(size - start), '\0');
        stream.read(text.data(), std::streamsize(text.size()));
        text.resize(size_t(stream.gcount()));
        return text;
    }

    std::string ReadWhole(const fs::path& file)
    {
        bool cut;
        return ReadTail(file, SIZE_MAX / 2, cut);
    }

    // The first `most` lines starting with `prefix`.
    std::vector<std::string> FindLines(const std::string& log, const char* prefix, size_t most)
    {
        std::vector<std::string> lines;
        size_t at = 0, length = std::strlen(prefix);
        while (at < log.size() && lines.size() < most)
        {
            size_t end = log.find('\n', at);
            if (end == std::string::npos)
                end = log.size();
            if (log.compare(at, length, prefix) == 0)
            {
                std::string line = log.substr(at, end - at);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
            at = end + 1;
        }
        return lines;
    }

    std::string Stamp(const char* format)
    {
        std::time_t now = std::time(nullptr);
        std::tm parts{};
#ifdef _WIN32
        localtime_s(&parts, &now);
#else
        localtime_r(&now, &parts);
#endif
        char text[32] = {};
        std::strftime(text, sizeof(text), format, &parts);
        return text;
    }

    void Section(std::string& out, const std::string& title, const std::string& text, bool cut = false)
    {
        out += "===== " + title + " =====\n";
        if (cut)
            out += "(the start of the file is left out; the version, system and GPU lines are quoted at the top)\n";
        out += text.empty() ? "(empty or not found)\n" : text;
        if (out.back() != '\n')
            out += '\n';
        out += '\n';
    }
}

std::string report::Redact(const std::string& text, const Identity& who)
{
    std::vector<Rule> rules;
    AddPath(rules, who.home, "~");
    if (!who.user.empty())
    {
        AddPath(rules, "/Users/" + who.user, "~");
        AddPath(rules, "/home/" + who.user, "~");
        AddPath(rules, "C:\\Users\\" + who.user, "~");
    }
    if (!who.computer.empty())
    {
        AddRule(rules, who.computer, "<computer>", true);
        if (size_t dot = who.computer.find('.'); dot != std::string::npos && dot > 0)
            AddRule(rules, who.computer.substr(0, dot), "<computer>", true);
    }
    AddRule(rules, who.user, "<user>", true);
    // The longest first: a whole path before the name it ends with.
    std::stable_sort(
        rules.begin(), rules.end(), [](const Rule& a, const Rule& b) { return a.needle.size() > b.needle.size(); });

    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size();)
    {
        const Rule* match = nullptr;
        for (const Rule& rule : rules)
        {
            if (!MatchesAt(text, at, rule.needle))
                continue;
            if (rule.word && ((at > 0 && WordChar(text[at - 1])) ||
                                 (at + rule.needle.size() < text.size() && WordChar(text[at + rule.needle.size()]))))
                continue;
            match = &rule;
            break;
        }
        if (match != nullptr)
        {
            out += match->replacement;
            at += match->needle.size();
        }
        else
            out += text[at++];
    }
    return out;
}

report::Identity report::CurrentIdentity()
{
    Identity who;
#ifdef _WIN32
    if (const char* profile = std::getenv("USERPROFILE"))
        who.home = profile;
    char name[256] = {};
    DWORD size = DWORD(sizeof(name));
    if (GetUserNameA(name, &size))
        who.user = name;
    size = DWORD(sizeof(name));
    if (GetComputerNameA(name, &size))
        who.computer = name;
#else
    if (const passwd* entry = getpwuid(getuid()))
    {
        who.user = entry->pw_name != nullptr ? entry->pw_name : "";
        who.home = entry->pw_dir != nullptr ? entry->pw_dir : "";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && who.home.empty())
        who.home = home;
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) == 0)
        who.computer = name;
#endif
    return who;
}

std::string report::CrashDetails(const std::string& log)
{
    size_t at = std::string::npos;
    for (size_t found = log.find("Fault ("); found != std::string::npos; found = log.find("Fault (", found + 1))
        at = found;
    return at == std::string::npos ? std::string() : log.substr(at);
}

std::string report::Build(const Sources& sources, const Identity& who, size_t maxLogBytes)
{
    bool cut = false, cutPrevious = false;
    std::string log = ReadTail(sources.log, maxLogBytes, cut);
    std::string previous = ReadTail(sources.previousLog, maxLogBytes, cutPrevious);
    std::string launcherLog = ReadTail(sources.launcherLog, maxLogBytes, cutPrevious);
    // The fault block of this run's log, else of the run before (a crash ends a run; the next start moves its log aside).
    std::string crash = CrashDetails(ReadWhole(sources.log));
    std::string crashFrom = "this run";
    if (crash.empty())
    {
        crash = CrashDetails(ReadWhole(sources.previousLog));
        crashFrom = "the previous run";
    }
    // The header lines are the log's first ones: read them from the whole file, not the cut tail.
    std::string head = ReadTail(sources.log, 16 * 1024, cutPrevious);
    if (cut || head.empty())
        head = log;
    std::string first = head.substr(0, 16 * 1024);

    std::string out = "Game problem report\n";
    out += "Generated: " + Stamp("%Y-%m-%d %H:%M:%S") + "\n";
    out += "Game: " + sources.game + " " + sources.gameVersion + "\n";
    // The log's own header: version and build, OS, CPU, memory, the GPU adapter and backend, the settings.
    for (const char* prefix : { "[version] ", "[system] ", "[gpu] ", "[settings] " })
        for (const std::string& line : FindLines(first, prefix, 6))
            out += line + "\n";
    out += crash.empty() ? "Last crash: none in the logs\n" : "Last crash: yes (" + crashFrom + "), see below\n";
    out += "\n";

    Section(out, crash.empty() ? "last crash" : "last crash (" + crashFrom + ")",
        crash.empty() ? "(no crash in the logs)" : crash);
    Section(out, "launcher.ini", ReadWhole(sources.settingsFile));
    Section(out, "Arkeyan.log", log, cut);
    Section(out, "Arkeyan.prev.log", previous, cutPrevious);
    Section(out, "skylauncher-game.log", launcherLog);

    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return Redact(out, who) + "\n";
}

std::string report::FileName(const std::string& gameId)
{
    return gameId + "-report-" + Stamp("%Y%m%d-%H%M%S") + ".txt";
}
