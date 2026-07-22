// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// FIX-2 acceptance: the multiplayer tutorial's two hands-on checks, proven end to end. The
// web edge owns one authoritative arena; a World simulated once, injected into each
// per_session Arena Source by name, and integrates every blob itself from an aim point.
// Verifies the tutorial's "try it, then think" checks:
//   1. a console steer(3999, 3999) does not teleport: the edge walks the blob toward the
//      corner at its size's speed, a tick's budget at a time (movement authority);
//   2. a signed-out or unapproved caller never has `arena` acquired: the connect point's
//      scope: player is the barrier, so an under-scoped session cannot even reach the
//      Replica, let alone call steer.
// The third hands-on check (client-as-consumer-of-scores fails `synqt check`) is proven in
// tools/synqt/tests/test_examples.py.

#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "arena_sourcehelper.h"  // synqtRegisterArenaSources()

#include <QElapsedTimer>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QSslSocket>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include <cmath>
#include <memory>

using namespace SynQt;

namespace {

double speedFor(double mass)
{
    return 260.0 / std::pow(mass, 0.22);
}

QByteArray cookieFor(const QByteArray &token)
{
    return QByteArrayLiteral("synqt_session=") + token;
}

QVariantMap identityFor(const QString &sub, const QString &login)
{
    return QVariantMap{{QStringLiteral("sub"), sub},
                       {QStringLiteral("login"), login},
                       {QStringLiteral("name"), login},
                       {QStringLiteral("email"), sub + QStringLiteral("@example.com")}};
}

SynClientConfig clientConfig(quint16 port, const QByteArray &cookie)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    config.connectPoints = {{QStringLiteral("arena"), QStringLiteral("Arena")}};
    config.pinnedCaCertPath = QStringLiteral(FIX2_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("player")};
    config.reconnectBaseMs = 200;
    return config;
}

QObject *arenaReplica(SynClient *client)
{
    return client->server()->value(QStringLiteral("arena")).value<QObject *>();
}

} // namespace

