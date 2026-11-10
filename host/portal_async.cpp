#include "host/portal_async.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

AsyncPortal::AsyncPortal(std::unique_ptr<PortalBackend> device, bool connected, Timing timing)
    : m_device(std::move(device)), m_name(m_device->Name()), m_timing(timing), m_connected(connected)
{
    m_takesSound = connected && m_device->TakesSound();
    m_thread = std::thread([this] { Run(); });
}

AsyncPortal::~AsyncPortal()
{
    m_stop = true;
    m_wake.notify_all();
    m_thread.join();
}

bool AsyncPortal::Write(const uint8_t* command, size_t size)
{
    if (!m_connected)
        return false;
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back({ Report(command, command + size) });
        if (m_commands.size() > QueueLimit)
            m_commands.pop_front();
    }
    m_wake.notify_all();
    return true;
}

bool AsyncPortal::WriteSound(const uint8_t* block, size_t size)
{
    if (!m_connected)
        return false;
    {
        std::lock_guard lock(m_mutex);
        m_commands.push_back({ Report(block, block + size), true });
        if (m_commands.size() > QueueLimit)
            m_commands.pop_front();
    }
    m_wake.notify_all();
    return true;
}

namespace
{
    bool IsStatus(const std::vector<uint8_t>& report) { return report.size() >= 5 && report[0] == 'S'; }
}

size_t AsyncPortal::Read(uint8_t* report, size_t capacity)
{
    std::lock_guard lock(m_mutex);
    if (m_reports.empty())
        return 0;
    auto next = std::find_if(m_reports.begin(), m_reports.end(), [](const Report& r) { return !IsStatus(r); });
    if (next == m_reports.end())
        next = m_reports.begin();
    size_t size = std::min(next->size(), capacity);
    memcpy(report, next->data(), size);
    m_reports.erase(next);
    return size;
}

void AsyncPortal::Queue(const uint8_t* report, size_t size)
{
    Report incoming(report, report + size);
    if (IsStatus(incoming))
    {
        auto waiting = std::find_if(m_reports.begin(), m_reports.end(), IsStatus);
        if (waiting != m_reports.end())
        {
            // 2 bits a slot: present (the latest says), changed (any of them said).
            auto bits = [](const Report& r) { return uint32_t(r[1] | r[2] << 8 | r[3] << 16 | uint32_t(r[4]) << 24); };
            const uint32_t merged = (bits(incoming) & 0x55555555u) | ((bits(incoming) | bits(*waiting)) & 0xAAAAAAAAu);
            for (int i = 0; i < 4; i++)
                incoming[1 + i] = uint8_t(merged >> (8 * i));
            *waiting = std::move(incoming);
            return;
        }
    }
    m_reports.push_back(std::move(incoming));
    if (m_reports.size() > QueueLimit)
        m_reports.pop_front();
}

std::chrono::milliseconds AsyncPortal::RetryDelay()
{
    std::lock_guard lock(m_mutex);
    return m_delay;
}

void AsyncPortal::Run()
{
    using Clock = std::chrono::steady_clock;
    auto backoff = m_timing.firstRetry;
    Clock::time_point next = Clock::now();
    // Logged once per change: a portal that opens and then fails at once says so once, not at every attempt.
    bool failing = false;

    auto lose = [&](const char* why) {
        m_connected = false;
        std::lock_guard lock(m_mutex);
        if (!failing)
            fprintf(stderr, "[portal] %s: lost (%s); trying again after %lld s, then up to %lld s apart\n", m_name, why,
                static_cast<long long>(backoff.count() / 1000),
                static_cast<long long>(m_timing.maxRetry.count() / 1000));
        failing = true;
        m_delay = backoff;
        next = Clock::now() + backoff;
        backoff = std::min(backoff * 2, m_timing.maxRetry);
    };

    while (!m_stop)
    {
        if (!m_connected)
        {
            {
                std::unique_lock lock(m_mutex);
                if (m_wake.wait_until(lock, next, [&] { return m_stop.load(); }))
                    break;
            }
            if (m_device->IsConnected())
            {
                std::lock_guard lock(m_mutex);
                m_commands.clear();
                m_reports.clear();
                m_delay = std::chrono::milliseconds(0);
                m_takesSound = m_device->TakesSound();
                m_connected = true;
                if (!failing)
                    fprintf(stderr, "[portal] %s: connected\n", m_name);
            }
            else
            {
                std::lock_guard lock(m_mutex);
                m_delay = backoff;
                next = Clock::now() + backoff;
                backoff = std::min(backoff * 2, m_timing.maxRetry);
            }
            continue;
        }

        Command command;
        {
            std::lock_guard lock(m_mutex);
            if (!m_commands.empty())
            {
                command = std::move(m_commands.front());
                m_commands.pop_front();
            }
        }
        // A speaker block the device refuses only stops its sound (the backend says so); commands decide the connection.
        if (command.sound)
        {
            m_device->WriteSound(command.bytes.data(), command.bytes.size());
            m_takesSound = m_device->TakesSound();
        }
        else if (!command.bytes.empty())
        {
            if (!m_device->Write(command.bytes.data(), command.bytes.size()))
            {
                lose("a write failed");
                continue;
            }
            if (failing)
                fprintf(stderr, "[portal] %s: working again\n", m_name);
            failing = false;
            backoff = m_timing.firstRetry;
        }

        uint8_t buffer[64];
        for (int i = 0; i < 8; i++)
        {
            size_t size = m_device->Read(buffer, sizeof(buffer));
            if (size == 0)
                break;
            std::lock_guard lock(m_mutex);
            Queue(buffer, size);
        }
        if (!m_device->IsOpen())
        {
            lose("it closed: unplugged, or a read failed");
            continue;
        }

        std::unique_lock lock(m_mutex);
        m_wake.wait_for(lock, m_timing.poll, [&] { return m_stop || !m_commands.empty(); });
    }
}
