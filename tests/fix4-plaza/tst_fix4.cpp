// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The 3D plaza tutorial's hands-on checks, proven end to end against the example's own
// web/edge/Edge.qml. Verifies:
//   1. a console walk(1000, 0, 90) does not carry a walker anywhere faster than walking: the
//      push is bounded to a full one and the edge moves the walker at walking speed;
//   2. two walkers never stand closer than two radii, however hard one walks at the other;
//   3. a visitor who has not signed in never has the plaza acquired, because the connect
//      point is `scope: user`.
// The client as a consumer of anything but the edge failing `synqt check` is proven in
// tools/synqt/tests/test_examples.py, like every other tutorial's.

#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "edge_sourcehelper.h"  // synqtRegisterEdgeSources()

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QSslSocket>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <cmath>
#include <memory>
#include <numbers>

using namespace SynQt;

namespace {

// The rules the example's Edge.qml moves walkers by.
constexpr double kSpeed{350.0};
constexpr double kRadius{30.0};

QByteArray cookieFor(const QByteArray &token)
{
    return QByteArrayLiteral("synqt_session=") + token;
}

QVariantMap identityFor(const QString &sub, const QString &login)
{
    return QVariantMap{{QStringLiteral("sub"), sub},
                       {QStringLiteral("login"), login},
                       {QStringLiteral("name"), login},
                       {QStringLiteral("email"), QVariant{}}};
}

SynClientConfig clientConfig(quint16 port, const QByteArray &cookie)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    config.connectPoints = {{QStringLiteral("edge"), QStringLiteral("Edge")}};
    config.pinnedCaCertPath = QStringLiteral(FIX4_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};
    config.reconnectBaseMs = 200;
    return config;
}

QRemoteObjectDynamicReplica *plazaOf(SynClient *client)
{
    return qobject_cast<QRemoteObjectDynamicReplica *>(
        client->server()->point(QStringLiteral("edge")));
}

// One walker's row, as a browser reads it off the `walkers` model, or empty while the model
// has not carried it yet.
QVariantMap rowOf(QRemoteObjectDynamicReplica *plaza, const QString &sub)
{
    QAbstractItemModel *walkers{
        plaza->property("walkers").value<QAbstractItemModel *>()};
    if (!walkers) {
        return {};
    }
    const QHash<int, QByteArray> names{walkers->roleNames()};
    for (int row{0}; row < walkers->rowCount(); ++row) {
        QVariantMap values;
        for (auto role{names.cbegin()}; role != names.cend(); ++role) {
            values.insert(QString::fromUtf8(role.value()),
                          walkers->data(walkers->index(row, 0), role.key()));
        }
        if (values.value(QStringLiteral("id")).toString() == sub
            && values.value(QStringLiteral("x")).isValid()) {
            return values;
        }
    }
    return {};
}

// The same, waited for. The edge publishes the whole model every step, and for a moment after
// each publish the replica has the new rows and not yet their data, so a single read can
// come back empty. An empty read is not a position, and treating it as one is (0, 0).
QVariantMap settledRowOf(QRemoteObjectDynamicReplica *plaza, const QString &sub)
{
    QElapsedTimer waited;
    waited.start();
    QVariantMap row{rowOf(plaza, sub)};
    while (row.isEmpty() && waited.elapsed() < 2000) {
        QTest::qWait(5);
        row = rowOf(plaza, sub);
    }
    return row;
}

double distance(const QVariantMap &one, const QVariantMap &other)
{
    return std::hypot(one.value(QStringLiteral("x")).toDouble()
                          - other.value(QStringLiteral("x")).toDouble(),
                      one.value(QStringLiteral("z")).toDouble()
                          - other.value(QStringLiteral("z")).toDouble());
}

// The heading that walks from `from` towards `to`, as the edge reads a heading: forward is
// -z turned by the heading about the vertical axis.
double headingTowards(const QVariantMap &from, const QVariantMap &to)
{
    const double dx{to.value(QStringLiteral("x")).toDouble()
                    - from.value(QStringLiteral("x")).toDouble()};
    const double dz{to.value(QStringLiteral("z")).toDouble()
                    - from.value(QStringLiteral("z")).toDouble()};
    return std::atan2(-dx, -dz) * 180.0 / std::numbers::pi;
}

} // namespace

