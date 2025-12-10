// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A front's relay. The Source a browser acquires holds nothing of its own and is one
// caller's view of the entity serving their scope. Everything that entity publishes is
// followed outward. Every slot the caller asks for is forwarded back with the session it
// is being asked for.
//
// Wired here without a mesh, because the relay is by name through the meta-object and has
// no opinion about what is on the other side of it. What it needs is an object carrying
// the contract's members, and a Source of the entity's own contract is exactly that. The
// mesh half is tst_m7's, next door.

#include "caller.h"
#include "sessionmanager.h"
#include "sourcefactory.h"

#include "backoffice_sourcehelper.h"
#include "gated_sourcehelper.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

using namespace SynQt;

namespace {

constexpr int OneMinuteTtl{1};

QVariantList catalogueRows()
{
    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("sku"), QStringLiteral("SKU-1")},
                            {QStringLiteral("price"), 9.5}});
    rows.append(QVariantMap{{QStringLiteral("sku"), QStringLiteral("SKU-2")},
                            {QStringLiteral("price"), 4.25}});
    return rows;
}

/// The entity behind the front, with its one slot implemented the way an owner's QML
/// implements one: a method beyond the generated helper's own, which is what the dispatch
/// looks for.
class Tier : public BackofficeSourceHelper
{
    Q_OBJECT

public:
    using BackofficeSourceHelper::restock;

    Q_INVOKABLE QVariant restock(QVariant sku, QVariant count)
    {
        // A map per call rather than a list per call: QList::append takes a whole list as
        // well as one element, so a call recorded as a list is spread over the record and
        // one call reads as two.
        QVariantMap call;
        call.insert(QStringLiteral("sku"), sku);
        call.insert(QStringLiteral("count"), count);
        asked.append(call);
        return QVariant{};
    }

    QList<QVariantMap> asked;
};

} // namespace

class TestFront : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_sessions = new SessionManager{QStringLiteral("anonymous"), OneMinuteTtl, this};
        m_tier = new Tier{};
        m_tier->setParent(this);
        SourceFactory::holdsSharedState(m_tier);
        m_tier->setHeadline(QStringLiteral("prices are up"));
        m_tier->setPending(7);
        m_tier->setCatalogue(catalogueRows());
    }

    /// What the entity behind the front is holding is what the caller sees, without the
    /// front implementing any of it.
    void theFrontPublishesWhatIsBehindIt()
    {
        GatedSourceHelper *front{frontFor(QStringLiteral("admin"))};

        QCOMPARE(front->headline(), QStringLiteral("prices are up"));
        QCOMPARE(front->pending(), 7);
        QCOMPARE(front->catalogue()->rowCount(), 2);
    }

    /// And it keeps publishing it. The relay follows, rather than copying once.
    void aLaterChangeBehindTheFrontIsFollowed()
    {
        GatedSourceHelper *front{frontFor(QStringLiteral("admin"))};

        m_tier->setHeadline(QStringLiteral("prices are down"));
        m_tier->setPending(3);

        QCOMPARE(front->headline(), QStringLiteral("prices are down"));
        QCOMPARE(front->pending(), 3);
    }

    void rowsPublishedBehindTheFrontCrossIt()
    {
        GatedSourceHelper *front{frontFor(QStringLiteral("admin"))};
        QVariantList rows;
        rows.append(QVariantMap{{QStringLiteral("line"), QStringLiteral("ada raised one")}});

        m_tier->setAuditLog(rows);

        QCOMPARE(front->auditLog()->rowCount(), 1);
        QCOMPARE(front->auditLog()->data(front->auditLog()->index(0, 0),
                                         Qt::UserRole).toString(),
                 QStringLiteral("ada raised one"));
    }

    void aSignalRaisedBehindTheFrontReachesTheCaller()
    {
        GatedSourceHelper *front{frontFor(QStringLiteral("admin"))};
        QSignalSpy relayed{front, &GatedSource::restocked};

        m_tier->emitRestocked(QStringLiteral("SKU-1"));

        QCOMPARE(relayed.count(), 1);
        QCOMPARE(relayed.first().at(0).toString(), QStringLiteral("SKU-1"));
    }

    /// The call goes to the entity behind the front, carrying the session it is being made
    /// for, which is what lets that entity authorize the person rather than the edge.
    void aSlotIsForwardedWithTheSessionItIsAskedFor()
    {
        const QByteArray session{m_sessions->createSession(
            QStringLiteral("admin"), {{QStringLiteral("sub"), QStringLiteral("u1")}})};
        GatedSourceHelper *front{frontFor(session)};

        front->restock(QStringLiteral("SKU-1"), 3);

        QCOMPARE(m_tier->asked.size(), 1);
        const QVariantMap call{m_tier->asked.first()};
        QCOMPARE(call.value(QStringLiteral("sku")).toString(), QStringLiteral("SKU-1"));
        QCOMPARE(call.value(QStringLiteral("count")).toInt(), 3);
    }

    /// The front's own gate still runs, before anything is forwarded. Whoever is behind it
    /// never sees a call the caller had no scope for, so it never has to check.
    void anUnderScopedCallerIsRefusedAtTheFront()
    {
        GatedSourceHelper *front{frontFor(QStringLiteral("moderator"))};

        QTest::ignoreMessage(QtWarningMsg,
                             "Gated.restock: refused, the caller does not hold the scope "
                             "it needs");
        front->restock(QStringLiteral("SKU-1"), 3);

        QCOMPARE(m_tier->asked.size(), 0);
        // And the gated state never crossed either, though the entity behind is holding it.
        QCOMPARE(front->pending(), 0);
        QCOMPARE(m_tier->pending(), 7);
    }

private:
    GatedSourceHelper *frontFor(const QString &scope)
    {
        return frontFor(m_sessions->createSession(scope));
    }

    GatedSourceHelper *frontFor(const QByteArray &session)
    {
        Caller *caller{Caller::forUser(QString{}, m_sessions, session, nullptr, this)};
        caller->setScopeOrder({QStringLiteral("anonymous"), QStringLiteral("moderator"),
                               QStringLiteral("admin")}, true);
        GatedSourceHelper *front{new GatedSourceHelper{this}};
        caller->setSource(front);
        SourceFactory::bindCaller(front, caller);
        SourceFactory::relay(front, m_tier);
        return front;
    }

    SessionManager *m_sessions{nullptr};
    Tier *m_tier{nullptr};
};

QTEST_MAIN(TestFront)
#include "tst_front.moc"
