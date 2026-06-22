// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The SessionManager and Caller unit cases the test plan names. Time-to-live expiry and
// the purge behind it, credential rotation on setScope, and hierarchical versus set-based
// scope checks.
//
// tst_m7 next door proves the authorization matrix end to end over a real edge, which is
// the acceptance question. It cannot reach these. A live session never ages past its TTL
// inside a test run, a rotated credential is invisible from the browser side, and the edge
// it drives is configured hierarchical, so the set-based reading of the same vocabulary is
// never taken. Each of those is a fail-open if it breaks, so each gets a direct test.

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
    // so the aged record arrives the way a real one does. applyUpsert is the authoritative
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

        // And it stays gone. Expiry is not a race the caller can win by asking again.
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

    // The purge reclaims the memory an expired record holds. It is silent
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

    // The expiry queue holds hints, not truth. Overwriting a token leaves behind a hint
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

    // Rotation on privilege change. The credential the browser holds while anonymous must
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

    // A shared entity answers everyone from one Source, so the Caller its QML reads is the
    // shared one, made to be the calling mirror's for the length of the slot (Caller::adopt).
    // `Caller.setScope()` therefore runs on the shared Caller and rotates the credential
    // there, while the mirror's own Caller is the one that outlives the call and the one the
    // next call adopts from. If the rotation does not reach it, the session it names is the
    // one setScope erased. The visitor is signed in, and every later call on that
    // connection sees no session at all.
    void setScopeReachesEveryCallerOnTheSameSession()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray anonymous{sessions.createSession()};

        // What the edge builds per connection. The Caller the mirror keeps, and the shared
        // Source's own, which adopts it for the call.
        Caller *mirror{Caller::forUser(QString{}, &sessions, anonymous, nullptr, this)};
        Caller *shared{Caller::forUser(QString{}, &sessions, anonymous, nullptr, this)};
        const QStringList order{QStringLiteral("anonymous"), QStringLiteral("user")};
        mirror->setScopeOrder(order, true);
        shared->setScopeOrder(order, true);

        shared->adopt(mirror);
        shared->setScope(QStringLiteral("user"),
                         {{QStringLiteral("sub"), QStringLiteral("u1")}});

        QCOMPARE(shared->scope(), QStringLiteral("user"));
        QCOMPARE(mirror->scope(), QStringLiteral("user"));
        QVERIFY(mirror->hasSession());
        QVERIFY(mirror->hasScope(QStringLiteral("user")));

        // And the next call adopts from the mirror, so what it carries has to still be live.
        shared->adopt(mirror);
        QCOMPARE(shared->identity().toMap().value(QStringLiteral("sub")).toString(),
                 QStringLiteral("u1"));
    }

    // The same rotation, with more than two Callers on the session and the elevation asked
    // for by the one that was made first.
    //
    // A Caller elevates by handing its own `m_sessionId` to setScope, and the rotation
    // that raises is what sets that member to the new credential. While the parameter was
    // a reference to it, the first receiver to run changed the value every later receiver
    // was about to be handed. They were told the session had rotated from itself, matched
    // nothing, and were left naming a credential that had been erased. What the edge
    // was then told is worse than that. `sessionRemoved` is emitted from the same value, so
    // it named the session that had this moment been issued rather than the one being
    // replaced, and the edge acts on that by closing the connections of the session it just
    // created. On a browser that is a visitor signed out one instant after signing in.
    //
    // Two Callers could not see it. The aliased one is also a receiver, and with two it is
    // the last one, so nothing runs after it has spoiled the value.
    void setScopeReachesCallersMadeAfterTheOneThatAsked()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray anonymous{sessions.createSession()};
        const QStringList order{QStringLiteral("anonymous"), QStringLiteral("user")};

        // In creation order, which is receiver order. The one that asks, and two that only
        // have to keep up. An edge has several per connection (the gate, each connect
        // point's Source, the framework's own).
        Caller *asks{Caller::forUser(QString{}, &sessions, anonymous, nullptr, this)};
        Caller *first{Caller::forUser(QString{}, &sessions, anonymous, nullptr, this)};
        Caller *second{Caller::forUser(QString{}, &sessions, anonymous, nullptr, this)};
        for (Caller *caller : {asks, first, second}) {
            caller->setScopeOrder(order, true);
        }

        QSignalSpy removed{&sessions, &SessionManager::sessionRemoved};
        asks->setScope(QStringLiteral("user"),
                       {{QStringLiteral("sub"), QStringLiteral("u1")}});

        const QByteArray elevated{sessions.snapshot().first().toMap()
                                      .value(QStringLiteral("token")).toString().toLatin1()};
        QVERIFY(elevated != anonymous);

        for (Caller *caller : {asks, first, second}) {
            QVERIFY2(caller->hasSession(),
                     "a caller was left naming the credential the rotation replaced");
            QCOMPARE(caller->scope(), QStringLiteral("user"));
        }

        // And the removal names the credential that was replaced, never the one issued.
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toString().toLatin1(), anonymous);
    }

    // Two elevations before the visitor's next page load. The browser is still holding the
    // credential the FIRST one replaced, in an httpOnly cookie no slot call can rewrite, so
    // the hand-off from that credential is the only thing that gets the visitor their new
    // one. The second elevation erased what the first hand-off pointed at, `rotationOf`
    // refuses a hand-off whose target is gone, and the next page load handed the visitor a
    // fresh anonymous session. Signed in, then signed out again by being promoted. Signing
    // somebody in and then granting them a role in the same call is an ordinary thing to
    // write.
    void rotationChainStillLeadsToTheLiveSession()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray held{sessions.createSession()};  // what the cookie holds, throughout

        const QByteArray signedIn{sessions.setScope(held, QStringLiteral("user"))};
        QVERIFY(!signedIn.isEmpty());
        QCOMPARE(sessions.rotationOf(held), signedIn);

        const QByteArray promoted{sessions.setScope(signedIn, QStringLiteral("moderator"))};
        QVERIFY(!promoted.isEmpty());

        // The cookie the browser is still presenting leads to the session it now names.
        QCOMPARE(sessions.rotationOf(held), promoted);
        QCOMPARE(sessions.rotationOf(signedIn), promoted);
        const SessionRecord *record{sessions.lookup(promoted)};
        QVERIFY(record);
        QCOMPARE(record->scope, QStringLiteral("moderator"));
        QCOMPARE(sessions.snapshot().size(), 1);

        // A hand-off is a redirection and never an authorization: neither dead credential
        // is live, whatever it can still point at.
        QVERIFY(!sessions.isLive(held));
        QVERIFY(!sessions.isLive(signedIn));

        // And when the session at the end of the chain ends, the chain leads nowhere.
        sessions.revoke(promoted);
        QVERIFY(sessions.rotationOf(held).isEmpty());
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

    // The table has a ceiling, and at the ceiling it lets go of the sessions nobody would
    // miss before it refuses anybody. Anyone who can reach an edge can mint a session (a
    // page load with no live cookie is enough), and until this the only thing that ever
    // took one away was the TTL: a stranger could grow the table for twelve hours at one
    // request per record, and on a replicated edge every record went to every replica.
    void theTableHasACeilingAndEvictsTheAnonymousIdleFirst()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        sessions.setMaximumSessions(4);
        QCOMPARE(sessions.maximumSessions(), 4);
        QSignalSpy removals{&sessions, &SessionManager::sessionRemoved};

        // Two anonymous visitors, one of them still connected, and a signed-in one who is
        // idle. Then the ceiling, then a mint over it.
        const QByteArray oldestIdle{sessions.createSession()};
        const QByteArray connected{sessions.createSession()};
        const QByteArray signedIn{sessions.createSession(
            QStringLiteral("user"), QVariantMap{{QStringLiteral("sub"), QStringLiteral("ada")}})};
        const QByteArray fourth{sessions.createSession()};
        sessions.setInUseCheck([connected](const QByteArray &id) { return id == connected; });
        QVERIFY(sessions.hasRoom());  // full, but the oldest idle anonymous one can go

        const QByteArray fifth{sessions.createSession()};
        QVERIFY2(!fifth.isEmpty(), "at the ceiling a mint must evict rather than refuse");
        QVERIFY2(!sessions.isLive(oldestIdle), "the oldest idle anonymous session goes first");
        QVERIFY(sessions.isLive(connected));  // never one with a live connection
        QVERIFY(sessions.isLive(signedIn));   // never one somebody signed in to
        QVERIFY(sessions.isLive(fourth));
        QVERIFY(sessions.isLive(fifth));
        QCOMPARE(removals.count(), 1);         // told the way a revocation is told
        QCOMPARE(removals.first().at(0).toByteArray(), oldestIdle);

        // Nothing left to give. The connected one is in use, the signed-in one is kept,
        // and the two youngest anonymous ones are the only candidates.
        sessions.setInUseCheck([connected, fourth, fifth](const QByteArray &id) {
            return id == connected || id == fourth || id == fifth;
        });
        QVERIFY(!sessions.hasRoom());
        QVERIFY2(sessions.createSession().isEmpty(),
                 "with nothing evictable a mint over the ceiling must refuse, not grow");
        QCOMPARE(sessions.snapshot().size(), 4);

        // A raised scope with no identity is a visitor who proved something (the password
        // gate elevates without an identity), and is kept too.
        sessions.setInUseCheck([connected](const QByteArray &id) { return id == connected; });
        const QByteArray elevated{sessions.setScope(fourth, QStringLiteral("user"))};
        QVERIFY(!elevated.isEmpty());
        QVERIFY(!sessions.createSession().isEmpty());  // fifth went, the last anonymous idle
        QVERIFY(sessions.isLive(elevated));
        QVERIFY(sessions.isLive(connected));
        QVERIFY(!sessions.isLive(fifth));

        // Zero is no ceiling.
        sessions.setMaximumSessions(0);
        QVERIFY(!sessions.createSession().isEmpty());
        QCOMPARE(sessions.snapshot().size(), 5);
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
    // authorization model of an app. Hierarchical, a moderator satisfies a user gate;
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
        // The Caller now names the rotated session, by its key. The credential is what
        // the browser is handed on its next page load, and not something QML holds.
        const QVariantMap row{sessions.snapshot().first().toMap()};
        QCOMPARE(caller->id(),
                 SessionManager::keyFor(row.value(QStringLiteral("token")).toByteArray()));
    }

    // Nothing an owner's QML can read off a user Caller is the credential. `Client.id` is
    // the documented ownership key, and an owner writes it into rows, into logs, and
    // sometimes into a declared model role that reaches every browser. The cookie behind
    // it must not be what travels. The key names the session without being it.
    void aUserCallerNeverHandsOutItsCredential()
    {
        SessionManager sessions{QStringLiteral("anonymous"), OneMinuteTtl};
        const QByteArray credential{sessions.createSession(
            QStringLiteral("user"), QVariantMap{{QStringLiteral("sub"), QStringLiteral("u1")}})};
        QObject owner;
        Caller *caller{Caller::forUser(QString{}, &sessions, credential, nullptr, &owner)};

        QCOMPARE(caller->id(), SessionManager::keyFor(credential));
        QVERIFY(caller->id() != QString::fromLatin1(credential));
        const QVariantMap session{caller->session().toMap()};
        QCOMPARE(session.value(QStringLiteral("key")).toString(),
                 SessionManager::keyFor(credential));
        QVERIFY(!session.contains(QStringLiteral("id")));
        for (const QVariant &value : session) {
            QVERIFY2(value.toString() != QString::fromLatin1(credential),
                     "the credential is readable off Caller.session");
        }
    }

    // A revoked or expired session leaves the Caller with nothing to read, and nothing to
    // read must mean no scope. An owner-side gate keyed on hasScope has to close.
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
        QVERIFY(caller->isUser());  // still a user caller, one with no session left
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

        // The opt-in local socket. The OS confirms the user, nothing confirms the entity.
        Caller *colocated{Caller::forEntity(QString{}, QStringLiteral("database"), false,
                                            nullptr, &owner)};
        QVERIFY(colocated->isEntity());
        QVERIFY(!colocated->isEntityVerified());
        QCOMPARE(colocated->entity(), QStringLiteral("database"));
    }
};

QTEST_GUILESS_MAIN(TestSessions)
#include "tst_sessions.moc"
