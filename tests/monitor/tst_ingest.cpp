// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The half of monitoring that lives in a reporting entity. What it has to get right is
// what happens when the monitor is not there, which is the ordinary case: a monitor
// restarts, redeploys, or falls behind, and the entity reporting to it must not notice.
//
// The Replica is stood in for by a plain QObject with a `publish` slot, because that is
// all the client knows about it. It reaches the Replica by name, so it is agnostic to the
// generated type and this test reflects that coupling rather than compiling one in.

#include "ingestclient.h"
#include "traceevent.h"

#include <QDir>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRemoteObjectReplica>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QVariantList>

#include <atomic>
#include <thread>

using namespace SynQt;

namespace {

/// What the monitor's Replica looks like from here. One slot, taking one batch.
class MonitorStandIn : public QObject
{
    Q_OBJECT

public:
    QList<QVariantList> batches;

    QVariantList flattened() const
    {
        QVariantList all;
        for (const QVariantList &batch : batches) {
            all += batch;
        }
        return all;
    }

public slots:
    void publish(const QVariantList &events) { batches.append(events); }

signals:
    /// What a QRemoteObjectReplica says when its link goes: Valid to Suspect, and the
    /// signature is the one the client connects to by name.
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
};

QList<TraceEvent> events(int count, int from = 0)
{
    QList<TraceEvent> batch;
    batch.reserve(count);
    for (int index{0}; index < count; ++index) {
        TraceEvent event;
        event.timestampMs = 1750000000000 + from + index;
        event.category = Category::Application;
        event.message = QString::number(from + index);
        batch.append(event);
    }
    return batch;
}

/// The entity side of monitoring, on the entity's thread. The IngestClient and the Replicas
/// it is pointed at, retired the way a reconnect retires them. Everything here runs on the
/// thread this is moved to, which is what makes the client and its Replica share a thread
/// the way they do in a running entity. publish() is driven from another thread, as the
/// tracer's writer thread drives it.
class EntitySide : public QObject
{
    Q_OBJECT

public:
    IngestClient *client{nullptr};
    QObject *current{nullptr};
    std::atomic<long> swaps{0};
    std::atomic<long> consumed{0};

    // The last stand-in is the one no swap retired, and it holds every batch the storm's
    // tail delivered to it. Tens of megabytes a pass, kept for the life of the process.
    ~EntitySide() override { delete current; }

public slots:
    void bringUp() { client = new IngestClient{QString{}, 0, this}; swap(); }

    void swap()
    {
        MonitorStandIn *fresh{new MonitorStandIn{}};
        client->setReplica(fresh);
        // Deleted here, synchronously and on this (the Replica's own) thread. A reconnect
        // uses deleteLater, which frees at the next turn of this loop. Deleting now stands
        // in for that by forcing the free into the window a publish in flight on the writer
        // thread is reading the old Replica in. It is the deterministic form of the timing
        // a reconnect hits by chance.
        delete current;
        current = fresh;
        swaps.fetch_add(1, std::memory_order_relaxed);
    }

    /// Posted by the writer behind each publish, into the same queue. When this has run,
    /// the payload ahead of it has been handed on or dropped, and the writer may post more.
    void mark() { consumed.fetch_add(1, std::memory_order_release); }
};

} // namespace

