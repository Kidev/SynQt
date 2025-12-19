// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client half of staying signed in. What goes into the OS secure store, that nothing
// goes anywhere else, and that a second launch does come back signed in.
//
// The first test here is the one that matters most, and it is the one that would still pass
// if the feature were quietly broken. With no store available, nothing is written anywhere.
// There is no file fallback in SynQt, and the moment one exists every sentence about
// where this credential lives stops being true. It is asserted against the real config,
// data and cache directories rather than against a promise in a comment.
//
// The rest needs a real keyring. On Linux ctest runs this suite through
// tests/lib/keyring-session.sh, which gives it a private session bus and a private keyring
// rather than the developer's own, so the Secret Service backend is exercised on a CI runner
// too. Where those tools are absent the tests skip, which is the right outcome and not a gap:
// it is exactly what a visitor on such a machine gets, which is a sign-in per launch. CI sets
// SYNQT_REQUIRE_SECURE_STORE on the column that does provide one, so a skip there is a broken
// recipe and fails rather than passing quietly.

#include "devicecredential.h"
#include "identityconfig.h"
#include "nullstore.h"
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
#include <QHash>
#include <QMutex>
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
#include <QThread>
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

/// Point the writable locations at a test directory for as long as the return value lives.
///
/// Nothing is given up by it. A fallback that resolved one of these locations would resolve
/// the redirected one and still land where the walk looks. What it buys is that the walk stops
/// being a recursive crawl of the user's entire profile, which on a Windows runner cost a
/// minute per call, and that a developer's own files are never what the walk is counting.
[[nodiscard]] auto scopedStandardPaths()
{
    QStandardPaths::setTestModeEnabled(true);
    return qScopeGuard([]() { QStandardPaths::setTestModeEnabled(false); });
}

/// The locations an application is allowed to write to, which is where a file fallback would
/// have to put its file. Call it inside the scope above, so it reports the redirected paths.
QStringList writableRoots()
{
    return {QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)};
}

/// A store that answers everything except a write, on demand.
///
/// It is not a convenience. It is the one machine state a working keyring cannot be asked
/// to reproduce, and it is the state that decides whether an innocent machine is later read as
/// a stolen one. Real examples are a Secret Service with no default collection, a keychain
/// item whose ACL has been revoked, and a full disk.
/// It is locked because a call abandoned at its deadline keeps running: the credential goes
/// on to erase while the write it gave up on is still inside this object, which is exactly
/// what happens on a real store and would otherwise be a race in the test rather than in the
/// thing under test.
class HalfWorkingStore : public SecureStore
{
public:
    /// Fail every write, the way a locked collection or a revoked ACL does.
    bool refusesWrites{false};
    /// Answer a write only after this long, the way a wedged keyring does. Past the
    /// credential's deadline this is a write whose outcome nobody ever learns.
    int writeDelayMs{0};

    bool isAvailable(QString *reason) const override
    {
        Q_UNUSED(reason);
        return true;
    }

    bool store(const QString &account, const QByteArray &secret, QString *error) override
    {
        if (writeDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(writeDelayMs));
        }
        if (refusesWrites) {
            if (error) {
                *error = QStringLiteral("the collection will not take a new value");
            }
            return false;
        }
        const QMutexLocker locked{&m_lock};
        m_items.insert(account, secret);
        return true;
    }

    bool load(const QString &account, QByteArray *secret, QString *error) override
    {
        Q_UNUSED(error);
        const QMutexLocker locked{&m_lock};
        const auto found{m_items.constFind(account)};
        if (found == m_items.constEnd()) {
            return false;
        }
        if (secret) {
            *secret = found.value();
        }
        return true;
    }

    bool erase(const QString &account, QString *error) override
    {
        Q_UNUSED(error);
        const QMutexLocker locked{&m_lock};
        m_items.remove(account);
        return true;
    }

    Binding binding() const override { return Binding::User; }
    QString name() const override { return QStringLiteral("half-working"); }

    /// What is on this store, asked directly rather than through the credential:
    /// once a store has been written off for the launch, the credential answers from that
    /// and would report an empty store however much was left in it.
    bool holdsAnything()
    {
        const QMutexLocker locked{&m_lock};
        return !m_items.isEmpty();
    }

