// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M8 acceptance: a full provider login runs entirely on the edge (Authorization Code +
// PKCE, framework-generated state verified on the callback, the client secret and tokens
// held on the edge), the browser ends with only an httpOnly session cookie, the session
// carries the normalized identity and the scope the mapping hook returned, and tokens
// never appear in what the browser receives. The dev stub provider is refused unless the
// dev gate is on.

#include "connectpointhost.h"
#include "identityconfig.h"
#include "identityprovider.h"
#include "identityservice.h"
#include "meshclient.h"
#include "oauthbackend.h"
#include "sessionmanager.h"
#include "stubidentityserver.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "session_sourcehelper.h"   // synqtRegisterSessionSources()
#include "identity_sourcehelper.h"  // synqtRegisterIdentitySources()

#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSslCertificate>
#include <QSslKey>
#include <QTest>
#include <QUrlQuery>

#include <memory>

using namespace SynQt;

namespace {

struct Response
{
    int status{0};
    QString location;
    QByteArray setCookie;
    QByteArray body;
};

QByteArray sessionToken(const QByteArray &setCookie)
{
    const QByteArray prefix{QByteArrayLiteral("synqt_session=")};
    for (QByteArray part : setCookie.split(';')) {
        part = part.trimmed();
        if (part.startsWith(prefix)) {
            return part.mid(prefix.size());
        }
    }
    return {};
}

MeshCredentials credsFor(const QString &entity)
{
    MeshCredentials credentials;
    credentials.caCertPath = QStringLiteral(M8_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(M8_CERT_DIR "/") + entity + QStringLiteral(".crt");
    credentials.keyPath = QStringLiteral(M8_CERT_DIR "/") + entity + QStringLiteral(".key");
    return credentials;
}

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

// An OpenID Connect provider: identity comes from the JWKS-verified ID token. It has NO
// userinfo endpoint, so a session can only be created if the ID token verified.
IdentityProviderConfig stubOidcProvider(const QString &base, const QString &name,
                                        const QString &issuer)
{
    IdentityProviderConfig provider;
    provider.name = name;
    provider.devStub = true;
    provider.authorizeUrl = QUrl{base + QStringLiteral("/authorize")};
    provider.tokenUrl = QUrl{base + QStringLiteral("/token")};
    provider.clientId = QStringLiteral("stub-client");
    provider.clientSecret = QStringLiteral("stub-secret");
    provider.scopes = {QStringLiteral("openid"), QStringLiteral("email"), QStringLiteral("profile")};
    provider.useIdToken = true;
    provider.jwksUrl = QUrl{base + QStringLiteral("/jwks")};
    provider.issuer = issuer;
    return provider;
}

} // namespace

class TestM8 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_engine;
    std::unique_ptr<StubIdentityServer> m_stub;
    std::unique_ptr<WebEdge> m_edge;
    QNetworkAccessManager m_browser;
    quint16 m_edgePort{0};

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
        response.setCookie = reply->rawHeader("Set-Cookie");
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    QString edgeUrl(const QString &path) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_edgePort).arg(path);
    }

    // Run the whole browser round trip (login -> provider -> callback) and return the
    // callback response; capture the authorization request query if asked.
    Response completeLogin(const QString &providerQuery, QUrlQuery *authQuery = nullptr)
    {
        const Response login{get(QUrl{edgeUrl(QStringLiteral("/auth/login") + providerQuery)})};
        if (authQuery) {
            *authQuery = QUrlQuery{QUrl{login.location}.query()};
        }
        const Response authorize{get(QUrl{login.location})};
        return get(QUrl{authorize.location});
    }

