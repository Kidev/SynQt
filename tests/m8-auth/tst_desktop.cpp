// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The desktop half of easy auth. A native client signs in through the system browser and
// gets the finished session back over a loopback redirect (the native-app pattern of
// RFC 8252), without the browser ever being left signed in and without the session itself
// ever appearing in a URL.
//
// What is proved here, in order of how much it would cost to get wrong:
//
//  - `return` is an allowlist of one shape. Anything else refuses the login outright,
//    before a single byte goes to the provider. An open redirect here hands out sessions.
//  - The loopback carries a claim code, not the session, and the code is single use, short
//    lived, and spendable only by the process that started the sign-in (S256, as PKCE does
//    it), because the URL a browser is sent to is written into that browser's history.
//  - The system browser is not left holding a session cookie at the end of a desktop login.
//  - None of it exists at all unless the project builds a desktop client.
//  - And the whole thing works end to end: SynClient::beginLogin through a real WebEdge and
//    a real provider, ending with a connected client holding an authenticated session.

#include "identityconfig.h"
#include "identityprovider.h"
#include "loopbackreceiver.h"
#include "sessionmanager.h"
#include "session.h"
#include "stubidentityserver.h"
#include "synclient.h"
#include "synclientconfig.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>

#include <memory>

using namespace SynQt;

namespace {

struct Response
{
    int status{0};
    QString location;
    QList<QByteArray> setCookies;
    QByteArray cacheControl;
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

/// The S256 challenge for a verifier, computed here rather than borrowed from the client,
/// so the test agrees with the edge about the algorithm and not merely with itself.
QString challengeFor(const QByteArray &verifier)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

class TestDesktop : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<StubIdentityServer> m_stub;
    std::unique_ptr<WebEdge> m_edge;
    QNetworkAccessManager m_browser;
    quint16 m_edgePort{0};

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

    Response post(const QUrl &url, const QUrlQuery &form)
    {
        QNetworkRequest request{url};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
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
        for (const QNetworkReply::RawHeaderPair &header : reply->rawHeaderPairs()) {
            if (header.first.toLower() == QByteArrayLiteral("set-cookie")) {
                response.setCookies.append(header.second);
            }
        }
        response.cacheControl = reply->rawHeader("Cache-Control");
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    /// The whole system-browser round trip of a desktop login. The login route, the
    /// provider's authorize endpoint, the edge's callback. Returns the callback response,
    /// which is the redirect back to the loopback listener.
    Response completeDesktopLogin(const QString &returnUrl, const QString &state,
                                  const QString &challenge)
    {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), returnUrl);
        query.addQueryItem(QStringLiteral("return_state"), state);
        query.addQueryItem(QStringLiteral("return_challenge"), challenge);
        QUrl login{edgeUrl(QStringLiteral("/auth/login"))};
        login.setQuery(query);

        const Response started{get(login)};
        if (started.status != 302) {
            return started;
        }
        const Response authorize{get(QUrl{started.location})};
        return get(QUrl{authorize.location});
    }

