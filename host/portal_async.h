#pragma once

// A real portal's device I/O on a thread of its own. The title polls the portal from its portal thread, and a game
// frame waits on that thread: on the first Windows PC every HID write blocked for hidapi's 1 s timeout, then the
// backend closed and reopened the portal, and the game froze for a second every 2 s. Here the game side only takes
// queues and a flag under a short lock, never the device: writes go to a queue the thread sends, reports come back
// through another, and the thread opens the device again after a failure, waiting 2 s, then 4, 8 and 10 s at most.
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
    explicit AsyncPortal(std::unique_ptr<PortalBackend> device, bool connected = false) : AsyncPortal(std::move(device), connected, Timing()) {}
    ~AsyncPortal() override;

    const char* Name() const override { return m_name; }
    bool IsConnected() override { return m_connected; }
    bool IsOpen() override { return m_connected; }
    // Queues the command; false only while the portal is not connected.
    bool Write(const uint8_t* command, size_t size) override;
    size_t Read(uint8_t* report, size_t capacity) override;

    // The wait before the next attempt to open the device, 0 while connected (tests).
    std::chrono::milliseconds RetryDelay();

private:
    void Run();

    using Report = std::vector<uint8_t>;
    static constexpr size_t QueueLimit = 64; // older reports and commands are dropped past this

    std::unique_ptr<PortalBackend> m_device;
    const char* m_name;
    Timing m_timing;
    std::atomic<bool> m_connected, m_stop{ false };
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Report> m_commands, m_reports; // under m_mutex
    std::chrono::milliseconds m_delay{ 0 };    // under m_mutex
    std::thread m_thread;
};
