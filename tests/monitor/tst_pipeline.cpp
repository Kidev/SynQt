// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The event pipeline every entity carries: the record, the bounded ring, the tracer and
// its per-category levels, and the writer thread. What these tests are about is a
// single promise, that recording an event never blocks the entity that recorded it, so
// they assert the shape of the losses (bounded, counted, newest kept) rather than only
// the happy path.

#include "eventring.h"
#include "traceevent.h"

#include <QTest>
#include <QThread>

using namespace SynQt;

class TestPipeline : public QObject
{
    Q_OBJECT

private slots:
    void anEventRoundTripsThroughAVariant()
    {
        TraceEvent event;
        event.timestampMs = 1750000000000;
        event.severity = Severity::Warning;
        event.category = Category::Authorization;
        event.entity = QStringLiteral("web");
        event.traceId = QStringLiteral("0af7651916cd43dd8448eb211c80319c");
        event.spanId = QStringLiteral("b7ad6b7169203331");
        event.message = QStringLiteral("slot refused");
        event.attributes.insert(QStringLiteral("member"), QStringLiteral("placeBid"));

        const TraceEvent back{TraceEvent::fromVariant(event.toVariant())};
        QCOMPARE(back.timestampMs, event.timestampMs);
        QCOMPARE(back.severity, event.severity);
        QCOMPARE(back.category, event.category);
        QCOMPARE(back.entity, event.entity);
        QCOMPARE(back.traceId, event.traceId);
        QCOMPARE(back.spanId, event.spanId);
        QCOMPARE(back.message, event.message);
        QCOMPARE(back.attributes.value(QStringLiteral("member")).toString(),
                 QStringLiteral("placeBid"));
    }

    void aFullRingDropsAndCountsRatherThanGrowing()
    {
        EventRing ring{4};
        for (int index{0}; index < 10; ++index) {
            TraceEvent event;
            event.message = QString::number(index);
            ring.push(event);
        }
        // Bounded: the capacity is the capacity, whatever arrives.
        QCOMPARE(ring.size(), 4);
        QCOMPARE(ring.dropped(), static_cast<qint64>(6));

        // The newest are kept, not the oldest. The burst before a crash is the part
        // worth having, and a ring that drops the newest keeps the least interesting
        // window of all.
        const QList<TraceEvent> drained{ring.drain(16)};
        QCOMPARE(drained.size(), 4);
        QCOMPARE(drained.first().message, QStringLiteral("6"));
        QCOMPARE(drained.last().message, QStringLiteral("9"));

        // Draining clears it, and the drop counter survives so the gap stays visible.
        QCOMPARE(ring.size(), 0);
        QCOMPARE(ring.dropped(), static_cast<qint64>(6));
    }

    void drainTakesAtMostWhatItIsAskedFor()
    {
        EventRing ring{8};
        for (int index{0}; index < 8; ++index) {
            ring.push(TraceEvent{});
        }
        QCOMPARE(ring.drain(3).size(), 3);
        QCOMPARE(ring.size(), 5);
    }

    void pushIsSafeFromManyThreads()
    {
        EventRing ring{1024};
        QList<QThread *> threads;
        for (int worker{0}; worker < 4; ++worker) {
            QThread *thread{QThread::create([&ring]() {
                for (int index{0}; index < 500; ++index) {
                    ring.push(TraceEvent{});
                }
            })};
            threads.append(thread);
            thread->start();
        }
        for (QThread *thread : std::as_const(threads)) {
            QVERIFY(thread->wait(10000));
            delete thread;
        }
        // 2000 pushed into 1024: nothing lost track of, nothing counted twice.
        QCOMPARE(ring.size() + ring.dropped(), static_cast<qint64>(2000));
    }
};

QTEST_GUILESS_MAIN(TestPipeline)
#include "tst_pipeline.moc"
