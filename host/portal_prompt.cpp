#include "host/portal_prompt.h"

namespace portalprompt
{
    Tracker::Event Tracker::Update(const Prompt& polled, Prompt& appeared)
    {
        if (polled == m_pending)
            m_count++;
        else
        {
            m_pending = polled;
            m_count = 1;
        }
        if (m_count < Stable || m_pending == m_current)
            return Event::None;

        bool hadPrompt = m_current.kind == Kind::Player; // the menu was opened for it
        m_current = m_pending;
        if (m_current.kind == Kind::Player)
        {
            appeared = m_current;
            return Event::Appeared;
        }
        return hadPrompt ? Event::Gone : Event::None;
    }
}
