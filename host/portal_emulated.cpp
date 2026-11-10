// A virtual Portal of Power serving figure dumps, for players without portal hardware.
// Protocol facts come from the title's own portal driver (docs/runtime/host/portal.md) and the public protocol descriptions.
#include "host/portal_backend.h"
#include <SDL3/SDL_iostream.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
    // 'A' answer after the activation state: battery level (wired portals report full) and a constant.
    constexpr uint8_t BatteryFull = 0xFF;
    constexpr uint8_t ActivateTrailer = 0x77;
    constexpr uint8_t SpeakerTrailer = 0x19;
    // Second byte of 'Q'/'W' answers: block count in the high nibble, slot in the low nibble; 0x01 means failure.
    constexpr uint8_t OneBlock = 0x10;
    constexpr uint8_t Failure = 0x01;

    bool IsSectorTrailer(size_t block) { return block % 4 == 3; }
}

bool LoadFigure(const std::string& path, FigureData& data)
{
    size_t size = 0;
    void* file = SDL_LoadFile(path.c_str(), &size);
    if (file == nullptr)
        return false;

    // Dumps are the 1 KB tag image; some tools append metadata after it.
    bool valid = size >= data.size();
    if (valid)
        memcpy(data.data(), file, data.size());
    SDL_free(file);
    return valid;
}

bool SaveFigure(const std::string& path, const FigureData& data)
{
    SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "r+b");
    if (file == nullptr)
        return false;

    bool written = SDL_WriteIO(file, data.data(), data.size()) == data.size();
    return SDL_CloseIO(file) && written;
}

int EmulatedPortal::Place(const FigureData& data, const std::string& path, int wanted)
{
    std::lock_guard lock(m_mutex);

    // Prefer a slot whose last change was already reported, so that a swap shows up as a removal and an arrival.
    auto free = [this](int i) { return !m_slots[i].data && (m_reserved >> i & 1) == 0; };
    int slot = wanted >= 0 && wanted < SlotCount && free(wanted) ? wanted : -1;
    for (int i = 0; i < SlotCount && slot < 0; i++)
    {
        if (free(i) && !m_slots[i].changed)
            slot = i;
    }
    for (int i = 0; i < SlotCount && slot < 0; i++)
    {
        if (free(i))
            slot = i;
    }
    if (slot < 0)
        return -1;

    m_slots[slot].data = data;
    m_slots[slot].path = path;
    m_slots[slot].changed = true;
    return slot;
}

void EmulatedPortal::Remove(int slot)
{
    std::lock_guard lock(m_mutex);
    if (slot < 0 || slot >= SlotCount || !m_slots[slot].data)
        return;

    m_slots[slot].data.reset();
    m_slots[slot].path.clear();
    m_slots[slot].changed = true;
}

void EmulatedPortal::Renumber()
{
    std::lock_guard lock(m_mutex);
    int next = 0;
    for (int i = 0; i < SlotCount; i++)
    {
        Slot toy = std::move(m_slots[i]);
        m_slots[i] = {};
        if (!toy.data)
            continue;
        toy.changed = true;
        m_slots[next++] = std::move(toy);
    }
}

int EmulatedPortal::SlotOfSerial(const uint8_t* serial)
{
    std::lock_guard lock(m_mutex);
    for (int i = 0; i < SlotCount; i++)
    {
        if (m_slots[i].data && memcmp(m_slots[i].data->data(), serial, 4) == 0)
            return i;
    }
    return -1;
}

std::string EmulatedPortal::SlotPath(int slot)
{
    std::lock_guard lock(m_mutex);
    return slot >= 0 && slot < SlotCount && m_slots[slot].data ? m_slots[slot].path : std::string();
}

bool EmulatedPortal::Holds(int slot)
{
    std::lock_guard lock(m_mutex);
    return slot >= 0 && slot < SlotCount && m_slots[slot].data.has_value();
}

uint32_t EmulatedPortal::TakeStatus()
{
    std::lock_guard lock(m_mutex);
    return StatusBits();
}

bool EmulatedPortal::IsActive()
{
    std::lock_guard lock(m_mutex);
    return m_active;
}

