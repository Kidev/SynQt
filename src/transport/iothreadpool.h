// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IOTHREADPOOL_H
#define SYNQT_IOTHREADPOOL_H

#include <QList>
#include <QObject>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace SynQt {

/// The threads a threaded entity spreads its accepted sockets across.
///
/// Every socket belongs to one thread for its whole life, and the objects its traffic is
/// decoded into (the QtRO host, the Sources, the QML engine) stay where they are. Only the
/// socket moves, so the programming model is unchanged, and the pool hands out threads rather
/// than tasks.
///
/// These are QThreads running exec(), not QThreadPool or QtConcurrent workers: a QWebSocket
/// needs an event loop on its own thread for its socket notifiers to fire, and a pool worker
/// has none.
class IoThreadPool : public QObject
{
    Q_OBJECT

public:
    /// Start `threadCount` threads (at least one. A smaller number is raised to one, since
    /// a pool of none could answer nextThread() with nothing).
    explicit IoThreadPool(int threadCount, QObject *parent = nullptr);
    ~IoThreadPool() override;

    int threadCount() const;
    QThread *threadAt(int index) const;

    /// The thread the next socket goes on, round robin.
    ///
    /// The cost of a connection is unknown when it is accepted, and an even spread suits many
    /// similar browser connections.
    QThread *nextThread();

private:
    QList<QThread *> m_threads;
    int m_next{0};
};

} // namespace SynQt

#endif // SYNQT_IOTHREADPOOL_H