    /// One request to a loopback listener, as a browser following a redirect would make it.
    /// The reply is read back so the test sees what the visitor would be left looking at.
    static QByteArray hitLoopback(quint16 port, const QByteArray &target)
    {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        // Waited on through the event loop rather than through waitForConnected(): the
        // listener on the other end accepts from its own event loop, and a blocking wait
        // here would never let it run.
        const bool connected{QTest::qWaitFor([&socket]() {
            return socket.state() == QAbstractSocket::ConnectedState; }, 3000)};
        if (!connected) {
            return {};
        }
        socket.write("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        QByteArray answer;
        // The answer is complete when the far end closes, which is what Connection: close
        // means. A timeout here is a failed assertion in the caller, not here.
        const bool closed{QTest::qWaitFor([&socket, &answer]() {
            answer += socket.readAll();
            return socket.state() == QAbstractSocket::UnconnectedState;
        }, 3000)};
        Q_UNUSED(closed);
        answer += socket.readAll();
        return answer;
    }

private slots:
    void initTestCase()
    {
        // A browser keeps cookies across requests, so the login-state cookie set on the
        // login redirect rides back to the callback.
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
        // The mapping hook that gives a session its scope, and the vocabulary its
        // Scope members were generated from. An edge with a login and neither
        // refuses every login, because there is nothing to resolve an answer against.
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        // What the generated edge sets when a client entity lists the `desktop` target.
        config.identity.allowDesktopLogin = true;
        config.identity.providers = {stubProvider(m_stub->baseUrl())};
        // The end-to-end case rests on this. An anonymous connection is refused at the
        // upgrade, so a client that reaches "connected" has proved it is presenting an
        // authenticated credential, and not merely that a session exists somewhere on the
        // edge. It changes nothing for the HTTP cases above, which never upgrade.
        config.identityRequired = true;

        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    // The listener exists for one sign-in. It is on the loopback interface, it survives the
    // stray requests a browser makes beside the redirect, and it stops the moment the
    // answer it was waiting for arrives.
    void loopbackReceiverServesOneAnswer()
    {
        LoopbackReceiver receiver;
        QVERIFY(receiver.listen());
        QVERIFY(receiver.port() != 0);
        QCOMPARE(receiver.returnUrl(),
                 QStringLiteral("http://127.0.0.1:%1/").arg(receiver.port()));

        QSignalSpy received{&receiver, &LoopbackReceiver::received};

        // A browser asks for a favicon beside the page it was sent to. Answering it must
        // not end the sign-in, or the answer that matters arrives at a closed port.
        const QByteArray favicon{hitLoopback(receiver.port(), "/favicon.ico")};
        QVERIFY(favicon.startsWith("HTTP/1.1 404"));
        QCOMPARE(received.count(), 0);

        const QByteArray answer{hitLoopback(receiver.port(),
                                            "/?code=the-code&state=the-state")};
        QVERIFY(answer.startsWith("HTTP/1.1 200"));
        // Fixed text, and in particular nothing from the request. A page that echoed the
        // query back would be reflecting attacker-chosen bytes into the visitor's browser.
        QVERIFY(!answer.contains("the-code"));
        QVERIFY(answer.contains("no-store"));

        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QStringLiteral("the-code"));
        QCOMPARE(received.at(0).at(1).toString(), QStringLiteral("the-state"));
        QVERIFY(received.at(0).at(2).toString().isEmpty());

        // And it is closed. The port that could hand this app a session is not left open
        // for the life of the process.
        QTcpSocket late;
        late.connectToHost(QHostAddress::LocalHost, receiver.port());
        QVERIFY(!late.waitForConnected(500));
    }

    void loopbackReceiverReportsAnError()
    {
        LoopbackReceiver receiver;
        QVERIFY(receiver.listen());
        QSignalSpy received{&receiver, &LoopbackReceiver::received};
        hitLoopback(receiver.port(), "/?error=access_denied&state=s");
        QCOMPARE(received.count(), 1);
        QVERIFY(received.at(0).at(0).toString().isEmpty());
        QCOMPARE(received.at(0).at(2).toString(), QStringLiteral("access_denied"));
    }

    // The line the whole feature rests on. Every one of these is refused before the
    // provider is contacted, and refused outright rather than downgraded to a browser
    // login, because a downgrade signs somebody in while their app is still waiting.
    void returnMustBeLoopback_data()
    {
        QTest::addColumn<QString>("returnUrl");

        QTest::newRow("another host") << QStringLiteral("http://evil.example/");
        QTest::newRow("https on loopback") << QStringLiteral("https://127.0.0.1:5555/");
        QTest::newRow("userinfo prefix") << QStringLiteral("http://127.0.0.1@evil.example/");
        QTest::newRow("userinfo with port")
            << QStringLiteral("http://127.0.0.1:5555@evil.example/");
        QTest::newRow("localhost by name") << QStringLiteral("http://localhost:5555/");
        QTest::newRow("no port") << QStringLiteral("http://127.0.0.1/");
        QTest::newRow("a path of its own") << QStringLiteral("http://127.0.0.1:5555/take");
        QTest::newRow("a query of its own") << QStringLiteral("http://127.0.0.1:5555/?x=1");
        QTest::newRow("a fragment") << QStringLiteral("http://127.0.0.1:5555/#f");
        QTest::newRow("not a url at all") << QStringLiteral("not a url");
        QTest::newRow("scheme relative") << QStringLiteral("//127.0.0.1:5555/");
        QTest::newRow("a private address") << QStringLiteral("http://10.0.0.1:5555/");
        QTest::newRow("looks loopback") << QStringLiteral("http://127.0.0.1.evil.example/");
    }

    void returnMustBeLoopback()
    {
        QFETCH(QString, returnUrl);

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), returnUrl);
        query.addQueryItem(QStringLiteral("return_state"), QStringLiteral("nonce"));
        query.addQueryItem(QStringLiteral("return_challenge"),
                           challengeFor(QByteArrayLiteral("verifier")));
        QUrl login{edgeUrl(QStringLiteral("/auth/login"))};
        login.setQuery(query);

