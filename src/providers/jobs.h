// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_JOBS_H
#define SYNQT_JOBS_H

#include <QHash>
#include <QJSValue>
#include <QList>
#include <QObject>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The jobs helper exposed to a jobs entity's QML as `Jobs`: Qt-timer scheduling and a
/// bounded background work queue, run off the request path on the entity's event loop.
/// Internal only. The queue never grows past its bound (enqueue returns false when full),
/// so a flood of work cannot exhaust memory.
class Jobs : public QObject
{
    Q_OBJECT

public:
    explicit Jobs(int maxQueue = 1000, QObject *parent = nullptr);

    /// Run callback every intervalMs; returns a handle that cancel() stops.
    Q_INVOKABLE int every(int intervalMs, const QJSValue &callback);
    Q_INVOKABLE void cancel(int handle);

    /// Queue a one-shot job; returns false if the queue is full (the work is dropped, not
    /// buffered without bound).
    Q_INVOKABLE bool enqueue(const QJSValue &job);

    Q_INVOKABLE int queued() const; ///< pending jobs, for backpressure/tests

private:
    void drain();

    int m_maxQueue;
    int m_nextHandle{1};
    bool m_draining{false};
    QList<QJSValue> m_queue;
    /// Handle to repeating timer, for cancel(). The timers are children of this object, so
    /// they die with it; this map has to die with it too. It was a process-lifetime static
    /// keyed on the owner's address, which leaked an entry per Jobs instance and, worse,
    /// handed a later Jobs allocated at a recycled address the dead one's map, whose
    /// QTimers had already been destroyed with their old parent: cancel() then stopped a
    /// dangling pointer.
    QHash<int, QTimer *> m_timers;
};

} // namespace SynQt

#endif // SYNQT_JOBS_H