private slots:
    void initTestCase()
    {
        synqtRegisterSessionSources();
        synqtRegisterIdentitySources();

        // A browser keeps cookies across requests, so the login-state cookie set on the
        // login redirect rides back to the callback.
        m_browser.setCookieJar(new QNetworkCookieJar{&m_browser});

        m_stub = std::make_unique<StubIdentityServer>(StubIdentityServer::DevOnly{});
        m_stub->setClientCredentials(QStringLiteral("stub-client"), QStringLiteral("stub-secret"));
        QVERIFY(m_stub->start());

        m_engine = std::make_unique<QQmlEngine>();

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;  // plaintext dev edge; TLS is orthogonal to the OAuth flow here
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        config.identity.providers = {
            stubProvider(m_stub->baseUrl()),
            stubOidcProvider(m_stub->baseUrl(), QStringLiteral("stub-oidc"), m_stub->baseUrl()),
            stubOidcProvider(m_stub->baseUrl(), QStringLiteral("stub-oidc-badiss"),
                             QStringLiteral("https://evil.example"))};

        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    void fullLoginFlow()
    {
        // 1. The browser hits the login route; the edge starts the flow and redirects to
        //    the provider with PKCE + a framework-generated state.
        const Response login{get(QUrl{edgeUrl(QStringLiteral("/auth/login"))})};
        QCOMPARE(login.status, 302);
        QVERIFY2(login.location.startsWith(m_stub->baseUrl() + QStringLiteral("/authorize")),
                 qPrintable(login.location));
        const QUrlQuery authQuery{QUrl{login.location}.query()};
        QCOMPARE(authQuery.queryItemValue(QStringLiteral("client_id")), QStringLiteral("stub-client"));
        QCOMPARE(authQuery.queryItemValue(QStringLiteral("response_type")), QStringLiteral("code"));
        QCOMPARE(authQuery.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QVERIFY(!authQuery.queryItemValue(QStringLiteral("code_challenge")).isEmpty());
        const QString state{authQuery.queryItemValue(QStringLiteral("state"))};
        QVERIFY(!state.isEmpty());
        QVERIFY(authQuery.queryItemValue(QStringLiteral("redirect_uri"))
                    .contains(QStringLiteral("/auth/callback")));

        // 2. The provider authenticates and redirects back to the edge callback.
        const Response authorize{get(QUrl{login.location})};
        QCOMPARE(authorize.status, 302);
        QVERIFY2(authorize.location.contains(QStringLiteral("/auth/callback")),
                 qPrintable(authorize.location));
        const QUrlQuery callbackQuery{QUrl{authorize.location}.query()};
        QVERIFY(!callbackQuery.queryItemValue(QStringLiteral("code")).isEmpty());
        QCOMPARE(callbackQuery.queryItemValue(QStringLiteral("state")), state);

        // 3. The edge callback exchanges the code (server-side), creates the session, and
        //    sets only an httpOnly session cookie.
        const Response callback{get(QUrl{authorize.location})};
        QCOMPARE(callback.status, 302);
        QCOMPARE(callback.location, QStringLiteral("/"));
        QVERIFY2(!callback.setCookie.isEmpty(), "callback must set the session cookie");
        QVERIFY(callback.setCookie.contains("synqt_session="));
        QVERIFY2(callback.setCookie.contains("HttpOnly"), "session cookie must be httpOnly");
        QVERIFY(callback.setCookie.contains("SameSite"));

        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());

        // The session carries the mapping hook's scope and the normalized identity.
        const SessionRecord *record{m_edge->sessionManager()->lookup(token)};
        QVERIFY(record != nullptr);
        QCOMPARE(record->scope, QStringLiteral("moderator"));  // map.qml mapped octocat
        QCOMPARE(record->identity.value(QStringLiteral("sub")).toString(), QStringLiteral("1001"));
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(), QStringLiteral("octocat"));
        QCOMPARE(record->identity.value(QStringLiteral("email")).toString(),
                 QStringLiteral("octocat@example.com"));

        // Tokens stay on the edge, associated with the session, and never reach the browser.
        const QVariantMap tokens{m_edge->identityProvider()->tokensForSession(token)};
        const QString accessToken{tokens.value(QStringLiteral("access_token")).toString()};
        QVERIFY(!accessToken.isEmpty());
        QVERIFY2(accessToken.toUtf8() != token, "the session cookie must not be the access token");
        QVERIFY2(!callback.setCookie.contains(accessToken.toUtf8()),
                 "the access token must not appear in the Set-Cookie");
        QVERIFY2(!callback.body.contains(accessToken.toUtf8()),
                 "the access token must not appear in the response body");
    }

    void oidcLoginVerifiesIdToken()
    {
        // OpenID Connect: the provider has no userinfo endpoint, so a session can be
        // created only if the ID token's RS256 signature verified against the JWKS.
        QUrlQuery authQuery;
        const Response callback{completeLogin(QStringLiteral("?provider=stub-oidc"), &authQuery)};
        QVERIFY2(!authQuery.queryItemValue(QStringLiteral("nonce")).isEmpty(),
                 "the OIDC authorization request must carry a nonce");
        QCOMPARE(callback.status, 302);
        QVERIFY2(!callback.setCookie.isEmpty(),
                 "a verified ID token must create a session");

        const SessionRecord *record{m_edge->sessionManager()->lookup(sessionToken(callback.setCookie))};
        QVERIFY(record != nullptr);
        QCOMPARE(record->identity.value(QStringLiteral("sub")).toString(), QStringLiteral("1001"));
        QCOMPARE(record->identity.value(QStringLiteral("email")).toString(),
                 QStringLiteral("octocat@example.com"));
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("octocat"));  // from preferred_username
        QCOMPARE(record->scope, QStringLiteral("moderator"));
    }

    void oidcWrongIssuerRejected()
    {
        // The ID token's iss will not match this provider's configured issuer, so
        // verification fails and no session is created.
        const Response callback{completeLogin(QStringLiteral("?provider=stub-oidc-badiss"))};
        QCOMPARE(callback.status, 302);
        // The failure path clears the login-state cookie but must never set a session.
        QVERIFY2(!callback.setCookie.contains("synqt_session="),
                 "an ID token failing verification must not create a session");
    }

    void loginCsrfRejected()
    {
        // Login is bound to the browser that started it: the login redirect sets a state
        // cookie the callback must present. A different browser cannot complete the login,
        // even with a valid state and code; the defense against login CSRF / fixation.
        const Response login{get(QUrl{edgeUrl(QStringLiteral("/auth/login?provider=stub"))})};
        QCOMPARE(login.status, 302);
        QVERIFY2(login.setCookie.contains("synqt_oauth_state="),
                 "login must bind the flow to the browser with a state cookie");
        QVERIFY(login.setCookie.contains("HttpOnly"));

        const Response authorize{get(QUrl{login.location})};
        QCOMPARE(authorize.status, 302);
        QVERIFY(authorize.location.contains(QStringLiteral("/auth/callback")));

        // A different browser (fresh cookie jar) tries to finish the login: refused.
        QNetworkAccessManager attacker;
        QNetworkRequest request{QUrl{authorize.location}};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply *reply{attacker.get(request)};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 400);
        QVERIFY2(reply->rawHeader("Set-Cookie").isEmpty(),
                 "a mismatched login must not create a session");
        reply->deleteLater();
    }

    // identity.provider_entity: a dedicated auth entity owns the authoritative session
    // store behind a per_peer Session connect point; edges consume it over the mesh. A
    // session created (as login does) on one edge is validated on another.
    void providerEntityDistributedSessions()
    {
        SessionManager authStore{QStringLiteral("anonymous"), 720};

        ConnectPointConfig config;
        config.name = QStringLiteral("sessions");
        config.contract = QStringLiteral("Session");
        config.owner = QStringLiteral("auth");
        config.consumers = {QStringLiteral("web"), QStringLiteral("web2")};
        config.serverFile = QStringLiteral(M8_SRCDIR "/auth/Session.qml");
        config.instance = ConnectPointInstance::PerPeer;
        config.endpoint.mode = MeshTransportMode::MutualTls;
        config.endpoint.host = QStringLiteral("127.0.0.1");
        config.endpoint.port = 0;

        QQmlEngine authEngine;
        ConnectPointHost authHost{config, credsFor(QStringLiteral("auth")), &authEngine};
        authHost.setContextObject(QStringLiteral("Sessions"), &authStore);
        QVERIFY2(authHost.start(), qPrintable(authHost.errorString()));
        const quint16 port{authHost.serverPort()};

        // Own the edges' mesh objects in a scope declared after the auth host, so at method
        // end they tear down first; while the host and store they are connected to are
        // still alive; instead of at the test object's destruction in an undefined order.
        QObject meshScope;

        // Bring up an edge's session cache: a SessionManager in remote mode, fed by the
        // auth entity's Session Replica over mutual TLS.
        const auto attachEdge = [&](const QString &entity) -> SessionManager * {
            // Create the cache before its node, so at teardown meshScope destroys the cache
            // (the Replica's receiver) first, while the Replica is still alive.
            SessionManager *sessions{new SessionManager{QStringLiteral("anonymous"), 720,
                                                        &meshScope}};
            QRemoteObjectNode *node{new QRemoteObjectNode{&meshScope}};
            MeshClient *client{new MeshClient{&meshScope}};
            connect(client, &MeshClient::connected, node, [node, sessions](QIODevice *device) {
                node->addClientSideConnection(device);
                QRemoteObjectDynamicReplica *replica{node->acquireDynamic(QStringLiteral("sessions"))};
                connect(replica, &QRemoteObjectDynamicReplica::initialized, sessions,
                        [sessions, replica]() { sessions->attachRemote(replica); });
            });
            client->connectMutualTls(QHostAddress::LocalHost, port, QStringLiteral("auth"),
                loadCertificate(QStringLiteral(M8_CERT_DIR "/ca.crt")),
                loadCertificate(QStringLiteral(M8_CERT_DIR "/") + entity + QStringLiteral(".crt")),
                loadPrivateKey(QStringLiteral(M8_CERT_DIR "/") + entity + QStringLiteral(".key")));
            return sessions;
        };

        SessionManager *edgeA{attachEdge(QStringLiteral("web"))};
        SessionManager *edgeB{attachEdge(QStringLiteral("web2"))};

        // Both edges attach their Replica (deny-by-default lets these listed consumers in).
        QTRY_VERIFY(authHost.serverPort() != 0);
        QTest::qWait(600);  // let both mesh links come up and attach

        // Edge A creates an authenticated session, exactly as the login flow does.
        const QByteArray token{edgeA->createSession(QStringLiteral("moderator"),
            QVariantMap{{QStringLiteral("sub"), QStringLiteral("alice")},
                        {QStringLiteral("email"), QStringLiteral("alice@example.com")}})};
        QVERIFY(!token.isEmpty());

        // It reaches the authoritative store and, through it, the other edge.
        QTRY_VERIFY(authStore.isLive(token));
        QTRY_VERIFY(edgeB->isLive(token));
        const SessionRecord *record{edgeB->lookup(token)};
        QVERIFY(record != nullptr);
        QCOMPARE(record->scope, QStringLiteral("moderator"));
        QCOMPARE(record->identity.value(QStringLiteral("sub")).toString(), QStringLiteral("alice"));

        // Revocation on one edge propagates everywhere.
        edgeA->revoke(token);
        QTRY_VERIFY(!authStore.isLive(token));
        QTRY_VERIFY(!edgeB->isLive(token));
    }

    // AUTH-2: the edge refreshes an access token before it expires, server-side, using the
    // refresh token, without ever involving the browser. The session is untouched.
    void refreshRenewsAccessTokenServerSide()
    {
        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());

        OAuthBackend *backend{m_edge->identityProvider()->backend()};
        QVERIFY(backend != nullptr);
        const QString before{backend->tokens(QString::fromLatin1(token))
                                 .value(QStringLiteral("access_token")).toString()};
        QVERIFY(!before.isEmpty());

        // Refresh everything due within a wide margin (the stub's tokens expire in 3600s), so
        // this session's access token is renewed via its refresh token.
        const int refreshed{backend->refreshExpiring(4000)};
        QVERIFY2(refreshed >= 1, "the near-expiry access token must be refreshed server-side");

        const QString after{backend->tokens(QString::fromLatin1(token))
                                .value(QStringLiteral("access_token")).toString()};
        QVERIFY(!after.isEmpty());
        QVERIFY2(after != before, "refresh must yield a new access token");
        QVERIFY2(m_edge->sessionManager()->isLive(token),
                 "a server-side refresh must not disturb the session");
    }

    // AUTH-1: with identity.provider_entity set, the client secret and the tokens live only
    // on a dedicated auth entity. The edge delegates begin/exchange over the Identity mesh
    // connect point, holds no OAuth backend, no secret and no token, and only issues the
    // session cookie.
    void providerEntityCentralizedLogin()
    {
        // The auth entity owns the OAuth engine, with the FULL provider (secret included),
        // behind a per_peer Identity Source over mutual TLS.
        IdentityConfig authConfig;
        authConfig.enabled = true;
        authConfig.allowDevStub = true;
        authConfig.providers = {stubProvider(m_stub->baseUrl())};
        IdentityService authService{authConfig};

        ConnectPointConfig cp;
        cp.name = QStringLiteral("identity");
        cp.contract = QStringLiteral("Identity");
        cp.owner = QStringLiteral("auth");
        cp.consumers = {QStringLiteral("web")};
        cp.serverFile = QStringLiteral(M8_SRCDIR "/auth/Identity.qml");
        cp.instance = ConnectPointInstance::PerPeer;
        cp.endpoint.mode = MeshTransportMode::MutualTls;
        cp.endpoint.host = QStringLiteral("127.0.0.1");
        cp.endpoint.port = 0;

        QQmlEngine authEngine;
        ConnectPointHost authHost{cp, credsFor(QStringLiteral("auth")), &authEngine};
        authHost.setContextObject(QStringLiteral("IdentityEngine"), &authService);
        QVERIFY2(authHost.start(), qPrintable(authHost.errorString()));
        const quint16 authPort{authHost.serverPort()};

        // The edge's mesh objects (node/Replica) live in a scope declared before the edge, so
        // at method end the edge (and its IdentityProvider, the Replica's receiver) is
        // destroyed first, while the Replica is still alive. A dynamic Replica frees its
        // runtime metaobject on destruction, so its receiver must not outlive it.
        QObject meshScope;

        // The edge: identity enabled, provider_entity="auth", and a SECRET-LESS provider list
        // (names only). It builds no OAuth backend and holds no secret.
        QQmlEngine edgeEngine;
        WebEdgeConfig edgeConfig;
        edgeConfig.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        edgeConfig.host = QStringLiteral("127.0.0.1");
        edgeConfig.port = 0;
        edgeConfig.identity.enabled = true;
        edgeConfig.identity.providerEntity = QStringLiteral("auth");
        edgeConfig.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        IdentityProviderConfig nameOnly;
        nameOnly.name = QStringLiteral("stub");
        edgeConfig.identity.providers = {nameOnly};

        WebEdge edge{edgeConfig, &edgeEngine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));
        QVERIFY2(edge.identityProvider()->backend() == nullptr,
                 "a provider_entity edge must build no OAuth backend (no secret)");
        QVERIFY(edge.identityProvider()->isRemote());
        const quint16 edgePort{edge.serverPort()};

        // Bring up the edge's Identity mesh link and attach the Replica (owned by meshScope).
        QRemoteObjectNode *node{new QRemoteObjectNode{&meshScope}};
        MeshClient *client{new MeshClient{&meshScope}};
        IdentityProvider *provider{edge.identityProvider()};
        connect(client, &MeshClient::connected, node, [node, provider](QIODevice *device) {
            node->addClientSideConnection(device);
            QRemoteObjectDynamicReplica *replica{node->acquireDynamic(QStringLiteral("identity"))};
            connect(replica, &QRemoteObjectDynamicReplica::initialized, provider,
                    [provider, replica]() { provider->attachRemote(replica); });
        });
        client->connectMutualTls(QHostAddress::LocalHost, authPort, QStringLiteral("auth"),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/web.crt")),
            loadPrivateKey(QStringLiteral(M8_CERT_DIR "/web.key")));
        QTest::qWait(700);  // let the mesh link come up and the replica initialize

        // A browser completes the whole login through the edge (its own cookie jar so the
        // login-state cookie rides login -> callback).
        QNetworkAccessManager browser;
        browser.setCookieJar(new QNetworkCookieJar{&browser});
        const auto hop = [&browser](const QString &url) {
            QNetworkRequest request{QUrl{url}};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            QNetworkReply *reply{browser.get(request)};
            QEventLoop loop;
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            Response response;
            response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            response.location = QString::fromUtf8(reply->rawHeader("Location"));
            response.setCookie = reply->rawHeader("Set-Cookie");
            response.body = reply->readAll();
            reply->deleteLater();
            return response;
        };
        const QString base{QStringLiteral("http://127.0.0.1:%1").arg(edgePort)};
        const Response login{hop(base + QStringLiteral("/auth/login?provider=stub"))};
        QCOMPARE(login.status, 302);
        QVERIFY2(login.location.startsWith(m_stub->baseUrl()), qPrintable(login.location));
        const Response authorize{hop(login.location)};
        QCOMPARE(authorize.status, 302);
        QVERIFY(authorize.location.contains(QStringLiteral("/auth/callback")));
        const Response callback{hop(authorize.location)};
        QCOMPARE(callback.status, 302);
        QVERIFY2(!callback.setCookie.isEmpty(), "the edge must issue the session cookie");
        QVERIFY(callback.setCookie.contains("synqt_session="));

        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());
        const SessionRecord *record{edge.sessionManager()->lookup(token)};
        QVERIFY(record != nullptr);
        QCOMPARE(record->scope, QStringLiteral("moderator"));  // the edge's map.qml mapped octocat
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("octocat"));

        // The tokens live ONLY on the auth entity, bound to the session; the edge holds none.
        QVERIFY2(edge.identityProvider()->tokensForSession(token).isEmpty(),
                 "a provider_entity edge must hold no tokens");
        QTRY_VERIFY2(!authService.backend()->tokens(QString::fromLatin1(token))
                          .value(QStringLiteral("access_token")).toString().isEmpty(),
                     "the auth entity must hold the tokens, bound to the session");
        const QString access{authService.backend()->tokens(QString::fromLatin1(token))
                                 .value(QStringLiteral("access_token")).toString()};
        QVERIFY2(!callback.setCookie.contains(access.toUtf8()),
                 "the access token must never appear in what the browser receives");
        QVERIFY(!callback.body.contains(access.toUtf8()));
    }

    void unknownStateRejected()
    {
        // The framework accepts only a state it issued: a forged/expired state is refused
        // before any token exchange.
        const Response forged{get(QUrl{edgeUrl(
            QStringLiteral("/auth/callback?code=whatever&state=forged-nonsense"))})};
        QCOMPARE(forged.status, 400);
        QVERIFY(forged.setCookie.isEmpty());
    }

    void devStubRefusedWithoutGate()
    {
        // A second edge with the dev gate OFF must refuse the dev stub provider entirely.
        QQmlEngine engine;
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = false;  // gate off: dev stub must never run
        config.identity.providers = {stubProvider(m_stub->baseUrl())};

        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkRequest request{QUrl{QStringLiteral("http://127.0.0.1:%1/auth/login").arg(edge.serverPort())}};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply *reply{m_browser.get(request)};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
        reply->deleteLater();
    }
};

QTEST_MAIN(TestM8)
#include "tst_m8.moc"
