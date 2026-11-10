#pragma once

// Frame generation's rules (gpu/fsr/framegen.h), header only so framegen_test.cpp checks them: when a frame is made
// (Decide), and the pacing: the real frames' interval, measured, and how evenly the frames were shown. The generated
// frame goes out when the real one arrives and the real one half an interval later, so the shown frames are evenly spaced
// at twice the real rate.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace framegen
{
    // What a real frame gets.
    enum class Outcome
    {
        Generate, // optical flow and interpolation run, the generated frame goes out before the real one
        Reset, // they run (to start their history) but the real frame goes alone: no frame before it to interpolate from
        NoScene, // nothing runs: no 3D scene this frame (a movie, a loading screen, a menu): the history ends
        NoHudless, // nothing runs: no copy of the frame without its HUD, which would be interpolated into ghosts
        Display, // nothing runs: VSync is on and the display cannot show twice the real rate (a 60 FPS game on 60 Hz)
        Off,
    };

    struct FrameState
    {
        bool enabled; // the setting, and the game allows it
        bool scene; // FSR upscaled a scene this frame
        bool hudless; // the frame was copied after the composite, and the copy matches the presented frame
        bool history; // the real frame before went through optical flow and interpolation
        bool cut; // the camera jumped (CameraCut)
        bool displayRoom = true; // DisplayHasRoom
    };

    // With VSync on, the display shows at most its refresh rate: twice the real rate must fit (5 % of slack), or the
    // generated frames would hold the real ones back (a 60 FPS game on a 60 Hz display would fall to 30 real frames).
    // Without VSync, or an unknown refresh rate (0), there is always room. `realIntervalMs`: the measured interval.
    inline bool DisplayHasRoom(bool vsync, double refreshHz, double realIntervalMs)
    {
        if (!vsync || refreshHz <= 0 || realIntervalMs <= 0)
            return true;
        return 2.0 * 1000.0 / realIntervalMs <= refreshHz * 1.05;
    }

    // How far the camera's reprojection (this frame's clip space to the previous one's, row-major, column vectors:
    // gpu::UpscaleDesc::reprojection) takes points of the picture: the largest move, in clip units (the picture is 2
    // wide), or 1e9 when one goes behind the camera or the matrix is not finite. At the scene's depth, not the near
    // plane's: ndc z 0.99 is about a hundred times the near distance (ndc 0.5 is twice it, where an ordinary step of the
    // camera already moves a point across the picture: Swap Force's tutorial, near plane 32).
    inline double CameraShift(const double m[16])
    {
        const double points[5][2] = { { 0, 0 }, { -0.5, -0.5 }, { 0.5, -0.5 }, { -0.5, 0.5 }, { 0.5, 0.5 } };
        double largest = 0;
        for (const auto& p : points)
        {
            const double x = p[0], y = p[1], z = 0.99;
            const double w = m[12] * x + m[13] * y + m[14] * z + m[15];
            if (!(w > 1e-6))
                return 1e9;
            const double px = (m[0] * x + m[1] * y + m[2] * z + m[3]) / w;
            const double py = (m[4] * x + m[5] * y + m[6] * z + m[7]) / w;
            const double move = std::max(std::fabs(px - x), std::fabs(py - y));
            if (!(move == move))
                return 1e9;
            largest = std::max(largest, move);
        }
        return largest;
    }

    // A camera cut: the camera's reprojection moves a point of the picture more than a quarter of it (or behind the
    // camera). Between two such frames there is nothing to interpolate.
    inline bool CameraCut(const double m[16]) { return CameraShift(m) > 0.5; }

    // A camera that jumps (a collision pushing it against a post, a respawn): its shift is a tenth of the picture or
    // more and three times the frame before's. Cutscenes and a ship's pitch build up over frames; a jump is sudden, and
    // the frames around it interpolated into fragments (Swap Force's Swap Zones).
    inline bool CameraJump(double shift, double previous) { return shift > 0.1 && shift > 3.0 * previous; }

    // Why frame generation is off whatever the setting. It is held back for players (it does not render as it should yet:
    // the F3 row and the launcher's switch say "Coming later") unless SKY_FSR_FRAME_GENERATION_PREVIEW=1 (a test:
    // tools/regress/fsr_framegen.sh); with the preview, a game whose adapter keeps it off (game::Adapter::frameGeneration)
    // still does, unless SKY_FSR_FRAME_GENERATION_ANY_GAME=1.
    enum class Hold
    {
        None,
        Later,
        Game,
    };
    inline Hold HeldBack(bool preview, bool gameClean, bool anyGame)
    {
        if (!preview)
            return Hold::Later;
        return gameClean || anyGame ? Hold::None : Hold::Game;
    }

    // The frames shown must stay even: when the GPU cannot do the interpolation in time (a 60 FPS game on a slow GPU),
    // the two presents bunch up. Two 10 s windows in a row that made frames for most real ones (90 %) with the shown
    // spacing's deviation above 5 ms (games' own pacing stays under 3) stop frame generation.
    class UnevenGuard
    {
    public:
        // One window: true when frame generation must stop.
        bool Window(double deviationMs, double generatedShare)
        {
            m_strikes = generatedShare >= 0.9 && deviationMs > 5.0 ? m_strikes + 1 : 0;
            return m_strikes >= 2;
        }
        void Reset() { m_strikes = 0; }

    private:
        int m_strikes = 0;
    };

    inline Outcome Decide(const FrameState& f)
    {
        if (!f.enabled)
            return Outcome::Off;
        if (!f.scene)
            return Outcome::NoScene;
        if (!f.hudless)
            return Outcome::NoHudless;
        if (!f.displayRoom)
            return Outcome::Display;
        return f.history && !f.cut ? Outcome::Generate : Outcome::Reset;
    }

    class Pacing
    {
    public:
        // The real frames' interval is held between these (a hitch or a pause must not hold the next frame back long).
        static constexpr uint64_t MinIntervalNs = 4'000'000, MaxIntervalNs = 100'000'000;

        // A real frame arrives at `now`: the time since the one before, in milliseconds (FFX's frameTimeDelta).
        float RealFrame(uint64_t now)
        {
            const uint64_t since = m_lastReal != 0 && now > m_lastReal ? now - m_lastReal : 0;
            m_lastReal = now;
            if (since == 0)
                return 16.7f;
            const double clamped = double(std::clamp(since, MinIntervalNs, MaxIntervalNs));
            // A moving average over about ten frames: steady against one late frame, quick after a rate change.
            m_intervalNs = m_intervalNs == 0 ? clamped : m_intervalNs + (clamped - m_intervalNs) * 0.1;
            return float(clamped / 1e6);
        }

        // How long after the generated frame the real one goes out.
        uint64_t HalfIntervalNs() const { return uint64_t(m_intervalNs / 2); }
        // The real frames' measured interval (0 before two frames).
        double IntervalMs() const { return m_intervalNs / 1e6; }

        // A frame was shown at `now` (generated or real).
        void Shown(uint64_t now)
        {
            if (m_lastShown != 0 && now > m_lastShown)
            {
                const double ms = double(now - m_lastShown) / 1e6;
                if (ms < 200) // a pause or a loading screen is not pacing
                {
                    m_count++;
                    m_sum += ms;
                    m_squares += ms * ms;
                }
            }
            m_lastShown = now;
        }

        struct Summary
        {
            double meanMs = 0, deviationMs = 0;
            uint64_t count = 0;
        };
        // The spacing of the frames shown since the last call.
        Summary TakeSummary()
        {
            Summary s;
            s.count = m_count;
            if (m_count > 0)
            {
                s.meanMs = m_sum / double(m_count);
                s.deviationMs = std::sqrt(std::max(0.0, m_squares / double(m_count) - s.meanMs * s.meanMs));
            }
            m_count = 0;
            m_sum = m_squares = 0;
            return s;
        }

        // Frame generation stopped (off, a frame without a scene): the next real frame starts a new measure.
        void Reset()
        {
            m_lastReal = m_lastShown = 0;
            m_intervalNs = 0;
        }

    private:
        uint64_t m_lastReal = 0, m_lastShown = 0;
        double m_intervalNs = 0;
        uint64_t m_count = 0;
        double m_sum = 0, m_squares = 0;
    };
}
