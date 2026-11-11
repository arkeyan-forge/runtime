// Checks the guard on Direct3D 12's first frames (gpu/first_frame.h) with fake backends: one that never presents, one
// whose presents return while the GPU completes nothing, one that loses its device, one that works (no switch).
// usage: ArkeyanFirstFrameTest
#include "gpu/first_frame.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    using namespace gpu::firstframe;

    enum class Kind
    {
        Works,
        PresentsFail,    // every present returns an error
        NeverCompletes,  // presents return S_OK, the GPU finishes nothing: the next frame's fence wait never returns
        LosesDevice,     // removed after its first presents
        LosesDeviceLate, // removed after two minutes of frames
        SlowBoot,        // nothing presented for 20 s (loading), then works
        Minimised,       // presents fail while the window is minimised for 30 s, then works
    };

    // A game at 60 FPS on a fake backend, sampled every WatchTickMs as the watch thread does. The time of the verdict.
    struct Run
    {
        Verdict verdict = Verdict::Watching;
        uint64_t atMs = 0;
        std::string why;
    };

    Run Play(Kind kind, uint64_t forMs = 200000)
    {
        Watch watch;
        Sample s;
        Run run;
        const uint64_t startMs = 1000; // the watch starts at gpu::Init's end
        uint64_t nextFrameMs = startMs + (kind == Kind::SlowBoot ? 20000 : 500);
        bool stuck = false;
        for (uint64_t now = startMs; now < startMs + forMs; now += WatchTickMs)
        {
            s.nowMs = now;
            s.visible = !(kind == Kind::Minimised && now < startMs + 30000);
            while (!stuck && now >= nextFrameMs)
            {
                s.presentAttempts++;
                if (kind == Kind::PresentsFail || !s.visible)
                    s.presentFailures++;
                else if (kind == Kind::NeverCompletes)
                {
                    stuck = true; // the GPU thread now waits for this frame's fence for good
                    s.waitingSinceMs = now;
                }
                else
                    s.framesShown++;
                nextFrameMs += 16;
            }
            if (kind == Kind::LosesDevice && s.presentAttempts >= 3)
                s.removedReason = int32_t(0x887A0006u); // DXGI_ERROR_DEVICE_HUNG
            if (kind == Kind::LosesDeviceLate && now >= startMs + 120000)
                s.removedReason = int32_t(0x887A0005u);
            Verdict verdict = watch.Update(s);
            if (verdict != Verdict::Watching)
            {
                run.verdict = verdict;
                run.atMs = now - startMs;
                run.why = watch.Why();
                return run;
            }
        }
        return run;
    }
}

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

    Run run = Play(Kind::Works);
    check(run.verdict == Verdict::Fine, "a working backend: no switch");
    check(
        run.atMs >= Watch::EarlyMs && run.atMs < Watch::EarlyMs + 2000, "  and the watch ends after the first minute");

    run = Play(Kind::PresentsFail);
    check(run.verdict == Verdict::Switch, "presents that all fail: switch");
    check(run.atMs >= 500 + Watch::NoFrameMs && run.atMs <= 500 + Watch::NoFrameMs + 500, "  6 s after the first");
    check(run.why.find("all ") != std::string::npos && run.why.find("presents failed") != std::string::npos,
        "  and the log says they failed");

    run = Play(Kind::NeverCompletes);
    check(run.verdict == Verdict::Switch, "presents that return while the GPU completes nothing: switch");
    check(run.atMs >= 500 + Watch::HungMs && run.atMs <= 500 + Watch::HungMs + 500, "  5 s after the stuck wait");
    check(run.why.find("did not finish") != std::string::npos, "  and the log says the GPU did not finish");

    run = Play(Kind::LosesDevice);
    check(run.verdict == Verdict::Switch, "a device removed before its first frame: switch");
    check(run.atMs < 1000, "  at once");
    check(
        run.why.find("DXGI_ERROR_DEVICE_HUNG") != std::string::npos && run.why.find("0x887A0006") != std::string::npos,
        "  and the log names the reason");

    run = Play(Kind::LosesDeviceLate);
    check(run.verdict == Verdict::Fine, "a device removed after the first minute: not the backend's to change");

    run = Play(Kind::SlowBoot);
    check(run.verdict == Verdict::Fine, "20 s of loading before the first present: no switch");

    run = Play(Kind::Minimised);
    check(run.verdict == Verdict::Fine, "a minimised window that cannot present for 30 s: no switch");

    // Presents that "work" without the GPU ever finishing, while no fence wait is under way (the present itself blocks).
    {
        Watch watch;
        Sample s;
        s.presentAttempts = 2;
        Verdict verdict = Verdict::Watching;
        uint64_t now = 1000;
        for (; now < 20000 && verdict == Verdict::Watching; now += WatchTickMs)
        {
            s.nowMs = now;
            verdict = watch.Update(s);
        }
        check(verdict == Verdict::Switch && std::string(watch.Why()).find("completed none") != std::string::npos,
            "presents that return S_OK and a blocked thread: switch, the GPU completed none");
    }

    // The verdict holds: a later sample never changes it.
    {
        Watch watch;
        Sample s;
        s.nowMs = 1000;
        s.removedReason = int32_t(0x887A0005u);
        check(watch.Update(s) == Verdict::Switch, "removed");
        s.removedReason = 0;
        s.framesShown = 10;
        s.nowMs = 200000;
        check(watch.Update(s) == Verdict::Switch, "  stays a switch");
    }

    // What the switch writes: Vulkan and its reason in the game's own section, every other key kept.
    {
        std::map<std::string, std::map<std::string, std::string>> config;
        config["trapteam.video"]["resolution_scale"] = "2";
        config["trapteam.video"]["gpu_backend"] = "d3d12";
        WriteSwitch(config, "trapteam.video", "no frame reached the screen in 6 s: all 360 presents failed");
        check(config["trapteam.video"]["gpu_backend"] == "vulkan", "gpu_backend = vulkan");
        check(config["trapteam.video"]["resolution_scale"] == "2", "  the other keys kept");
        const std::string note = config["trapteam.video"][NoteKey];
        check(note.rfind("Direct3D 12 showed nothing (", 0) == 0 &&
                  note.find("all 360 presents failed") != std::string::npos &&
                  note.find("switched to Vulkan") != std::string::npos,
            "  and the note says why");
        check(config.count("video") == 0, "  not in [video]");
    }
    check(RestartExitCode == 75, "the restart's exit code");

    if (failures != 0)
    {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("first frame guard: %d checks passed\n", checks);
    return 0;
}
