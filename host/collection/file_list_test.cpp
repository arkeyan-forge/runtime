// Checks the file list and its checker (file_list.h) on synthetic files in a scratch folder. No game data.
// usage: SSAFileListTest <scratch dir>
#include "file_list.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace filelist;

namespace
{
    int g_failures = 0;
#define EXPECT(condition)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    void Put(const fs::path& file, const std::string& text)
    {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
    }

    std::string Sha(const fs::path& folder, const std::string& text)
    {
        Put(folder / "sha.bin", text);
        return Sha256OfFile(folder / "sha.bin");
    }

    bool Has(const Result& result, Problem::Kind kind, const std::string& path)
    {
        for (const Problem& problem : result.problems)
            if (problem.kind == kind && problem.path == path)
                return true;
        return false;
    }

    // FIPS 180-4's examples, the block edges among them (55, 56 and 64 bytes, a million).
    void TestSha(const fs::path& scratch)
    {
        EXPECT(Sha(scratch, "") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT(Sha(scratch, "abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT(Sha(scratch, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        EXPECT(
            Sha(scratch, std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
        EXPECT(
            Sha(scratch, std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
        EXPECT(Sha(scratch, std::string(1000000, 'a')) ==
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        EXPECT(Sha256OfFile(scratch / "nothing").empty());
    }

    void TestFormat()
    {
        const std::string a(64, 'a'), b(64, '0');
        std::vector<Entry> entries;
        // shasum's own output, a comment, CRLF and the binary-mode star.
        EXPECT(Parse(
            "# made by make_kit.sh\n" + a + "  bin/Arkeyan Builder.exe\r\n\n" + b + " *res/fonts/x.ttf\n", entries));
        EXPECT(entries.size() == 2 && entries[0].path == "bin/Arkeyan Builder.exe" && entries[0].sha256 == a &&
               entries[1].path == "res/fonts/x.ttf");
        std::vector<Entry> again;
        EXPECT(Parse(Format(entries), again) && again.size() == 2 && again[1].sha256 == b &&
               again[1].path == entries[1].path);
        EXPECT(Parse("", entries) && entries.empty());
        // Malformed lines and paths that leave the folder.
        for (const std::string& line : { a + " bin/x", a.substr(1) + "  bin/x", std::string(64, 'A') + "  bin/x",
                 a + "  ../x", a + "  /etc/x", a + "  bin\\x", a + "  C:/x", a + "  bin//x", a + "  ./x", a + "  " })
            EXPECT(!Parse(line + "\n", entries));
    }

    void TestCheck(const fs::path& scratch)
    {
        const fs::path kit = scratch / "kit";
        Put(kit / "Arkeyan-Builder", "builder");
        Put(kit / "bin/XenonRecomp", "xenon recomp");
        Put(kit / "res/fonts/Some Font.ttf", "font");
        std::string error;
        EXPECT(Write(kit, { "bin/XenonRecomp", "Arkeyan-Builder", "res/fonts/Some Font.ttf" }, error));
        std::vector<Entry> entries;
        std::ifstream listed(kit / ListName, std::ios::binary);
        EXPECT(Parse(std::string(std::istreambuf_iterator<char>(listed), {}), entries) && entries.size() == 3 &&
               entries[0].path == "Arkeyan-Builder");
        EXPECT(!Write(kit, { "bin/none" }, error) && !error.empty());
        EXPECT(!Write(kit, { "../x" }, error));
        EXPECT(Write(kit, { "bin/XenonRecomp", "Arkeyan-Builder", "res/fonts/Some Font.ttf" }, error));

        // Freshly written: nothing to read again, nothing wrong; files outside the list are ignored.
        Put(kit / "settings.ini", "anything");
        Result result = Check(kit);
        EXPECT(result.listed && result.problems.empty() && result.files == 3 && result.hashed == 0);

        // Without the verified record every file is read once, then not again.
        fs::remove(kit / VerifiedName);
        result = Check(kit);
        EXPECT(result.problems.empty() && result.hashed == 3 && result.hashedBytes == 7 + 12 + 4);
        result = Check(kit);
        EXPECT(result.problems.empty() && result.hashed == 0);

        // A quarantined file is missing; a "cleaned" one is changed (same size, a new time); the others stay unread.
        fs::remove(kit / "bin/XenonRecomp");
        Put(kit / "Arkeyan-Builder", "BUILDER");
        fs::last_write_time(
            kit / "Arkeyan-Builder", fs::last_write_time(kit / "Arkeyan-Builder") + std::chrono::seconds(5));
        result = Check(kit);
        EXPECT(result.problems.size() == 2 && Has(result, Problem::Kind::Missing, "bin/XenonRecomp") &&
               Has(result, Problem::Kind::Changed, "Arkeyan-Builder"));
        EXPECT(result.hashed == 1);
        // Still reported on the next start: a changed file never enters the record.
        result = Check(kit);
        EXPECT(result.problems.size() == 2 && result.hashed == 1);

        // Put back as they were: clean again (a touched but identical file is read once more, then recorded).
        Put(kit / "bin/XenonRecomp", "xenon recomp");
        Put(kit / "Arkeyan-Builder", "builder");
        result = Check(kit);
        EXPECT(result.problems.empty() && result.hashed == 2);

        // A record for another hash (a list replaced by a newer kit's) is not trusted.
        Put(kit / VerifiedName, std::string(64, '0') + " 4 0 res/fonts/Some Font.ttf\nnot a line\n");
        result = Check(kit);
        EXPECT(result.problems.empty() && result.hashed == 3);

        // A damaged list is a problem of its own; no list: nothing to check (a kit or a game from before lists).
        Put(kit / ListName, "garbage\n");
        result = Check(kit);
        EXPECT(result.listed && result.problems.size() == 1 && Has(result, Problem::Kind::Changed, ListName));
        fs::remove(kit / ListName);
        result = Check(kit);
        EXPECT(!result.listed && result.problems.empty());

        // Cancelled: says so, reports nothing.
        EXPECT(Write(kit, { "Arkeyan-Builder" }, error));
        fs::remove(kit / VerifiedName);
        std::atomic<bool> cancel{ true };
        result = Check(kit, &cancel);
        EXPECT(result.cancelled && result.problems.empty());
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path scratch = fs::path(argv[1]) / "file_list";
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch);
    TestSha(scratch);
    TestFormat();
    TestCheck(scratch);
    fs::remove_all(scratch, error);
    printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
