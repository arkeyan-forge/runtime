#pragma once

#include <cstdint>

// The host's frame rate limiter (docs/runtime/xbox360/video_vblank.md, "Above 60"): the guest's vertical blank stays 60 Hz,
// and a frame is shown no earlier than one period after the previous one. Times are nanoseconds on any steady clock; the
// clock and the sleep are passed in, so a test runs it without waiting.
class FrameLimiter
{
public:
    // 0: no limit.
    void SetRate(uint32_t fps)
    {
        m_fps = fps;
        m_period = fps == 0 ? 0 : 1'000'000'000ull / fps;
        m_due = 0;
    }

    uint32_t Rate() const { return m_fps; }

    // The time the frame that reached its swap at `now` may be shown at (`now` itself when it is late). A frame more than
    // a period late starts a new schedule at `now` instead of showing the next ones in a burst to catch up.
    uint64_t Due(uint64_t now)
    {
        if (m_period == 0)
            return now;
        uint64_t due = m_due == 0 || now > m_due + m_period ? now : m_due;
        m_due = due + m_period;
        return due;
    }

    // Waits until `due`: sleeps while more than `spin` ns remain, then spins, since a sleep wakes up a millisecond late.
    template <class Now, class Sleep>
    static void WaitUntil(uint64_t due, uint64_t spin, Now now, Sleep sleep)
    {
        for (uint64_t t = now(); t < due; t = now())
            if (due - t > spin)
                sleep(due - t - spin);
    }

private:
    uint32_t m_fps = 0;
    uint64_t m_period = 0;
    uint64_t m_due = 0; // when the next frame may be shown
};
