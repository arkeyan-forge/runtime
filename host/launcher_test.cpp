// Checks the launcher's disc verification and settings file without showing any UI.
// usage: SSALauncherTest <game disc .iso> <scratch dir> [<damaged copy of the image>...]
#include "launcher.h"
#include "launcher_assets.h"
#include "launcher_ui.h"
#include "vfs.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace
{
    int g_failures = 0;

#define EXPECT(condition) \
    do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); g_failures++; } } while (0)

    const char* StatusName(GameSourceStatus status)
    {
        switch (status)
        {
        case GameSourceStatus::Ok: return "Ok";
        case GameSourceStatus::NotFound: return "NotFound";
        case GameSourceStatus::Unreadable: return "Unreadable";
        case GameSourceStatus::Folder: return "Folder";
        case GameSourceStatus::NotXboxDisc: return "NotXboxDisc";
        case GameSourceStatus::NotXbox360Game: return "NotXbox360Game";
        case GameSourceStatus::Damaged: return "Damaged";
        case GameSourceStatus::WrongGame: return "WrongGame";
        case GameSourceStatus::WrongVersion: return "WrongVersion";
        }
        return "?";
    }

    GameSourceStatus Check(const std::filesystem::path& path)
    {
        GameSourceCheck check = CheckGameSource(path.string(), *FindGame("ssa"));
        printf("  %-14s %s\n", StatusName(check.status), path.filename().string().c_str());
        if (!check.message.empty())
            printf("    -> %s\n", check.message.c_str());
        return check.status;
    }

    void Put32LE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
    void Put32BE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }

    void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
    {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    // Minimal XDVDFS image (plain layout: volume descriptor at sector 32) whose root holds one file.
    void WriteDiscImage(const std::filesystem::path& path, const std::string& name, const std::vector<uint8_t>& file, uint32_t extraDeclaredSize = 0)
    {
        constexpr uint32_t Sector = 2048;
        std::vector<uint8_t> image(34 * Sector + file.size());
        memcpy(&image[32 * Sector], "MICROSOFT*XBOX*MEDIA", 20);
        Put32LE(&image[32 * Sector + 20], 33);
        Put32LE(&image[32 * Sector + 24], Sector);

        uint8_t* entry = &image[33 * Sector]; // no left/right subtrees
        Put32LE(entry + 4, 34);
        Put32LE(entry + 8, uint32_t(file.size()) + extraDeclaredSize);
        entry[12] = 0x80;
        entry[13] = uint8_t(name.size());
        memcpy(entry + 14, name.data(), name.size());

        memcpy(&image[34 * Sector], file.data(), file.size());
        WriteFile(path, image);
    }

    // XEX2 header carrying only the execution info.
    std::vector<uint8_t> FakeXex(uint32_t titleId, uint32_t mediaId, uint32_t version)
    {
        std::vector<uint8_t> xex(0x100);
        Put32BE(&xex[0x00], 0x58455832);
        Put32BE(&xex[0x14], 1);
        Put32BE(&xex[0x18], 0x00040006);
        Put32BE(&xex[0x1C], 0x40);
        Put32BE(&xex[0x40], mediaId);
        Put32BE(&xex[0x44], version);
        Put32BE(&xex[0x4C], titleId);
        return xex;
    }

    std::vector<uint8_t> RandomBytes(size_t size)
    {
        std::mt19937 random(1234);
        std::vector<uint8_t> data(size);
        for (uint8_t& byte : data)
            byte = uint8_t(random());
        return data;
    }

    std::string ResolveHeadless(std::vector<std::string> arguments)
    {
        arguments.insert(arguments.begin(), "SSARecomp");
        std::vector<char*> argv;
        for (std::string& argument : arguments)
            argv.push_back(argument.data());
        return ResolveGameSource(int(argv.size()), argv.data(), *FindGame("ssa"), false);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: SSALauncherTest <game disc .iso> <scratch dir> [<damaged copy of the image>...]\n");
        return 2;
    }
    std::filesystem::path iso = argv[1];
    std::filesystem::path scratch = std::filesystem::path(argv[2]) / "launcher_test";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const GameInfo& game = *FindGame("ssa");
    using Clock = std::chrono::steady_clock;
    auto milliseconds = [](Clock::time_point start)
    {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    };

    // FIPS 180-4 examples, covering one- and two-block padding.
    auto sha256 = [](std::string_view text) { return Sha256Hex({ reinterpret_cast<const uint8_t*>(text.data()), text.size() }); };
    EXPECT(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT(sha256(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    printf("Game table: %zu game(s); \"ssa\" = %s, %zu release(s)\n", g_games.size(), game.name, game.releases.size());
    EXPECT(FindGame("unknown") == nullptr);

    printf("Player's disc image:\n");
    auto start = Clock::now();
    EXPECT(Check(iso) == GameSourceStatus::Ok);
    printf("  verified in %lld ms\n", milliseconds(start));

    DiscFileSystem disc;
    EXPECT(disc.Open(iso.string().c_str()));
    start = Clock::now();
    TitleMetadata metadata = ReadTitleMetadata(disc.ReadAll("default.xex"));
    const std::vector<uint8_t>& png = metadata.iconPng;
    bool isPng = png.size() > 24 && memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
    EXPECT(!metadata.name.empty());
    EXPECT(metadata.titleId == 0x415608AB);
    EXPECT(isPng);
    if (isPng)
    {
        auto be32 = [&](size_t offset) { return uint32_t(png[offset]) << 24 | png[offset + 1] << 16 | png[offset + 2] << 8 | png[offset + 3]; };
        printf("  XDBF title name \"%s\", icon PNG %ux%u (%zu bytes), read in %lld ms (kept in memory only)\n",
            metadata.name.c_str(), be32(16), be32(20), png.size(), milliseconds(start));
    }

    printf("Disc frontend texture decoder:\n");
    std::vector<uint8_t> startMenu = disc.ReadAll("ui/eng/startmenu.str");
    std::vector<uint8_t> logo;
    start = Clock::now();
    EXPECT(!startMenu.empty());
    EXPECT(LauncherAssets::DecodeFrontendTexture(startMenu, "skylanderslogo.dds", 1024, 512, logo));
    EXPECT(logo.size() == 1024ull * 512 * 4);
    size_t transparent = 0, opaque = 0;
    for (size_t i = 3; i < logo.size(); i += 4)
    {
        transparent += logo[i] == 0 ? 1 : 0;
        opaque += logo[i] > 200 ? 1 : 0;
    }
    printf("  skylanderslogo.dds 1024x512, %zu transparent and %zu opaque texels, decoded in %lld ms\n",
        transparent, opaque, milliseconds(start));
    EXPECT(transparent > 1000);
    EXPECT(opaque > 1000);
    EXPECT(!LauncherAssets::DecodeFrontendTexture(startMenu, "missing.dds", 1024, 512, logo));

    printf("Wrong inputs:\n");
    EXPECT(Check(scratch / "missing.iso") == GameSourceStatus::NotFound);

    std::filesystem::create_directories(scratch / "extracted");
    WriteFile(scratch / "extracted" / "default.xex", FakeXex(0x415608AB, 0x53CE7B97, 2));
    EXPECT(Check(scratch / "extracted") == GameSourceStatus::Folder);

    WriteFile(scratch / "locked.iso", RandomBytes(4096));
    std::filesystem::permissions(scratch / "locked.iso", std::filesystem::perms::none);
    EXPECT(Check(scratch / "locked.iso") == GameSourceStatus::Unreadable);
    std::filesystem::permissions(scratch / "locked.iso", std::filesystem::perms::owner_all);

    WriteFile(scratch / "empty.iso", {});
    EXPECT(Check(scratch / "empty.iso") == GameSourceStatus::NotXboxDisc);
    WriteFile(scratch / "random.iso", RandomBytes(1 << 20));
    EXPECT(Check(scratch / "random.iso") == GameSourceStatus::NotXboxDisc);

    WriteDiscImage(scratch / "original_xbox.iso", "default.xbe", RandomBytes(4096));
    EXPECT(Check(scratch / "original_xbox.iso") == GameSourceStatus::NotXbox360Game);
    WriteDiscImage(scratch / "not_a_xex.iso", "default.xex", RandomBytes(4096));
    EXPECT(Check(scratch / "not_a_xex.iso") == GameSourceStatus::Damaged);
    WriteDiscImage(scratch / "cut_short.iso", "default.xex", FakeXex(0x415608AB, 0x53CE7B97, 2), 4096);
    EXPECT(Check(scratch / "cut_short.iso") == GameSourceStatus::Damaged);
    WriteDiscImage(scratch / "other_game.iso", "default.xex", FakeXex(0x4D5307E6, 0x11111111, 0));
    EXPECT(Check(scratch / "other_game.iso") == GameSourceStatus::WrongGame);
    WriteDiscImage(scratch / "other_region.iso", "default.xex", FakeXex(0x415608AB, 0x12345678, 0x00000100));
    EXPECT(Check(scratch / "other_region.iso") == GameSourceStatus::WrongVersion);
    WriteDiscImage(scratch / "modified.iso", "default.xex", FakeXex(0x415608AB, 0x53CE7B97, 2));
    EXPECT(Check(scratch / "modified.iso") == GameSourceStatus::Damaged);

    printf("Damaged copies of the player's image:\n");
    for (int i = 3; i < argc; i++)
    {
        GameSourceStatus status = Check(argv[i]);
        EXPECT(status != GameSourceStatus::Ok);
    }

    printf("Command line without UI:\n");
    EXPECT(ResolveHeadless({ iso.string() }) == iso.string());
    EXPECT(ResolveHeadless({ "--select-iso", iso.string() }) == iso.string());
    EXPECT(ResolveHeadless({}).empty());
    EXPECT(ResolveHeadless({ (scratch / "random.iso").string() }).empty());

    printf("Launcher catalog and display helpers:\n");
    auto catalog = LauncherGameCatalog();
    EXPECT(catalog.size() == 6);
    EXPECT(std::string(catalog.front().id) == "ssa");
    EXPECT(catalog.front().enabled);
    EXPECT(std::string(catalog.front().name) == "Spyro's Adventure");
    size_t enabledEntries = 0;
    for (const LauncherGameEntry& entry : catalog)
    {
        EXPECT(entry.id != nullptr && *entry.id != '\0');
        EXPECT(entry.name != nullptr && *entry.name != '\0');
        EXPECT(entry.accent != 0);
        enabledEntries += entry.enabled ? 1 : 0;
    }
    EXPECT(enabledEntries == g_games.size());

    LauncherConfig helperConfig;
    EXPECT(LauncherLanguageLabel(helperConfig, game) == "English");
    helperConfig[""]["language"] = "sv";
    EXPECT(LauncherLanguageLabel(helperConfig, game) == "sv");
    helperConfig["ssa"]["language"] = "French";
    EXPECT(LauncherLanguageLabel(helperConfig, game) == "French");
    helperConfig["launcher"]["language"] = "fr-FR";
    EXPECT(LauncherLanguageLabel(helperConfig, game) == "fr-FR");
    helperConfig["launcher"]["language"].clear();
    helperConfig["ssa"]["language"].clear();
    helperConfig[""]["language"].clear();

    EXPECT(!LauncherFullscreen(helperConfig));
    helperConfig["launcher"]["fullscreen"] = "YES";
    EXPECT(LauncherFullscreen(helperConfig));
    helperConfig["launcher"]["fullscreen"] = "0";
    EXPECT(!LauncherFullscreen(helperConfig));
    helperConfig["launcher"]["fullscreen"].clear();
    helperConfig[""]["fullscreen"] = "on";
    EXPECT(LauncherFullscreen(helperConfig));

    EXPECT(LauncherFormatSize(0) == "0 bytes");
    EXPECT(LauncherFormatSize(1023) == "1023 bytes");
    EXPECT(LauncherFormatSize(1024) == "1 KB");
    EXPECT(LauncherFormatSize(5 * 1024) == "5 KB");
    EXPECT(LauncherFormatSize(5ull * 1024 * 1024) == "5 MB");
    EXPECT(LauncherFormatSize(2ull * 1024 * 1024 * 1024) == "2.0 GB");
    EXPECT(LauncherFormatSize(8738846720ull) == "8.1 GB");

    EXPECT(LauncherMiddleEllipsis("hello", 10) == "hello");
    EXPECT(LauncherMiddleEllipsis("hello", 5) == "hello");
    EXPECT(LauncherMiddleEllipsis("abcdefghij", 3) == "abc");
    EXPECT(LauncherMiddleEllipsis("abcdefghij", 7) == "ab...ij");
    EXPECT(LauncherMiddleEllipsis("d\xc3\xa9j\xc3\xa0 vu's.iso", 9) == "d\xc3\xa9j...iso");
    std::string longPath = "/Volumes/USB disk/Giants [Disc] #1 = a very long name.iso";
    std::string shortened = LauncherMiddleEllipsis(longPath, 20);
    EXPECT(shortened.size() < longPath.size());
    EXPECT(shortened.find("...") != std::string::npos);
    EXPECT(shortened.rfind(".iso") == shortened.size() - 4);

    EXPECT(LauncherDisplayName("Skylanders: Spyro's Adventure", game) == "Skylanders: Spyro's Adventure");
    EXPECT(LauncherDisplayName("", game) == game.name);

    printf("Settings file:\n");
    LauncherConfig config;
    config[""]["version"] = "1";
    config["ssa"]["iso"] = iso.string();
    config["giants"]["iso"] = "/Volumes/USB disk/Giants [Disc] #1 = d\xc3\xa9j\xc3\xa0 vu's.iso";
    std::filesystem::path settings = scratch / "launcher.ini";
    EXPECT(SaveLauncherConfig(settings.string(), config));
    EXPECT(LoadLauncherConfig(settings.string()) == config);
    std::ifstream saved(settings);
    printf("%s", std::string(std::istreambuf_iterator<char>(saved), {}).c_str());

    std::string handEdited = "# comment\r\n  [ ssa ]  \r\n\r\n iso = /a b/c.iso \r\n; other\r\nnot a pair\r\n[empty]\r\n";
    WriteFile(scratch / "edited.ini", { handEdited.begin(), handEdited.end() });
    LauncherConfig edited = LoadLauncherConfig((scratch / "edited.ini").string());
    EXPECT(edited.size() == 1 && edited["ssa"].size() == 1 && edited["ssa"]["iso"] == "/a b/c.iso");
    EXPECT(LoadLauncherConfig((scratch / "missing.ini").string()).empty());

    std::filesystem::remove_all(scratch);
    printf(g_failures == 0 ? "All launcher checks passed\n" : "%d launcher check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
