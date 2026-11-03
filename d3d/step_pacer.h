#pragma once

#include <algorithm>
#include <cstdint>

// Passes that advance a simulation by one step for each frame the game renders (a water ripple height field: constants
// that never change, a step tuned for 30 FPS) must not run more than 30 times a second, or the waves travel and fade
// twice as fast at 60. The pacer says, once per frame, whether the passes run in it. Times are nanoseconds on any steady
// clock, passed in so that a test runs it without waiting. At a limit of 30 or below it always says yes: nothing changes.
class StepPacer
{
public:
    static constexpr uint64_t Step = 1'000'000'000ull / 30;
    static constexpr uint64_t Slack = 4'000'000;      // a step may come this early (frame times jitter)
    static constexpr uint64_t MaxElapsed = 100'000'000; // a pause (a load) is not time to catch up on
    static constexpr uint64_t MaxCarry = 8'000'000;

    bool Run(uint64_t now, uint32_t fpsLimit)
    {
        const uint64_t elapsed = m_last == 0 ? Step : std::min(now - m_last, MaxElapsed);
        m_last = now;
        if (fpsLimit != 0 && fpsLimit <= 30)
        {
            m_carry = 0;
            return true;
        }
        m_carry += elapsed;
        if (m_carry + Slack < Step)
            return false;
        m_carry = m_carry > Step ? std::min(m_carry - Step, MaxCarry) : 0;
        return true;
    }

private:
    uint64_t m_last = 0, m_carry = 0;
};
