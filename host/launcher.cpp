#include "launcher.h"
#include "launcher_ui.h"
#include "vfs.h"
#include "collection/datadir.h"
#include "collection/game_data.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <optional>

#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace
{
#ifdef __APPLE__
    constexpr char SelectAgainHint[] = "hold Shift (or Option) while the app starts, or start it with --launcher";
    constexpr char PermissionHint[] = "\n\nIf the file is there, allow access in System Settings > Privacy & Security > Files and Folders.";
#else
    constexpr char SelectAgainHint[] = "hold Shift while the app starts, or start it with --launcher";
    constexpr char PermissionHint[] = "";
#endif

    uint32_t ReadBE32(const uint8_t* p)
    {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }

    std::string FormatVersion(uint32_t version)
    {
        return fmt::format("{}.{}.{}.{}", version >> 28, (version >> 24) & 0xF, (version >> 8) & 0xFFFF, version & 0xFF);
    }

    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            text.remove_suffix(1);
        return text;
    }

    bool SelectionKeyHeld()
    {
#ifdef __APPLE__
        // Asked from the window server: SDL only learns modifier state from window events.
        CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
        return (flags & (kCGEventFlagMaskAlternate | kCGEventFlagMaskShift)) != 0;
#elif defined(_WIN32)
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
        return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
    }

    void ShowError(const GameInfo& game, const std::string& message, bool interactive)
    {
        fprintf(stderr, "%s\n", message.c_str());
        if (interactive)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, game.name, message.c_str(), nullptr);
    }

}

