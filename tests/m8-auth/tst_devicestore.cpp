// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client half of staying signed in: what goes into the OS secure store, that nothing
// goes anywhere else, and that a second launch comes back signed in.
//
// The first test matters most, because it would still pass if the feature were quietly
// broken. With no store available, nothing is written anywhere. SynQt has no file fallback,
// and one would make every statement about where this credential lives false. It is
// asserted against the real config, data and cache directories.
//
// The rest needs a real keyring. On Linux ctest runs this suite through
// tests/lib/keyring-session.sh, which gives it a private session bus and a private keyring,
// so the Secret Service backend is exercised on a CI runner too. Where those tools are
// absent the tests skip, which is what a visitor on such a machine gets: a sign-in per
// launch. CI sets SYNQT_REQUIRE_SECURE_STORE on the column that provides a store, so a skip
// there fails.

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
/// A fallback that resolved one of these locations would resolve the redirected one and
/// still land where the walk looks. The redirect keeps the walk from crawling the user's
/// whole profile (slow on Windows) and keeps a developer's own files out of the count.
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
/// It is the one machine state a working keyring cannot be asked to reproduce, and it
/// decides whether an innocent machine is later read as a stolen one. Real examples are a
/// Secret Service with no default collection, a keychain item whose ACL has been revoked,
/// and a full disk.
///
/// It is locked because a call abandoned at its deadline keeps running: the credential goes
/// on to erase while the abandoned write is still inside this object, as on a real store.
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
/// `isAvailable()` is the client's question, and it is right there: it must be cheap and
/// never prompt. It cannot decide whether a test can run, because two of the three backends
/// answer it without touching the store: the macOS backend says yes unconditionally, and
/// what varies is whether this process may use the keychain; the same holds for a Windows
/// Credential Manager behind a policy. So the suite proves it with a round trip through the
/// public API a client uses, and removes its probe again.
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
/// A skip is right on a headless box or a container, and it is also how a backend goes
/// unexercised unnoticed. CI sets SYNQT_REQUIRE_SECURE_STORE on the column where the recipe
/// provides a store (tests/lib/keyring-session.sh), so there a missing store is a broken
/// recipe and fails.
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
    quint16 m_altEdgePort{0};
    QUrl m_browserLanding;

    QUrl edgeWsUrl(quint16 port = 0) const
    {
        return QUrl{QStringLiteral("ws://127.0.0.1:%1/sync")
                        .arg(port == 0 ? m_edgePort : port)};
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

    Response post(quint16 port, const QString &path, const QUrlQuery &form)
    {
        QNetworkRequest request{
            QUrl{QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)}};
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        QNetworkReply *reply{
            m_browser.post(request, form.toString(QUrl::FullyEncoded).toUtf8())};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    /// Sign one client in and wait for the credential to reach the store.
    ///
    /// Not waiting for "connected": one caller's edge refuses every socket on
    /// purpose, and enrolment does not depend on one. It rides the claim, over HTTP, which
    /// is why a client can be enrolled and unable to connect at the same time.
    DeviceCredential::Held signInAndEnrol(SynClient &client, DeviceCredential &probe)
    {
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "driveBrowser");
        const auto releaseHandler{qScopeGuard([]() {
            QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        })};

        client.start();
        client.session()->login(QStringLiteral("stub"));
        DeviceCredential::Held enrolled;
        // Polled at a human interval rather than through QTRY_VERIFY: every turn of this is
        // a real read of a real keyring, and a 50 ms poll would be hundreds of them.
        for (int attempt{0}; attempt < 40 && !enrolled.isValid(); ++attempt) {
            QTest::qWait(250);
            enrolled = probe.load();
        }
        return enrolled;
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

    /// The edge every test here talks to, before the one tweak a test makes to it. The two
    /// tests that need an edge in a state the rest must not see build their own from this.
    WebEdgeConfig baseEdgeConfig(const QString &deviceStoreFile) const
    {
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.allowDesktopLogin = true;
        // The mapping hook that gives a session its scope, and the vocabulary its
        // Scope members were generated from. An edge with a login and neither
        // refuses every login, because there is nothing to resolve an answer against.
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        config.identity.providers = {stubProvider(m_stub->baseUrl())};
        config.identity.device.enabled = true;
        config.identity.device.store.name = QStringLiteral("sqlite");
        config.identity.device.store.file = deviceStoreFile;
        // Nothing anonymous gets on, so a client that reaches "connected" has proved it is
        // presenting an authenticated session and not merely that a session exists.
        config.identityRequired = true;
        return config;
    }

    /// Bring up an edge of its own for one test, and let driveBrowser know about its port so
    /// the stand-in browser can still tell a loopback redirect from an edge hop.
    std::unique_ptr<WebEdge> startOwnEdge(const WebEdgeConfig &config, quint16 *port)
    {
        auto edge{std::make_unique<WebEdge>(config, m_engine.get())};
        if (!edge->start()) {
            return {};
        }
        *port = edge->serverPort();
        m_altEdgePort = *port;
        return edge;
    }

    SynClientConfig clientConfig(quint16 port = 0) const
    {
        SynClientConfig config;
        config.edgeUrl = edgeWsUrl(port);
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

        const WebEdgeConfig config{baseEdgeConfig(
            QDir{m_storeDir.path()}.filePath(QStringLiteral("devices.db")))};
        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    // The promise the design rests on: with no store, nothing is persisted anywhere, not
    // under the config, data or cache directory.
    //
    // The store is injected rather than taken from the machine, because Windows and macOS
    // cannot be put into a "no keyring" state, and stripping a session bus only affects the
    // Linux backend. The claim is about what DeviceCredential does when its store holds
    // nothing, so it is asked of the store that stands for a machine with none, on every
    // platform. What the platform picker does when its keyring is out of reach is the test
    // below.
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
        // Erasing what is not there is a success. Signing out must never depend on the
        // store agreeing about what it held.
        credential.erase();
        QVERIFY(!credential.load().isValid());
    }

    // The launch this feature exists for, asked of the store alone: one that has written
    // nothing, and whose first question is a read.
    //
    // Every other test here writes before it reads, the skip guard included, and a store
    // can answer a read that follows its own write and still fail the only read that
    // matters. It is the first call SynClient::openSession makes. A miss reads as an
    // ordinary first launch, so the app signs in again with the credential still on the
    // store. On macOS, which of its two keychains an item lands in depends on how the build
    // was signed, and a read is not told about it the way a write is.
    void aLaunchWhoseFirstCallIsAReadFindsIt()
    {
        DeviceCredential writer{edgeWsUrl()};
        SYNQT_SKIP_WITHOUT_A_STORE(writer);
        const auto cleanup{qScopeGuard([&writer]() { writer.erase(); })};

        DeviceCredential::Held held;
        held.id = QStringLiteral("a-family-id");
        held.secret = QByteArrayLiteral("what-the-next-launch-has-to-find");
        QVERIFY(writer.save(held));

        // Not the credential above, and not through the skip guard:
        // both of those have written to this store already, and having written is exactly the
        // thing a next launch has not done.
        DeviceCredential reader{edgeWsUrl()};
        const DeviceCredential::Held found{reader.load()};
        QVERIFY2(found.isValid(),
                 "the store held a credential and a launch that had written nothing did not "
                 "find it, so every launch signs in again with the credential still there");
        QCOMPARE(found.id, held.id);
        QCOMPARE(found.secret, held.secret);
    }

    // The failure that would otherwise stage a theft. A store that reads but cannot write
    // leaves behind the generation this rotation replaced. The edge has already retired
    // that one, so presenting it at the next launch looks like a second copy in
    // circulation, and the family and every session on it are revoked. The visitor did
    // nothing wrong, so a failed write leaves nothing behind instead, and the next launch
    // is an ordinary sign-in.
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
    // the deadline, and nobody knows whether the write landed: the stored value is either
    // the new generation or the retired one, and the retired one reads as theft. Neither is
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

    // End to end, and what all of it is for: sign in once, close the app, open it again,
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

    // What the credential is spent on. It buys a session, not a connection.
    //
    // Here is an edge that is up and answering HTTP and will not accept this client's
    // socket, because its allowed origins are somebody else's. A hardened or a
    // misconfigured deployment looks like that from the client, which cannot see the
    // reason: a refused socket says only that it was refused.
    //
    // So the client reconnects every few hundred milliseconds. Spending the stored
    // credential on each reconnect, for a session identical to the one already held, would
    // retire a generation per reconnect and use up the edge's rate window (30 a minute per
    // address, shared with everyone behind it) within a minute. The refusal that follows
    // would read as "this credential is dead" and delete it: a month of staying signed in,
    // lost to a bad afternoon at the edge.
    void aClientThatCannotConnectSpendsTheCredentialOnce()
    {
        WebEdgeConfig config{baseEdgeConfig(
            QDir{m_storeDir.path()}.filePath(QStringLiteral("unreachable.db")))};
        config.allowedOrigins = {QStringLiteral("https://somewhere.else.example")};
        quint16 port{0};
        const std::unique_ptr<WebEdge> edge{startOwnEdge(config, &port)};
        QVERIFY(edge);
        // The port is only a landmark for the stand-in browser while this edge is alive;
        // left behind, it is one a later test's loopback listener could be handed.
        const auto forgetPort{qScopeGuard([this]() { m_altEdgePort = 0; })};

        DeviceCredential probe{edgeWsUrl(port)};
        SYNQT_SKIP_WITHOUT_A_STORE(probe);
        probe.erase();
        const auto cleanup{qScopeGuard([&probe]() { probe.erase(); })};

        QQmlEngine engine;
        SynClient client{clientConfig(port), &engine};
        const DeviceCredential::Held enrolled{signInAndEnrol(client, probe)};
        QVERIFY2(enrolled.isValid(), "signing in did not enrol a device credential");

        // Reconnects are 200 to 400 ms apart here, so this is a dozen or so attempts, and
        // every one of them is an opportunity to spend the credential again.
        QTest::qWait(4000);
        QVERIFY2(client.state() != QStringLiteral("connected"), qPrintable(client.state()));

        const DeviceCredential::Held afterwards{probe.load()};
        QVERIFY2(afterwards.isValid(),
                 "the stored sign-in was deleted while the edge was refusing sockets");
        QCOMPARE(afterwards.id, enrolled.id);
        QVERIFY2(afterwards.secret == enrolled.secret,
                 "the credential rotated while the client was failing to connect, so it is "
                 "being spent once per reconnect for a session it already has");
    }

    // And the refusal that is not about the credential at all. The route is rate limited
    // per address, and the limit is shared with every machine behind the same address: a
    // home, an office, a container host, a mobile carrier. When somebody else spends that
    // window, this client must wait, not conclude that its credential is dead and delete
    // it.
    //
    // Both halves are needed. The edge answers the limit differently from a refused
    // credential (it decides before reading the credential, so it gives nothing away), and
    // the client acts only on the answer about the credential. Either one missing signs the
    // visitor out.
    void aRateLimitDoesNotCostTheStoredSignIn()
    {
        quint16 port{0};
        const std::unique_ptr<WebEdge> edge{startOwnEdge(
            baseEdgeConfig(QDir{m_storeDir.path()}.filePath(QStringLiteral("busy.db"))),
            &port)};
        QVERIFY(edge);
        // The port is only a landmark for the stand-in browser while this edge is alive;
        // left behind, it is one a later test's loopback listener could be handed.
        const auto forgetPort{qScopeGuard([this]() { m_altEdgePort = 0; })};

        DeviceCredential probe{edgeWsUrl(port)};
        SYNQT_SKIP_WITHOUT_A_STORE(probe);
        probe.erase();
        const auto cleanup{qScopeGuard([&probe]() { probe.erase(); })};

        QQmlEngine engine;
        DeviceCredential::Held enrolled;
        {
            SynClient first{clientConfig(port), &engine};
            enrolled = signInAndEnrol(first, probe);
            QVERIFY2(enrolled.isValid(), "signing in did not enrol a device credential");
            QTRY_COMPARE_WITH_TIMEOUT(first.state(), QStringLiteral("connected"), 15000);
        }

        // The neighbour, spending the window on credentials of their own that are worth
        // nothing. Bounded rather than counted out exactly, so the window's size stays the
        // edge's business.
        QUrlQuery junk;
        junk.addQueryItem(QStringLiteral("device_id"), QStringLiteral("not-a-family"));
        junk.addQueryItem(QStringLiteral("device_secret"), QStringLiteral("not-a-secret"));
        Response refused;
        for (int attempt{0}; attempt < 60 && refused.status != 429; ++attempt) {
            refused = post(port, QStringLiteral("/auth/login/device"), junk);
        }
        QVERIFY2(refused.status == 429,
                 "the rate window answers exactly as a dead credential does, so no client "
                 "can tell being told to wait from being told to give up");

        // And now the visitor opens their app, into a window they never touched.
        SynClient second{clientConfig(port), &engine};
        second.start();
        QTest::qWait(3000);

        const DeviceCredential::Held afterwards{probe.load()};
        QVERIFY2(afterwards.isValid(),
                 "a rate limit somebody else spent deleted this visitor's stored sign-in, "
                 "so their app asks them to sign in again over a busy minute");
        QCOMPARE(afterwards.secret, enrolled.secret);
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
                && next.port() != m_altEdgePort
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
