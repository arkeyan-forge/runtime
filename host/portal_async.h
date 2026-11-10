#pragma once

// A real portal's device I/O on a thread of its own. The title polls the portal from its portal thread, and a game
// frame waits on that thread: on the first Windows PC every HID write blocked for hidapi's 1 s timeout, then the
// backend closed and reopened the portal, and the game froze for a second every 2 s. Here the game side only takes
// queues and a flag under a short lock, never the device: writes go to a queue the thread sends, reports come back
// through another, and the thread opens the device again after a failure, waiting 2 s, then 4, 8 and 10 s at most.
// The title reads one report per poll (12 ms) and asks again when the answer is not the one it reads, while a real portal
// streams status reports: queued in turn, every answer came a poll late, each command was sent two or three times, the
// answers to the copies piled up and Giants reset the portal again and again (issue #20). So answers go first, and the
// status reports waiting are one, the latest, carrying every change the others reported.
#include "host/portal_backend.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class AsyncPortal final : public PortalBackend
{
public:
    struct Timing
    {
        std::chrono::milliseconds firstRetry{ 2000 }, maxRetry{ 10000 };
        std::chrono::milliseconds poll{ 2 }; // between reads while there is nothing to send
    };

    // Takes the device as created; `connected` is whether its IsConnected() just found it (PortalInit asks once).
    AsyncPortal(std::unique_ptr<PortalBackend> device, bool connected, Timing timing);
    explicit AsyncPortal(std::unique_ptr<PortalBackend> device, bool connected = false)
        : AsyncPortal(std::move(device), connected, Timing())
    {}
    ~AsyncPortal() override;

    const char* Name() const override { return m_name; }
    bool IsConnected() override { return m_connected; }
    bool IsOpen() override { return m_connected; }
    // Queues the command; false only while the portal is not connected.
    bool Write(const uint8_t* command, size_t size) override;
    size_t Read(uint8_t* report, size_t capacity) override;
    // What the device said on this thread when it connected, and after each speaker block.
    bool TakesSound() override { return m_connected && m_takesSound; }
    // Queued with the commands, in order (an `M` before the blocks it starts).
    bool WriteSound(const uint8_t* block, size_t size) override;

    // The wait before the next attempt to open the device, 0 while connected (tests).
    std::chrono::milliseconds RetryDelay();

private:
    void Run();
    void Queue(const uint8_t* report, size_t size); // under m_mutex

    using Report = std::vector<uint8_t>;
    struct Command
    {
        Report bytes;
        bool sound = false; // a speaker block (WriteSound), not a command
    };
    static constexpr size_t QueueLimit = 64; // older reports and commands are dropped past this

    std::unique_ptr<PortalBackend> m_device;
    const char* m_name;
    Timing m_timing;
    std::atomic<bool> m_connected, m_stop{ false }, m_takesSound{ false };
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Command> m_commands;           // under m_mutex
    std::deque<Report> m_reports;             // under m_mutex: answers, in order, and one status report at most
    std::chrono::milliseconds m_delay{ 0 };    // under m_mutex
    std::thread m_thread;
};
