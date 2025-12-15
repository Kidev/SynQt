// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client half of staying signed in: what goes into the OS secure store, that nothing
// goes anywhere else, and that a second launch really does come back signed in.
//
// The first test here is the one that matters most, and it is the one that would still pass
// if the feature were quietly broken: with no store available, nothing is written anywhere.
// There is no file fallback in SynQt, and the moment one exists every honest sentence about
// where this credential lives stops being true. It is asserted against the actual config,
// data and cache directories rather than against a promise in a comment.
//
// The rest needs a real keyring, so it skips where there is none (a container, CI without a
// session bus, a headless box). Skipping is the right outcome there and not a gap: it is
// exactly what a visitor on such a machine gets, which is a sign-in per launch.

#include "devicecredential.h"
#include "identityconfig.h"
#include "securestore.h"
#include "session.h"
#include "stubidentityserver.h"
#include "synclient.h"
#include "synclientconfig.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <memory>

using namespace SynQt;

namespace {

struct Response
{
    int status{0};
    QString location;
    QByteArray body;
};

IdentityProviderConfig stubProvider(const QString &base)
{
    IdentityProviderConfig provider;
    provider.name = QStringLiteral("stub");
    provider.devStub = true;
    provider.authorizeUrl = QUrl{base + QStringLiteral("/authorize")};
    provider.tokenUrl = QUrl{base + QStringLiteral("/token")};
    provider.userinfoUrl = QUrl{base + QStringLiteral("/userinfo")};
    provider.clientId = QStringLiteral("stub-client");
    provider.clientSecret = QStringLiteral("stub-secret");
    provider.scopes = {QStringLiteral("read:user")};
    return provider;
}

/// Every file under a directory, so a "nothing was written" claim is checked rather than
/// asserted. Recursive, because a fallback would put its file in a subdirectory of its own.
QSet<QString> filesUnder(const QStringList &roots)
{
    QSet<QString> seen;
    for (const QString &root : roots) {
        QDirIterator walk{root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories};
        while (walk.hasNext()) {
            seen.insert(walk.next());
        }
    }
    return seen;
}

} // namespace

class DeviceStoreTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<StubIdentityServer> m_stub;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<WebEdge> m_edge;
    QTemporaryDir m_storeDir;
    QNetworkAccessManager m_browser;
    quint16 m_edgePort{0};
    QUrl m_browserLanding;

    QUrl edgeWsUrl() const
    {
        return QUrl{QStringLiteral("ws://127.0.0.1:%1/sync").arg(m_edgePort)};
    }

    Response get(const QUrl &url)
    {
        QNetworkRequest request{url};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply *reply{m_browser.get(request)};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.location = QString::fromUtf8(reply->rawHeader("Location"));
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    static void hitLoopback(quint16 port, const QByteArray &target)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!QTest::qWaitFor([&socket]() {
                return socket.state() == QAbstractSocket::ConnectedState; }, 3000)) {
            return;
        }
        socket.write("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        QByteArray answer;
        const bool closed{QTest::qWaitFor([&socket, &answer]() {
            answer += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        }, 3000)};
        Q_UNUSED(closed);
    }

    SynClientConfig clientConfig() const
    {
        SynClientConfig config;
        config.edgeUrl = edgeWsUrl();
        config.loginRoute = QStringLiteral("/auth/login");
        config.logoutRoute = QStringLiteral("/auth/logout");
        config.deviceSession = true;
        config.heartbeatMs = 500;
        config.reconnectBaseMs = 200;
        config.reconnectMaxMs = 400;
        return config;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_storeDir.isValid());
        m_browser.setCookieJar(new QNetworkCookieJar{&m_browser});

        m_stub = std::make_unique<StubIdentityServer>(StubIdentityServer::DevOnly{});
        m_stub->setClientCredentials(QStringLiteral("stub-client"),
                                     QStringLiteral("stub-secret"));
        QVERIFY(m_stub->start());

        m_engine = std::make_unique<QQmlEngine>();

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.allowDesktopLogin = true;
        config.identity.providers = {stubProvider(m_stub->baseUrl())};
        config.identity.device.enabled = true;
        config.identity.device.store.name = QStringLiteral("sqlite");
        config.identity.device.store.file =
            QDir{m_storeDir.path()}.filePath(QStringLiteral("devices.db"));
        // Nothing anonymous gets on, so a client that reaches "connected" has proved it is
        // presenting an authenticated session and not merely that a session exists.
        config.identityRequired = true;

        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    // The promise the whole design rests on: with no store, nothing is persisted anywhere.
    // Not a file under the config directory, not one under data, not one under cache.
    void withoutAStoreNothingIsWritten()
    {
        const QStringList roots{
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
        const QSet<QString> before{filesUnder(roots)};

        // A stripped environment is what a container, a CI runner and an SSH session look
        // like, and it is the case a file fallback would have been written for.
        const QByteArray bus{qgetenv("DBUS_SESSION_BUS_ADDRESS")};
        qunsetenv("DBUS_SESSION_BUS_ADDRESS");
        const auto restore{qScopeGuard([&bus]() {
            if (!bus.isEmpty()) {
                qputenv("DBUS_SESSION_BUS_ADDRESS", bus);
            }
        })};

        DeviceCredential credential{edgeWsUrl()};
        QVERIFY(!credential.isAvailable());
        QCOMPARE(credential.binding(), SecureStore::Binding::None);
        QCOMPARE(credential.bindingName(), QStringLiteral("none"));

        DeviceCredential::Held held;
        held.id = QStringLiteral("family");
        held.secret = QByteArrayLiteral("secret");
        QVERIFY(!credential.save(held));
        QVERIFY(!credential.load().isValid());
        credential.erase();  // must not throw, hang, or create anything

        QCOMPARE(filesUnder(roots), before);
    }

    // The round trip, on whatever store this machine has.
    void storeRoundTrip()
    {
        DeviceCredential credential{edgeWsUrl()};
        if (!credential.isAvailable()) {
            QSKIP("no secure store on this machine, which is a supported outcome");
        }
        const auto cleanup{qScopeGuard([&credential]() { credential.erase(); })};

        DeviceCredential::Held held;
        held.id = QStringLiteral("a-family-id");
        held.secret = QByteArrayLiteral("a-device-secret");
        QVERIFY(credential.save(held));

        const DeviceCredential::Held read{credential.load()};
        QVERIFY(read.isValid());
        QCOMPARE(read.id, held.id);
        QCOMPARE(read.secret, held.secret);

        // Rotation overwrites in place rather than accumulating one item per launch.
        DeviceCredential::Held next;
        next.id = held.id;
        next.secret = QByteArrayLiteral("the-next-generation");
        QVERIFY(credential.save(next));
        QCOMPARE(credential.load().secret, next.secret);

        credential.erase();
        QVERIFY(!credential.load().isValid());
        // Erasing what is not there is a success: signing out must never depend on the
        // store agreeing about what it held.
        credential.erase();
        QVERIFY(!credential.load().isValid());
    }

    // Whatever the store does, it does within the deadline. A keyring that stopped to ask
    // the visitor something would otherwise be an app that never draws its first frame.
    void theStoreAnswersWithinItsDeadline()
    {
        DeviceCredential credential{edgeWsUrl()};
        if (!credential.isAvailable()) {
            QSKIP("no secure store on this machine, which is a supported outcome");
        }
        QElapsedTimer clock;
        clock.start();
        credential.load();
        QVERIFY2(clock.elapsed() < 2500, qPrintable(QStringLiteral("the store took %1 ms")
                                                        .arg(clock.elapsed())));
    }

    // End to end, and the point of all of it: sign in once, close the app, open it again,
    // and be signed in without a browser, a password or a prompt.
    void aSecondLaunchIsStillSignedIn()
    {
        DeviceCredential probe{edgeWsUrl()};
        if (!probe.isAvailable()) {
            QSKIP("no secure store on this machine, which is a supported outcome");
        }
        probe.erase();
        const auto cleanup{qScopeGuard([&probe]() { probe.erase(); })};

        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "driveBrowser");
        const auto releaseHandler{qScopeGuard([]() {
            QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        })};

        QQmlEngine engine;
        {
            SynClient first{clientConfig(), &engine};
            first.start();
            QTest::qWait(1000);
            QVERIFY2(first.state() != QStringLiteral("connected"), qPrintable(first.state()));

            first.session()->login(QStringLiteral("stub"));
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), QStringLiteral("connected"), 15000);
        }
        // The app is gone, and with it every session it held in memory. What is left is what
        // the store has.
        const DeviceCredential::Held enrolled{probe.load()};
        QVERIFY2(enrolled.isValid(), "signing in did not enrol a device credential");

        // The second launch: no session, no browser, no sign-in. The edge refuses anonymous
        // connections, so reaching "connected" is the whole proof.
        SynClient second{clientConfig(), &engine};
        second.start();
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), QStringLiteral("connected"), 15000);

        // And it rotated. What is stored now is not what was stored a moment ago, so a copy
        // taken between the two launches is already worthless.
        const DeviceCredential::Held rotated{probe.load()};
        QVERIFY(rotated.isValid());
        QCOMPARE(rotated.id, enrolled.id);
        QVERIFY(rotated.secret != enrolled.secret);

        // Signing out takes the credential with it, on both sides. A third launch is a
        // stranger again.
        second.session()->logout();
        QTRY_VERIFY_WITH_TIMEOUT(second.state() != QStringLiteral("connected"), 10000);
        QVERIFY(!probe.load().isValid());

        SynClient third{clientConfig(), &engine};
        third.start();
        QTest::qWait(2000);
        QVERIFY2(third.state() != QStringLiteral("connected"), qPrintable(third.state()));
    }

    void cleanupTestCase()
    {
        m_edge.reset();
    }

public slots:
    /// The stand-in system browser, as tst_desktop uses it. Walk the redirects the way a
    /// browser would and deliver the last hop to the client's own loopback listener.
    void driveBrowser(const QUrl &url)
    {
        Response hop{get(url)};
        for (int step{0}; step < 5 && hop.status == 302; ++step) {
            const QUrl next{hop.location};
            if (next.host() == QLatin1String("127.0.0.1")
                && next.port() != m_edgePort
                && next.port() != QUrl{m_stub->baseUrl()}.port()) {
                m_browserLanding = next;
                hitLoopback(static_cast<quint16>(next.port()),
                            (next.path() + QLatin1Char('?') + next.query()).toUtf8());
                return;
            }
            hop = get(next);
        }
        QFAIL("the stand-in browser never reached a loopback redirect");
    }
};

QTEST_MAIN(DeviceStoreTest)
#include "tst_devicestore.moc"