std::string Sha256Hex(std::span<const uint8_t> data)
{
    static constexpr uint32_t K[64] =
    {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };
    uint32_t hash[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    auto rotate = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    auto processBlock = [&](const uint8_t* block)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ReadBE32(block + i * 4);
        for (int i = 16; i < 64; i++)
        {
            uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4], f = hash[5], g = hash[6], h = hash[7];
        for (int i = 0; i < 64; i++)
        {
            uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d;
        hash[4] += e; hash[5] += f; hash[6] += g; hash[7] += h;
    };

    size_t whole = data.size() / 64 * 64;
    for (size_t offset = 0; offset < whole; offset += 64)
        processBlock(&data[offset]);

    // Padding: 0x80, zeros, then the message length in bits (big-endian), filling one or two blocks.
    uint8_t tail[128] = {};
    size_t rest = data.size() - whole;
    if (rest != 0)
        memcpy(tail, &data[whole], rest);
    tail[rest] = 0x80;
    size_t tailSize = rest < 56 ? 64 : 128;
    uint64_t bits = uint64_t(data.size()) * 8;
    for (int i = 0; i < 8; i++)
        tail[tailSize - 1 - i] = uint8_t(bits >> (i * 8));
    for (size_t offset = 0; offset < tailSize; offset += 64)
        processBlock(tail + offset);

    std::string hex;
    for (uint32_t word : hash)
        hex += fmt::format("{:08x}", word);
    return hex;
}

GameSourceCheck CheckGameSource(const std::string& path, const GameInfo& game, const GameSourceProgress& progress)
{
    using enum GameSourceStatus;
    auto report = [&](float fraction, std::string_view step) { if (progress) progress(fraction, step); };

    report(0.03f, "Looking for the disc image");
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path.c_str(), &info))
        return { NotFound, fmt::format("\"{}\" doesn't exist. Was it moved, renamed, or is its drive disconnected?", path) };
    // A folder is only the copy of the game data (game_data.h); the disc reader reads it like an image.
    const bool copy = info.type == SDL_PATHTYPE_DIRECTORY;
    if (copy && !std::filesystem::exists(datadir::FromText(path) / gamedata::ManifestName))
        return { Folder, fmt::format("\"{}\" is a folder. Extracted game folders aren't supported yet: choose the disc image (.iso).", path) };

    report(0.10f, "Opening the disc image");
    if (!copy)
    {
        SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "rb");
        if (file == nullptr)
            return { Unreadable, fmt::format("\"{}\" can't be read ({}).{}", path, SDL_GetError(), PermissionHint) };
        SDL_CloseIO(file);
    }

    report(0.25f, "Reading the Xbox 360 disc layout");
    DiscFileSystem disc;
    if (!disc.Open(path.c_str()))
        return { NotXboxDisc, fmt::format("\"{}\" isn't an Xbox 360 disc image, or the file is incomplete.", path) };

    report(0.42f, "Finding default.xex");
    const DiscFileSystem::Entry* entry = disc.Find("default.xex");
    if (entry == nullptr || entry->directory)
        return { NotXbox360Game, "This disc image has no default.xex: it isn't an Xbox 360 game, or the image is incomplete." };

    report(0.55f, "Reading default.xex");
    std::vector<uint8_t> xex = disc.ReadAll("default.xex");
    if (xex.size() != entry->size)
        return { Damaged, "This disc image is incomplete: default.xex is cut short. Copy or dump the disc again." };

    report(0.70f, "Checking the Xbox 360 executable");
    auto execution = ReadExecutionInfo(xex);
    if (!execution)
        return { Damaged, "This disc image is damaged: its default.xex isn't a valid Xbox 360 executable." };

    const GameInfo* owner = nullptr;
    for (const GameInfo& candidate : g_games)
    {
        for (const GameRelease& release : candidate.releases)
        {
            if (release.titleId == execution->titleId)
                owner = &candidate;
        }
    }
    if (owner != &game)
    {
        std::string found = owner != nullptr ? owner->name : fmt::format("another game (title ID {:08X})", execution->titleId);
        return { WrongGame, fmt::format("This disc image is {}, not {}.", found, game.name) };
    }

    report(0.84f, "Fingerprinting default.xex (SHA-256)");
    std::string sha256 = Sha256Hex(xex);
    report(0.97f, "Comparing with the supported releases");
    for (const GameRelease& release : game.releases)
    {
        if (sha256 == release.defaultXexSha256)
        {
            report(1.0f, "Verified");
            return { Ok, {} };
        }
    }
    for (const GameRelease& release : game.releases)
    {
        if (release.mediaId == execution->mediaId && release.version == execution->version)
        {
            return { Damaged, fmt::format("This looks like the {} disc of {}, but its default.xex isn't the original one: "
                "the image is damaged or modified. Copy or dump the disc again.", release.label, game.name) };
        }
    }

    std::string supported;
    for (const GameRelease& release : game.releases)
        supported += fmt::format("\n- {} (media ID {:08X}, version {})", release.label, release.mediaId, FormatVersion(release.version));
    return { WrongVersion, fmt::format("This is {}, but a disc version this build doesn't support (media ID {:08X}, version {}). "
        "The game code is recompiled from one exact default.xex, so other regions and revisions can't run yet.\n\nSupported:{}",
        game.name, execution->mediaId, FormatVersion(execution->version), supported) };
}

std::string LauncherConfigPath()
{
    // SSA_SETTINGS=<file> (or SSA_LAUNCHER_INI): settings from that file instead (a test run's own; a missing or empty
    // value means none), so test runs do not read the player's launcher.ini.
    for (const char* name : { "SSA_SETTINGS", "SSA_LAUNCHER_INI" })
        if (const char* file = getenv(name))
            return file;
    return datadir::Text(datadir::LauncherIni());
}

LauncherConfig LoadLauncherConfig(const std::string& path)
{
    LauncherConfig config;
    size_t size = 0;
    char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
    if (data == nullptr)
        return config;

    std::string_view text(data, size);
    std::string section;
    while (!text.empty())
    {
        size_t end = text.find('\n');
        std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);

        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t equals = line.find('=');
        if (equals != std::string_view::npos)
            config[section][std::string(Trim(line.substr(0, equals)))] = Trim(line.substr(equals + 1));
    }
    SDL_free(data);
    return config;
}

