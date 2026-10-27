#pragma once

// Test aid (SSA_PORTAL=stall, SSAPortalAsyncTest): a real portal as the first Windows PC's logs show one (0.1.0): it is
// always found, sends a status report when opened, and every write blocks for `writeDelay` (hidapi's 1 s write timeout
// on Windows) and then fails, which closes it. Like the HID backend, it is found again at most every 2 s.
#include "host/portal_backend.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

class StallingPortal final : public PortalBackend
{
public:
    explicit StallingPortal(std::chrono::milliseconds writeDelay = std::chrono::milliseconds(1000), bool writesFail = true,
        std::chrono::milliseconds rescan = std::chrono::milliseconds(2000))
        : m_writeDelay(writeDelay), m_rescan(rescan), m_writesFail(writesFail) {}

    const char* Name() const override { return "stalling test"; }

    bool IsConnected() override
    {
        if (!m_open && std::chrono::steady_clock::now() >= m_nextScan)
        {
            m_nextScan = std::chrono::steady_clock::now() + m_rescan;
            m_open = m_status = true;
            opens++;
        }
        return m_open;
    }

    bool IsOpen() override { return m_open; }

    bool Write(const uint8_t*, size_t) override
    {
        if (!m_open)
            return false;
        writes++;
        std::this_thread::sleep_for(m_writeDelay);
        if (m_writesFail)
            m_open = false;
        return !m_writesFail;
    }

    size_t Read(uint8_t* report, size_t capacity) override
    {
        if (!m_open || !m_status || capacity < PortalReportSize)
            return 0;
        m_status = false;
        memset(report, 0, PortalReportSize);
        report[0] = 'S';
        return PortalReportSize;
    }

    std::atomic<int> opens{ 0 }, writes{ 0 };

private:
    std::chrono::milliseconds m_writeDelay, m_rescan;
    bool m_writesFail;
    std::atomic<bool> m_open{ false }, m_status{ false };
    std::chrono::steady_clock::time_point m_nextScan{};
};