        const Response refused{get(login)};
        QCOMPARE(refused.status, 400);
        QVERIFY(refused.location.isEmpty());
    }

    // The other half of the same argument: a well-formed loopback return that is missing
    // either of the two values that bind the answer to the app is no better than a bad one.
    void returnNeedsItsNonceAndChallenge_data()
    {
        QTest::addColumn<QString>("state");
        QTest::addColumn<QString>("challenge");

        const QString good{challengeFor(QByteArrayLiteral("verifier"))};
        QTest::newRow("no nonce") << QString{} << good;
        QTest::newRow("no challenge") << QStringLiteral("nonce") << QString{};
        QTest::newRow("short challenge") << QStringLiteral("nonce") << QStringLiteral("abc");
        QTest::newRow("not base64url") << QStringLiteral("nonce")
                                       << good.left(42) + QStringLiteral("+");
        QTest::newRow("oversized nonce") << QString{200, QLatin1Char('n')} << good;
    }

    void returnNeedsItsNonceAndChallenge()
    {
        QFETCH(QString, state);
        QFETCH(QString, challenge);

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), QStringLiteral("http://127.0.0.1:5555/"));
        if (!state.isEmpty()) {
            query.addQueryItem(QStringLiteral("return_state"), state);
        }
        if (!challenge.isEmpty()) {
            query.addQueryItem(QStringLiteral("return_challenge"), challenge);
        }
        QUrl login{edgeUrl(QStringLiteral("/auth/login"))};
        login.setQuery(query);

        const Response refused{get(login)};
        QCOMPARE(refused.status, 400);
        QVERIFY(refused.location.isEmpty());
    }

    // The full desktop round trip, without the client. The callback lands on the loopback
    // URL with a code and the app's own nonce, leaves the browser with no session, and the
    // code buys the session exactly once.
    void claimIsSingleUseAndBoundToItsVerifier()
    {
        const QByteArray verifier{QByteArrayLiteral("a-verifier-only-this-process-has")};
        const QString state{QStringLiteral("the-apps-own-nonce")};
        const Response callback{completeDesktopLogin(
            QStringLiteral("http://127.0.0.1:5555/"), state, challengeFor(verifier))};

        QCOMPARE(callback.status, 302);
        const QUrl landing{callback.location};
        QCOMPARE(landing.host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(landing.port(), 5555);
        const QUrlQuery answer{landing.query()};
        const QString code{answer.queryItemValue(QStringLiteral("code"))};
        QVERIFY(!code.isEmpty());
        QCOMPARE(answer.queryItemValue(QStringLiteral("state")), state);
        QVERIFY(answer.queryItemValue(QStringLiteral("error")).isEmpty());

        // The system browser is not the app, and must not be left signed in: on a shared
        // machine that session would outlive the sign-in with nothing to end it.
        for (const QByteArray &cookie : callback.setCookies) {
            QVERIFY2(!cookie.startsWith(QByteArrayLiteral("synqt_session=")),
                     cookie.constData());
        }

        QUrlQuery wrong;
        wrong.addQueryItem(QStringLiteral("code"), code);
        wrong.addQueryItem(QStringLiteral("verifier"), QStringLiteral("not-the-verifier"));
        const Response refused{post(QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))}, wrong)};
        QCOMPARE(refused.status, 404);

        // And the wrong guess spent the code: one code, one attempt, so a verifier cannot
        // be tried repeatedly against a code somebody read out of a browser history.
        QUrlQuery right;
        right.addQueryItem(QStringLiteral("code"), code);
        right.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        const Response afterwards{post(QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))},
                                       right)};
        QCOMPARE(afterwards.status, 404);
    }

    void claimBuysTheSessionOnce()
    {
        const QByteArray verifier{QByteArrayLiteral("another-verifier")};
        const Response callback{completeDesktopLogin(
            QStringLiteral("http://127.0.0.1:5556/"), QStringLiteral("n"),
            challengeFor(verifier))};
        QCOMPARE(callback.status, 302);
        const QString code{QUrlQuery{QUrl{callback.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};

        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        const Response claimed{post(QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))}, form)};
        QCOMPARE(claimed.status, 200);

        const QJsonObject answer{QJsonDocument::fromJson(claimed.body).object()};
        const QString session{answer.value(QStringLiteral("session")).toString()};
        QVERIFY(!session.isEmpty());
        QCOMPARE(answer.value(QStringLiteral("cookie_name")).toString(),
                 QStringLiteral("synqt_session"));
        // A live credential in a response body, so nothing between here and the app is
        // allowed to keep a copy of it.
        QCOMPARE(claimed.cacheControl, QByteArrayLiteral("no-store"));

        // It is a real session, carrying the identity the provider returned.
        const SessionRecord *record{m_edge->sessionManager()->lookup(session.toUtf8())};
        QVERIFY(record != nullptr);
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("octocat"));

        const Response replayed{post(QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))}, form)};
        QCOMPARE(replayed.status, 404);
    }

    // Page script has no business at this endpoint, so it is out of reach of any of it
    // rather than merely protected by the code being unguessable.
    void claimRefusesAnythingWithAnOrigin()
    {
        const QByteArray verifier{QByteArrayLiteral("origin-verifier")};
        const Response callback{completeDesktopLogin(
            QStringLiteral("http://127.0.0.1:5557/"), QStringLiteral("n"),
            challengeFor(verifier))};
        const QString code{QUrlQuery{QUrl{callback.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};

        QNetworkRequest request{QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))}};
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        request.setRawHeader("Origin", "https://evil.example");
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        const Response refused{
            await(m_browser.post(request, form.toString(QUrl::FullyEncoded).toUtf8()))};
        QCOMPARE(refused.status, 404);
    }

    // A GET is not a way in. It matters because a GET is what puts a code into a request
    // log, a browser history and a Referer header, which is the whole reason the exchange
    // is a POST; a real code offered over GET has to buy nothing and cost nothing.
    void claimRefusesAGet()
    {
        const QByteArray verifier{QByteArrayLiteral("get-verifier")};
        const Response callback{completeDesktopLogin(
            QStringLiteral("http://127.0.0.1:5559/"), QStringLiteral("n"),
            challengeFor(verifier))};
        const QString code{QUrlQuery{QUrl{callback.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        QVERIFY(!code.isEmpty());

        QUrl target{edgeUrl(QStringLiteral("/auth/login/claim"))};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("code"), code);
        query.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        target.setQuery(query);
        const Response over{get(target)};
        // Whatever answers a GET on that path (the application shell falls through to it),
        // it is not the claim endpoint and it hands out no session.
        QVERIFY(!over.body.contains(QByteArrayLiteral("cookie_name")));
        QVERIFY(QJsonDocument::fromJson(over.body).object()
                    .value(QStringLiteral("session")).toString().isEmpty());

        // And it did not spend the code either, so a GET is not a way to burn somebody
        // else's sign-in halfway through it.
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        QCOMPARE(post(QUrl{edgeUrl(QStringLiteral("/auth/login/claim"))}, form).status, 200);
    }

    // An edge whose project builds no desktop client has none of this: not the redirect,
    // and not the endpoint that would collect it.
    void withoutADesktopClientThereIsNoDesktopLogin()
    {
        QQmlEngine engine;
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        config.identity.providers = {stubProvider(m_stub->baseUrl())};
        // allowDesktopLogin left at its default, which is the default of every project
        // that does not list the desktop target.

        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));
        const QString base{QStringLiteral("http://127.0.0.1:%1").arg(edge.serverPort())};

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), QStringLiteral("http://127.0.0.1:5555/"));
        query.addQueryItem(QStringLiteral("return_state"), QStringLiteral("n"));
        query.addQueryItem(QStringLiteral("return_challenge"),
                           challengeFor(QByteArrayLiteral("v")));
        QUrl login{base + QStringLiteral("/auth/login")};
        login.setQuery(query);
        QCOMPARE(get(login).status, 400);

        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), QStringLiteral("anything"));
        form.addQueryItem(QStringLiteral("verifier"), QStringLiteral("anything"));
        QCOMPARE(post(QUrl{base + QStringLiteral("/auth/login/claim")}, form).status, 404);
    }

    // A code that nobody collects stops standing for its session. Run against an edge of
    // its own so the deadline can be one second rather than a minute. The session itself is
    // untouched by the code expiring, and lives or expires on its own terms.
    void anUncollectedClaimExpires()
    {
        QQmlEngine engine;
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.allowDesktopLogin = true;
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        config.identity.claimTtlSeconds = 1;
        config.identity.providers = {stubProvider(m_stub->baseUrl())};

        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));
        const quint16 port{edge.serverPort()};
        const QString base{QStringLiteral("http://127.0.0.1:%1").arg(port)};

        const QByteArray verifier{QByteArrayLiteral("slow-verifier")};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"), QStringLiteral("http://127.0.0.1:5558/"));
        query.addQueryItem(QStringLiteral("return_state"), QStringLiteral("n"));
        query.addQueryItem(QStringLiteral("return_challenge"), challengeFor(verifier));
        QUrl login{base + QStringLiteral("/auth/login")};
        login.setQuery(query);

        const Response started{get(login)};
        QCOMPARE(started.status, 302);
        const Response authorize{get(QUrl{started.location})};
        const Response callback{get(QUrl{authorize.location})};
        QCOMPARE(callback.status, 302);
        const QString code{QUrlQuery{QUrl{callback.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        QVERIFY(!code.isEmpty());

        QTest::qWait(1200);

        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(verifier));
        QCOMPARE(post(QUrl{base + QStringLiteral("/auth/login/claim")}, form).status, 404);
    }

    // End to end, through the client runtime itself: Session.login() takes a port, opens
    // the system browser, receives the answer, exchanges it over its own connection, and
    // reconnects holding an authenticated session.
    //
    // The system browser is stood in for through QDesktopServices::setUrlHandler, which is
    // Qt's own seam for exactly this. Nothing in the client is widened to be testable: it
    // calls openUrl() the way it does in a shipped app, and what is substituted is the
    // browser, which is the one part of the flow that is not SynQt's.
    void nativeClientSignsInEndToEnd()
    {
        SynClientConfig config;
        config.edgeUrl = QUrl{QStringLiteral("ws://127.0.0.1:%1/sync").arg(m_edgePort)};
        config.loginRoute = QStringLiteral("/auth/login");
        config.logoutRoute = QStringLiteral("/auth/logout");
        config.heartbeatMs = 500;
        config.reconnectBaseMs = 200;
        config.reconnectMaxMs = 400;

        QQmlEngine engine;
        SynClient client{config, &engine};

        // Stand in for the system browser. Follow the login redirect to the provider and
        // the provider's redirect back to the callback, exactly as a browser would, and let
        // the callback's own redirect land on the client's loopback listener.
        //
        // Substituted through QDesktopServices::setUrlHandler, which is Qt's own seam for
        // it. Nothing in the client is widened to be testable: it calls openUrl() the way a
        // shipped app does, and what is replaced is the browser, which is the one part of
        // this flow that was never SynQt's.
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "driveBrowser");
        const auto releaseHandler{qScopeGuard([]() {
            QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        })};

        client.start();
        // Signed out, it cannot get on at all. The edge requires identity at the upgrade.
        QTest::qWait(1500);
        QVERIFY2(client.state() != QStringLiteral("connected"),
                 qPrintable(client.state()));

        client.session()->login(QStringLiteral("stub"));

        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QStringLiteral("connected"), 15000);

        // The browser was sent to the app's own port, and that port is gone: the
        // window in which anything could hand this client a session was the sign-in itself.
        QCOMPARE(m_browserLanding.host(), QStringLiteral("127.0.0.1"));
        QTcpSocket late;
        late.connectToHost(QHostAddress::LocalHost,
                           static_cast<quint16>(m_browserLanding.port()));
        QVERIFY(!late.waitForConnected(500));

        // And signing out ends it at the edge, not only here. The client goes back to being
        // refused, which is the same thing an anonymous client is.
        client.session()->logout();
        QTRY_VERIFY_WITH_TIMEOUT(client.state() != QStringLiteral("connected"), 10000);
    }

public slots:
    /// The stand-in system browser. Invoked by QDesktopServices::openUrl with whatever the
    /// client asked to open. It walks the redirect chain and delivers the last hop to the
    /// loopback listener, which is what a browser does with the same three responses.
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

private:
    QUrl m_browserLanding;
};

QTEST_MAIN(TestDesktop)
#include "tst_desktop.moc"
