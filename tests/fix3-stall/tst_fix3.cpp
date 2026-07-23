// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// FIX-3 acceptance: the stall storefront's edge-delivered pages and its real page seed,
// proven end to end. The web edge serves the framework's Pages connect point from the
// example's own web/pages and web/campaign-seed.qml; a native SynClient acts as the
// browser and fetches pages over the same authenticated wss link.
//
// The non-negotiable of assertion 5 is that the seed rides the PRODUCTION per-connection
// path: the client makes a real connection, so WebEdge::hostConnection() mints a
// PagesEdgeSource carrying that connection's own Caller (Caller::forUser with the
// configured scope order), and fetchPage() reaches PagesService::fetchPageFor() through
// it. Nothing here builds a Caller by hand (no edgeCaller()); every fetch is a real round
// trip an actual browser would make.
//
//   1. synqt check passes on examples/stall            -> tools/synqt/tests/test_examples.py
//   2. client-as-consumer-of-inventory fails check     -> tools/synqt/tests/test_examples.py
//   3. an under-scoped fetch of /members is forbidden with no markup; a user fetch succeeds
//   4. a route the client never compiled in is reachable through the edge's pushed table
//   5. the seed is real and fresh per parameter, across a notModified reply

#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "rep_pages_source.h"  // PageResponse, exposed publicly by SynQtService

#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectPendingCall>
#include <QSslSocket>
#include <QString>
#include <QTest>
#include <QUrl>

#include <memory>

using namespace SynQt;

namespace {

QByteArray cookieFor(const QByteArray &token)
{
    return QByteArrayLiteral("synqt_session=") + token;
}

SynClientConfig clientConfig(quint16 port, const QByteArray &cookie)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    // The framework's Pages connect point, acquired as an ordinary consumed connect point.
    // No consumer facade is registered here, so ServerAccessor exposes the raw Replica,
    // which is all this test needs to invoke fetchPage() directly.
    config.connectPoints = {{QStringLiteral("Pages"), QStringLiteral("Pages")}};
    config.pinnedCaCertPath = QStringLiteral(FIX3_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};
    config.reconnectBaseMs = 200;
    // Deliberately no routes and no remotePalette: this "browser" compiles in nothing, so a
    // page it reaches proves the page is delivered by the edge, not carried by the bundle.
    return config;
}

QObject *pagesReplica(SynClient *client)
{
    return client->server()->value(QStringLiteral("Pages")).value<QObject *>();
}

QString seedHeadline(const PageResponse &response)
{
    const QJsonObject seed{QJsonDocument::fromJson(response.seed().toUtf8()).object()};
    return seed.value(QStringLiteral("headline")).toString();
}

} // namespace