bool SaveLauncherConfig(const std::string& path, const LauncherConfig& config)
{
    std::string text = "# Launcher settings: one [section] per game.\n";
    for (const auto& [section, values] : config)
    {
        if (values.empty())
            continue;
        if (!section.empty())
            text += "\n[" + section + "]\n";
        for (const auto& [key, value] : values)
            text += key + "=" + value + "\n";
    }
    return SDL_SaveFile(path.c_str(), text.data(), text.size());
}

LanguageChoice ResolveLanguages(const GameInfo& game)
{
    LauncherConfig config = LoadLauncherConfig(LauncherConfigPath());
    auto pick = [&](const char* variable, const char* key)
    {
        const char* value = getenv(variable);
        return value != nullptr ? std::string(value) : config[game.id][key];
    };
    return { pick("SSA_LANGUAGE", "language"), pick("SSA_VOICE_LANGUAGE", "voice_language") };
}

// The folder of the copied game data when it is complete, intact and of a disc this game knows; else empty (a damaged copy is
// said in the log: the disc image is read instead).
static std::string CopiedGameData(const GameInfo& game)
{
    std::filesystem::path folder = gamedata::Folder(game.id);
    gamedata::Status status = gamedata::Inspect(folder);
    if (status.state == gamedata::State::Damaged)
        fprintf(stderr, "game data: the copy in %s is damaged (%s): reading the disc image instead\n", datadir::Text(folder).c_str(), status.problem.c_str());
    if (status.state != gamedata::State::Complete)
        return {};
    std::string path = datadir::Text(folder);
    if (CheckGameSource(path, game).status != GameSourceStatus::Ok)
    {
        fprintf(stderr, "game data: the copy in %s is not this game's disc: reading the disc image instead\n", path.c_str());
        return {};
    }
    return path;
}

std::string ResolveGameSource(int argc, char** argv, const GameInfo& game, bool interactive)
{
    std::string path;
    bool selectAgain = false;
    bool launcherRequested = false;
    for (int i = 1; i < argc; i++)
    {
        std::string_view argument = argv[i];
        if (argument == "--select-iso")
            selectAgain = true;
        else if (argument == "--launcher")
            launcherRequested = true;
        else if (path.empty())
            path = argument;
    }

    // The copy of the game data (Skylauncher's "Copy the game data") is read in preference to the disc image, and makes it
    // optional. --launcher and --select-iso still open the launcher window to choose one.
    if (!launcherRequested && !selectAgain)
    {
        std::string copied = CopiedGameData(game);
        if (!copied.empty())
        {
            fprintf(stderr, "game data: reading the copy in %s\n", copied.c_str());
            return copied;
        }
    }

    if (!interactive)
    {
        if (path.empty())
        {
            fprintf(stderr, "usage: %s [--launcher] [--select-iso] [<disc image .iso>]\n", game.runtime);
            return {};
        }
        GameSourceCheck check = CheckGameSource(path, game);
        if (check.status != GameSourceStatus::Ok)
        {
            ShowError(game, check.message, false);
            return {};
        }
        return path;
    }

    SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (!path.empty())
    {
        GameSourceCheck check = CheckGameSource(path, game);
        if (check.status == GameSourceStatus::Ok)
            return path;
        ShowError(game, check.message, true);
        return {};
    }

    std::string configPath = LauncherConfigPath();
    LauncherConfig config = LoadLauncherConfig(configPath);
    std::string saved = config[game.id]["iso"];
    if (!launcherRequested && !selectAgain && !SelectionKeyHeld() && !saved.empty())
    {
        GameSourceCheck check = CheckGameSource(saved, game);
        if (check.status == GameSourceStatus::Ok)
            return saved;
        // The remembered image moved or changed: open the launcher so the player can see why and pick another.
    }

    std::string chosen = RunLauncherWindow(game, configPath, config, saved);
    if (!chosen.empty())
    {
        config[game.id]["iso"] = chosen;
        if (!SaveLauncherConfig(configPath, config))
            fprintf(stderr, "launcher: couldn't save %s (%s); the choice will be asked again next time\n",
                configPath.c_str(), SDL_GetError());
    }
    return chosen;
}
