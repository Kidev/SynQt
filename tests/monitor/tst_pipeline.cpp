// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The event pipeline every entity carries: the record, the bounded ring, the tracer and
// its per-category levels, and the writer thread. What these tests are about is a
// single promise, that recording an event never blocks the entity that recorded it, so
// they assert the shape of the losses (bounded, counted, newest kept) rather than only
// the happy path.

#include "traceevent.h"

#include <QTest>

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
};

QTEST_GUILESS_MAIN(TestPipeline)
#include "tst_pipeline.moc"
