// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The SessionManager and Caller unit cases the test plan names: time-to-live expiry and
// the purge behind it, credential rotation on setScope, and hierarchical versus set-based
// scope checks.
//
// tst_m7 next door proves the authorization matrix end to end over a real edge, which is
// the acceptance question. It cannot reach these: a live session never ages past its TTL
// inside a test run, a rotated credential is invisible from the browser side, and the edge
// it drives is configured hierarchical, so the set-based reading of the same vocabulary is
// never taken. Each of those is a fail-open if it breaks, so each is worth a direct test.

#include "caller.h"
#include "sessionmanager.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

using SynQt::Caller;
using SynQt::SessionManager;
using SynQt::SessionRecord;

namespace {

const int OneMinuteTtl{1};

QStringList vocabulary()
{
    return {QStringLiteral("anonymous"), QStringLiteral("user"), QStringLiteral("moderator")};
}

qint64 minutesAgo(int minutes)
{
    return QDateTime::currentMSecsSinceEpoch() - (static_cast<qint64>(minutes) * 60 * 1000);
}

} // namespace

class TestSessions : public QObject
{
    Q_OBJECT

private slots:
    void createsAnAnonymousSessionByDefault()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        QCOMPARE(sessions.defaultScope(), QStringLiteral("anonymous"));

        const QByteArray id{sessions.createSession()};
        const SessionRecord *record{sessions.lookup(id)};
        QVERIFY(record);
        QCOMPARE(record->scope, QStringLiteral("anonymous"));
        QVERIFY(record->identity.isEmpty());  // anonymous is the absence of an identity
        QVERIFY(sessions.isLive(id));

