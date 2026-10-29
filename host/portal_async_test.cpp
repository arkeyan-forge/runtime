// Test of AsyncPortal (host/portal_async.h) with a fake device (host/portal_stall.h): the game-side calls return at
// once whatever the device does, and the reopen attempts back off. Run with no arguments; exit code 0 = pass.
#include "host/portal_async.h"
#include "host/portal_stall.h"
#include <cstdio>
#include <vector>

namespace
{
    int g_checks;
    int g_failures;

    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;

    double Ms(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

    // Polls like the title (one read, then one write, every 12 ms) for `duration`; returns the slowest call in ms.
    double PollLikeTheTitle(AsyncPortal& portal, milliseconds duration, int* reports = nullptr)
    {
        double slowest = 0;
        const uint8_t status[] = { 'S' };
        for (Clock::time_point end = Clock::now() + duration; Clock::now() < end;)
        {
            Clock::time_point start = Clock::now();
            uint8_t report[32];
            if (portal.IsConnected() && portal.Read(report, sizeof(report)) > 0 && reports != nullptr)
                (*reports)++;
            portal.Write(status, sizeof(status));
            slowest = std::max(slowest, Ms(Clock::now() - start));
            std::this_thread::sleep_for(milliseconds(12));
        }
        return slowest;
    }

    // The first Windows PC: writes block for 1 s, then fail and close the portal.
    void TestFailingWrites()
    {
        auto device = std::make_unique<StallingPortal>(milliseconds(1000), true, milliseconds(0));
        StallingPortal& fake = *device;
        AsyncPortal portal(std::move(device), false, { milliseconds(200), milliseconds(500), milliseconds(2) });
        double slowest = PollLikeTheTitle(portal, milliseconds(3500));
        fprintf(stderr, "failing writes: slowest game-side call %.3f ms, %d opens, %d writes\n", slowest,
            fake.opens.load(), fake.writes.load());
        CHECK(slowest < 5);
        // Open at 0, write fails at 1 s, reopen 0.2 s later, fails at 2.2 s, reopen 0.4 s later: 3 opens in 3.5 s.
        CHECK(fake.opens == 3);
        CHECK(fake.writes == 3);
    }

    // Backoff: 2x per failed attempt up to the maximum, and back to the first delay once a write works.
    void TestBackoff()
    {
        auto device = std::make_unique<StallingPortal>(milliseconds(0), true, milliseconds(0));
        AsyncPortal portal(std::move(device), false, { milliseconds(20), milliseconds(100), milliseconds(1) });
        std::vector<long long> delays;
        const uint8_t status[] = { 'S' };
        for (Clock::time_point end = Clock::now() + milliseconds(700); Clock::now() < end;)
        {
            portal.Write(status, sizeof(status));
            long long delay = portal.RetryDelay().count();
            if (delay != 0 && (delays.empty() || delays.back() != delay))
                delays.push_back(delay);
            std::this_thread::sleep_for(milliseconds(1));
        }
        fprintf(stderr, "backoff:");
        for (long long delay : delays)
            fprintf(stderr, " %lld", delay);
        fprintf(stderr, " ms\n");
        CHECK((delays == std::vector<long long>{ 20, 40, 80, 100 }));
    }

    // A slow device that works: the game never waits, the portal stays connected and its reports come through.
    void TestSlowWrites()
    {
        auto device = std::make_unique<StallingPortal>(milliseconds(300), false, milliseconds(0));
        StallingPortal& fake = *device;
        AsyncPortal portal(std::move(device), false, { milliseconds(20), milliseconds(100), milliseconds(2) });
        int reports = 0;
        double slowest = PollLikeTheTitle(portal, milliseconds(1000), &reports);
        fprintf(stderr, "slow writes: slowest game-side call %.3f ms, %d writes, %d report(s)\n", slowest,
            fake.writes.load(), reports);
        CHECK(slowest < 5);
        CHECK(portal.IsConnected());
        CHECK(fake.opens == 1);
        CHECK(fake.writes >= 3);
        CHECK(reports == 1);
        CHECK(portal.RetryDelay().count() == 0);
    }

    // Not connected: writes are refused at once, nothing is queued for later.
    void TestDisconnected()
    {
        auto device = std::make_unique<StallingPortal>(milliseconds(0), true, milliseconds(60000));
        StallingPortal& fake = *device;
        fake.IsConnected(); // found once; the next scan is a minute away
        fake.Write(nullptr, 0);
        AsyncPortal portal(std::move(device), false, { milliseconds(20), milliseconds(100), milliseconds(1) });
        std::this_thread::sleep_for(milliseconds(50));
        const uint8_t status[] = { 'S' };
        CHECK(!portal.IsConnected());
        CHECK(!portal.Write(status, sizeof(status)));
        uint8_t report[32];
        CHECK(portal.Read(report, sizeof(report)) == 0);
    }
}

int main()
{
    TestFailingWrites();
    TestBackoff();
    TestSlowWrites();
    TestDisconnected();
    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
