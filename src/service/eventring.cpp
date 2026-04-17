// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "eventring.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>

namespace SynQt {

EventRing::EventRing(int capacity)
    : m_capacity{std::max(1, capacity)}
{
    // Allocated once, here: growing on the recording path is the unbounded allocation this
    // class prevents.
    m_events.resize(m_capacity);
}

bool EventRing::push(TraceEvent event)
{
    QMutexLocker locker{&m_mutex};
    const int tail{(m_head + m_size) % m_capacity};
    m_events[tail] = std::move(event);
    if (m_size < m_capacity) {
        ++m_size;
        return true;
    }
    // Full: the write replaced the oldest slot, so advancing the head makes it the newest.
    m_head = (m_head + 1) % m_capacity;
    ++m_dropped;
    return false;
}

QList<TraceEvent> EventRing::drain(int max)
{
    QMutexLocker locker{&m_mutex};
    // Clamped at zero as well as at the size: a negative `max` would walk the head
    // backwards through a negative modulo, grow m_size, and index out of range on the next
    // push.
    const int taken{std::clamp(max, 0, m_size)};
    QList<TraceEvent> events;
    events.reserve(taken);
    for (int index{0}; index < taken; ++index) {
        const int slot{(m_head + index) % m_capacity};
        events.append(std::move(m_events[slot]));
        m_events[slot] = TraceEvent{};
    }
    m_head = (m_head + taken) % m_capacity;
    m_size -= taken;
    return events;
}

int EventRing::size() const
{
    QMutexLocker locker{&m_mutex};
    return m_size;
}

int EventRing::capacity() const
{
    QMutexLocker locker{&m_mutex};
    return m_capacity;
}

qint64 EventRing::dropped() const
{
    QMutexLocker locker{&m_mutex};
    return m_dropped;
}

} // namespace SynQt
