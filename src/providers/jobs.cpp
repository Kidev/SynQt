// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "jobs.h"

#include <QTimer>

namespace SynQt {

Jobs::Jobs(int maxQueue, QObject *parent)
    : QObject{parent}
    , m_maxQueue{maxQueue > 0 ? maxQueue : 1}
{
}

int Jobs::every(int intervalMs, const QJSValue &callback)
{
    const int handle{m_nextHandle++};
    QTimer *timer{new QTimer{this}};
    timer->setInterval(intervalMs);
    // The closure outlives this call, so it owns its own copy of the callback. Capturing
    // the parameter by reference would dangle at the first timeout. Copy straight into the
    // capture (mutable because QJSValue::call is not const).
    connect(timer, &QTimer::timeout, this, [job = callback]() mutable {
        if (job.isCallable()) {
            job.call();
        }
    });
    m_timers.insert(handle, timer);
    timer->start();
    return handle;
}

void Jobs::cancel(int handle)
{
    if (QTimer *timer{m_timers.take(handle)}) {
        timer->stop();
        timer->deleteLater();
    }
}

bool Jobs::enqueue(const QJSValue &job)
{
    if (m_queue.size() >= m_maxQueue) {
        return false;  // bounded: reject rather than grow without limit
    }
    m_queue.append(job);
    if (!m_draining) {
        m_draining = true;
        QTimer::singleShot(0, this, [this]() { drain(); });
    }
    return true;
}

void Jobs::drain()
{
    // One pass runs what was waiting when it started, and no more. A job is ordinary QML and
    // may perfectly well enqueue the next one -- a batch that walks a list a page at a time is
    // exactly that shape -- and a loop that drained until the queue was empty would then never
    // return to the event loop at all. The entity stops answering its connect points, stops
    // reconnecting, stops reporting, and nothing says why: the queue is bounded, so it never
    // grows, and each pass through the loop looks like progress. Taking a pass at a time turns
    // that into a job that runs on every turn, which is what somebody writing it meant.
    qsizetype remaining{m_queue.size()};
    while (remaining > 0 && !m_queue.isEmpty()) {
        QJSValue job{m_queue.takeFirst()};
        --remaining;
        if (job.isCallable()) {
            job.call();
        }
    }
    if (m_queue.isEmpty()) {
        m_draining = false;
        return;
    }
    // What a job added while this pass ran. Asked for again rather than looped over, so
    // everything else waiting on this entity gets its turn in between. `m_draining` stays
    // set, which is what keeps enqueue() from asking for a second one.
    QTimer::singleShot(0, this, [this]() { drain(); });
}

int Jobs::queued() const
{
    // int, not qsizetype. This is a QML-visible count of a queue the entity bounds, and
    // QML has one integer type. The cast is written out because a 64-bit size silently
    // becoming a 32-bit one is the kind of conversion this project spells.
    return static_cast<int>(m_queue.size());
}

} // namespace SynQt
