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
    // Allocated once, here, and never again. Growing the storage on the recording path is
    // the unbounded allocation this class exists to avoid.
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
    // Full: the write above landed on the oldest slot, so advancing the head is what
    // makes it the newest and forgets what was there.
    m_head = (m_head + 1) % m_capacity;
    ++m_dropped;
    return false;
}

QList<TraceEvent> EventRing::drain(int max)
{
    QMutexLocker locker{&m_mutex};
    // Clamped at zero as well as at the size held. A negative `max` would otherwise take a
    // negative count, which walks the head backwards through a modulo of a negative number
    // and grows m_size instead of shrinking it -- an out-of-range index on the next push,
    // reached by a caller doing nothing worse than passing a batch size it read from
    // somewhere. Every caller today clamps its own; a ring buffer should not need them to.
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
