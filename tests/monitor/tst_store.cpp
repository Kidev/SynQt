// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitor's history. What it has to get right is the two things an operator does with
// it: ask it questions, and not have it fill the disk.

#include "eventstore.h"
#include "monitorservice.h"
#include "operatorstore.h"
#include "traceevent.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace SynQt;

namespace {

TraceEvent made(const QString &entity, Category category, Severity severity,
                const QString &message, qint64 ts)
{
    TraceEvent event;
    event.timestampMs = ts;
    event.severity = severity;
    event.category = category;
    event.entity = entity;
    event.message = message;
    event.attributes.insert(QStringLiteral("member"), QStringLiteral("placeBid"));
    return event;
}

} // namespace

class TestStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
                 "the QSQLITE driver is missing; the monitor cannot keep a history without it");
    }

    void aBatchIsOneTransactionAndNotOneWritePerEvent()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY2(store.open(), qPrintable(store.errorString()));

        QList<TraceEvent> batch;
        batch.reserve(10000);
        for (int index{0}; index < 10000; ++index) {
            batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                              QStringLiteral("call %1").arg(index), 1750000000000 + index));
        }
        QElapsedTimer clock;
        clock.start();
        QVERIFY2(store.append(batch), qPrintable(store.errorString()));
        // Ten thousand events committed one at a time is ten thousand fsyncs. The number
        // is generous, because what it refuses is a per-event commit rather than a slow disk.
        QVERIFY2(clock.elapsed() < 10000,
                 "appending one batch took long enough to suggest a commit per event");
        QCOMPARE(store.count(), static_cast<qint64>(10000));
    }

    void theStoreIsStrictAndJournalsAhead()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());

        QSqlDatabase db{QSqlDatabase::database(QSqlDatabase::connectionNames().last())};
        QSqlQuery query{db};

        // STRICT: a column declared to hold an integer holds an integer. Without it SQLite
        // stores whatever it is handed, and a wrong type reads back wrong months later
        // instead of failing at the write.
        QVERIFY(query.exec(QStringLiteral("SELECT sql FROM sqlite_master "
                                          "WHERE name='events'")));
        QVERIFY(query.next());
        QVERIFY2(query.value(0).toString().contains(QStringLiteral("STRICT")),
                 qPrintable(query.value(0).toString()));

        // WAL, so a console reading the history never blocks an entity's batch.
        QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString().toLower(), QStringLiteral("wal"));
    }

    void theHistoryAnswersTheQuestionsAnOperatorAsks()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());

        const qint64 now{QDateTime::currentMSecsSinceEpoch()};
        QList<TraceEvent> batch;
        batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                          QStringLiteral("placeBid answered"), now - 1000));
        batch.append(made(QStringLiteral("web"), Category::Authorization, Severity::Warning,
                          QStringLiteral("upgrade refused"), now - 500));
        batch.append(made(QStringLiteral("db"), Category::Data, Severity::Info,
                          QStringLiteral("rows published"), now - 100));
        batch.append(made(QStringLiteral("db"), Category::Call, Severity::Error,
                          QStringLiteral("query failed"), now - 50));
        QVERIFY(store.append(batch));

        EventQuery byEntity;
        byEntity.entities = {QStringLiteral("db")};
        QCOMPARE(store.query(byEntity).size(), 2);

        EventQuery byCategory;
        byCategory.categories = {Category::Call};
        QCOMPARE(store.query(byCategory).size(), 2);

        // "Warnings and worse", which is the filter an operator leaves switched on.
        EventQuery bySeverity;
        bySeverity.minimumSeverity = Severity::Warning;
        QCOMPARE(store.query(bySeverity).size(), 2);

        EventQuery byWindow;
        byWindow.fromMs = now - 600;
        QCOMPARE(store.query(byWindow).size(), 3);

        EventQuery bySearch;
        bySearch.search = QStringLiteral("refused");
        const QList<TraceEvent> found{store.query(bySearch)};
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().message, QStringLiteral("upgrade refused"));

        // Newest first. What happened is what an operator opened the console for.
        const QList<TraceEvent> everything{store.query(EventQuery{})};
        QCOMPARE(everything.size(), 4);
        QCOMPARE(everything.first().message, QStringLiteral("query failed"));
        // And an event comes back whole, attributes included.
        QCOMPARE(everything.first().attributes.value(QStringLiteral("member")).toString(),
                 QStringLiteral("placeBid"));
    }

    void oneClickComesBackAsOneStory()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());

        const QString trace{QStringLiteral("0af7651916cd43dd8448eb211c80319c")};
        QList<TraceEvent> batch;
        for (const QString &entity : {QStringLiteral("web"), QStringLiteral("db"),
                                      QStringLiteral("cache")}) {
            TraceEvent event{made(entity, Category::Call, Severity::Info,
                                  QStringLiteral("hop"), 1750000000000)};
            event.traceId = trace;
            batch.append(event);
        }
        batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                          QStringLiteral("unrelated"), 1750000000001));
        QVERIFY(store.append(batch));

        EventQuery byTrace;
        byTrace.traceId = trace;
        QCOMPARE(store.query(byTrace).size(), 3);
    }

    void whatAnOperatorTypesIsNeverSql()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());
        QVERIFY(store.append({made(QStringLiteral("web"), Category::Call, Severity::Info,
                                   QStringLiteral("hello"), 1750000000000)}));

        // The search box is the one field a person writes freely. Bound as a parameter
        // like every other value, so this is a search that finds nothing rather than a
        // statement that runs.
        EventQuery hostile;
        hostile.search = QStringLiteral("x'; DROP TABLE events; --");
        store.query(hostile);
        QCOMPARE(store.count(), static_cast<qint64>(1));

        EventQuery byEntity;
        byEntity.entities = {QStringLiteral("web'; DROP TABLE events; --")};
        QCOMPARE(store.query(byEntity).size(), 0);
        QCOMPARE(store.count(), static_cast<qint64>(1));
    }

    // How many rows one question may return.
    //
    // The number arrives from a console over a connect point as a plain `int` (the
    // contract vocabulary sizes strings and lists and has nothing to say about integers),
    // and every row it asks for is built into a QVariantList and serialized back over the
    // link. Unbounded, one question would be a way to make the monitor materialize its whole
    // table at once, which is the one thing every other stage of this pipeline (the ring,
    // the batch, the spool, the retention) is bounded against.
    void oneQuestionCannotAskForTheWholeTable()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY2(store.open(), qPrintable(store.errorString()));

        QList<TraceEvent> batch;
        const int stored{EventQuery::MaxRows + 500};
        batch.reserve(stored);
        for (int index{0}; index < stored; ++index) {
            batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                              QStringLiteral("call %1").arg(index), 1750000000000 + index));
        }
        QVERIFY(store.append(batch));

        EventQuery greedy;
        greedy.limit = 1000000000;
        QCOMPARE(store.query(greedy).size(), EventQuery::MaxRows);

        // And the other end, so the clamp is a range rather than a ceiling. A limit of zero
        // or less is one row, not none and not everything.
        EventQuery none;
        none.limit = 0;
        QCOMPARE(store.query(none).size(), 1);
        EventQuery negative;
        negative.limit = -5;
        QCOMPARE(store.query(negative).size(), 1);
    }

    void retentionDropsTheOldestAndKeepsTheNewest()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());

        const qint64 now{QDateTime::currentMSecsSinceEpoch()};
        QList<TraceEvent> batch;
        batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                          QStringLiteral("ancient"), now - (40LL * 86400000LL)));
        batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                          QStringLiteral("recent"), now - 1000));
        QVERIFY(store.append(batch));

        QVERIFY(store.retire(30, 0));
        const QList<TraceEvent> left{store.query(EventQuery{})};
        QCOMPARE(left.size(), 1);
        QCOMPARE(left.first().message, QStringLiteral("recent"));
    }

    void aStoreLeftRunningDoesNotFillTheDisk()
    {
        QTemporaryDir dir;
        const QString path{dir.filePath(QStringLiteral("events.db"))};
        EventStore store{path};
        QVERIFY(store.open());

        for (int round{0}; round < 12; ++round) {
            QList<TraceEvent> batch;
            batch.reserve(2000);
            for (int index{0}; index < 2000; ++index) {
                batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                                  QStringLiteral("event %1 %2").arg(round).arg(index),
                                  1750000000000 + (round * 2000) + index));
            }
            QVERIFY(store.append(batch));
        }
        const qint64 before{store.count()};
        QVERIFY(before > 0);

        // A monitor left running is a monitor whose store grows until the disk is full,
        // and a full disk is an outage caused by the thing that reports outages.
        QVERIFY(store.retire(0, 256 * 1024));
        QVERIFY2(store.count() < before, "retention removed nothing");
        QVERIFY(QFileInfo{path}.size() <= (512 * 1024));

        // The newest survived. They are what an operator is reading when something has
        // gone wrong.
        const QList<TraceEvent> left{store.query(EventQuery{})};
        QVERIFY(!left.isEmpty());
        QCOMPARE(left.first().message, QStringLiteral("event 11 1999"));
    }

    // A sweep runs every few minutes on the monitor's only thread. A store under its cap
    // has nothing to give back, and rewriting it (VACUUM) on every sweep would stall the
    // console for as long as the rewrite takes, which grows with the store.
    void aStoreUnderItsCapIsNotRewritten()
    {
        QTemporaryDir dir;
        const QString path{dir.filePath(QStringLiteral("events.db"))};
        EventStore store{path};
        QVERIFY(store.open());
        QList<TraceEvent> batch;
        for (int index{0}; index < 500; ++index) {
            batch.append(made(QStringLiteral("web"), Category::Call, Severity::Info,
                              QStringLiteral("event %1").arg(index), 1750000000000 + index));
        }
        QVERIFY(store.append(batch));
        QVERIFY(store.retire(0, 64 * 1024 * 1024));  // settle whatever the first sweep does
        const QDateTime written{QFileInfo{path}.lastModified()};
        const qint64 rows{store.count()};

        QTest::qWait(20);
        QVERIFY(store.retire(0, 64 * 1024 * 1024));
        QCOMPARE(QFileInfo{path}.lastModified(), written);
        QCOMPARE(store.count(), rows);
    }

    // The console's gate. An operator with the right password is let in, and nobody else:
    // a wrong password, a name nobody configured, and a monitor with no operator store at
    // all are the same refusal.
    void theConsoleOpensForAnOperatorAndNobodyElse()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());

        MonitorService withoutOperators{&store, nullptr, MonitorService::Retention{}};
        QVERIFY(!withoutOperators.signIn(QStringLiteral("ada"), QStringLiteral("s3cret")));

        OperatorStore operators;
        QVERIFY(operators.add(OperatorStore::mint(QStringLiteral("ada"),
                                                  QStringLiteral("s3cret"))));
        MonitorService service{&store, &operators, MonitorService::Retention{}};
        QVERIFY(service.signIn(QStringLiteral("ada"), QStringLiteral("s3cret")));
        QVERIFY(!service.signIn(QStringLiteral("ada"), QStringLiteral("wrong")));
        QVERIFY(!service.signIn(QStringLiteral("bob"), QStringLiteral("s3cret")));
    }

    // What the console is shown: the rows its model declares, each under the name the
    // transport verified rather than the one the batch carried, filtered the way the
    // operator asked, and a misspelled severity showing everything rather than nothing.
    void theConsoleIsAnsweredUnderTheVerifiedNameAndTheOperatorsFilter()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());
        MonitorService service{&store, nullptr, MonitorService::Retention{}};

        TraceEvent refused{made(QStringLiteral("claimed-name"), Category::Authorization,
                                Severity::Warning, QStringLiteral("slot refused"), 1000)};
        refused.ok = false;
        // Both identifiers, in the tracer's shape: a record naming only one is stored with
        // neither.
        refused.traceId = QStringLiteral("4bf92f3577b34da6a3ce929d0e0e4736");
        refused.spanId = QStringLiteral("00f067aa0ba902b7");
        refused.durationUs = 2500;
        const TraceEvent routine{made(QStringLiteral("claimed-name"), Category::Call,
                                      Severity::Info, QStringLiteral("call"), 2000)};
        service.take({refused.toVariant(), routine.toVariant()}, QStringLiteral("web"));
        service.take({routine.toVariant()}, QStringLiteral("db"));

        // `=`, not braces, for these three: a QVariantList converts to a QVariant, so a braced
        // copy would be a list holding the list.
        const QVariantList everything =
            service.ask(QString{}, QString{}, QStringLiteral("no such word"), 0);
        QCOMPARE(everything.size(), 3);
        for (const QVariant &row : everything) {
            QVERIFY(row.toMap().value(QStringLiteral("entity")).toString()
                    != QStringLiteral("claimed-name"));
        }

        const QVariantList warnings =
            service.ask(QString{}, QStringLiteral("web"), QStringLiteral("warning"), 10);
        QCOMPARE(warnings.size(), 1);
        const QVariantMap row{warnings.first().toMap()};
        QCOMPARE(row.value(QStringLiteral("entity")).toString(), QStringLiteral("web"));
        QCOMPARE(row.value(QStringLiteral("severity")).toString(), QStringLiteral("warning"));
        QCOMPARE(row.value(QStringLiteral("category")).toString(),
                 QStringLiteral("authorization"));
        QCOMPARE(row.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(row.value(QStringLiteral("durationMs")).toDouble(), 2.5);
        QVERIFY(row.value(QStringLiteral("attributes")).toString()
                    .contains(QStringLiteral("member=placeBid")));

        const QVariantList story = service.follow(refused.traceId);
        QCOMPARE(story.size(), 1);
        QCOMPARE(story.first().toMap().value(QStringLiteral("message")).toString(),
                 QStringLiteral("slot refused"));
    }

    // The health strip: one row per entity heard from, its refusals counted, and live
    // while it keeps reporting. A heartbeat alone keeps an idle entity live.
    void anEntityIsLiveWhileItKeepsReporting()
    {
        QTemporaryDir dir;
        EventStore store{dir.filePath(QStringLiteral("events.db"))};
        QVERIFY(store.open());
        MonitorService service{&store, nullptr, MonitorService::Retention{}};

        TraceEvent refused{made(QStringLiteral("web"), Category::Authorization,
                                Severity::Warning, QStringLiteral("slot refused"), 1000)};
        refused.ok = false;
        service.take({refused.toVariant()}, QStringLiteral("web"));
        service.heartbeat(QStringLiteral("jobs"), 0);

        QHash<QString, QVariantMap> byName;
        for (const QVariant &row : service.entityRows()) {
            byName.insert(row.toMap().value(QStringLiteral("name")).toString(), row.toMap());
        }
        QCOMPARE(byName.size(), 2);
        QCOMPARE(byName.value(QStringLiteral("web")).value(QStringLiteral("refusals")).toInt(),
                 1);
        QVERIFY(byName.value(QStringLiteral("web")).value(QStringLiteral("live")).toBool());
        QVERIFY(byName.value(QStringLiteral("jobs")).value(QStringLiteral("live")).toBool());
        QCOMPARE(static_cast<int>(service.received()), 1);
        QCOMPARE(static_cast<int>(service.stored()), 1);
        QCOMPARE(static_cast<int>(service.dropped()), 0);
    }
};

QTEST_GUILESS_MAIN(TestStore)
#include "tst_store.moc"
