// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The event pipeline every entity carries: the record, the bounded ring, the tracer and
// its per-category levels, and the writer thread. What these tests are about is a
// single promise, that recording an event never blocks the entity that recorded it, so
// they assert the shape of the losses (bounded, counted, newest kept) rather than only
// the happy path.

#include "caller.h"
#include "eventring.h"
#include "tracecontext.h"
#include "tracer.h"
#include "traceevent.h"

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
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

    // A number the wire carries that this build has no enumerator for.
    //
    // Severity and category cross the link as their numbers, so a reporting entity built
    // against a later vocabulary, or one that has been compromised and is choosing what
    // to send, can put any integer in either field. A static_cast alone turns that into a
    // value of the enum type anyway, and the event carrying it is then invisible to every
    // category filter the console offers and to the severity floor as well: it is written
    // down and cannot be found again, which is worse for whoever is reading the record than
    // never receiving it. It is also what keeps both enums inside the six-element array
    // `Tracer::isEnabled` indexes without checking.
    void aNumberOutsideTheVocabularyIsReadAsTheOrdinaryValue()
    {
        QVariantMap wire;
        wire.insert(QStringLiteral("severity"), 9999);
        wire.insert(QStringLiteral("category"), -3);
        wire.insert(QStringLiteral("message"), QStringLiteral("from somewhere newer"));

        const TraceEvent event{TraceEvent::fromVariant(wire)};
        QCOMPARE(event.severity, Severity::Info);
        QCOMPARE(event.category, Category::Lifecycle);
        // So a filter can find it.
        QCOMPARE(severityName(event.severity), QStringLiteral("info"));
        QCOMPARE(categoryName(event.category), QStringLiteral("lifecycle"));

        // A field that is not a number at all is the same case, and so is one that is
        // missing. Neither may become an enumerator nobody declared.
        QVariantMap nonsense;
        nonsense.insert(QStringLiteral("severity"), QStringLiteral("loud"));
        const TraceEvent guessed{TraceEvent::fromVariant(nonsense)};
        QCOMPARE(guessed.severity, Severity::Info);
        QCOMPARE(guessed.category, Category::Lifecycle);

        // And every value the vocabulary does have still crosses unchanged.
        for (int level{0}; level <= static_cast<int>(Severity::Fatal); ++level) {
            for (int which{0}; which <= static_cast<int>(Category::Application); ++which) {
                TraceEvent sent;
                sent.severity = static_cast<Severity>(level);
                sent.category = static_cast<Category>(which);
                const TraceEvent back{TraceEvent::fromVariant(sent.toVariant())};
                QCOMPARE(back.severity, sent.severity);
                QCOMPARE(back.category, sent.category);
            }
        }
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

    void anEventBelowItsCategoryLevelIsNotRecorded()
    {
        Tracer tracer;
        tracer.setLevel(Category::Call, Severity::Warning);
        QVERIFY(!tracer.isEnabled(Category::Call, Severity::Info));
        QVERIFY(tracer.isEnabled(Category::Call, Severity::Error));
        // Per category, not global. An operator turns call tracing down without losing
        // the refusals, which are the events worth alerting on.
        QVERIFY(tracer.isEnabled(Category::Authorization, Severity::Info));
    }

    void aCategoryTurnedOffRecordsNothingAtAnySeverity()
    {
        Tracer tracer;
        tracer.setCategoryOff(Category::Data);
        // Off is not "fatal and worse": it is the category refused, including the severity
        // nothing is above.
        QVERIFY(!tracer.isEnabled(Category::Data, Severity::Fatal));
        QVERIFY(tracer.isEnabled(Category::Call, Severity::Info));
    }

    void aLevelWrittenAsAWordIsReadBackAsTheSameOne()
    {
        // The round trip a deployment setting depends on. `monitoring.levels` is written in
        // words, and a word this build does not know has to be recognisable as unknown
        // rather than read as the quietest thing it could have meant.
        Severity severity{Severity::Info};
        QVERIFY(severityFromName(QStringLiteral("warning"), &severity));
        QCOMPARE(severity, Severity::Warning);
        QVERIFY(!severityFromName(QStringLiteral("verbose"), &severity));
        QCOMPARE(severity, Severity::Warning);

        Category category{Category::Application};
        QVERIFY(categoryFromName(QStringLiteral("authorization"), &category));
        QCOMPARE(category, Category::Authorization);
        QVERIFY(!categoryFromName(QStringLiteral("calls"), &category));

        for (int level{0}; level <= static_cast<int>(Severity::Fatal); ++level) {
            const Severity written{static_cast<Severity>(level)};
            Severity read{Severity::Info};
            QVERIFY(severityFromName(severityName(written), &read));
            QCOMPARE(read, written);
        }
    }

    void aDisabledTracerReportsNothingEnabledAndRemembersTheLevels()
    {
        Tracer tracer;
        tracer.setLevel(Category::Call, Severity::Error);
        tracer.setEnabled(false);
        QVERIFY(!tracer.isEnabled());
        QVERIFY(!tracer.isEnabled(Category::Authorization, Severity::Fatal));

        // Switching it back on restores what was configured rather than the defaults:
        // an operator who silences an entity for an hour does not lose their filters.
        tracer.setEnabled(true);
        QVERIFY(tracer.isEnabled(Category::Authorization, Severity::Info));
        QVERIFY(!tracer.isEnabled(Category::Call, Severity::Warning));
        QVERIFY(tracer.isEnabled(Category::Call, Severity::Error));
    }

    void aBatchReachesTheSinkOnceItIsFull()
    {
        // Declared before the tracer, and so destroyed after it. The sink runs on the
        // writer thread, and the tracer's destructor is what stops that thread.
        QMutex mutex;
        QList<TraceEvent> delivered;
        const auto count = [&mutex, &delivered]() {
            QMutexLocker locker{&mutex};
            return delivered.size();
        };

        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });
        tracer.setBatch(4, 60000);  // size trigger only, so the test is not timing-bound
        for (int index{0}; index < 4; ++index) {
            tracer.record(TraceEvent{});
        }
        QTRY_COMPARE(count(), 4);
    }

    void flushDeliversAPartialBatch()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;
        const auto count = [&mutex, &delivered]() {
            QMutexLocker locker{&mutex};
            return delivered.size();
        };

        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });
        tracer.setBatch(1024, 60000);
        tracer.record(TraceEvent{});
        tracer.flush();
        // flush() returns once the writer has drained, so this is a plain compare and
        // not a QTRY: a flush that has to be waited on afterwards is not a flush.
        QCOMPARE(count(), 1);
    }

    void theElapsedTriggerDeliversWithoutAFullBatch()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;
        const auto count = [&mutex, &delivered]() {
            QMutexLocker locker{&mutex};
            return delivered.size();
        };

        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });
        tracer.setBatch(1024, 20);
        tracer.record(TraceEvent{});
        // Nobody calls flush here, and the batch is nowhere near full. The timer lives
        // on the writer thread precisely so that an entity too busy to run its own event
        // loop still gets its events out.
        QTRY_COMPARE_WITH_TIMEOUT(count(), 1, 5000);
    }

    void recordingWithNoSinkIsHarmless()
    {
        // An entity with no monitor in its topology still traces into the ring. Nothing
        // reads it and nothing fails. That is what lets instrumentation be unconditional.
        Tracer tracer;
        for (int index{0}; index < 100; ++index) {
            tracer.record(TraceEvent{});
        }
        QVERIFY(true);
    }

    void aSpanContinuesItsParentTrace()
    {
        Tracer tracer;
        const TraceContext root{tracer.startSpan(TraceContext{}, QStringLiteral("upgrade"))};
        QCOMPARE(root.traceId.size(), 32);
        QCOMPARE(root.spanId.size(), 16);

        const TraceContext child{tracer.startSpan(root, QStringLiteral("placeBid"))};
        // Same trace, new span, parent recorded. That is what makes a click one story
        // across three entities rather than three unrelated lines.
        QCOMPARE(child.traceId, root.traceId);
        QVERIFY(child.spanId != root.spanId);
        QCOMPARE(child.parentSpanId, root.spanId);
    }

    void aTraceIdIsAValidW3cValue()
    {
        Tracer tracer;
        const TraceContext context{tracer.startSpan(TraceContext{}, QStringLiteral("x"))};
        // Lower-case hex and not all zeroes, or a collector rejects the traceparent.
        QVERIFY(QRegularExpression{QStringLiteral("^[0-9a-f]{32}$")}
                    .match(context.traceId).hasMatch());
        QVERIFY(QRegularExpression{QStringLiteral("^[0-9a-f]{16}$")}
                    .match(context.spanId).hasMatch());
        QVERIFY(context.traceId != QString(32, QLatin1Char('0')));
        QVERIFY(context.spanId != QString(16, QLatin1Char('0')));
    }

    void endingASpanRecordsHowLongItTook()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;
        const auto taken = [&mutex, &delivered]() {
            QMutexLocker locker{&mutex};
            return delivered;
        };

        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });
        const TraceContext span{tracer.startSpan(TraceContext{}, QStringLiteral("placeBid"))};
        tracer.endSpan(span, Category::Call, SpanOutcome::Refused);
        tracer.flush();

        const QList<TraceEvent> events{taken()};
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().category, Category::Call);
        QCOMPARE(events.first().traceId, span.traceId);
        QCOMPARE(events.first().spanId, span.spanId);
        QCOMPARE(events.first().message, QStringLiteral("placeBid"));
        QVERIFY(!events.first().ok);
        // A refusal is the event an operator alerts on, so it does not arrive as Info.
        QCOMPARE(events.first().severity, Severity::Warning);
        QVERIFY(events.first().durationUs >= 0);
    }

    void theTraceTravelsWithTheSessionAndABrowserCannotForgeIt()
    {
        QObject owner;
        Tracer tracer;
        const TraceContext edgeSpan{tracer.startSpan(TraceContext{}, QStringLiteral("call"))};

        QVariantMap forwarded;
        forwarded.insert(QStringLiteral("key"), QStringLiteral("k"));
        forwarded.insert(QStringLiteral("scope"), QStringLiteral("user"));
        forwarded.insert(QStringLiteral("traceId"), edgeSpan.traceId);
        forwarded.insert(QStringLiteral("spanId"), edgeSpan.spanId);

        // A service reached over the mesh. The peer is certificate-authenticated, so the
        // trace it says it is continuing is taken, exactly as the session is.
        Caller *entity{Caller::forEntity(QString{}, QStringLiteral("web"), true, nullptr, &owner)};
        entity->assumeSession(forwarded);
        QCOMPARE(entity->traceContext().traceId, edgeSpan.traceId);
        QCOMPARE(entity->traceContext().spanId, edgeSpan.spanId);
        // And it keeps travelling, or the chain stops at the first hop.
        QCOMPARE(entity->forwardedSession().value(QStringLiteral("traceId")).toString(),
                 edgeSpan.traceId);

        // A browser could put the same fields in a call. It is the one caller whose
        // assertions are never read, and a trace id is no different from a session key.
        Caller *user{Caller::forUser(QString{}, nullptr, QByteArray{}, nullptr, &owner)};
        user->assumeSession(forwarded);
        QVERIFY(!user->traceContext().isValid());
    }

    void whatOneEventMayCarryIsBounded()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;
        const auto taken = [&mutex, &delivered]() {
            QMutexLocker locker{&mutex};
            return delivered;
        };

        Tracer tracer;
        tracer.setEntity(QStringLiteral("web"));
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });

        // The shape of a hostile request being recorded. An Origin header a megabyte long.
        TraceEvent oversized;
        oversized.category = Category::Authorization;
        oversized.severity = Severity::Warning;
        oversized.message = QString(200000, QLatin1Char('a'));
        oversized.attributes.insert(QStringLiteral("origin"),
                                    QString(200000, QLatin1Char('b')));
        tracer.record(oversized);

        // And more attributes than any event has business carrying.
        TraceEvent crowded;
        crowded.category = Category::Authorization;
        for (int index{0}; index < 200; ++index) {
            crowded.attributes.insert(QStringLiteral("k%1").arg(index), index);
        }
        tracer.record(crowded);
        tracer.flush();

        const QList<TraceEvent> events{taken()};
        QCOMPARE(events.size(), 2);
        QCOMPARE(events.first().message.size(), Tracer::MaxMessageChars);
        QCOMPARE(events.first().attributes.value(QStringLiteral("origin")).toString().size(),
                 Tracer::MaxAttributeChars);
        // Recorded without saying where, so the tracer says it. A call site records what
        // happened, never where it happened.
        QCOMPARE(events.first().entity, QStringLiteral("web"));
        QVERIFY(events.first().timestampMs > 0);

        QVERIFY(events.last().attributes.size() <= Tracer::MaxAttributes);
        // And the part it shed is reported, not swallowed. An event that quietly loses
        // half of itself reads exactly like one that never had it.
        QCOMPARE(events.last().attributes.value(QStringLiteral("attributesDropped")).toInt(),
                 200 - (Tracer::MaxAttributes - 1));
    }
};

QTEST_GUILESS_MAIN(TestPipeline)
#include "tst_pipeline.moc"
