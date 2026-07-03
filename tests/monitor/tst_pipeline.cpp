// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The event pipeline every entity carries: the record, the bounded ring, the tracer and
// its per-category levels, and the writer thread. What these tests are about is a
// single promise, that recording an event never blocks the entity that recorded it, so
// they assert the shape of the losses (bounded, counted, newest kept) rather than only
// the happy path.

#include "actingfor.h"
#include "caller.h"
#include "eventring.h"
#include "tracecontext.h"
#include "tracer.h"
#include "traceevent.h"
#include "tracescope.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

#include <thread>
#include <vector>
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

        // And it keeps travelling, or the chain stops at the first hop. What travels is
        // the span this entity opens under the one it was sent, as the generated slot body
        // does, and not the parent again. A call made further on is a child of this
        // call, which is what makes the console draw a chain and not a fan.
        const TraceContext own{tracer.startSpan(entity->traceContext(), QStringLiteral("insert"))};
        {
            const TraceScope scope{own};
            const ActingFor acting{entity};
            const QVariantMap onward{ActingFor::current()};
            QCOMPARE(onward.value(QStringLiteral("traceId")).toString(), edgeSpan.traceId);
            QCOMPARE(onward.value(QStringLiteral("spanId")).toString(), own.spanId);
            QCOMPARE(onward.value(QStringLiteral("key")).toString(), QStringLiteral("k"));
        }
        // Outside any span the map carries the session and no trace. Nothing is left on
        // the thread by a call that ended.
        {
            const ActingFor acting{entity};
            QVERIFY(!ActingFor::current().contains(QStringLiteral("traceId")));
        }

        // A browser could put the same fields in a call. It is the one caller whose
        // assertions are never read, and a trace id is no different from a session key.
        Caller *user{Caller::forUser(QString{}, nullptr, QByteArray{}, nullptr, &owner)};
        user->assumeSession(forwarded);
        QVERIFY(!user->traceContext().isValid());
    }

    /// A peer's trace identifiers are read as far as their shape, and no further.
    ///
    /// The session a mesh caller forwards is that entity's word, worth the certificate
    /// that got it through the handshake, and the trace rides in the same map. What the
    /// shape rule adds is a bound on what that word can do. The two fields are stored on
    /// this entity's Caller, stamped onto every record the call writes, and forwarded on
    /// under this entity's name, so a peer that could put any string there could fill the
    /// monitor with it and have every entity further down repeat it. Both identifiers have
    /// to be exactly what the tracer would mint, or neither is taken and the call starts
    /// a trace of its own.
    void aPeerNamesATraceOnlyInTheShapeTheTracerMints()
    {
        QObject owner;
        Caller *entity{Caller::forEntity(QString{}, QStringLiteral("web"), true, nullptr, &owner)};
        const QString goodTrace{QStringLiteral("0af7651916cd43dd8448eb211c80319c")};
        const QString goodSpan{QStringLiteral("b7ad6b7169203331")};

        auto sent = [](const QString &traceId, const QString &spanId) {
            QVariantMap forwarded;
            forwarded.insert(QStringLiteral("key"), QStringLiteral("k"));
            forwarded.insert(QStringLiteral("traceId"), traceId);
            forwarded.insert(QStringLiteral("spanId"), spanId);
            return forwarded;
        };

        entity->assumeSession(sent(goodTrace, goodSpan));
        QCOMPARE(entity->traceContext().traceId, goodTrace);
        QCOMPARE(entity->traceContext().spanId, goodSpan);

        const QList<QPair<QString, QString>> refused{
            {QString{QStringLiteral("a")}.repeated(1 << 20), goodSpan},  // unbounded
            {goodTrace.left(31), goodSpan},                             // short
            {goodTrace + QLatin1Char('0'), goodSpan},                   // long
            {goodTrace.toUpper(), goodSpan},                            // not the alphabet
            {QStringLiteral("0af7651916cd43dd8448eb211c80319g"), goodSpan},
            {QString{32, QLatin1Char('0')}, goodSpan},                  // all zeroes
            {goodTrace, QString{16, QLatin1Char('0')}},
            {goodTrace, goodSpan.left(15)},
            {goodTrace, QStringLiteral("<script>alert(1)")},            // 16 chars, still no
            {goodTrace, QString{}},                                     // one of the two
            {QString{}, goodSpan},
        };
        for (const QPair<QString, QString> &pair : refused) {
            entity->assumeSession(sent(pair.first, pair.second));
            QVERIFY2(!entity->traceContext().isValid(),
                     qPrintable(QStringLiteral("taken: '%1' / '%2'")
                                    .arg(pair.first.left(40), pair.second)));
            QVERIFY(entity->traceContext().traceId.isEmpty());
            QVERIFY(entity->traceContext().spanId.isEmpty());
        }

        // A map that holds a trace and no key is a call by an entity acting for nobody;
        // it is not a person with an empty name.
        QVariantMap traceOnly;
        traceOnly.insert(QStringLiteral("traceId"), goodTrace);
        traceOnly.insert(QStringLiteral("spanId"), goodSpan);
        entity->assumeSession(traceOnly);
        QVERIFY(!entity->hasSession());
        QCOMPARE(entity->traceContext().traceId, goodTrace);
        QVariantMap emptyKey;
        emptyKey.insert(QStringLiteral("key"), QString{});
        emptyKey.insert(QStringLiteral("scope"), QStringLiteral("admin"));
        entity->assumeSession(emptyKey);
        QVERIFY(!entity->hasSession());
        QVERIFY(entity->scope().isEmpty());
    }

    /// Two threads minting spans at once mint different ones.
    ///
    /// Every instrumented call opens a span, and a `threads: N` edge opens them on N
    /// threads at once, so the generator behind them is on the request path of a threaded
    /// entity. A process-wide one puts every thread through one mutex there, which is what
    /// the `open_span_threads_*` sweep in benchmarks/monitor measures. A per-thread one
    /// costs nothing and has a failure mode of its own, and this is that failure mode. A
    /// generator seeded per thread from anything but real entropy gives every thread the
    /// same stream, so two entities' spans, or two threads' of one entity, collide and the
    /// console shows one trace made of unrelated work. Silent, and worse than slow.
    void twoThreadsMintingAtOnceMintDifferentIdentifiers()
    {
        constexpr int kThreads{4};
        constexpr int kSpansPerThread{2000};
        QMutex guard;
        QStringList minted;
        minted.reserve(kThreads * kSpansPerThread * 2);

        std::vector<std::thread> pool;
        pool.reserve(kThreads);
        for (int thread{0}; thread < kThreads; ++thread) {
            pool.emplace_back([&guard, &minted]() {
                QStringList mine;
                mine.reserve(kSpansPerThread * 2);
                for (int index{0}; index < kSpansPerThread; ++index) {
                    const TraceContext span{
                        Tracer::instance()->startSpan(TraceContext{},
                                                      QStringLiteral("placeBid"))};
                    mine.append(span.traceId);
                    mine.append(span.spanId);
                }
                QMutexLocker locker{&guard};
                minted.append(mine);
            });
        }
        for (std::thread &one : pool) {
            one.join();
        }

        QCOMPARE(minted.size(), kThreads * kSpansPerThread * 2);
        for (const QString &identifier : std::as_const(minted)) {
            QVERIFY2(TraceContext::isTraceId(identifier) || TraceContext::isSpanId(identifier),
                     qPrintable(QStringLiteral("not an identifier: '%1'").arg(identifier)));
        }
        const QSet<QString> distinct{minted.cbegin(), minted.cend()};
        QCOMPARE(distinct.size(), minted.size());
    }

    /// A reported identifier is a trace identifier, or it is not stored as one.
    ///
    /// Severities and categories cross the ingest link as numbers and are checked against
    /// the vocabulary; trace identifiers cross as strings and are checked here. A reporting
    /// entity's certificate says who it is, not that everything it sends is well formed,
    /// and an unchecked string would put a value and length of its choosing into a history
    /// column, every export, and the operator's console. The console asks for a trace by
    /// `string[32]`, so a longer one could never be followed either: recorded and
    /// unfindable.
    void aReportedTraceIdentifierIsOneOrIsNotStoredAsOne()
    {
        const QString goodTrace{QStringLiteral("0af7651916cd43dd8448eb211c80319c")};
        const QString goodSpan{QStringLiteral("b7ad6b7169203331")};

        QVariantMap wire;
        wire.insert(QStringLiteral("traceId"), goodTrace);
        wire.insert(QStringLiteral("spanId"), goodSpan);
        wire.insert(QStringLiteral("parentSpanId"), QStringLiteral("00f067aa0ba902b7"));
        const TraceEvent kept{TraceEvent::fromVariant(wire)};
        QCOMPARE(kept.traceId, goodTrace);
        QCOMPARE(kept.spanId, goodSpan);
        QCOMPARE(kept.parentSpanId, QStringLiteral("00f067aa0ba902b7"));

        QVariantMap forged;
        forged.insert(QStringLiteral("traceId"), QString{QStringLiteral("a")}.repeated(1 << 16));
        forged.insert(QStringLiteral("spanId"), QStringLiteral("'; DROP TABLE"));
        forged.insert(QStringLiteral("parentSpanId"), goodTrace);  // right alphabet, wrong length
        forged.insert(QStringLiteral("message"), QStringLiteral("still a record"));
        const TraceEvent cleaned{TraceEvent::fromVariant(forged)};
        QVERIFY(cleaned.traceId.isEmpty());
        QVERIFY(cleaned.spanId.isEmpty());
        QVERIFY(cleaned.parentSpanId.isEmpty());
        // The record is kept. What was wrong with it was the identifiers, not the event,
        // and an operator reading a quiet period must not be reading a hole.
        QCOMPARE(cleaned.message, QStringLiteral("still a record"));

        // A root span carries no parent, which is not the same as a malformed one.
        QVariantMap root;
        root.insert(QStringLiteral("traceId"), goodTrace);
        root.insert(QStringLiteral("spanId"), goodSpan);
        const TraceEvent rooted{TraceEvent::fromVariant(root)};
        QCOMPARE(rooted.traceId, goodTrace);
        QVERIFY(rooted.parentSpanId.isEmpty());
    }

    /// A credential handed to a trace call is not what gets recorded.
    ///
    /// The framework's own call sites record handles, and that is the property. This is
    /// the backstop under it, for an application's `Log` and for a call site added later.
    void aValueUnderASecretNameIsNeverWhatIsRecorded()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;

        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });

        // What an application writes when it is trying to work out why a request was
        // refused, and what a log then holds forever afterwards.
        TraceEvent event;
        event.category = Category::Application;
        event.severity = Severity::Warning;
        event.message = QStringLiteral("refused");
        event.attributes.insert(QStringLiteral("Authorization"),
                                QStringLiteral("Bearer ya29.a0AfB_real_token"));
        event.attributes.insert(QStringLiteral("set-cookie"),
                                QStringLiteral("synqt_session=abcdef; HttpOnly"));
        event.attributes.insert(QStringLiteral("refreshToken"), QStringLiteral("1//0eXyZ"));
        event.attributes.insert(QStringLiteral("clientSecret"), QStringLiteral("s3cr3t"));
        event.attributes.insert(QStringLiteral("x-api-key"), QStringLiteral("k-1234"));
        event.attributes.insert(QStringLiteral("db.password"), QStringLiteral("hunter2"));
        // And the things an operator is reading the record for, which stay.
        event.attributes.insert(QStringLiteral("member"), QStringLiteral("placeBid"));
        event.attributes.insert(QStringLiteral("session"), QStringLiteral("9f86d081"));
        event.attributes.insert(QStringLiteral("client_id"), QStringLiteral("app-42"));
        tracer.record(event);
        tracer.flush();

        QMutexLocker locker{&mutex};
        QCOMPARE(delivered.size(), 1);
        const QVariantMap recorded{delivered.first().attributes};
        for (const QString &name : {QStringLiteral("Authorization"),
                                    QStringLiteral("set-cookie"),
                                    QStringLiteral("refreshToken"),
                                    QStringLiteral("clientSecret"),
                                    QStringLiteral("x-api-key"),
                                    QStringLiteral("db.password")}) {
            // The name stays. A record that shed the key would read as a request that
            // never carried one.
            QVERIFY2(recorded.contains(name), qPrintable(name));
            QCOMPARE(recorded.value(name).toString(), Tracer::redacted());
        }
        QCOMPARE(recorded.value(QStringLiteral("member")).toString(),
                 QStringLiteral("placeBid"));
        QCOMPARE(recorded.value(QStringLiteral("session")).toString(),
                 QStringLiteral("9f86d081"));
        QCOMPARE(recorded.value(QStringLiteral("client_id")).toString(),
                 QStringLiteral("app-42"));
    }

    /// The names it holds back, and the ones it must not. A redaction that took an
    /// operator's own evidence away would teach them the marker means nothing much.
    void theNamesHeldBackAreTheOnesThatMeanTheCredentialItself()
    {
        for (const QString &name : {QStringLiteral("password"),
                                    QStringLiteral("PASSWORD"),
                                    QStringLiteral("passphrase"),
                                    QStringLiteral("client_secret"),
                                    QStringLiteral("access_token"),
                                    QStringLiteral("authToken"),
                                    QStringLiteral("Authorization"),
                                    QStringLiteral("proxy-authorization"),
                                    QStringLiteral("Cookie"),
                                    QStringLiteral("credentials"),
                                    QStringLiteral("apiKey"),
                                    QStringLiteral("api_key"),
                                    QStringLiteral("private_key"),
                                    QStringLiteral("bearer")}) {
            QVERIFY2(Tracer::isSecretAttributeName(name), qPrintable(name));
        }
        for (const QString &name : {QStringLiteral("member"),
                                    QStringLiteral("session"),
                                    QStringLiteral("key"),
                                    QStringLiteral("id"),
                                    QStringLiteral("client_id"),
                                    QStringLiteral("peer"),
                                    QStringLiteral("origin"),
                                    QStringLiteral("scope"),
                                    QStringLiteral("reason"),
                                    QStringLiteral("connectPoint")}) {
            QVERIFY2(!Tracer::isSecretAttributeName(name), qPrintable(name));
        }
    }

    // The same bound, for a value that is not text. A slot the contract marked `capture`
    // records what a caller passed, and a `list` or a `var` argument arrives as a list or
    // a map, not a string. Measured by what it serializes to, or a caller who found such
    // a member could hand the ring a megabyte per call and the monitor's disk the same.
    void aValueThatIsNotTextIsBoundedByWhatItSerializesTo()
    {
        QMutex mutex;
        QList<TraceEvent> delivered;
        Tracer tracer;
        tracer.setSink([&mutex, &delivered](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&mutex};
            delivered.append(batch);
        });

        QVariantList rows;
        for (int index{0}; index < 20000; ++index) {
            rows.append(QStringLiteral("row %1").arg(index));
        }
        QVariantMap nested;
        nested.insert(QStringLiteral("text"), QString(200000, QLatin1Char('n')));
        TraceEvent captured;
        captured.category = Category::Call;
        captured.attributes.insert(QStringLiteral("rows"), rows);
        captured.attributes.insert(QStringLiteral("blob"), QByteArray(200000, 'b'));
        captured.attributes.insert(QStringLiteral("nested"), nested);
        captured.attributes.insert(QStringLiteral("count"), 3);
        tracer.record(captured);
        tracer.flush();

        QMutexLocker locker{&mutex};
        QCOMPARE(delivered.size(), 1);
        const QVariantMap attributes{delivered.first().attributes};
        const qsizetype serialized{
            QJsonDocument{QJsonObject::fromVariantMap(attributes)}
                .toJson(QJsonDocument::Compact).size()};
        QVERIFY2(serialized <= Tracer::MaxAttributes * Tracer::MaxAttributeChars,
                 qPrintable(QStringLiteral("%1 bytes of attributes reached the ring")
                                .arg(serialized)));
        // What was too large is marked as dropped. What fit is kept.
        QVERIFY(attributes.value(QStringLiteral("rows")).toString().contains(
            QStringLiteral("dropped")));
        QCOMPARE(attributes.value(QStringLiteral("count")).toInt(), 3);
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