class TestFix4 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_edgePort{0};

    QByteArray walkerSession(const QString &sub, const QString &login)
    {
        return m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                       identityFor(sub, login));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterEdgeSources();
        m_engine = std::make_unique<QQmlEngine>();

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(FIX4_BUNDLE_DIR);
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(FIX4_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(FIX4_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};

        // The plaza, as the example's synqt.yaml declares it: shared, gated `user`.
        WebEdgeConnectPoint plaza;
        plaza.name = QStringLiteral("edge");
        plaza.contract = QStringLiteral("Edge");
        plaza.serverFile = QStringLiteral(FIX4_PLAZA_DIR "/web/edge/Edge.qml");
        plaza.scope = QStringLiteral("user");
        plaza.shared = true;
        config.connectPoints = {plaza};

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

    // Hands-on check 3. The gate is the connect point's scope. A session that has not signed
    // in never acquires the plaza, so there is no `walk` to call and no walker to read.
    void aVisitorWhoHasNotSignedInHasNoPlaza()
    {
        const QByteArray walker{walkerSession(QStringLiteral("gate-1"), QStringLiteral("ada"))};
        const QByteArray visitor{m_edge->sessionManager()->createSession()};

        QQmlEngine clientEngine;
        SynClient signedIn{clientConfig(m_edgePort, cookieFor(walker)), &clientEngine};
        SynClient anonymous{clientConfig(m_edgePort, cookieFor(visitor)), &clientEngine};
        signedIn.start();
        anonymous.start();
        QTRY_COMPARE_WITH_TIMEOUT(signedIn.session()->state(), QStringLiteral("connected"),
                                  8000);
        QTRY_COMPARE_WITH_TIMEOUT(anonymous.session()->state(), QStringLiteral("connected"),
                                  8000);

        QRemoteObjectDynamicReplica *theirs{plazaOf(&signedIn)};
        QRemoteObjectDynamicReplica *visitors{plazaOf(&anonymous)};
        QVERIFY(theirs && visitors);
        QTRY_VERIFY(theirs->isReplicaValid());
        QTest::qWait(1500);
        QVERIFY2(!visitors->isReplicaValid(),
                 "a visitor who has not signed in must not acquire the scope: user plaza");
    }

    // Hands-on check 1. `walk` takes what the keys ask for, never a position, and a push
    // larger than a full one is a full one. So the walker covers what walking covers.
    void aConsoleWalkIsStillWalkingSpeed()
    {
        const QString sub{QStringLiteral("racer-1")};
        QQmlEngine clientEngine;
        SynClient racer{clientConfig(m_edgePort, cookieFor(walkerSession(sub,
                                                                     QStringLiteral("racer")))),
                        &clientEngine};
        racer.start();
        QTRY_COMPARE_WITH_TIMEOUT(racer.session()->state(), QStringLiteral("connected"), 8000);
        QRemoteObjectDynamicReplica *plaza{plazaOf(&racer)};
        QVERIFY(plaza);
        QTRY_VERIFY(plaza->isReplicaValid());

        // Standing still, to arrive and be read.
        QVERIFY(QMetaObject::invokeMethod(plaza, "walk", Q_ARG(double, 0.0),
                                          Q_ARG(double, 0.0), Q_ARG(double, 90.0)));
        const QVariantMap start{settledRowOf(plaza, sub)};
        QVERIFY(!start.isEmpty());

        // A thousand times a full push, as from the browser console, kept up as a browser
        // keeps it up.
        QElapsedTimer clock;
        clock.start();
        QTimer keys;
        connect(&keys, &QTimer::timeout, plaza, [plaza]() {
            QMetaObject::invokeMethod(plaza, "walk", Q_ARG(double, 1000.0),
                                      Q_ARG(double, 0.0), Q_ARG(double, 90.0));
        });
        keys.start(100);
        QTest::qWait(1000);
        keys.stop();
        const QVariantMap later{settledRowOf(plaza, sub)};
        const double seconds{static_cast<double>(clock.elapsed()) / 1000.0};
        QVERIFY(!later.isEmpty());

        const double walked{distance(start, later)};
        QVERIFY2(walked > 50.0, qPrintable(QStringLiteral("the edge must move the walker, "
                                                          "moved %1").arg(walked)));
        // A walker against a wall or a pillar covers less, never more. The margin is the
        // publish interval either side of the two readings.
        QVERIFY2(walked <= (kSpeed * seconds) + (kSpeed * 0.2),
                 qPrintable(QStringLiteral("walked %1 in %2 s, faster than %3 a second")
                                .arg(walked).arg(seconds).arg(kSpeed)));
    }

    // Hands-on check 2. However one walker walks at another, the edge keeps them two radii
    // apart. The client's physics stops it at the same place, so this is also what the
    // prediction agrees with.
    void nobodyWalksThroughAnybody()
    {
        const QString still{QStringLiteral("still-1")};
        const QString pusher{QStringLiteral("pusher-1")};
        QQmlEngine clientEngine;
        SynClient standing{clientConfig(m_edgePort,
                                        cookieFor(walkerSession(still, QStringLiteral("still")))),
                           &clientEngine};
        SynClient walking{clientConfig(m_edgePort,
                                       cookieFor(walkerSession(pusher,
                                                               QStringLiteral("pusher")))),
                          &clientEngine};
        standing.start();
        walking.start();
        QTRY_COMPARE_WITH_TIMEOUT(standing.session()->state(), QStringLiteral("connected"),
                                  8000);
        QTRY_COMPARE_WITH_TIMEOUT(walking.session()->state(), QStringLiteral("connected"), 8000);
        QRemoteObjectDynamicReplica *theirs{plazaOf(&standing)};
        QRemoteObjectDynamicReplica *mine{plazaOf(&walking)};
        QVERIFY(theirs && mine);
        QTRY_VERIFY(theirs->isReplicaValid());
        QTRY_VERIFY(mine->isReplicaValid());

        // The aim is kept between reads. A read that comes back empty says nothing about where
        // anybody is, so the pusher keeps walking the way it last knew to.
        double aim{0.0};
        QTimer keys;
        connect(&keys, &QTimer::timeout, this, [theirs, mine, still, pusher, &aim]() {
            QMetaObject::invokeMethod(theirs, "walk", Q_ARG(double, 0.0), Q_ARG(double, 0.0),
                                      Q_ARG(double, 0.0));
            const QVariantMap target{rowOf(mine, still)};
            const QVariantMap self{rowOf(mine, pusher)};
            if (!target.isEmpty() && !self.isEmpty()) {
                aim = headingTowards(self, target);
            }
            QMetaObject::invokeMethod(mine, "walk", Q_ARG(double, 1.0), Q_ARG(double, 0.0),
                                      Q_ARG(double, aim));
        });
        keys.start(100);
        const QVariantMap firstTarget{settledRowOf(mine, still)};
        const QVariantMap firstSelf{settledRowOf(mine, pusher)};
        QVERIFY(!firstTarget.isEmpty() && !firstSelf.isEmpty());
        const double apartAtFirst{distance(firstTarget, firstSelf)};

        // Long enough to walk between any two places a walker can arrive at (at most 2830
        // apart, 8.1 s at walking speed), read every tick.
        double closest{apartAtFirst};
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 11000) {
            QTest::qWait(50);
            const QVariantMap self{rowOf(mine, pusher)};
            const QVariantMap target{rowOf(mine, still)};
            if (!self.isEmpty() && !target.isEmpty()) {
                closest = std::min(closest, distance(self, target));
            }
        }
        keys.stop();

        QVERIFY2(closest >= (2.0 * kRadius) - 0.5,
                 qPrintable(QStringLiteral("two walkers came within %1 of each other")
                                .arg(closest)));
        // And it did reach them. A pusher that stopped short would prove nothing above.
        QVERIFY2(closest <= (2.0 * kRadius) + 1.0,
                 qPrintable(QStringLiteral("the walker never reached the other one (closest "
                                           "%1, from %2)").arg(closest).arg(apartAtFirst)));
    }
};

QTEST_GUILESS_MAIN(TestFix4)
#include "tst_fix4.moc"