// 'S' <LE32: 2 bits per slot, bit 0 = present, bit 1 = changed since the last report> <counter> <active>
uint32_t EmulatedPortal::StatusBits()
{
    uint32_t status = 0;
    for (int i = 0; i < SlotCount; i++)
    {
        uint32_t bits = (m_slots[i].data ? 1 : 0) | (m_slots[i].changed ? 2 : 0);
        m_slots[i].changed = false;
        status |= bits << (i * 2);
    }
    return status;
}

EmulatedPortal::Report EmulatedPortal::StatusReport()
{
    uint32_t status = StatusBits();

    Report report{};
    report[0] = 'S';
    report[1] = uint8_t(status);
    report[2] = uint8_t(status >> 8);
    report[3] = uint8_t(status >> 16);
    report[4] = uint8_t(status >> 24);
    report[5] = m_statusCounter++;
    report[6] = m_active ? 1 : 0;
    return report;
}

bool EmulatedPortal::Write(const uint8_t* command, size_t size)
{
    if (size == 0)
        return true;

    std::lock_guard lock(m_mutex);
    Report response{};
    response[0] = command[0];
    switch (command[0])
    {
    case 'R':
        m_active = false;
        m_responses.clear();
        // The id of the portal the running game was made for (host/portal_identity.h).
        memcpy(&response[1], m_model.id, m_model.idLength);
        break;

    case 'A':
        m_active = size > 1 && (command[1] & 1) != 0;
        response[1] = m_active ? 1 : 0;
        response[2] = BatteryFull;
        response[3] = ActivateTrailer;
        break;

    case 'S': response = StatusReport(); break;

    case 'C':
        // Colour of the portal light (Giants fades it all the time); real portals send no answer.
        m_light = { size > 1 ? command[1] : uint8_t(0), size > 2 ? command[2] : uint8_t(0),
            size > 3 ? command[3] : uint8_t(0) };
        m_lightCommands++;
        return true;

    case 'M':
        // The speaker, on or off. Trap Team stops reading figures until this is answered, which it asks for as soon
        // as a trap speaks.
        response[1] = size > 1 ? command[1] : uint8_t(0);
        response[3] = SpeakerTrailer;
        break;

    case 'Q':
    case 'W':
    {
        int slot = size > 1 ? command[1] & 0x0F : -1;
        size_t block = size > 2 ? command[2] : FigureBlockCount;
        response[2] = uint8_t(block);
        response[1] = Failure;
        if (slot < 0 || block >= FigureBlockCount || !m_slots[slot].data)
            break;

        uint8_t* data = m_slots[slot].data->data() + block * FigureBlockSize;
        if (command[0] == 'Q')
        {
            memcpy(&response[3], data, FigureBlockSize);
            response[1] = uint8_t(OneBlock | slot);
            break;
        }

        // Block 0 holds the tag's serial and cannot be written; sector trailers are left alone too.
        if (size < 3 + FigureBlockSize || block == 0 || IsSectorTrailer(block))
            break;
        memcpy(data, command + 3, FigureBlockSize);
        if (!m_slots[slot].path.empty() && !SaveFigure(m_slots[slot].path, *m_slots[slot].data))
            fprintf(stderr, "[portal] cannot save figure %s\n", m_slots[slot].path.c_str());
        response[1] = uint8_t(OneBlock | slot);
        break;
    }

    default:
        // Later games' commands (J, L, V: lights) are not known to be used by Spyro's Adventure or Giants:
        // logged once each, so that a new one shows up.
        if (!m_unknownSeen[command[0]])
        {
            m_unknownSeen[command[0]] = true;
            fprintf(stderr, "[portal] emulated portal ignores command 0x%02X (%zu bytes)\n", command[0], size);
        }
        return true;
    }

    m_responses.push_back(response);
    return true;
}

size_t EmulatedPortal::Read(uint8_t* report, size_t capacity)
{
    std::lock_guard lock(m_mutex);

    // Answers first. An active portal also streams status reports; an inactive one stays silent (the title takes an
    // unprompted "inactive" status for a portal that was just powered on, and resets it).
    Report next;
    if (!m_responses.empty())
    {
        next = m_responses.front();
        m_responses.pop_front();
    }
    else if (m_active)
    {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastStatus < m_statusInterval)
            return 0;
        m_lastStatus = now;
        next = StatusReport();
    }
    else
    {
        return 0;
    }

    size_t size = std::min(capacity, next.size());
    memcpy(report, next.data(), size);
    return size;
}
