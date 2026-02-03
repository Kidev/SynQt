// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_EVENTRING_H
#define SYNQT_EVENTRING_H

#include "traceevent.h"

#include <QList>
#include <QMutex>

namespace SynQt {

/// A bounded queue of events, shared between the threads that record and the one thread
/// that writes.
///
/// It is bounded because the alternative is worse in exactly the situation monitoring
/// exists for. An unbounded queue under a burst grows until the entity is killed for its
/// memory, and a queue that blocks its producer makes every slot call wait on the
/// monitor. Either way the observability of an entity in trouble is what finishes it off.
/// So this one has a ceiling, and it drops the oldest to stay under it. What it never
/// does is lie about it: `dropped()` counts every event that did not fit, and the writer
/// reports that count so a gap in the record is visible as a gap.
///
/// Locking is one `QMutex`, not a lock-free queue, deliberately. The critical section is
/// a move and two integer updates, and it is uncontended in the common case, whereas a
/// lock-free ring is a great deal of subtle code to save nanoseconds on a path that is
/// already off the wire. Whether that judgement holds is measured rather than argued:
/// `benchmarks/monitor` is the number, and it is a committed baseline.
class EventRing
{
public:
    explicit EventRing(int capacity);

    /// Records an event, evicting the oldest when the ring is full. Returns whether it
    /// fit without evicting anything, which is what the drop counter is derived from;
    /// no caller is expected to do anything differently when it returns false.
    bool push(TraceEvent event);

    /// Takes up to `max` of the oldest events, in the order they were recorded.
    QList<TraceEvent> drain(int max);

    int size() const;
    int capacity() const;
    qint64 dropped() const;

private:
    mutable QMutex m_mutex;
    QList<TraceEvent> m_events;
    int m_capacity{0};
    int m_head{0};
    int m_size{0};
    qint64 m_dropped{0};
};

} // namespace SynQt

#endif // SYNQT_EVENTRING_H
