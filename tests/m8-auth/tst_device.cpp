// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Staying signed in on the desktop: the device credential, its rotation, and the reuse
// detection that is the whole reason it is not a stored session.
//
// What is proved here, in order of how much it would cost to get wrong:
//
//  - What is stored is not a session. It buys one, once, at one route, and the session it
//    buys is the ordinary length. Presenting it as a session credential gets nowhere.
//  - Every redemption rotates. The generation spent stops working, so a credential
//    copied off a disk is good only until the machine it came from next starts up.
//  - A retired generation presented past the overlap window means two copies exist: the
//    family and every session it opened are revoked. Inside the window it is the innocent
//    case (the client never received the answer) and costs nothing.
//  - Scope is re-derived at every redemption, so somebody demoted since enrolment does not
//    keep what they had for the rest of the month.
//  - Signing out ends the credential, and both clocks (absolute and inactivity) end it too.
//  - A client whose store is below `min_binding` is still signed in. It persists nothing,
//    which is exactly what a project that never asked for any of this gets.

#include "deviceregistry.h"
#include "identityconfig.h"
#include "identityprovider.h"
#include "sessionmanager.h"
#include "stubidentityserver.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <QWebSocket>

#include <memory>

using namespace SynQt;

namespace {

struct Response
{
    int status{0};
    QString location;
    QByteArray cacheControl;
    QByteArray retryAfter;
    QByteArray body;

    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

/// What a client is left holding after a sign-in or a redemption.
struct Held
{
    QString session;
    QString deviceId;
    QString deviceSecret;

    bool hasCredential() const { return !deviceId.isEmpty() && !deviceSecret.isEmpty(); }
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

QString challengeFor(const QByteArray &verifier)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

class DeviceTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<StubIdentityServer> m_stub;
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<WebEdge> m_edge;
    QTemporaryDir m_storeDir;
    QNetworkAccessManager m_browser;
    quint16 m_edgePort{0};

    QString storeFile() const
    {
        return QDir{m_storeDir.path()}.filePath(QStringLiteral("devices.db"));
    }

    QString edgeUrl(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_edgePort).arg(path);
    }

    Response get(const QUrl &url)
    {
        QNetworkRequest request{url};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        return await(m_browser.get(request));
    }

    Response post(const QString &path, const QUrlQuery &form,
                  const QByteArray &origin = QByteArray{})
    {
        QNetworkRequest request{QUrl{edgeUrl(path)}};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        if (!origin.isEmpty()) {
            request.setRawHeader("Origin", origin);
        }
        return await(m_browser.post(request, form.toString(QUrl::FullyEncoded).toUtf8()));
    }

    Response await(QNetworkReply *reply)
    {
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.location = QString::fromUtf8(reply->rawHeader("Location"));
        response.cacheControl = reply->rawHeader("Cache-Control");
        response.retryAfter = reply->rawHeader("Retry-After");
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    /// The whole desktop sign-in, ending at the claim: the login route, the provider, the
    /// callback, and then the exchange the native client makes over its own connection. The
    /// browser half is driven exactly as tst_desktop drives it, because enrolment is
    /// deliberately not a route of its own.
    Held signIn(const QString &binding)
    {
        const QByteArray verifier{"verifier-" + QByteArray::number(++m_signIns)};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), QStringLiteral("http://127.0.0.1:5555/"));
        query.addQueryItem(QStringLiteral("return_state"), QStringLiteral("nonce"));
        query.addQueryItem(QStringLiteral("return_challenge"), challengeFor(verifier));
        QUrl login{edgeUrl(QStringLiteral("/auth/login"))};
        login.setQuery(query);

