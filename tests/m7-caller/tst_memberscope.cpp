// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A `<scope>` gate on a member, over the shape the runtime builds: one shared
// Source holding the state for everybody, and a mirror per caller republishing it.
//
// The question these answer is not "is the call refused" but "did anything cross". A gate
// that only refused slots would still push an admin model's rows into a moderator's
// replica, where reading them takes no call at all. So every case below asks what the
// mirror is publishing, not what it says when asked.

#include "caller.h"
#include "sessionmanager.h"
#include "sourcefactory.h"

#include "gated_sourcehelper.h"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

using namespace SynQt;

namespace {

constexpr int OneMinuteTtl{1};

const QStringList &vocabulary()
{
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("moderator"),
                                   QStringLiteral("admin")};
    return order;
}

QVariantList catalogueRows()
{
    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("sku"), QStringLiteral("SKU-1")},
                            {QStringLiteral("price"), 9.5}});
    rows.append(QVariantMap{{QStringLiteral("sku"), QStringLiteral("SKU-2")},
                            {QStringLiteral("price"), 4.25}});
    return rows;
}

QVariantList auditRows()
{
    QVariantList rows;
    rows.append(QVariantMap{{QStringLiteral("line"), QStringLiteral("ada raised a price")}});
    return rows;
}

/// The shared Source, with the one slot implemented the way an owner's QML implements it:
/// a method beyond the generated helper's own, which is exactly what the dispatch looks
/// for. Without it a forwarded call reaches nothing and a refused one looks identical.
class OwnerSource : public GatedSourceHelper
{
    Q_OBJECT

public:
    using GatedSourceHelper::restock;

    Q_INVOKABLE QVariant restock(QVariant sku, QVariant count)
    {
        Q_UNUSED(sku);
        Q_UNUSED(count);
        restocked += 1;
        return QVariant{};
    }

    int restocked{0};
};

} // namespace