class TestFix2 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    QObject *m_world{nullptr};
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_edgePort{0};

    QVariantMap posOf(const QString &sub) const
    {
        QVariant result;
        QMetaObject::invokeMethod(m_world, "posOf", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant{sub}));
        return result.toMap();
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterArenaSources();

        m_engine = std::make_unique<QQmlEngine>();

        // The one authoritative world: the tutorial's `pragma Singleton` World, registered as
        // a QML singleton type so every per_session Arena Source reaches it by name (`World`).
        // singletonInstance forces its creation now and hands the test the same instance the
        // Arena Sources see, so the assertions read the authoritative state directly.
        const int worldTypeId{qmlRegisterSingletonType(
            QUrl::fromLocalFile(QStringLiteral(FIX2_SRCDIR "/web/World.qml")),
            "SynQt", 1, 0, "World")};
        m_world = m_engine->singletonInstance<QObject *>(worldTypeId);
        QVERIFY(m_world);

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(FIX2_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(FIX2_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(FIX2_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("player")};

        WebEdgeConnectPoint arena;
        arena.name = QStringLiteral("arena");
        arena.contract = QStringLiteral("Arena");
        arena.serverFile = QStringLiteral(FIX2_SRCDIR "/web/Arena.qml");
        arena.scope = QStringLiteral("player");        // only approved players acquire it
        arena.instance = InstanceMode::PerSession;     // one per player, so Caller is bound
        config.connectPoints = {arena};

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

    // Hands-on check 2: the guest-list gate is the connect point's scope, not the UI. An
    // approved player (scope "player") acquires the arena; a signed-out / unapproved session
    // (scope "anonymous") never does, so steer/ping/roster are all out of reach.
    void scopeGateAdmitsOnlyApprovedPlayers()
    {
        const QByteArray playerToken{m_edge->sessionManager()->createSession(
            QStringLiteral("player"),
            identityFor(QStringLiteral("p1"), QStringLiteral("playerone")))};
        const QByteArray anonToken{m_edge->sessionManager()->createSession()};

        QQmlEngine clientEngine;
        SynClient player{clientConfig(m_edgePort, cookieFor(playerToken)), &clientEngine};
        SynClient anon{clientConfig(m_edgePort, cookieFor(anonToken)), &clientEngine};
        player.start();
        anon.start();

        QTRY_COMPARE_WITH_TIMEOUT(player.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(anon.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectDynamicReplica *playerArena{
            qobject_cast<QRemoteObjectDynamicReplica *>(arenaReplica(&player))};
        QRemoteObjectDynamicReplica *anonArena{
            qobject_cast<QRemoteObjectDynamicReplica *>(arenaReplica(&anon))};
        QVERIFY(playerArena && anonArena);
        QTRY_VERIFY(playerArena->isReplicaValid());

        // The unapproved session must never acquire the scope-gated arena.
        QTest::qWait(1500);
        QVERIFY2(!anonArena->isReplicaValid(),
                 "an under-scoped session must not acquire the scope: player arena");
    }

    // Hands-on check 1: steer is a goal, not a position. A single steer to the far corner
    // does not teleport the blob there; the edge advances it at most speedFor(mass) * dt per
    // tick, so it only crawls toward the corner.
    void steerCrawlsAndNeverTeleports()
    {
        const QByteArray playerToken{m_edge->sessionManager()->createSession(
            QStringLiteral("player"),
            identityFor(QStringLiteral("p2"), QStringLiteral("racer")))};
        QQmlEngine clientEngine;
        SynClient player{clientConfig(m_edgePort, cookieFor(playerToken)), &clientEngine};
        player.start();
        QTRY_COMPARE_WITH_TIMEOUT(player.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectDynamicReplica *playerArena{
            qobject_cast<QRemoteObjectDynamicReplica *>(arenaReplica(&player))};
        QVERIFY(playerArena);
        QTRY_VERIFY(playerArena->isReplicaValid());

        // Aim at the far corner in one shot (as from the browser console).
        const double targetX{3999.0};
        const double targetY{3999.0};
        QVERIFY(QMetaObject::invokeMethod(playerArena, "steer",
                                          Q_ARG(double, targetX), Q_ARG(double, targetY)));

        // Wait for the spawn (the edge stamps the blob on the first aim of this session).
        QTRY_VERIFY(!posOf(QStringLiteral("p2")).isEmpty());

        const QVariantMap start{posOf(QStringLiteral("p2"))};
        const double x0{start.value(QStringLiteral("x")).toDouble()};
        const double y0{start.value(QStringLiteral("y")).toDouble()};
        const double mass{start.value(QStringLiteral("mass")).toDouble()};

        // The aim was accepted as a GOAL (clamped into the map), not as a position: the blob
        // itself is nowhere near the corner it was aimed at.
        QVERIFY2(std::hypot(targetX - x0, targetY - y0) > 1000.0,
                 "the blob must not jump to the aim point");
        QVERIFY(start.value(QStringLiteral("tx")).toDouble() > 3990.0);   // goal clamped in

        QElapsedTimer clock;
        clock.start();
        QTest::qWait(250);
        const QVariantMap later{posOf(QStringLiteral("p2"))};
        const double x1{later.value(QStringLiteral("x")).toDouble()};
        const double y1{later.value(QStringLiteral("y")).toDouble()};
        const double elapsed{clock.elapsed() / 1000.0};

        const double moved{std::hypot(x1 - x0, y1 - y0)};
        const double budget{speedFor(mass) * elapsed};
        const double d0{std::hypot(targetX - x0, targetY - y0)};
        const double d1{std::hypot(targetX - x1, targetY - y1)};

        // It moved (the edge integrates motion itself)...
        QVERIFY2(moved > 1.0, "the edge must advance the blob toward its goal");
        // ...but only within its speed budget (a crawl, never a teleport)...
        QVERIFY2(moved <= budget * 1.6,
                 qPrintable(QStringLiteral("moved %1 exceeds crawl budget %2")
                                .arg(moved).arg(budget * 1.6)));
        // ...toward the goal, and still far from the corner.
        QVERIFY2(d1 < d0, "the blob must move toward its aim");
        QVERIFY2(d1 > 1000.0, "one steer must not carry the blob to the corner");
    }
};

QTEST_GUILESS_MAIN(TestFix2)
#include "tst_fix2.moc"