        const Response started{get(login)};
        if (started.status != 302) {
            return Held{};
        }
        const Response authorize{get(QUrl{started.location})};
        const Response callback{get(QUrl{authorize.location})};
        const QString code{QUrlQuery{QUrl{callback.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        if (code.isEmpty()) {
            return Held{};
        }

        QUrlQuery claim;
        claim.addQueryItem(QStringLiteral("code"), code);
        claim.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        if (!binding.isEmpty()) {
            claim.addQueryItem(QStringLiteral("device"), QStringLiteral("1"));
            claim.addQueryItem(QStringLiteral("binding"), binding);
            claim.addQueryItem(QStringLiteral("label"), QStringLiteral("a test machine"));
        }
        const Response claimed{post(QStringLiteral("/auth/login/claim"), claim)};
        if (claimed.status != 200) {
            return Held{};
        }
        const QJsonObject answer{claimed.json()};
        Held held;
        held.session = answer.value(QStringLiteral("session")).toString();
        held.deviceId = answer.value(QStringLiteral("device_id")).toString();
        held.deviceSecret = answer.value(QStringLiteral("device_secret")).toString();
        return held;
    }

    /// One relaunch. Spend a credential for a session and the next credential.
    Response redeem(const Held &held, const QByteArray &origin = QByteArray{})
    {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("device_id"), held.deviceId);
        form.addQueryItem(QStringLiteral("device_secret"), held.deviceSecret);
        return post(QStringLiteral("/auth/login/device"), form, origin);
    }

    static Held heldFrom(const Response &response)
    {
        const QJsonObject answer{response.json()};
        Held held;
        held.session = answer.value(QStringLiteral("session")).toString();
        held.deviceId = answer.value(QStringLiteral("device_id")).toString();
        held.deviceSecret = answer.value(QStringLiteral("device_secret")).toString();
        return held;
    }

    /// Reach into the store the way time or an operator would. Both clocks and the overlap
    /// window are measured in days and seconds, so the only way to test their far side
    /// without waiting for it is to move the row's timestamps. The alternative is
    /// millisecond-precision configuration that exists for no reason but this test.
    void amendRow(const QString &family, const QString &assignment, const QVariant &value)
    {
        const QString connection{QStringLiteral("device-test-amend")};
        {
            QSqlDatabase db{QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection)};
            db.setDatabaseName(storeFile());
            QVERIFY(db.open());
            QSqlQuery update{db};
            QVERIFY(update.prepare(QStringLiteral("UPDATE synqt_devices SET %1 = ? "
                                                  "WHERE family = ?").arg(assignment)));
            update.addBindValue(value);
            update.addBindValue(family);
            QVERIFY2(update.exec(), qPrintable(update.lastError().text()));
        }
        QSqlDatabase::removeDatabase(connection);
    }

    int m_signIns{0};

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
        // The scope on a redeemed session comes back through this hook, every time, from the
        // identity stored with the family. Without it every redemption would hand out the
        // default scope, which would pass most of these tests just as happily.
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.identity.device.enabled = true;
        config.identity.device.store.name = QStringLiteral("sqlite");
        config.identity.device.store.file = storeFile();

        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
        QVERIFY(m_edge->identityProvider()->devices() != nullptr);
    }

    // A sign-in that asks for a credential gets one, and it is not the session: three
    // distinct values, and the two device fields are of no use as a session credential.
    void enrolmentIssuesACredentialAndNotASession()
    {
        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(!held.session.isEmpty());
        QVERIFY(held.hasCredential());
        QVERIFY(held.deviceId != held.session);
        QVERIFY(held.deviceSecret != held.session);

        SessionManager *sessions{m_edge->sessionManager()};
        QVERIFY(sessions->isLive(held.session.toUtf8()));
        // The two halves of what is about to be written to a disk are not credentials for
        // anything else. This is the property that lets the credential outlive the session.
        QVERIFY(!sessions->isLive(held.deviceId.toUtf8()));
        QVERIFY(!sessions->isLive(held.deviceSecret.toUtf8()));
    }