class TestMemberScope : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_sessions = new SessionManager{QStringLiteral("anonymous"), OneMinuteTtl, this};

        m_shared = new OwnerSource{};
        m_shared->setParent(this);
        SourceFactory::holdsSharedState(m_shared);
        m_shared->setHeadline(QStringLiteral("prices are up"));
        m_shared->setPending(7);
        m_shared->setCatalogue(catalogueRows());
        m_shared->setAuditLog(auditRows());
    }

    /// The shared Source is nobody's view. It holds every value for every caller, and
    /// gating it would leave the mirrors nothing to publish when a scope arrives.
    void theSharedSourceHoldsEverything()
    {
        QCOMPARE(m_shared->pending(), 7);
        QCOMPARE(m_shared->auditLog()->rowCount(), 1);
    }

    void aMirrorPublishesOnlyWhatItsCallerMaySee()
    {
        GatedSourceHelper *mirror{mirrorFor(QStringLiteral("moderator"))};

        // Ungated: the same as the owner published.
        QCOMPARE(mirror->headline(), QStringLiteral("prices are up"));
        QCOMPARE(mirror->catalogue()->rowCount(), 2);

        // Gated: not filtered on arrival, never sent. The prop is its default and the
        // model has no rows in it, which is what the consumer's replica gets.
        QCOMPARE(mirror->pending(), 0);
        QCOMPARE(mirror->auditLog()->rowCount(), 0);
    }

    void anAdminMirrorPublishesAllOfIt()
    {
        GatedSourceHelper *mirror{mirrorFor(QStringLiteral("admin"))};

        QCOMPARE(mirror->pending(), 7);
        QCOMPARE(mirror->auditLog()->rowCount(), 1);
    }

    /// A change the owner makes after the mirrors exist is published the same way: through
    /// each mirror's own gate, not around it.
    void aLaterChangeIsGatedToo()
    {
        GatedSourceHelper *moderator{mirrorFor(QStringLiteral("moderator"))};
        GatedSourceHelper *admin{mirrorFor(QStringLiteral("admin"))};

        m_shared->setPending(12);

        QCOMPARE(moderator->pending(), 0);
        QCOMPARE(admin->pending(), 12);
    }

    void aGatedSignalReachesOnlyAScopedMirror()
    {
        GatedSourceHelper *moderator{mirrorFor(QStringLiteral("moderator"))};
        GatedSourceHelper *admin{mirrorFor(QStringLiteral("admin"))};
        QSignalSpy moderatorOpen{moderator, &GatedSource::pinged};
        QSignalSpy moderatorGated{moderator, &GatedSource::restocked};
        QSignalSpy adminGated{admin, &GatedSource::restocked};

        m_shared->emitPinged();
        m_shared->emitRestocked(QStringLiteral("SKU-1"));

        QCOMPARE(moderatorOpen.count(), 1);   // ungated: everyone's
        QCOMPARE(moderatorGated.count(), 0);
        QCOMPARE(adminGated.count(), 1);
    }

    void aGatedSlotIsRefusedBeforeTheOwnerSeesIt()
    {
        GatedSourceHelper *moderator{mirrorFor(QStringLiteral("moderator"))};
        GatedSourceHelper *admin{mirrorFor(QStringLiteral("admin"))};

        QTest::ignoreMessage(QtWarningMsg,
                             "Gated.restock: refused, the caller does not hold the scope "
                             "it needs");
        moderator->restock(QStringLiteral("SKU-1"), 3);
        QCOMPARE(m_shared->restocked, 0);

        admin->restock(QStringLiteral("SKU-1"), 3);
        QCOMPARE(m_shared->restocked, 1);
    }

    /// The gate follows the session rather than the connection. A visitor who signs in
    /// mid-connection sees what they have become entitled to, without reconnecting,
    /// and one whose scope is taken away has it withdrawn from the replica they hold.
    void aScopeChangeOpensAndClosesTheGateLive()
    {
        const QByteArray session{m_sessions->createSession(QStringLiteral("moderator"))};
        GatedSourceHelper *mirror{mirrorFor(session)};
        QCOMPARE(mirror->pending(), 0);
        QCOMPARE(mirror->auditLog()->rowCount(), 0);

        const QByteArray elevated{m_sessions->setScope(session, QStringLiteral("admin"))};
        QCOMPARE(mirror->pending(), 7);
        QCOMPARE(mirror->auditLog()->rowCount(), 1);

        m_sessions->setScope(elevated, QStringLiteral("moderator"));
        QCOMPARE(mirror->pending(), 0);
        QCOMPARE(mirror->auditLog()->rowCount(), 0);
    }

    /// A binding on `Caller` follows whoever is calling now, and not the first one.
    ///
    /// This is the shared entity's shape: one Source, one Caller in its QML context, and
    /// `adopt` re-pointing that Caller at the caller of the slot about to run. QML that
    /// reads `Caller.scope` or `Caller.identity` in a binding has to be told when it
    /// moves. Told once and never again, a moderator's page would go on displaying the
    /// first visitor's name while the entity answered the second, which is not a stale
    /// value but somebody else's.
    void aBindingOnTheCallerFollowsWhoIsCallingNow()
    {
        const QVariantMap ada{{QStringLiteral("sub"), QStringLiteral("ada")}};
        Caller *held{Caller::forUser(
            QString{}, m_sessions,
            m_sessions->createSession(QStringLiteral("moderator"), ada), nullptr, this)};
        held->setScopeOrder(vocabulary(), true);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("Caller"), held);
        QQmlComponent component{&engine};
        component.setData(QByteArrayLiteral(
                              "import QtQml\n"
                              "QtObject {\n"
                              "    property string seenScope: Caller.scope\n"
                              "    property string seenSub: Caller.identity ? "
                              "Caller.identity.sub : \"\"\n"
                              "}\n"),
                          QUrl{});
        const std::unique_ptr<QObject> page{component.create()};
        QVERIFY2(page != nullptr, qPrintable(component.errorString()));
        QCOMPARE(page->property("seenScope").toString(), QStringLiteral("moderator"));
        QCOMPARE(page->property("seenSub").toString(), QStringLiteral("ada"));

        const QVariantMap grace{{QStringLiteral("sub"), QStringLiteral("grace")}};
        Caller *arriving{Caller::forUser(
            QString{}, m_sessions,
            m_sessions->createSession(QStringLiteral("admin"), grace), nullptr, this)};
        arriving->setScopeOrder(vocabulary(), true);
        held->adopt(arriving);

        QCOMPARE(page->property("seenScope").toString(), QStringLiteral("admin"));
        QCOMPARE(page->property("seenSub").toString(), QStringLiteral("grace"));
    }

    /// Adopting the caller already held changes nothing, so it says nothing.
    ///
    /// A shared entity adopts on every call, and most calls in a row are the same
    /// person's. Telling QML the caller changed when it did not would re-evaluate every
    /// binding on it once per call for no reason.
    void adoptingTheSameCallerTwiceSaysNothingTheSecondTime()
    {
        Caller *held{Caller::forUser(
            QString{}, m_sessions, m_sessions->createSession(QStringLiteral("anonymous")),
            nullptr, this)};
        const QByteArray session{m_sessions->createSession(QStringLiteral("moderator"))};
        Caller *first{Caller::forUser(QString{}, m_sessions, session, nullptr, this)};
        Caller *second{Caller::forUser(QString{}, m_sessions, session, nullptr, this)};

        QSignalSpy moved{held, SIGNAL(callerChanged())};
        held->adopt(first);
        QCOMPARE(moved.count(), 1);  // a different session: everything a binding reads moved
        held->adopt(second);
        QCOMPARE(moved.count(), 1);  // the same one again: nothing did
    }

    /// Fail closed. A Source that answers no caller at all cannot check a scope, so it
    /// publishes nothing gated. Only the runtime saying "this one holds the shared state"
    /// makes a Source ungated, and nothing says it by accident.
    void aSourceWithNoCallerPublishesNothingGated()
    {
        GatedSourceHelper orphan;
        orphan.setPending(7);
        orphan.setAuditLog(auditRows());

        QCOMPARE(orphan.pending(), 0);
        QCOMPARE(orphan.auditLog()->rowCount(), 0);
    }

private:
    GatedSourceHelper *mirrorFor(const QString &scope)
    {
        return mirrorFor(m_sessions->createSession(scope));
    }

    GatedSourceHelper *mirrorFor(const QByteArray &session)
    {
        Caller *caller{Caller::forUser(QString{}, m_sessions, session, nullptr, this)};
        caller->setScopeOrder(vocabulary(), true);
        GatedSourceHelper *mirror{new GatedSourceHelper{this}};
        caller->setSource(mirror);
        SourceFactory::mirror(mirror, m_shared, caller);
        return mirror;
    }

    SessionManager *m_sessions{nullptr};
    OwnerSource *m_shared{nullptr};
};

QTEST_MAIN(TestMemberScope)
#include "tst_memberscope.moc"
