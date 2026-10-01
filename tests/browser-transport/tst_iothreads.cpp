// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The threads a threaded entity spreads its sockets across. The cases here are about the
// pool itself: that it runs the threads it was asked for, hands them out evenly, gives
// each one a live event loop, and stops them all when it goes.
//
// The event-loop case is the one that matters. A QWebSocket needs an event loop on its own
// thread for its socket notifiers to fire at all, which is why this pool is real QThreads
// and not QThreadPool or QtConcurrent: their worker threads run no event loop, so a socket
// moved onto one would go quiet with nothing reporting an error.

#include "iothreadpool.h"

#include <QSet>
#include <QTest>
#include <QThread>

using namespace SynQt;

namespace {

/// Records which thread ran it, so a test can prove a pool thread delivers a queued call
/// rather than merely existing.
class ThreadRecorder : public QObject
{
    Q_OBJECT

public:
    QThread *ranOn() const { return m_ranOn; }

public slots:
    void record()
    {
        m_ranOn = QThread::currentThread();
    }

private:
    QThread *m_ranOn{nullptr};
};

} // namespace

class TestIoThreads : public QObject
{
    Q_OBJECT

private slots:
    void poolRunsTheThreadsItWasAskedFor();
    void nextThreadHandsOutEveryThreadInTurn();
    void aPoolThreadRunsAnEventLoop();
    void destroyingThePoolStopsItsThreads();
};

void TestIoThreads::poolRunsTheThreadsItWasAskedFor()
{
    IoThreadPool pool{4};

    QCOMPARE(pool.threadCount(), 4);
    QSet<QThread *> seen;
    for (int index{0}; index < pool.threadCount(); ++index) {
        QThread *thread{pool.threadAt(index)};
        QVERIFY(thread != nullptr);
        QVERIFY(thread != QThread::currentThread());
        QVERIFY(thread->isRunning());
        seen.insert(thread);
    }
    QCOMPARE(seen.size(), 4);
}

void TestIoThreads::nextThreadHandsOutEveryThreadInTurn()
{
    IoThreadPool pool{3};

    QList<QThread *> handedOut;
    for (int call{0}; call < 6; ++call) {
        handedOut.append(pool.nextThread());
    }

    // Round robin, so the second pass over the pool repeats the first exactly. Evenness is
    // the point. A pool that answered with one thread twice as often would still pass a
    // "every thread appears" check while carrying twice the sockets on it.
    QCOMPARE(handedOut.mid(0, 3), handedOut.mid(3, 3));
    QCOMPARE(QSet<QThread *>(handedOut.begin(), handedOut.end()).size(), 3);
}

void TestIoThreads::aPoolThreadRunsAnEventLoop()
{
    IoThreadPool pool{2};
    QThread *thread{pool.nextThread()};

    ThreadRecorder recorder;
    recorder.moveToThread(thread);
    QMetaObject::invokeMethod(&recorder, "record", Qt::QueuedConnection);

    QTRY_COMPARE(recorder.ranOn(), thread);
}

void TestIoThreads::destroyingThePoolStopsItsThreads()
{
    int finished{0};
    {
        IoThreadPool pool{3};
        for (int index{0}; index < pool.threadCount(); ++index) {
            // `this` as the context, so the count is raised on the main thread and nothing
            // here touches a QThread the pool has already destroyed.
            connect(pool.threadAt(index), &QThread::finished, this, [&finished]() {
                ++finished;
            });
        }
    }

    QTRY_COMPARE(finished, 3);
}

QTEST_MAIN(TestIoThreads)

#include "tst_iothreads.moc"