    // And the same thing at the door. A device secret presented where a session belongs is
    // refused before a socket exists, exactly as any other unknown credential is.
    void aDeviceSecretIsRefusedAtTheUpgrade()
    {
        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(held.hasCredential());

        QSignalSpy rejected{m_edge.get(), &WebEdge::upgradeRejected};
        QWebSocket socket;
        QSignalSpy connected{&socket, &QWebSocket::connected};
        QNetworkRequest request{QUrl{m_edge->wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", m_edge->httpOrigin().toUtf8());
        request.setRawHeader("Cookie", "synqt_session=" + held.deviceSecret.toUtf8());
        socket.open(request);

        QTRY_VERIFY(rejected.count() >= 1);
        QCOMPARE(connected.count(), 0);
    }

    // The heart of it. Redeeming issues the next generation and retires the one presented.
    void redemptionRotates()
    {
        const Held first{signIn(QStringLiteral("user"))};
        QVERIFY(first.hasCredential());

        const Response answer{redeem(first)};
        QCOMPARE(answer.status, 200);
        QCOMPARE(answer.cacheControl, QByteArrayLiteral("no-store"));
        const Held second{heldFrom(answer)};
        QCOMPARE(second.deviceId, first.deviceId);        // the family is stable
        QVERIFY(second.deviceSecret != first.deviceSecret); // the secret is not
        QVERIFY(!second.session.isEmpty());
        QVERIFY(second.session != first.session);          // a fresh session, not the old one
        QVERIFY(m_edge->sessionManager()->isLive(second.session.toUtf8()));

        // And the new one keeps working, which is what makes this rotation rather than a
        // credential that quietly stops after one relaunch.
        const Response third{redeem(second)};
        QCOMPARE(third.status, 200);
        QVERIFY(heldFrom(third).hasCredential());
    }

    // The innocent failure. The client never received the answer that retired its credential,
    // so it comes back holding the retired one. Inside the window that costs nothing.
    void aRetiredGenerationInsideTheWindowStillWorks()
    {
        const Held first{signIn(QStringLiteral("user"))};
        const Response rotated{redeem(first)};
        QCOMPARE(rotated.status, 200);

        // Presenting the retired generation again. The client crashed before it could store
        // what it was given, so what it holds is what it held before.
        const Response retry{redeem(first)};
        QCOMPARE(retry.status, 200);
        const Held recovered{heldFrom(retry)};
        QVERIFY(recovered.hasCredential());
        QCOMPARE(recovered.deviceId, first.deviceId);
        // It is not signed out, and the family is still there.
        QCOMPARE(redeem(recovered).status, 200);
    }

    // The same presentation past the window is the case the design exists for.
    void aRetiredGenerationPastTheWindowRevokesTheFamily()
    {
        const Held first{signIn(QStringLiteral("user"))};
        const Response rotated{redeem(first)};
        QCOMPARE(rotated.status, 200);
        const Held live{heldFrom(rotated)};
        QVERIFY(m_edge->sessionManager()->isLive(live.session.toUtf8()));

        // Time passes. The overlap window closes.
        amendRow(first.deviceId, QStringLiteral("prev_issued_ms"),
                 QDateTime::currentMSecsSinceEpoch() - 3600 * 1000);

        QCOMPARE(redeem(first).status, 404);
        // The family is gone, so the copy that did collect the live generation is refused
        // too. Both lose it, because there is no telling which of the two is the visitor.
        QCOMPARE(redeem(live).status, 404);
        // And the session that family opened is ended, not left running for its TTL.
        QVERIFY(!m_edge->sessionManager()->isLive(live.session.toUtf8()));
    }

    // Scope is re-derived from the stored identity on every redemption. The hook maps one
    // address to moderator. A family enrolled while it did keeps nothing when it stops.
    void redemptionRederivesScope()
    {
        m_stub->setUser(QVariantMap{{QStringLiteral("id"), 4242},
                                    {QStringLiteral("login"), QStringLiteral("octocat")},
                                    {QStringLiteral("name"), QStringLiteral("Octo Cat")},
                                    {QStringLiteral("email"),
                                     QStringLiteral("octocat@example.com")}});
        const Held enrolled{signIn(QStringLiteral("user"))};
        QVERIFY(enrolled.hasCredential());
        const SessionRecord *asModerator{
            m_edge->sessionManager()->lookup(enrolled.session.toUtf8())};
        QVERIFY(asModerator != nullptr);
        QCOMPARE(asModerator->scope, QStringLiteral("moderator"));

        // The visitor's record changes: same person, no longer that address, and so no
        // longer a moderator by the project's own rule.
        amendRow(enrolled.deviceId, QStringLiteral("identity_json"),
                 QStringLiteral("{\"sub\":\"4242\",\"login\":\"octocat\","
                                "\"email\":\"octocat@elsewhere.example\"}"));

        const Response answer{redeem(enrolled)};
        QCOMPARE(answer.status, 200);
        const SessionRecord *asUser{
            m_edge->sessionManager()->lookup(heldFrom(answer).session.toUtf8())};
        QVERIFY(asUser != nullptr);
        QCOMPARE(asUser->scope, QStringLiteral("user"));
    }

    // Both clocks. A family past either one is refused, and the row goes with it.
    void anExpiredFamilyIsRefused_data()
    {
        QTest::addColumn<QString>("column");
        QTest::addColumn<qint64>("offsetMs");

        QTest::newRow("absolute lifetime") << QStringLiteral("expires_ms") << qint64{-1000};
        QTest::newRow("inactivity") << QStringLiteral("last_used_ms")
                                    << qint64{-30LL * 24 * 3600 * 1000};
    }

    void anExpiredFamilyIsRefused()
    {
        QFETCH(QString, column);
        QFETCH(qint64, offsetMs);

        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(held.hasCredential());
        amendRow(held.deviceId, column, QDateTime::currentMSecsSinceEpoch() + offsetMs);
        QCOMPARE(redeem(held).status, 404);
        // Refused once and gone. A second attempt cannot find a row to fail against either.
        QCOMPARE(redeem(held).status, 404);
    }

    // Page script has no business at this route, so anything carrying an Origin is refused
    // outright rather than being allowed to depend on the credential staying secret.
    void anOriginHeaderIsRefused()
    {
        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(held.hasCredential());
        QCOMPARE(redeem(held, m_edge->httpOrigin().toUtf8()).status, 404);
        QCOMPARE(redeem(held, QByteArrayLiteral("https://evil.example")).status, 404);
        // And it was a refusal, not a spend. The credential still works without one.
        QCOMPARE(redeem(held).status, 200);
    }

    // Signing out ends the credential, not only the session. A logout that left a redeemable
    // credential on a disk would be worse than none, because the visitor believes it worked.
    void logoutEndsTheFamily()
    {
        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(held.hasCredential());

        QNetworkRequest request{QUrl{edgeUrl(QStringLiteral("/auth/logout"))}};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setRawHeader("Cookie", "synqt_session=" + held.session.toUtf8());
        await(m_browser.get(request));

        QVERIFY(!m_edge->sessionManager()->isLive(held.session.toUtf8()));
        QCOMPARE(redeem(held).status, 404);
    }

    // Which family a session came from has to outlive the process that learned it.
    //
    // It was a QHash on the edge, justified by "a session does not outlive this process
    // either". That stopped being true the moment the session store could be shared and
    // the edge could be replicated. A visitor who enrols through one process and signs out
    // against another would keep the credential the sign-out was supposed to end, silently
    // and for its full lifetime. Nothing about the sign-out would look wrong.
    void theSessionToFamilyBindingOutlivesTheProcess()
    {
        DeviceConfig config;
        config.enabled = true;
        config.store.name = QStringLiteral("sqlite");
        config.store.file = storeFile();

        const QByteArray session{QByteArrayLiteral("session-outlives-the-process")};
        {
            DeviceRegistry first{config};
            QString error;
            QVERIFY2(first.open(&error), qPrintable(error));
            first.bindSession(session, QStringLiteral("family-outlives"));
            QCOMPARE(first.familyOf(session), QStringLiteral("family-outlives"));
        }

        // A different registry over the same store is what a second replica is, and what
        // this process is after a restart. It has to reach the same answer.
        {
            DeviceRegistry second{config};
            QString error;
            QVERIFY2(second.open(&error), qPrintable(error));
            QCOMPARE(second.familyOf(session), QStringLiteral("family-outlives"));

            second.unbindSession(session);
            QCOMPARE(second.familyOf(session), QString{});
        }
    }

    // Rebinding is a replace and not a second row: a session id is rotated on elevation,
    // and the rotated id naming the same family must not leave the old one naming it too.
    void rebindingASessionReplacesTheFamily()
    {
        DeviceConfig config;
        config.enabled = true;
        config.store.name = QStringLiteral("sqlite");
        config.store.file = storeFile();

        DeviceRegistry registry{config};
        QString error;
        QVERIFY2(registry.open(&error), qPrintable(error));

        const QByteArray session{QByteArrayLiteral("session-rebound")};
        registry.bindSession(session, QStringLiteral("family-one"));
        registry.bindSession(session, QStringLiteral("family-two"));
        QCOMPARE(registry.familyOf(session), QStringLiteral("family-two"));
    }

    // Nonsense is refused the same way everything else is. One answer, no oracle.
    void anUnknownCredentialIsRefused()
    {
        const Held held{signIn(QStringLiteral("user"))};
        Held wrong{held};
        wrong.deviceSecret = QStringLiteral("0123456789abcdef");
        QCOMPARE(redeem(wrong).status, 404);
        // A wrong guess does not cost the visitor their device. Anyone who learned a family
        // id could otherwise sign its owner out at will.
        QCOMPARE(redeem(held).status, 200);

        Held unknown;
        unknown.deviceId = QStringLiteral("no-such-family");
        unknown.deviceSecret = QStringLiteral("no-such-secret");
        QCOMPARE(redeem(unknown).status, 404);
    }

    // A client whose store cannot meet the configured floor is still signed in. It
    // has nothing to persist, which is what every project that never asked for this gets.
    void aBindingBelowTheFloorStillSignsIn()
    {
        const Held held{signIn(QStringLiteral("none"))};
        QVERIFY(!held.session.isEmpty());
        QVERIFY(m_edge->sessionManager()->isLive(held.session.toUtf8()));
        QVERIFY(!held.hasCredential());
    }

    // And a sign-in that does not ask for one does not get one. Enrolment is opt-in per
    // client, on top of being opt-in per project.
    void aSignInThatDoesNotAskGetsNoCredential()
    {
        const Held held{signIn(QString{})};
        QVERIFY(!held.session.isEmpty());
        QVERIFY(!held.hasCredential());
    }

    // The one refusal on this route that is allowed to look different, and has to.
    //
    // Everything above answers alike so that nothing learns which half of a guess was right.
    // The rate window is decided before the credential is so much as read, so it says nothing
    // about it, and answering it with the same 404 costs a real visitor their sign-in: their
    // client reads "this credential is dead", deletes it, and asks them to sign in again,
    // over somebody else behind the same address spending a window they share. `Retry-After`
    // is how long is left of it, so a client can wait instead of guessing.
    //
    // Last in the file, because it spends the window it is testing, and the window is per
    // address for a whole minute, which is longer than this suite runs.
    void theRateWindowSaysWaitAndNotNo()
    {
        Held unknown;
        unknown.deviceId = QStringLiteral("no-such-family");
        unknown.deviceSecret = QStringLiteral("no-such-secret");

        Response answer;
        for (int attempt{0}; attempt < 60 && answer.status != 429; ++attempt) {
            answer = redeem(unknown);
            QVERIFY2(answer.status == 404 || answer.status == 429,
                     qPrintable(QStringLiteral("unexpected status %1").arg(answer.status)));
        }
        QCOMPARE(answer.status, 429);
        bool numeric{false};
        const int seconds{answer.retryAfter.toInt(&numeric)};
        QVERIFY2(numeric && seconds > 0,
                 "the refusal tells a client to wait and not how long, so it can only guess");

        // And it is the request that is refused, not the credential. A good one presented
        // into a spent window is told the same thing, and is still good when the window is.
        const Held held{signIn(QStringLiteral("user"))};
        QVERIFY(held.hasCredential());
        QCOMPARE(redeem(held).status, 429);
    }

    void cleanupTestCase()
    {
        // The edge holds the store open. Drop it before the temporary directory goes.
        m_edge.reset();
    }
};

QTEST_MAIN(DeviceTest)
#include "tst_device.moc"