class TestIngest : public QObject
{
    Q_OBJECT

private slots:
    void aLiveMonitorGetsTheBatchAndNothingIsSpooled()
    {
        QTemporaryDir dir;
        MonitorStandIn monitor;
        IngestClient client{dir.filePath(QStringLiteral("spool.bin")), 1 << 20};
        client.setReplica(&monitor);

        client.publish(events(3));
        // The publish is queued, which is not an implementation detail to work around
        // here. A Replica belongs to the thread that acquired it, and the reporting path
        // runs on the tracer's writer thread, so reaching into one directly is touching
        // another thread's QObject state. What crosses is one metacall carrying a list
        // that is already built, and this is where it is delivered.
        QTRY_COMPARE(monitor.flattened().size(), 3);
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));
    }

    void anUnreachableMonitorCostsTheEntityNothingAndLosesNothing()
    {
        QTemporaryDir dir;
        IngestClient client{dir.filePath(QStringLiteral("spool.bin")), 1 << 20};
        // No replica at all, which is what an entity that started before its monitor has.
        client.publish(events(100));
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(100));
        QCOMPARE(client.droppedBatches(), static_cast<qint64>(0));

        // And when it comes back, the window around the outage is there, in order.
        MonitorStandIn monitor;
        client.setReplica(&monitor);
        // '=' not '{}': brace-initializing a QVariantList from a QVariantList wraps it
        // in a single QVariant rather than copying it, because QVariant is constructible
        // from anything and the initializer_list overload wins.
        const QVariantList replayed = monitor.flattened();
        QCOMPARE(replayed.size(), 100);
        QCOMPARE(replayed.first().toMap().value(QStringLiteral("message")).toString(),
                 QStringLiteral("0"));
        QCOMPARE(replayed.last().toMap().value(QStringLiteral("message")).toString(),
                 QStringLiteral("99"));
        // Replayed once, not kept for the next reconnect as well.
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));
    }

    // The monitor goes away while the link is up, the ordinary outage: it restarts, or the
    // network between the two does. The Replica the entity holds does not disappear; QtRO
    // marks it Suspect and drops a call on it with a warning. Batches published between the
    // drop and the reconnect must go to the spool that exists for this window, or the
    // record has a hole exactly where an operator looks for what went wrong.
    void aMonitorThatGoesAwayIsSpooledForUntilItIsBack()
    {
        QTemporaryDir dir;
        MonitorStandIn monitor;
        IngestClient client{dir.filePath(QStringLiteral("spool.bin")), 1 << 20};
        client.setReplica(&monitor);
        client.publish(events(3));
        QTRY_COMPARE(monitor.flattened().size(), 3);

        // The link dropped. The same object, no longer valid.
        emit monitor.stateChanged(QRemoteObjectReplica::Suspect, QRemoteObjectReplica::Valid);
        client.publish(events(5, 3));
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(5));
        QTest::qWait(50);
        QCOMPARE(monitor.flattened().size(), 3);

        // Back, as the runtime brings it back. A fresh Replica, and everything the
        // outage held is replayed to it in order, then held nowhere else.
        MonitorStandIn fresh;
        client.setReplica(&fresh);
        const QVariantList replayed = fresh.flattened();
        QCOMPARE(replayed.size(), 5);
        QCOMPARE(replayed.first().toMap().value(QStringLiteral("message")).toString(),
                 QStringLiteral("3"));
        QCOMPARE(replayed.last().toMap().value(QStringLiteral("message")).toString(),
                 QStringLiteral("7"));
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));
        // And it is live again. The next batch goes straight through.
        client.publish(events(2, 8));
        QTRY_COMPARE(fresh.flattened().size(), 7);
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));
    }

    void theSpoolIsBoundedAndTheOverflowIsReported()
    {
        QTemporaryDir dir;
        // Small enough that forty batches are well over it, large enough to hold several.
        // A monitor that never comes back must not fill the disk of the entity it was
        // watching.
        constexpr qint64 cap{8192};
        IngestClient client{dir.filePath(QStringLiteral("spool.bin")), cap};
        for (int round{0}; round < 40; ++round) {
            client.publish(events(20, round * 20));
        }
        QVERIFY(QFileInfo{dir.filePath(QStringLiteral("spool.bin"))}.size() <= cap);
        QVERIFY(client.droppedBatches() > 0);

        MonitorStandIn monitor;
        client.setReplica(&monitor);
        const QVariantList replayed = monitor.flattened();  // '=': see above
        QVERIFY(!replayed.isEmpty());

        // The newest survived, not the oldest. The events just before a crash are the ones
        // worth having, and a spool that dropped the newest would keep the least useful
        // window there is. The overflow report comes after the replay, so the last thing
        // that was spooled is the one before it.
        QStringList spooled;
        for (const QVariant &event : replayed) {
            const QString message{event.toMap().value(QStringLiteral("message")).toString()};
            if (message != QStringLiteral("monitoring spool overflowed")) {
                spooled.append(message);
            }
        }
        // Something survived, or the assertions below would be reading an empty list.
        QVERIFY(!spooled.isEmpty());
        QCOMPARE(spooled.last(), QStringLiteral("799"));
        QVERIFY(spooled.first().toInt() > 0);

        // And the gap is named. A replay arriving with no word of what was lost would show
        // a quiet period where there had been an overflowing one.
        bool reported{false};
        for (const QVariant &event : replayed) {
            const QVariantMap map{event.toMap()};
            if (map.value(QStringLiteral("message")).toString()
                == QStringLiteral("monitoring spool overflowed")) {
                reported = true;
                QVERIFY(map.value(QStringLiteral("attributes")).toMap()
                            .value(QStringLiteral("droppedBatches")).toLongLong() > 0);
            }
        }
        QVERIFY2(reported, "the spool dropped batches and said nothing about it");
    }

    void aBatchLargerThanTheContractAllowsIsSplitRatherThanRefused()
    {
        QTemporaryDir dir;
        MonitorStandIn monitor;
        IngestClient client{dir.filePath(QStringLiteral("spool.bin")), 1 << 20};
        client.setReplica(&monitor);

        // The contract declares `list[512]`, and the owner's boundary refuses anything
        // longer. Splitting here is what keeps that refusal from meaning "lose 1300
        // events" the first time an entity has a busy second.
        client.publish(events(1300));
        QTRY_COMPARE(monitor.flattened().size(), 1300);
        for (const QVariantList &batch : monitor.batches) {
            QVERIFY(batch.size() <= 512);
        }
    }

    // An entity with no writable state directory has nowhere to keep what the monitor
    // missed, so it loses it. What it must not do is lose it quietly. This is the case
    // where the gap is total, and an operator reading the record has no other way to tell a
    // period when nothing happened from one when nothing could be kept.
    void anEntityWithNowhereToSpoolLosesTheEventsAndSaysSo()
    {
        MonitorStandIn monitor;
        IngestClient client{QString{}, 0};
        client.publish(events(5));
        QCOMPARE(client.droppedBatches(), static_cast<qint64>(1));
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));

        client.setReplica(&monitor);
        // Nothing was kept, so none of the five events comes back. What arrives is the one
        // record that says so.
        const QVariantList replayed = monitor.flattened();  // '=': see above
        QCOMPARE(replayed.size(), 1);
        const QVariantMap gap{replayed.first().toMap()};
        QCOMPARE(gap.value(QStringLiteral("message")).toString(),
                 QStringLiteral("monitoring spool overflowed"));
        QCOMPARE(gap.value(QStringLiteral("attributes")).toMap()
                     .value(QStringLiteral("droppedBatches")).toInt(), 1);
        // And it is reported once. The count is cleared by the replay that carried it.
        QCOMPARE(client.droppedBatches(), static_cast<qint64>(0));
    }

    // send() runs on the tracer's writer thread, while the Replica it publishes to belongs
    // to the entity's thread and is retired there on a reconnect. Reaching into the Replica
    // from the writer thread dereferences it there, and a reconnect can free it between the
    // mutex read and the metacall: a use-after-free inside QMetaObject (invokeMethodImpl),
    // invisible to AddressSanitizer because the read is in precompiled Qt. This drives that
    // shape: one thread publishes without pause while another swaps and frees the Replica
    // on its own thread. The hand-off is marshalled to the entity's thread, where the
    // Replica is touched and its deletion serialized, so the run completes.
    void publishingWhileTheReplicaIsRetiredNeverTouchesItOffItsThread()
    {
        QThread entity;
        EntitySide side;
        side.moveToThread(&entity);
        entity.start();

        // Bring the client up on the entity thread and wait until it is there.
        QMetaObject::invokeMethod(&side, "bringUp", Qt::BlockingQueuedConnection);

        std::atomic<bool> stop{false};
        std::thread writer{[&side, &stop]() {
            // A whole slice, so serializing it is a real window for the entity thread to
            // free the Replica in.
            const QList<TraceEvent> batch{events(400)};
            long published{0};
            while (!stop.load(std::memory_order_acquire)) {
                // A handful of payloads in the entity's queue at once, never more. Each is
                // half a megabyte of variants, and a writer that never waits builds them
                // faster than the loop drains them. The race is in the hand-off, not in
                // the depth of the queue, and an unbounded queue is gigabytes of fixture.
                if (published - side.consumed.load(std::memory_order_acquire) >= 16) {
                    std::this_thread::yield();
                    continue;
                }
                side.client->publish(batch);
                ++published;
                QMetaObject::invokeMethod(&side, "mark", Qt::QueuedConnection);
            }
        }};

        // Retire and free the Replica as fast as the entity loop will run it. Posted a
        // chunk at a time, with the next chunk held until the loop has reached this one:
        // the swaps and the publishes racing them share the entity's queue, and a queue
        // holding every swap at once runs every publish after the last swap rather than
        // against it. A chunk in flight keeps the loop swapping without pause and the
        // writer's payloads interleaved with the swaps.
        constexpr long target{200000};
        constexpr long chunk{2000};
        QElapsedTimer clock;
        clock.start();
        for (long posted{0}; posted < target && clock.elapsed() < 60000; posted += chunk) {
            for (long index{0}; index < chunk; ++index) {
                QMetaObject::invokeMethod(&side, "swap", Qt::QueuedConnection);
            }
            while (side.swaps.load() < posted && clock.elapsed() < 60000) {
                QThread::msleep(1);
            }
        }
        while (side.swaps.load() < target - 2000 && clock.elapsed() < 60000) {
            QThread::msleep(5);
        }

        stop.store(true, std::memory_order_release);
        writer.join();
        entity.quit();
        entity.wait();

        // Surviving is the assertion.
        QVERIFY(side.swaps.load() > 0);
    }
};

QTEST_GUILESS_MAIN(TestIngest)
#include "tst_ingest.moc"
