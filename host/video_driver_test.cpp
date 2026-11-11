// Checks startup retry order and native display requirements without initializing SDL or running a game.
#include "host/video_driver.h"
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) {
        checks++;
        if (!ok)
        {
            fprintf(stderr, "FAIL %s\n", what);
            failures++;
        }
    };
    using namespace videodriver;

    check(Usable(true, "", "x11", 1), "X11 desktop remains usable");
    check(Usable(true, "", "wayland", 1), "Wayland desktop remains usable");
    check(!Usable(true, "", "wayland", 0), "zero displays requires another native driver");
    check(!Usable(true, "", "offscreen", 1), "automatic offscreen cannot show the game");
    check(!Usable(true, "", "dummy", 1), "automatic dummy cannot show the game");
    check(Usable(true, "offscreen", "offscreen", 0), "explicit offscreen test remains usable");
    check(Usable(true, "dummy", "dummy", 0), "explicit dummy test keeps its old behavior");
    check(Usable(false, "", "offscreen", 0), "other platforms keep offscreen behavior");

    std::vector<std::string> tried;
    auto run = [&](bool linuxDesktop, const char* requested, const char* working) {
        tried.clear();
        return Start(linuxDesktop, requested, [&](const char* driver) {
            tried.emplace_back(driver != nullptr ? driver : "auto");
            return tried.back() == working;
        });
    };
    check(run(true, "", "auto") && tried == std::vector<std::string>{ "auto" },
        "a working desktop is never switched");
    check(run(true, "", "wayland") && tried == std::vector<std::string>{ "auto", "wayland" },
        "failed SDL init or Vulkan window: Wayland first");
    check(run(true, "", "x11") && tried == std::vector<std::string>{ "auto", "wayland", "x11" },
        "failed Wayland: X11 next");
    check(!run(true, "", "none") && tried == std::vector<std::string>{ "auto", "wayland", "x11" },
        "every driver failed: stop after three attempts");
    check(run(true, "x11", "wayland") && tried == std::vector<std::string>{ "auto", "wayland" },
        "a failed inherited X11 override can recover with Wayland");
    check(run(true, "wayland", "x11") && tried == std::vector<std::string>{ "auto", "x11" },
        "do not repeat an explicitly requested failed driver");
    for (const char* requested : { "offscreen", "dummy" })
    {
        check(!run(true, requested, "x11") && tried == std::vector<std::string>{ "auto" },
            "explicit offscreen test must not open a desktop window after failure");
    }
    check(!run(false, "", "x11") && tried == std::vector<std::string>{ "auto" },
        "Windows and macOS never try Linux drivers");

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
