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
    // The closure outlives this call, so it owns its own copy of the callback; capturing
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
    while (!m_queue.isEmpty()) {
        QJSValue job{m_queue.takeFirst()};
        if (job.isCallable()) {
            job.call();
        }
    }
    m_draining = false;
}

int Jobs::queued() const
{
    // int, not qsizetype: this is a QML-visible count of a queue the entity bounds, and
    // QML has one integer type. The cast is written out because a 64-bit size silently
    // becoming a 32-bit one is the kind of conversion this project spells.
    return static_cast<int>(m_queue.size());
}

} // namespace SynQt