class TestFix3 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_edgePort{0};

    // Fetch one page over a real connection, so the answer comes from the connection's own
    // per-connection PagesEdgeSource/Caller. Fails the test (never returns a bad value
    // silently) if the round trip does not complete.
    PageResponse fetchPage(QObject *replica, const QString &route, const QString &haveHash)
    {
        // A dynamic Replica's returning slot answers with a generic QRemoteObjectPendingCall
        // (a typed QRemoteObjectPendingReply<PageResponse> is only the shape a typed Replica
        // exposes). The reply value is the registered PageResponse metatype, carried in the
        // call's QVariant returnValue and cast back here.
        QRemoteObjectPendingCall call;
        const bool dispatched{QMetaObject::invokeMethod(
            replica, "fetchPage", Q_RETURN_ARG(QRemoteObjectPendingCall, call),
            Q_ARG(QString, route), Q_ARG(QString, haveHash))};
        [&]() { QVERIFY(dispatched); }();
        [&]() { QVERIFY(call.waitForFinished(5000)); }();
        [&]() { QCOMPARE(call.error(), QRemoteObjectPendingCall::NoError); }();
        return call.returnValue().value<PageResponse>();
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(FIX3_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(FIX3_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(FIX3_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};
        config.scopesHierarchical = true;
        config.pagesDir = QStringLiteral(FIX3_STALL_DIR "/web/pages");

        // The public, seeded campaign page (one file serves every slug).
        WebEdgePage campaign;
        campaign.path = QStringLiteral("/c/:campaign");
        campaign.file = QStringLiteral("Campaign.qml");
        campaign.seed = QStringLiteral(FIX3_STALL_DIR "/web/campaign-seed.qml");

        // The scoped page: an anonymous fetch is refused before a byte is sent.
        WebEdgePage members;
        members.path = QStringLiteral("/members");
        members.file = QStringLiteral("Members.qml");
        members.scope = QStringLiteral("user");

        // A page the "browser" never compiled a route for: reaching it proves the
        // edge-delivered nature (assertion 4). It reuses the campaign page file, so no
        // extra fixture is needed; it carries no seed of its own.
        WebEdgePage deal;
        deal.path = QStringLiteral("/deal-of-the-day");
        deal.file = QStringLiteral("Campaign.qml");

        config.pages = {campaign, members, deal};

        m_engine = std::make_unique<QQmlEngine>();
        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    void cleanupTestCase()
    {
        m_edge.reset();
        m_engine.reset();
    }

    // Assertion 3: the page's confidentiality lives on the edge, not in the client route
    // guard. An anonymous session is refused the scoped page with no markup, no hash, and
    // no seed; a user session gets it.
    void scopedPageIsForbiddenToAnonymousAndServedToAUser()
    {
        const QByteArray anonToken{m_edge->sessionManager()->createSession()};
        const QByteArray userToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"))};

        QQmlEngine clientEngine;
        SynClient anon{clientConfig(m_edgePort, cookieFor(anonToken)), &clientEngine};
        SynClient user{clientConfig(m_edgePort, cookieFor(userToken)), &clientEngine};
        anon.start();
        user.start();

        QTRY_COMPARE_WITH_TIMEOUT(anon.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(user.session()->state(), QStringLiteral("connected"), 8000);

        QObject *anonPages{pagesReplica(&anon)};
        QObject *userPages{pagesReplica(&user)};
        QVERIFY(anonPages && userPages);
        QTRY_VERIFY(qobject_cast<QRemoteObjectDynamicReplica *>(anonPages)->isReplicaValid());
        QTRY_VERIFY(qobject_cast<QRemoteObjectDynamicReplica *>(userPages)->isReplicaValid());

        const PageResponse refused{fetchPage(anonPages, QStringLiteral("/members"), QString{})};
        QCOMPARE(refused.status(), QStringLiteral("forbidden"));
        QVERIFY2(refused.qml().isEmpty(), "a forbidden page must carry no markup");
        QVERIFY2(refused.hash().isEmpty(), "a forbidden page must carry no hash");
        QVERIFY2(refused.seed().isEmpty(), "a forbidden page must carry no seed");

        const PageResponse served{fetchPage(userPages, QStringLiteral("/members"), QString{})};
        QCOMPARE(served.status(), QStringLiteral("ok"));
        QVERIFY2(!served.qml().isEmpty(), "an authorized user must receive the page markup");
    }

    // Assertion 4: a route the client never compiled in is still reachable, because the
    // edge pushes its own route table and delivers the page on demand. This "browser"
    // carries no compiled routes at all, so reaching /deal-of-the-day is only possible
    // through the edge: adding a page needs no client rebuild.
    void edgeDeliversARouteTheClientNeverCompiled()
    {
        const QByteArray token{m_edge->sessionManager()->createSession()};
        QQmlEngine clientEngine;
        SynClient browser{clientConfig(m_edgePort, cookieFor(token)), &clientEngine};
        browser.start();
        QTRY_COMPARE_WITH_TIMEOUT(browser.session()->state(),
                                  QStringLiteral("connected"), 8000);

        QObject *pages{pagesReplica(&browser)};
        QVERIFY(pages);
        auto *replica{qobject_cast<QRemoteObjectDynamicReplica *>(pages)};
        QVERIFY(replica);
        QTRY_VERIFY(replica->isReplicaValid());

        // The edge pushes its route table; the page the client never compiled is in it.
        QTRY_VERIFY(replica->property("routeTable").toString()
                        .contains(QStringLiteral("/deal-of-the-day")));

        const PageResponse delivered{
            fetchPage(pages, QStringLiteral("/deal-of-the-day"), QString{})};
        QCOMPARE(delivered.status(), QStringLiteral("ok"));
        QVERIFY2(!delivered.qml().isEmpty(),
                 "a page reachable only through the edge must still deliver its markup");
    }

    // Assertion 5: the seed is real and fresh per parameter, across a notModified reply,
    // driven through the production per-connection Caller (this is a real connection, so
    // the Source and Caller are the ones WebEdge::hostConnection() minted). One file serves
    // every slug, so the second fetch, carrying the first page's hash, comes back
    // notModified with NO markup but WITH the second parameter's own seed, never the
    // first's. This is the staleness fix and the per-connection wiring in one round trip.
    void seedIsRealAndFreshPerParameterAcrossNotModified()
    {
        const QByteArray token{m_edge->sessionManager()->createSession()};
        QQmlEngine clientEngine;
        SynClient browser{clientConfig(m_edgePort, cookieFor(token)), &clientEngine};
        browser.start();
        QTRY_COMPARE_WITH_TIMEOUT(browser.session()->state(),
                                  QStringLiteral("connected"), 8000);

        QObject *pages{pagesReplica(&browser)};
        QVERIFY(pages);
        QTRY_VERIFY(qobject_cast<QRemoteObjectDynamicReplica *>(pages)->isReplicaValid());

        // First slug: the page body and a seed built from "summer-sale".
        const PageResponse first{
            fetchPage(pages, QStringLiteral("/c/summer-sale"), QString{})};
        QCOMPARE(first.status(), QStringLiteral("ok"));
        QVERIFY2(!first.qml().isEmpty(), "the first fetch must carry the page markup");
        QVERIFY2(!first.hash().isEmpty(), "the delivered page must carry a content hash");
        QCOMPARE(seedHeadline(first), QStringLiteral("Summer Sale"));

        // Second slug, carrying the FIRST page's hash: the body is unchanged (one file
        // serves every slug), so the reply is notModified with no markup, but the seed must
        // be the SECOND parameter's, never the first's.
        const PageResponse second{
            fetchPage(pages, QStringLiteral("/c/black-friday"), first.hash())};
        QCOMPARE(second.status(), QStringLiteral("notModified"));
        QVERIFY2(second.qml().isEmpty(),
                 "a notModified reply must not resend the unchanged markup");
        QCOMPARE(second.hash(), first.hash());
        QCOMPARE(seedHeadline(second), QStringLiteral("Black Friday"));
    }
};

QTEST_MAIN(TestFix3)
#include "tst_fix3.moc"