private:
    QMutex m_lock;
    QHash<QString, QByteArray> m_items;
};

/// Does this machine have a store these tests can use?
///
/// `isAvailable()` is the question the client asks, and it is the right one there: it has to
/// be cheap and it must never prompt. It is not enough to decide whether a test can run,
/// because two of the three backends answer it without touching the store at all. A Mac has
/// a keychain, so the macOS backend says yes unconditionally and what varies is whether this
/// process may use it. The same is true of a Windows Credential Manager behind a policy. So
/// the suite proves it with a round trip through the same public API a client uses, and takes
/// its probe back out again.
bool aStoreThatWorks(DeviceCredential &credential)
{
    if (!credential.isAvailable()) {
        return false;
    }
    DeviceCredential::Held probe;
    probe.id = QStringLiteral("a-probe");
    probe.secret = QByteArrayLiteral("is this store usable");
    const bool stored{credential.save(probe)};
    credential.erase();
    return stored;
}

} // namespace

/// Skip when this machine has no usable secure store, unless it was supposed to have one.
///
/// A skip is the right answer on a headless box or a container, and it is also how a backend
/// goes unexercised for months without anybody noticing. CI sets SYNQT_REQUIRE_SECURE_STORE
/// on the column where the recipe provides a store (tests/lib/keyring-session.sh), so there
/// the absence of one is a broken recipe rather than a property of the machine, and it fails
/// instead of passing quietly.
#define SYNQT_SKIP_WITHOUT_A_STORE(credential)                                            \
    do {                                                                                  \
        if (!aStoreThatWorks(credential)) {                                               \
            if (qEnvironmentVariableIsSet("SYNQT_REQUIRE_SECURE_STORE")) {                \
                QFAIL("SYNQT_REQUIRE_SECURE_STORE is set, so this machine is supposed " \
                      "to have a secure store, and it has none it can write to");        \
            }                                                                             \
            QSKIP("no secure store on this machine, which is a supported outcome");       \
        }                                                                                 \
    } while (false)

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

    // The promise the whole design rests on. With no store, nothing is persisted anywhere.
    // Not a file under the config directory, not one under data, not one under cache.
    //
    // The store is injected rather than taken from the machine, because "this computer has
    // no keyring" is not a state Windows or macOS can be put into. Both always have one, and
    // stripping a session bus only speaks to the Linux backend. The claim being made is about
    // what DeviceCredential does when its store holds nothing, so it is asked of the store
    // that stands for a machine with none, on every platform. What the platform picker itself
    // does when its keyring is out of reach is the test below, where it belongs.
    void withoutAStoreNothingIsWritten()
    {
        const auto scoped{scopedStandardPaths()};
        const QStringList roots{writableRoots()};
        const QSet<QString> before{filesUnder(roots)};

        DeviceCredential credential{edgeWsUrl(), std::make_unique<NullStore>()};
        QVERIFY(!credential.isAvailable());
        QCOMPARE(credential.binding(), SecureStore::Binding::None);
        QCOMPARE(credential.bindingName(), QStringLiteral("none"));
        QCOMPARE(credential.storeName(), QStringLiteral("none"));

        DeviceCredential::Held held;
        held.id = QStringLiteral("family");
        held.secret = QByteArrayLiteral("secret");
        QVERIFY(!credential.save(held));
        QVERIFY(!credential.load().isValid());
        credential.erase();  // must not throw, hang, or create anything

        QCOMPARE(filesUnder(roots), before);
    }

    // The same promise where the platform picker makes it. A machine whose keyring is out of
    // reach must come back with a store that reports itself unavailable, never with one that
    // quietly became a file, and that is a decision makeSecureStore() takes rather than the
    // credential. Only Linux can be put into the state from a test, which is the whole reason
    // the test above injects instead of arranging for one.
    void anUnreachableKeyringIsNotAFileFallback()
    {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        QSKIP("the store on this platform is always reachable, so there is no such state to "
              "put it in; the injected case above carries the claim here");
#else
        const auto scoped{scopedStandardPaths()};
        const QStringList roots{writableRoots()};
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

        DeviceCredential::Held held;
        held.id = QStringLiteral("family");
        held.secret = QByteArrayLiteral("secret");
        QVERIFY(!credential.save(held));
        QVERIFY(!credential.load().isValid());
        credential.erase();

        QCOMPARE(filesUnder(roots), before);
#endif
    }

    // The round trip, on whatever store this machine has.
    void storeRoundTrip()
    {
        DeviceCredential credential{edgeWsUrl()};
        SYNQT_SKIP_WITHOUT_A_STORE(credential);
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

    // The failure that would otherwise stage a theft. A store that reads but cannot write
    // leaves the generation this rotation was replacing sitting there; the edge has already
    // retired that one, so presenting it at the next launch is the signature of a second copy
    // in circulation, and the family and every session on it are revoked. The visitor did
    // nothing wrong and their keyring is the only thing that failed, so a failed write leaves
    // nothing behind instead, and the next launch is an ordinary sign-in.
    void aRotationThatCannotBeStoredLeavesNothingBehind()
    {
        auto owned{std::make_unique<HalfWorkingStore>()};
        HalfWorkingStore *store{owned.get()};
        DeviceCredential credential{edgeWsUrl(), std::move(owned)};
        QVERIFY(credential.isAvailable());

        DeviceCredential::Held enrolled;
        enrolled.id = QStringLiteral("a-family-id");
        enrolled.secret = QByteArrayLiteral("the-first-generation");
        QVERIFY(credential.save(enrolled));
        QCOMPARE(credential.load().secret, enrolled.secret);

        store->refusesWrites = true;
        DeviceCredential::Held rotated;
        rotated.id = enrolled.id;
        rotated.secret = QByteArrayLiteral("the-second-generation");
        QVERIFY(!credential.save(rotated));
        QVERIFY2(!credential.load().isValid(),
                 "a rotation that could not be written left the retired generation behind, "
                 "which the next launch would present as a stolen copy");
    }

    // The same hazard through the other door. A store that answers too late is abandoned at
    // the deadline, and then nobody knows whether the write landed. What is on disk is either
    // the new generation or the retired one, and the retired one is read as theft. Neither is
    // worth keeping over a store this launch has already written off.
    void aRotationThatTimesOutLeavesNothingBehind()
    {
        auto owned{std::make_unique<HalfWorkingStore>()};
        HalfWorkingStore *store{owned.get()};
        DeviceCredential credential{edgeWsUrl(), std::move(owned)};

        DeviceCredential::Held enrolled;
        enrolled.id = QStringLiteral("a-family-id");
        enrolled.secret = QByteArrayLiteral("the-first-generation");
        QVERIFY(credential.save(enrolled));
        QVERIFY(store->holdsAnything());

        // Longer than the credential's deadline, and it ends in a refusal, so this models a
        // keyring that hung and stored nothing.
        store->writeDelayMs = 2500;
        store->refusesWrites = true;
        DeviceCredential::Held rotated;
        rotated.id = enrolled.id;
        rotated.secret = QByteArrayLiteral("the-second-generation");
        QVERIFY(!credential.save(rotated));

        // Asked of the store rather than the credential. The credential has written this
        // store off for the launch and would report it empty whatever is in it. Waited out
        // past the abandoned write, so a store that came back and wrote anyway is caught too.
        QTest::qWait(1200);
        QVERIFY2(!store->holdsAnything(),
                 "a rotation abandoned at its deadline left something on the store, and the "
                 "next launch would present it");
    }

    // Whatever the store does, it does within the deadline. A keyring that stopped to ask
    // the visitor something would otherwise be an app that never draws its first frame.
    void theStoreAnswersWithinItsDeadline()
    {
        DeviceCredential credential{edgeWsUrl()};
        SYNQT_SKIP_WITHOUT_A_STORE(credential);
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
        SYNQT_SKIP_WITHOUT_A_STORE(probe);
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
