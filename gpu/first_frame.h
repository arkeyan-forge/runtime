#pragma once

// Desktop Windows' guard on Direct3D 12's first frames (docs/runtime/gpu/plume_backends.md, "When D3D12 shows nothing").
// A player whose D3D12 showed a black screen while the game played on (sound, input) had a working Vulkan: when D3D12
// was the setting's choice (not forced with SKY_GPU_BACKEND) and no frame reaches the screen, the game switches to
// Vulkan for good: [<game>.video] gpu_backend = vulkan, with gpu_backend_note saying why, then a restart (main.cpp).
// "Reaches the screen" is a present that succeeded and whose GPU work then completed (its fence was waited for): a
// present that returns S_OK while the GPU never finishes is not a frame. A watch thread samples the GPU code's counters
// every WatchTickMs, so a GPU thread stuck in a fence wait or in a blocked present is seen too. No plume, DXGI or SDL
// here: tested on every host with fake backends (first_frame_test.cpp).
#include "gpu/swapchain_recovery.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace gpu::firstframe
{
    // What the GPU code reports, read by the watch thread.
    struct Sample
    {
        uint64_t nowMs = 0;
        bool visible = true;             // the window is shown and not minimised: a hidden one is given no frame
        int32_t removedReason = 0;       // the device's removal HRESULT, 0 while it is there
        uint32_t presentAttempts = 0;    // acquires and presents tried on the swap chain
        uint32_t presentFailures = 0;    // of them, the ones that failed
        uint32_t framesShown = 0;        // presented frames whose GPU work then completed
        uint64_t waitingSinceMs = 0;     // a fence wait under way since then; 0 for none
    };

    enum class Verdict
    {
        Watching,
        Fine,   // frames reach the screen: the watch ends
        Switch, // D3D12 shows nothing on this PC: Vulkan from now on (Why())
    };

    constexpr uint64_t WatchTickMs = 250;

    class Watch
    {
    public:
        // Visible time after the first present with no frame shown: a game shows its first frames in well under 1 s.
        static constexpr uint64_t NoFrameMs = 6000;
        // One fence wait this long: the GPU does not finish its work (Windows resets a GPU after 2 s of it).
        static constexpr uint64_t HungMs = 5000;
        // Time after the first frame shown during which a removed device or a hung GPU still switches; later, it is the
        // device-lost path's (gpu.cpp), not the backend's.
        static constexpr uint64_t EarlyMs = 60000;
        // Nothing presented for this long (a hidden window, a game stuck elsewhere): nothing to conclude, the watch ends.
        static constexpr uint64_t GiveUpMs = 120000;

        Verdict Update(const Sample& s)
        {
            if (m_verdict != Verdict::Watching)
                return m_verdict;
            if (m_startMs == 0)
                m_startMs = s.nowMs | 1;
            if (s.removedReason != 0)
            {
                snprintf(m_why, sizeof(m_why), "the Direct3D 12 device was removed (%s, 0x%08X)%s",
                    swapchain::ResultName(s.removedReason), uint32_t(s.removedReason),
                    s.framesShown == 0 ? " before its first frame" : " in the first minute");
                return m_verdict = Verdict::Switch;
            }
            if (s.waitingSinceMs != 0 && s.nowMs >= s.waitingSinceMs + HungMs)
            {
                snprintf(m_why, sizeof(m_why), "the GPU did not finish its work in %u s (a fence never signalled)%s",
                    uint32_t(HungMs / 1000), s.framesShown == 0 ? ", no frame shown" : "");
                return m_verdict = Verdict::Switch;
            }
            if (s.framesShown != 0)
            {
                if (m_firstShownMs == 0)
                    m_firstShownMs = s.nowMs | 1;
                if (s.nowMs >= m_firstShownMs + EarlyMs)
                    m_verdict = Verdict::Fine;
                return m_verdict;
            }
            if (s.presentAttempts == 0)
            {
                if (s.nowMs >= m_startMs + GiveUpMs)
                    m_verdict = Verdict::Fine;
                return m_verdict;
            }
            if (s.visible && m_lastMs != 0)
                m_visibleMs += s.nowMs - m_lastMs;
            m_lastMs = s.nowMs;
            if (m_visibleMs < NoFrameMs)
                return m_verdict;
            if (s.presentFailures >= s.presentAttempts)
                snprintf(m_why, sizeof(m_why), "no frame reached the screen in %u s: all %u presents failed",
                    uint32_t(NoFrameMs / 1000), s.presentAttempts);
            else
                snprintf(m_why, sizeof(m_why),
                    "no frame reached the screen in %u s: %u presents, the GPU completed none of them",
                    uint32_t(NoFrameMs / 1000), s.presentAttempts);
            return m_verdict = Verdict::Switch;
        }

        const char* Why() const { return m_why; }

    private:
        Verdict m_verdict = Verdict::Watching;
        uint64_t m_startMs = 0, m_firstShownMs = 0, m_lastMs = 0, m_visibleMs = 0;
        char m_why[160] = "";
    };

    // The keys the switch writes in the game's own section of the settings file (host/launcher.h's LauncherConfig).
    constexpr const char* NoteKey = "gpu_backend_note";

    inline void WriteSwitch(std::map<std::string, std::map<std::string, std::string>>& config,
        const std::string& videoSection, const char* why)
    {
        auto& section = config[videoSection];
        section["gpu_backend"] = "vulkan";
        section[NoteKey] = std::string("Direct3D 12 showed nothing (") + why + "): switched to Vulkan";
    }

    // The game's exit code after the switch: the launcher starts it again once, with SKY_GPU_BACKEND=vulkan (EX_TEMPFAIL).
    constexpr int RestartExitCode = 75;
}