        // An unknown credential is not a session, and asking does not create one.
        QVERIFY(!sessions.lookup(QByteArrayLiteral("not-a-token")));
        QVERIFY(!sessions.isLive(QByteArrayLiteral("not-a-token")));
        QCOMPARE(sessions.snapshot().size(), 1);
    }

    void tokensAreUniqueAndOpaque()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        QSet<QByteArray> issued;
        for (int index{0}; index < 500; ++index) {
            const QByteArray id{sessions.createSession()};
            QCOMPARE(id.size(), 32);  // a 128-bit UUID, hex
            QVERIFY(!issued.contains(id));
            issued.insert(id);
        }
    }

    // TTL expiry. The clock is not mocked and the shortest configurable TTL is a minute,
    // so the aged record arrives the way a real one does: applyUpsert is the authoritative
    // write the distributed-session path uses, and it carries the creation time with it.
    // A session that outlived its TTL is gone from every read, immediately.
    void anExpiredSessionIsInvisibleToEveryRead()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray stale{QByteArrayLiteral("stale-token")};
        sessions.applyUpsert(QString::fromLatin1(stale), QStringLiteral("moderator"),
                             QStringLiteral(R"({"sub":"u1"})"),
                             static_cast<double>(minutesAgo(2)));

        QVERIFY(!sessions.lookup(stale));
        QVERIFY(!sessions.isLive(stale));
        QVERIFY(sessions.snapshot().isEmpty());

        // And it stays gone: expiry is not a race the caller can win by asking again.
        QVERIFY(!sessions.isLive(stale));
    }

    void aSessionWithinItsTtlStaysLive()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray fresh{QByteArrayLiteral("fresh-token")};
        sessions.applyUpsert(QString::fromLatin1(fresh), QStringLiteral("user"), QString{},
                             static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
        QVERIFY(sessions.isLive(fresh));
        QCOMPARE(sessions.snapshot().size(), 1);
    }

    void noTtlMeansNothingExpires()
    {
        SessionManager sessions{QStringLiteral("anonymous"), 0};
        const QByteArray ancient{QByteArrayLiteral("ancient-token")};
        sessions.applyUpsert(QString::fromLatin1(ancient), QStringLiteral("user"), QString{},
                             static_cast<double>(minutesAgo(60 * 24 * 365)));
        QVERIFY(sessions.isLive(ancient));
    }

    // The purge reclaims the memory an expired record holds. It is deliberately silent
    // (expiry is observed by lookup, never broadcast), so the only window onto it from
    // outside is that removing a record that is already gone emits nothing. Hence the A/B:
    // the same aged record, with and without a createSession to drive the purge.
    void thePurgeReclaimsExpiredRecords()
    {
        const QString stale{QStringLiteral("stale-token")};
        const double staleCreated{static_cast<double>(minutesAgo(2))};

        SessionManager kept{QStringLiteral("anonymous"), OneMinuteTtl};
        kept.applyUpsert(stale, QStringLiteral("user"), QString{}, staleCreated);
        QSignalSpy keptRemovals{&kept, &SessionManager::sessionRemoved};
        kept.applyRemove(stale);
        QCOMPARE(keptRemovals.count(), 1);  // expired, but still occupying the table

        SessionManager purged{QStringLiteral("anonymous"), OneMinuteTtl};
        purged.applyUpsert(stale, QStringLiteral("user"), QString{}, staleCreated);
        purged.createSession();  // the purge runs here
        QSignalSpy purgedRemovals{&purged, &SessionManager::sessionRemoved};
        purged.applyRemove(stale);
        QCOMPARE(purgedRemovals.count(), 0);  // already reclaimed
    }

    // The expiry queue holds hints, not truth: overwriting a token leaves behind a hint
    // whose creation time no longer matches the record. The purge must drop the hint and
    // keep the record. Getting this wrong would silently sign out a user who just
    // refreshed their session, which no test above would catch.
    void thePurgeDropsAStaleHintAndKeepsTheRecord()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QString reused{QStringLiteral("reused-token")};

        sessions.applyUpsert(reused, QStringLiteral("user"), QString{},
                             static_cast<double>(minutesAgo(2)));
        sessions.applyUpsert(reused, QStringLiteral("user"), QString{},
                             static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
        sessions.createSession();  // drains the aged hint, which now names a live record

        QVERIFY(sessions.isLive(reused.toLatin1()));
    }

    // Rotation on privilege change: the credential the browser holds while anonymous must
    // not still be valid once it names a scope, or a fixated token becomes an elevated one.
    void setScopeRotatesTheCredential()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray anonymous{sessions.createSession()};
        QSignalSpy upserts{&sessions, &SessionManager::sessionUpserted};
        QSignalSpy removals{&sessions, &SessionManager::sessionRemoved};

        const QVariantMap identity{{QStringLiteral("sub"), QStringLiteral("u1")},
                                   {QStringLiteral("login"), QStringLiteral("ada")}};
        const QByteArray elevated{sessions.setScope(anonymous, QStringLiteral("moderator"),
                                                    identity)};

        QVERIFY(!elevated.isEmpty());
        QVERIFY(elevated != anonymous);
        QVERIFY(!sessions.isLive(anonymous));  // the old credential is dead on the spot
        const SessionRecord *record{sessions.lookup(elevated)};
        QVERIFY(record);
        QCOMPARE(record->scope, QStringLiteral("moderator"));
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("ada"));
        QCOMPARE(sessions.snapshot().size(), 1);  // one session, not two

        // The store is told about both halves, so a second edge sees the same rotation.
        QCOMPARE(upserts.count(), 1);
        QCOMPARE(upserts.first().at(0).toString(), QString::fromLatin1(elevated));
        QCOMPARE(removals.count(), 1);
        QCOMPARE(removals.first().at(0).toString(), QString::fromLatin1(anonymous));
    }

    void setScopeKeepsTheIdentityItIsNotGiven()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QVariantMap identity{{QStringLiteral("sub"), QStringLiteral("u1")}};
        const QByteArray signedIn{sessions.createSession(QStringLiteral("user"), identity)};

        const QByteArray elevated{sessions.setScope(signedIn, QStringLiteral("moderator"))};
        const SessionRecord *record{sessions.lookup(elevated)};
        QVERIFY(record);
        QCOMPARE(record->identity.value(QStringLiteral("sub")).toString(),
                 QStringLiteral("u1"));

        // An empty scope means the configured default, not an empty scope.
        const QByteArray reset{sessions.setScope(elevated, QString{})};
        QCOMPARE(sessions.lookup(reset)->scope, QStringLiteral("anonymous"));
    }

    void setScopeOnAnUnknownCredentialChangesNothing()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        QSignalSpy upserts{&sessions, &SessionManager::sessionUpserted};

        const QByteArray issued{sessions.setScope(QByteArrayLiteral("forged"),
                                                  QStringLiteral("moderator"))};

        QVERIFY(issued.isEmpty());  // no session is minted for a credential nobody issued
        QCOMPARE(upserts.count(), 0);
        QVERIFY(sessions.snapshot().isEmpty());
    }

    void revokeRemovesOnceAndOnlyWhatExists()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray id{sessions.createSession()};
        QSignalSpy removals{&sessions, &SessionManager::sessionRemoved};

        sessions.revoke(id);
        QVERIFY(!sessions.isLive(id));
        QCOMPARE(removals.count(), 1);

        sessions.revoke(id);  // idempotent, and silent the second time
        QCOMPARE(removals.count(), 1);
        sessions.revoke(QByteArrayLiteral("never-issued"));
        QCOMPARE(removals.count(), 1);
    }

    void snapshotCarriesLiveRowsOnly()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray live{sessions.createSession(QStringLiteral("user"))};
        sessions.applyUpsert(QStringLiteral("stale-token"), QStringLiteral("moderator"),
                             QString{}, static_cast<double>(minutesAgo(2)));

        // Assigned, not brace-initialized: QVariantList{aList} wraps the list in a
        // one-element list instead of copying it, and the row count would still be 1.
        const QVariantList rows = sessions.snapshot();
        QCOMPARE(rows.size(), 1);
        const QVariantMap row{rows.first().toMap()};
        QCOMPARE(row.value(QStringLiteral("token")).toString(), QString::fromLatin1(live));
        QCOMPARE(row.value(QStringLiteral("scope")).toString(), QStringLiteral("user"));
    }

    // Hierarchical versus set-based, over one vocabulary. The difference is the whole
    // authorization model of an app: hierarchical, a moderator satisfies a user gate;
    // set-based, it does not and the grant has to name the scope exactly.
    void scopeChecks()
    {
        QFETCH(QString, granted);
        QFETCH(QString, required);
        QFETCH(bool, hierarchical);
        QFETCH(bool, expected);

        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        QObject owner;
        Caller *caller{Caller::forUser(QString{}, &sessions,
                                       sessions.createSession(granted), nullptr, &owner)};
        caller->setScopeOrder(vocabulary(), hierarchical);

        QCOMPARE(caller->scope(), granted);
        QCOMPARE(caller->hasScope(required), expected);
    }

    void scopeChecks_data()
    {
        QTest::addColumn<QString>("granted");
        QTest::addColumn<QString>("required");
        QTest::addColumn<bool>("hierarchical");
        QTest::addColumn<bool>("expected");

        QTest::newRow("hierarchical exact")
            << "user" << "user" << true << true;
        QTest::newRow("hierarchical above the gate")
            << "moderator" << "user" << true << true;
        QTest::newRow("hierarchical below the gate")
            << "user" << "moderator" << true << false;
        QTest::newRow("hierarchical anonymous below the gate")
            << "anonymous" << "user" << true << false;
        // A scope outside the vocabulary ranks nowhere, so it satisfies nothing and is
        // satisfied by nothing. A typo in a gate must fail closed, never open.
        QTest::newRow("hierarchical unknown gate")
            << "moderator" << "wizard" << true << false;
        QTest::newRow("hierarchical unknown grant")
            << "wizard" << "anonymous" << true << false;

        QTest::newRow("set-based exact")
            << "moderator" << "moderator" << false << true;
        QTest::newRow("set-based above the gate")
            << "moderator" << "user" << false << false;
        QTest::newRow("set-based below the gate")
            << "user" << "moderator" << false << false;
    }

    void withoutAVocabularyOnlyAnExactScopeMatches()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        QObject owner;
        Caller *caller{Caller::forUser(QString{}, &sessions,
                                       sessions.createSession(QStringLiteral("moderator")),
                                       nullptr, &owner)};
        caller->setScopeOrder(QStringList{}, true);  // hierarchical, but nothing to rank

        QVERIFY(caller->hasScope(QStringLiteral("moderator")));
        QVERIFY(!caller->hasScope(QStringLiteral("user")));
    }

    // The Caller reads the manager live, so it follows its own elevation rather than
    // holding the credential it was built with.
    void theCallerFollowsItsOwnRotation()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray anonymous{sessions.createSession()};
        QObject owner;
        Caller *caller{Caller::forUser(QString{}, &sessions, anonymous, nullptr, &owner)};
        caller->setScopeOrder(vocabulary(), true);

        caller->setScope(QStringLiteral("user"),
                         QVariantMap{{QStringLiteral("sub"), QStringLiteral("u1")}});

        QCOMPARE(caller->scope(), QStringLiteral("user"));
        QVERIFY(caller->hasScope(QStringLiteral("anonymous")));
        QVERIFY(!sessions.isLive(anonymous));
        QCOMPARE(caller->identity().toMap().value(QStringLiteral("sub")).toString(),
                 QStringLiteral("u1"));
        // The rotated credential is the one the Caller now presents, so the edge writes
        // back the cookie that matches the session that exists.
        const QVariantMap row{sessions.snapshot().first().toMap()};
        QCOMPARE(caller->id(), row.value(QStringLiteral("token")).toString());
    }

    // A revoked or expired session leaves the Caller with nothing to read, and nothing to
    // read must mean no scope: an owner-side gate keyed on hasScope has to close.
    void theCallerFailsClosedOnceItsSessionIsGone()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray id{sessions.createSession(QStringLiteral("moderator"))};
        QObject owner;
        Caller *caller{Caller::forUser(QString{}, &sessions, id, nullptr, &owner)};
        caller->setScopeOrder(vocabulary(), true);
        QVERIFY(caller->hasScope(QStringLiteral("moderator")));

        sessions.revoke(id);

        QVERIFY(!caller->hasScope(QStringLiteral("moderator")));
        QVERIFY(!caller->hasScope(QStringLiteral("anonymous")));
        QVERIFY(caller->scope().isEmpty());
        QVERIFY(!caller->session().isValid());
        QVERIFY(!caller->identity().isValid());
        QVERIFY(caller->isUser());  // still a user caller; just one with no session left
    }

    // The two identity systems never mix. An entity caller has no scope to check, and the
    // colocation-trusted case is reported as unverified so an owner can refuse it.
    void anEntityCallerIsNeverScoped()
    {
        QObject owner;
        Caller *verified{Caller::forEntity(QString{}, QStringLiteral("database"), true,
                                           nullptr, &owner)};
        verified->setScopeOrder(vocabulary(), true);

        QVERIFY(verified->isEntity());
        QVERIFY(verified->isEntityVerified());
        QVERIFY(!verified->isUser());
        QCOMPARE(verified->entity(), QStringLiteral("database"));
        QCOMPARE(verified->id(), QStringLiteral("database"));
        QVERIFY(verified->scope().isEmpty());
        QVERIFY(!verified->hasScope(QStringLiteral("anonymous")));
        QVERIFY(!verified->session().isValid());
        QVERIFY(!verified->identity().isValid());

        // The opt-in local socket: the OS confirms the user, nothing confirms the entity.
        Caller *colocated{Caller::forEntity(QString{}, QStringLiteral("database"), false,
                                            nullptr, &owner)};
        QVERIFY(colocated->isEntity());
        QVERIFY(!colocated->isEntityVerified());
        QCOMPARE(colocated->entity(), QStringLiteral("database"));
    }
};

QTEST_GUILESS_MAIN(TestSessions)
#include "tst_sessions.moc"
