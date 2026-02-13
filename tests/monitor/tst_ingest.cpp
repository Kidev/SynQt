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
#include <QObject>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantList>

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
        // Something survived, or the assertions below would be reading an empty list.
        for (const QVariant &event : replayed) {
            const QString message{event.toMap().value(QStringLiteral("message")).toString()};
            if (message != QStringLiteral("monitoring spool overflowed")) {
                spooled.append(message);
            }
        }
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

    void anEntityWithNowhereToSpoolLosesTheEventsAndCountsThem()
    {
        MonitorStandIn monitor;
        IngestClient client{QString{}, 0};
        client.publish(events(5));
        QCOMPARE(client.droppedBatches(), static_cast<qint64>(1));
        QCOMPARE(client.spooledEvents(), static_cast<qint64>(0));

        // Nothing was kept, so nothing is replayed; what it does not do is pretend
        // otherwise or fail.
        client.setReplica(&monitor);
        QCOMPARE(monitor.flattened().size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestIngest)
#include "tst_ingest.moc"
