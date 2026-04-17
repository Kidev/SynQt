// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M8 acceptance. A full provider login runs entirely on the edge (Authorization Code +
// PKCE, framework-generated state verified on the callback, the client secret and tokens
// held on the edge), the browser ends with only an httpOnly session cookie, the session
// carries the normalized identity and the scope the mapping hook returned, and tokens
// never appear in what the browser receives. The dev stub provider is refused unless the
// dev gate is on.

#include "connectpointhost.h"
#include "identityconfig.h"
#include "identityprovider.h"
#include "identityservice.h"
#include "jwksverifier.h"
#include "meshclient.h"
#include "moduleimports.h"
#include "oauthbackend.h"
#include "sessionmanager.h"
#include "stubidentityserver.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "sessionstore_sourcehelper.h"   // synqtRegisterSessionStoreSources()
#include "identity_sourcehelper.h"  // synqtRegisterIdentitySources()

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSemaphore>
#include <QSslCertificate>
#include <QSslKey>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
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

// An OpenID Connect provider. Identity comes from the JWKS-verified ID token. It has NO
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

// A provider whose endpoints are plaintext and off-host. The shape of a copied config
// where someone changed https to http, or a provider reached through an internal proxy.
// Nothing about it may be spoken to.
IdentityProviderConfig plaintextProvider()
{
    IdentityProviderConfig provider;
    provider.name = QStringLiteral("plaintext");
    provider.devStub = true;  // so only the endpoint check can be what refuses it
    provider.authorizeUrl = QUrl{QStringLiteral("http://provider.example/authorize")};
    provider.tokenUrl = QUrl{QStringLiteral("http://provider.example/token")};
    provider.userinfoUrl = QUrl{QStringLiteral("http://provider.example/userinfo")};
    provider.clientId = QStringLiteral("stub-client");
    provider.clientSecret = QStringLiteral("stub-secret");
    return provider;
}

/// A JWKS endpoint on loopback, so a test can decide how many keys a provider publishes.
///
/// The stub serves exactly one key and cannot be made to serve two, and two is the
/// interesting number. It is what a provider publishes for the length of a rotation, and it
/// is where choosing a key by position rather than by name starts to matter.
class JwksHost
{
public:
    explicit JwksHost(const QJsonArray &keys)
    {
        // Built by inserting rather than by brace-initializing an object literal: a
        // QJsonValue is constructible from anything, so `QJsonObject{{"keys", keys}}` puts
        // the array inside a second array and serves `"keys":[[...]]`. The same trap the
        // note in JwksVerifier::selectKey is about.
        QJsonObject document;
        document.insert(QStringLiteral("keys"), keys);
        m_document = QJsonDocument{document}.toJson(QJsonDocument::Compact);
        m_server.route(QStringLiteral("/jwks"), [this]() {
            return QHttpServerResponse{QByteArrayLiteral("application/json"), m_document};
        });
        m_socket = new QTcpServer{&m_owner};
        m_socket->listen(QHostAddress::LocalHost, 0);
        m_server.bind(m_socket);
    }

    /// Loopback http, which is what an identity endpoint may be reached over when it is
    /// on this machine (isSecureIdentityEndpoint). Anywhere else it would have to be https.
    QUrl url() const
    {
        return QUrl{QStringLiteral("http://127.0.0.1:%1/jwks").arg(m_socket->serverPort())};
    }

private:
    QByteArray m_document;
    QObject m_owner;
    QHttpServer m_server;
    QTcpServer *m_socket{nullptr};
};

/// One web edge, on a thread with a quarter of a megabyte of stack.
///
/// What the nesting below spends is stack, and how much a level of it costs is decided by
/// the compiler: about 3.5 KB with GCC on Linux, several times that with MSVC. So the same
/// sixty-four levels that fit comfortably in the eight megabytes Linux and macOS give the
/// main thread overflowed the one megabyte Windows gives it, and the first Windows run of
/// this test is where that showed up -- as a stack overflow, in CI, on a ceiling written to
/// prevent exactly that.
///
/// Running the edge on the roomiest stack on offer is what let a bound nobody had measured
/// look fine for as long as it did, so it is given the smallest stack it can start on
/// instead. A quarter of a megabyte is under what sixty-four levels cost on the cheapest
/// platform, which is what makes the unbounded case fail here and not only on Windows. The
/// bound is a share of the stack rather than a count, so what is left over for everything
/// else is the same three quarters whatever the number is.
class SmallStackEdge : public QThread
{
public:
    explicit SmallStackEdge(WebEdgeConfig config)
        : m_config{std::move(config)}
    {
        setStackSize(kStackBytes);
    }

    ~SmallStackEdge() override
    {
        quit();
        wait();
    }

    /// Starts the thread and blocks until the edge has tried to listen.
    bool startAndWait()
    {
        start();
        m_ready.acquire();
        return m_started;
    }

    quint16 port() const { return m_port; }
    QString errorString() const { return m_error; }

protected:
    void run() override
    {
        // Created here rather than handed in, so the whole request path -- routing, the
        // callback handler and the nested exchange loops under it -- runs on this stack
        // instead of merely reaching it.
        QQmlEngine engine;
        WebEdge edge{m_config, &engine};
        m_started = edge.start();
        m_port = edge.serverPort();
        m_error = edge.errorString();
        m_ready.release();
        if (m_started) {
            exec();
        }
    }

private:
    static constexpr uint kStackBytes{256 * 1024};

    WebEdgeConfig m_config;
    QSemaphore m_ready;   ///< released once the three fields below are written
    bool m_started{false};
    quint16 m_port{0};
    QString m_error;
};

/// A token endpoint that accepts the connection and answers nothing for a while.
///
/// This is what puts a callback inside a nested event loop. The exchange has been started
/// and cannot finish, so its handler stays on the stack and whatever arrives next is served
/// from inside it. The connection is dropped rather than answered when the time is up,
/// because how the exchange ends is not what is under test. The time in between is.
class StallingTokenEndpoint : public QTcpServer
{
public:
    explicit StallingTokenEndpoint(int holdMs)
        : m_holdMs{holdMs}
    {
        listen(QHostAddress::LocalHost, 0);
    }

    QUrl tokenUrl() const
    {
        return QUrl{QStringLiteral("http://127.0.0.1:%1/token").arg(serverPort())};
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        // Never handed to addPendingConnection, so nothing else takes an interest in it.
        QTimer::singleShot(m_holdMs, socket, [socket]() {
            socket->abort();
            socket->deleteLater();
        });
    }

private:
    int m_holdMs;
};

/// One edge process, with everything it needs to reach an auth entity.
///
/// A replicated deployment is N of these against one auth entity, which is why this exists
/// as a thing that can be made twice rather than as a block of setup inlined once.
///
/// Declaration order is destruction order reversed, and it matters: the edge's
/// IdentityProvider is the receiver of a dynamic Replica that frees its runtime metaobject
/// when it is destroyed, so the Replica (parented to the node, in meshScope) must outlive
/// the edge. Declaring meshScope first destroys it last.
struct EdgeProcess
{
    QQmlEngine engine;
    QObject meshScope;
    std::unique_ptr<WebEdge> edge;
    quint16 port{0};
};

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

    /// The auth entity `identity.provider_entity` names. One OAuth engine, holding the
    /// secret, behind an Identity Source over mutual TLS. Every replica consumes this one.
    struct AuthEntity
    {
        IdentityConfig config;
        std::unique_ptr<IdentityService> service;
        QQmlEngine engine;
        std::unique_ptr<ConnectPointHost> host;
        QString error;

        bool start(TestM8 *owner)
        {
            config.enabled = true;
            config.allowDevStub = true;
            config.providers = {stubProvider(owner->m_stub->baseUrl())};
            service = std::make_unique<IdentityService>(config);

            ConnectPointConfig point;
            point.name = QStringLiteral("identity");
            point.contract = QStringLiteral("Identity");
            point.owner = QStringLiteral("auth");
            point.consumers = {QStringLiteral("web")};
            point.serverFile = QStringLiteral(M8_SRCDIR "/auth/Identity.qml");
            point.shared = false;
            point.endpoint.mode = MeshTransportMode::MutualTls;
            point.endpoint.host = QStringLiteral("127.0.0.1");
            point.endpoint.port = 0;

            host = std::make_unique<ConnectPointHost>(point, credsFor(QStringLiteral("auth")),
                                                      &engine);
            host->setContextObject(QStringLiteral("IdentityEngine"), service.get());
            if (!host->start()) {
                error = host->errorString();
                return false;
            }
            return true;
        }

        quint16 port() const { return host ? host->serverPort() : 0; }
    };

    /// One more replica. A secret-less edge that reaches the auth entity over the mesh.
    /// Every one of them presents the entity name "web", which is what makes them
    /// interchangeable to the auth entity rather than merely similar.
    std::unique_ptr<EdgeProcess> startEdge(quint16 authPort)
    {
        auto process{std::make_unique<EdgeProcess>()};

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.providerEntity = QStringLiteral("auth");
        config.identity.allowDesktopLogin = true;
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/map.qml");
        IdentityProviderConfig nameOnly;
        nameOnly.name = QStringLiteral("stub");
        config.identity.providers = {nameOnly};

        process->edge = std::make_unique<WebEdge>(config, &process->engine);
        if (!process->edge->start()) {
            return nullptr;
        }
        process->port = process->edge->serverPort();

        QRemoteObjectNode *node{new QRemoteObjectNode{&process->meshScope}};
        MeshClient *client{new MeshClient{&process->meshScope}};
        IdentityProvider *provider{process->edge->identityProvider()};
        connect(client, &MeshClient::connected, node, [node, provider](QIODevice *device) {
            node->addClientSideConnection(device);
            QRemoteObjectDynamicReplica *replica{node->acquireDynamic(QStringLiteral("identity"))};
            replica->setParent(node);
            connect(replica, &QRemoteObjectDynamicReplica::initialized, provider,
                    [provider, replica]() { provider->attachRemote(replica); });
        });
        client->connectMutualTls(QHostAddress::LocalHost, authPort, QStringLiteral("auth"),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/web.crt")),
            loadPrivateKey(QStringLiteral(M8_CERT_DIR "/web.key")));

        // The link has to be up and the Replica initialized before a login is driven through
        // it, or the first request fails on "auth entity not connected" and says nothing
        // about the thing under test. isRemote() is not the signal for that: it answers for
        // the configuration (this edge delegates) and is true from construction, not for the
        // link. The readiness that matters is the Replica having attached, and the login
        // route is what reports it, so this drives one and retries rather than sleeping a
        // number somebody guessed.
        for (int attempt{0}; attempt < 40; ++attempt) {
            QTest::qWait(50);
            QNetworkAccessManager probe;
            probe.setCookieJar(new QNetworkCookieJar{&probe});
            const Response ready{hopWith(probe, edgeBase(*process)
                                         + QStringLiteral("/auth/login?provider=stub"))};
            if (ready.status == 302) {
                return process;
            }
        }
        return nullptr;
    }

    static QString edgeBase(const EdgeProcess &process)
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(process.port);
    }

    /// The provider's redirect, pointed at a named replica.
    ///
    /// The provider sends the browser to the callback URL the login was begun with, which
    /// names the replica that began it. A balancer in front of N replicas would pick again
    /// here, independently, so this is what redirecting the callback elsewhere looks like
    /// from the edge's side. The same URL on a different port.
    static QString redirectedTo(const QString &location, const EdgeProcess &process)
    {
        QUrl url{location};
        url.setPort(process.port);
        return url.toString(QUrl::FullyEncoded);
    }

    /// Drive a whole desktop sign-in through one replica and return the claim code the
    /// loopback redirect carries. Empty if any hop of it did not do what it should.
    QString desktopClaimFrom(const EdgeProcess &process, const QByteArray &verifier)
    {
        const QByteArray challenge{
            QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)};

        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), QStringLiteral("stub"));
        query.addQueryItem(QStringLiteral("return"),
                           QStringLiteral("http://127.0.0.1:5555/"));
        query.addQueryItem(QStringLiteral("return_state"), QStringLiteral("apps-own-nonce"));
        query.addQueryItem(QStringLiteral("return_challenge"),
                           QString::fromLatin1(challenge));
        QUrl login{edgeBase(process) + QStringLiteral("/auth/login")};
        login.setQuery(query);

        QNetworkAccessManager browser;
        browser.setCookieJar(new QNetworkCookieJar{&browser});
        const Response begun{hopWith(browser, login.toString(QUrl::FullyEncoded))};
        if (begun.status != 302) {
            return {};
        }
        const Response authorize{hopWith(browser, begun.location)};
        if (authorize.status != 302) {
            return {};
        }
        const Response callback{hopWith(browser, authorize.location)};
        if (callback.status != 302) {
            return {};
        }
        return QUrlQuery{QUrl{callback.location}.query()}
            .queryItemValue(QStringLiteral("code"));
    }

    /// Redeem a claim at a named replica, the way the native client does. A POST with no
    /// Origin, over a connection of its own.
    Response claimAt(const EdgeProcess &process, const QString &code, const QString &verifier)
    {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("verifier"), verifier);

        QNetworkRequest request{QUrl{edgeBase(process)
                                     + QStringLiteral("/auth/login/claim")}};
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArrayLiteral("application/x-www-form-urlencoded"));
        QNetworkAccessManager client;
        QNetworkReply *reply{client.post(
            request, form.toString(QUrl::FullyEncoded).toUtf8())};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    Response hopWith(QNetworkAccessManager &browser, const QString &url)
    {
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
        response.setCookie = reply->rawHeader("Set-Cookie");
        response.body = reply->readAll();
        reply->deleteLater();
        return response;
    }

    /// The same GET, with the fetch-metadata header a browser would attach.
    Response getAs(const QUrl &url, const QByteArray &fetchSite)
    {
        QNetworkRequest request{url};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        if (!fetchSite.isEmpty()) {
            request.setRawHeader(QByteArrayLiteral("Sec-Fetch-Site"), fetchSite);
        }
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

    /// One real ID token from the stub provider, for a nonce of our choosing.
    ///
    /// Driven over the provider's own HTTP surface (/authorize for a code, /token to
    /// exchange it) rather than by reaching into the stub to sign one, so the token under
    /// test is the same object a real provider would hand the edge. Empty on any failure,
    /// which the caller asserts on.
    QString mintStubIdToken(const QString &nonce)
    {
        return mintIdTokenFrom(m_stub.get(), nonce);
    }

    /// The same round trip against any stub, so a test can stand one up that misbehaves.
    QString mintIdTokenFrom(StubIdentityServer *stub, const QString &nonce)
    {
        QUrl authorize{stub->baseUrl() + QStringLiteral("/authorize")};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client_id"), QStringLiteral("stub-client"));
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("redirect_uri"),
                           edgeUrl(QStringLiteral("/auth/callback")));
        query.addQueryItem(QStringLiteral("nonce"), nonce);
        authorize.setQuery(query);

        const Response redirected{get(authorize)};
        if (redirected.status != 302) {
            return {};
        }
        const QString code{QUrlQuery{QUrl{redirected.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        if (code.isEmpty()) {
            return {};
        }

        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("client_id"), QStringLiteral("stub-client"));
        form.addQueryItem(QStringLiteral("client_secret"), QStringLiteral("stub-secret"));
        QNetworkRequest request{QUrl{stub->baseUrl() + QStringLiteral("/token")}};
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        QNetworkReply *reply{m_browser.post(request,
                                            form.toString(QUrl::FullyEncoded).toUtf8())};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QJsonObject tokens{QJsonDocument::fromJson(reply->readAll()).object()};
        reply->deleteLater();
        return tokens.value(QStringLiteral("id_token")).toString();
    }

    /// The same token with a different header segment, base64url encoded as a JWT is.
    ///
    /// The signature is left alone, because every check this is used for (the
    /// algorithm, the key id) is made before the signature is verified, so a token that
    /// reaches the signature check has already got further than it should have.
    static QString reheadered(const QStringList &parts, const QJsonObject &header)
    {
        const QByteArray encoded{QJsonDocument{header}.toJson(QJsonDocument::Compact)
                                     .toBase64(QByteArray::Base64UrlEncoding
                                               | QByteArray::OmitTrailingEquals)};
        return QString::fromUtf8(encoded) + QLatin1Char('.') + parts.at(1)
               + QLatin1Char('.') + parts.at(2);
    }

    /// The same signature with one bit of it flipped. The cheapest forgery there is, and
    /// the one a signature check exists to catch.
    ///
    /// Decoded, altered, and re-encoded rather than edited as text. Changing the last
    /// character of the base64url segment looks equivalent and is not: 256 bytes of RSA
    /// signature encode to 342 characters whose last one carries only two significant bits,
    /// so 'A' -> 'B' there flips a padding bit and decodes to the identical signature. The
    /// token then verifies, and the test fails only for the tokens whose final character
    /// happened to land on a significant bit, so intermittently, in a test
    /// whose whole job is to prove a forgery is caught.
    static QString withForgedSignature(const QStringList &parts)
    {
        QByteArray signature{QByteArray::fromBase64(parts.at(2).toUtf8(),
                                                    QByteArray::Base64UrlEncoding)};
        signature[signature.size() / 2] = static_cast<char>(
            signature.at(signature.size() / 2) ^ 0x01);
        const QByteArray encoded{signature.toBase64(QByteArray::Base64UrlEncoding
                                                    | QByteArray::OmitTrailingEquals)};
        return parts.at(0) + QLatin1Char('.') + parts.at(1) + QLatin1Char('.')
               + QString::fromUtf8(encoded);
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
        synqtRegisterSessionStoreSources();
        synqtRegisterIdentitySources();
        // The two Source files below are the ones `synqt build` generates for a promoted
        // auth entity, byte for byte (test_provider_entity asserts it), and they are loaded
        // here by the same engine the generated auth main would load them with. That main
        // registers this, so `import SynQt` means the same thing in both places. Without it
        // a file that writes one import line rather than two fails to load here alone.
        SynQt::registerModuleImports();

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
                             QStringLiteral("https://evil.example")),
            plaintextProvider()};

        m_edge = std::make_unique<WebEdge>(config, m_engine.get());
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    void fullLoginFlow()
    {
        // 1. The browser hits the login route. The edge starts the flow and redirects to
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

    // Signing in ends with a redirect to the app, so the very next thing the browser does
    // is load the client route with the session cookie it was given. That load must
    // leave the session alone. An edge that mints one per page load signs the visitor out
    // one redirect after signing them in, and the app looks like it never logged in at all.
    void signingInSurvivesTheLandingPageLoad()
    {
        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        QCOMPARE(callback.location, QStringLiteral("/"));
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());
        const SessionRecord *before{m_edge->sessionManager()->lookup(token)};
        QVERIFY(before != nullptr);
        QCOMPARE(before->scope, QStringLiteral("moderator"));

        // The browser follows the redirect (m_browser holds the cookie jar, so the session
        // cookie rides along exactly as a real browser's would).
        const Response landing{get(QUrl{edgeUrl(QStringLiteral("/"))})};
        QCOMPARE(landing.status, 200);
        QVERIFY2(landing.setCookie.isEmpty(),
                 "landing on the app must not replace the session the login just created");

        const SessionRecord *after{m_edge->sessionManager()->lookup(token)};
        QVERIFY2(after != nullptr, "the signed-in session must survive the landing load");
        QCOMPARE(after->scope, QStringLiteral("moderator"));
        QCOMPARE(after->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("octocat"));
    }

    // The client secret goes to the token endpoint, the authorization code comes back
    // through the browser, and the JWKS decides which signatures are trusted. Over http
    // all three are readable and the last is forgeable, so a login through a plaintext
    // provider is refused before the browser is sent anywhere.
    void plaintextProviderRefusedBeforeSendingTheBrowser()
    {
        const Response login{get(QUrl{edgeUrl(QStringLiteral("/auth/login?provider=plaintext"))})};
        QCOMPARE(login.status, 500);
        QVERIFY2(login.location.isEmpty(),
                 "the browser must not be sent to a provider reached over http");
        QVERIFY2(!login.setCookie.contains("synqt_oauth_state="),
                 "no login is pending, so nothing binds one to this browser");
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

    // The ID-token verifier had exactly one refusal under test (the wrong issuer), and a
    // verifier is the sum of what it refuses. On the happy path it is indistinguishable
    // from `return payload`. These drive SynQt::JwksVerifier directly against a real
    // RS256 token the stub signed, and a real JWKS it serves, mutating one thing at a time.
    void idTokenRefusals()
    {
        // A genuine token for a known nonce, taken straight from the provider's own token
        // endpoint rather than minted here, so what is verified is what a provider sends.
        const QString nonce{QStringLiteral("nonce-for-the-verifier-test")};
        const QString idToken{mintStubIdToken(nonce)};
        QVERIFY2(!idToken.isEmpty(), "the stub provider issued no ID token");
        const QStringList parts{idToken.split(QLatin1Char('.'))};
        QCOMPARE(parts.size(), 3);

        QNetworkAccessManager network;
        JwksVerifier verifier{&network};
        // The stub issues under its own base URL when nothing overrides it, which is what
        // the OIDC provider above is configured against too.
        const IdentityProviderConfig good{
            stubOidcProvider(m_stub->baseUrl(), QStringLiteral("verifier"),
                             m_stub->baseUrl())};

        // The control: this token, this JWKS, this nonce, and the claims come back.
        QString error;
        const QVariantMap claims{verifier.verify(idToken, good, nonce, &error)};
        QVERIFY2(!claims.isEmpty(), qPrintable(error));
        QCOMPARE(claims.value(QStringLiteral("iss")).toString(), m_stub->baseUrl());
        QCOMPARE(claims.value(QStringLiteral("nonce")).toString(), nonce);

        const auto refuses{[&](const QString &token, const IdentityProviderConfig &provider,
                               const QString &expectedNonce, const QString &reason) {
            QString why;
            const QVariantMap result{verifier.verify(token, provider, expectedNonce, &why)};
            QVERIFY2(result.isEmpty(),
                     qPrintable(QStringLiteral("expected a refusal (%1) but the token "
                                               "verified").arg(reason)));
            QVERIFY2(why.contains(reason),
                     qPrintable(QStringLiteral("refused with '%1', expected '%2'")
                                    .arg(why, reason)));
        }};

        // Not a JWT at all. Two segments, and empty segments, are the shapes a hand-built
        // token arrives in.
        refuses(QStringLiteral("header.payload"), good, nonce, QStringLiteral("malformed"));
        refuses(QStringLiteral(".."), good, nonce, QStringLiteral("malformed"));

        // Algorithm confusion, the classic JWT attack. The attacker rewrites the header to
        // an algorithm whose "verification" they control. The header is read before any key
        // is fetched, so this must be refused on the algorithm alone.
        refuses(reheadered(parts, QJsonObject{{QStringLiteral("alg"), QStringLiteral("HS256")},
                                              {QStringLiteral("kid"), QStringLiteral("stub")}}),
                good, nonce, QStringLiteral("unsupported ID-token algorithm"));
        refuses(reheadered(parts, QJsonObject{{QStringLiteral("alg"), QStringLiteral("none")}}),
                good, nonce, QStringLiteral("unsupported ID-token algorithm"));

        // A key id the JWKS does not publish. There is nothing to verify against, and
        // "cannot find the key" must never degrade into "accept it".
        refuses(reheadered(parts, QJsonObject{{QStringLiteral("alg"), QStringLiteral("RS256")},
                                              {QStringLiteral("kid"), QStringLiteral("not-ours")}}),
                good, nonce, QStringLiteral("matches this ID token's kid"));

        // A forged signature over an otherwise perfect token.
        refuses(withForgedSignature(parts), good, nonce,
                QStringLiteral("signature invalid"));

        // A token minted for a different client. Accepting it is the token-substitution
        // confusion: a valid, correctly signed token from the same provider, issued to
        // someone else.
        IdentityProviderConfig otherAudience{good};
        otherAudience.audience = QStringLiteral("a-different-client");
        refuses(idToken, otherAudience, nonce, QStringLiteral("audience mismatch"));

        // A different issuer entirely.
        IdentityProviderConfig otherIssuer{good};
        otherIssuer.issuer = QStringLiteral("https://not.the.issuer");
        refuses(idToken, otherIssuer, nonce, QStringLiteral("issuer mismatch"));

        // Replay: a token that was fine for one login being presented for another. The
        // nonce is what binds a token to the request that asked for it.
        refuses(idToken, good, QStringLiteral("some-other-login"),
                QStringLiteral("nonce mismatch"));

        // And nothing above quietly broke the verifier: the good token still verifies.
        error.clear();
        QVERIFY2(!verifier.verify(idToken, good, nonce, &error).isEmpty(), qPrintable(error));
    }

    /// Which key verifies an ID token, when the provider publishes more than one.
    ///
    /// Two keys is a rotation in progress, and it is the ordinary state of a provider for a
    /// day or so. A token that names its key (`kid`) is unambiguous either way. A token that
    /// names none is not, and taking whichever key the provider happened to list first makes
    /// acceptance depend on the order of a JSON array. The same token verifies or does not
    /// depending on which entry came back at the top.
    ///
    /// The no-kid tokens here are reheadered, so their signatures no longer match, and that
    /// is what makes the test readable rather than a problem to work around. Key selection
    /// happens before the signature is checked, so a refusal naming the selection is one
    /// that got no further, and "signature invalid" is proof that selection succeeded and
    /// handed a key on.
    void anIdTokenThatNamesNoKeyIsRefusedWhenTheProviderPublishesTwo()
    {
        const QString nonce{QStringLiteral("nonce-for-the-key-selection-test")};
        const QString idToken{mintStubIdToken(nonce)};
        QVERIFY2(!idToken.isEmpty(), "the stub provider issued no ID token");
        const QStringList parts{idToken.split(QLatin1Char('.'))};
        QCOMPARE(parts.size(), 3);

        // The provider's own key, read back off its JWKS, so what is served below is the
        // key it signed with rather than one invented here.
        const Response served{get(QUrl{m_stub->baseUrl() + QStringLiteral("/jwks")})};
        QCOMPARE(served.status, 200);
        // '=' not '{}': QJsonArray{anArray} wraps the array as a single element rather
        // than copying it (see the note in JwksVerifier::selectKey), and the wrapper has a
        // size of one, so the assertion below would pass on the wrong thing.
        const QJsonArray published = QJsonDocument::fromJson(served.body).object()
                                         .value(QStringLiteral("keys")).toArray();
        QCOMPARE(published.size(), 1);

        // The same key under a second name. Enough to make the set ambiguous, which is all
        // selection looks at. A rotation publishes a different key, and that
        // difference is the signature check's business rather than this one's.
        QJsonObject rotatingIn = published.first().toObject();  // '=': see JwksHost
        rotatingIn.insert(QStringLiteral("kid"), QStringLiteral("the-key-being-rotated-in"));
        QJsonArray both;
        both.append(published.first());
        both.append(rotatingIn);

        JwksHost soleKeyHost{published};
        JwksHost rotatingHost{both};

        IdentityProviderConfig soleKey{
            stubOidcProvider(m_stub->baseUrl(), QStringLiteral("one-key"), m_stub->baseUrl())};
        soleKey.jwksUrl = soleKeyHost.url();
        IdentityProviderConfig rotating{soleKey};
        rotating.name = QStringLiteral("two-keys");
        rotating.jwksUrl = rotatingHost.url();

        const QString noKid{reheadered(parts, QJsonObject{{QStringLiteral("alg"),
                                                           QStringLiteral("RS256")}})};


        QNetworkAccessManager network;
        JwksVerifier verifier{&network};

        // One key published: the token names none and there is only one it could be, so
        // selection succeeds and the token is refused on its signature instead.
        QString why;
        QVERIFY(verifier.verify(noKid, soleKey, nonce, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("signature invalid")), qPrintable(why));

        // Two keys published: there is no telling which of them signed it, and guessing by
        // position is exactly what must not happen.
        why.clear();
        QVERIFY(verifier.verify(noKid, rotating, nonce, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("cannot be told")), qPrintable(why));

        // And a token that does name its key still verifies against the rotating set, which
        // is the whole reason a provider publishes two.
        why.clear();
        QVERIFY2(!verifier.verify(idToken, rotating, nonce, &why).isEmpty(), qPrintable(why));
    }

    /// Callbacks arriving together are bounded, with identity running on the edge.
    ///
    /// Every callback waits for the token exchange inside a nested event loop, and a nested
    /// loop goes on serving requests, so a second callback arriving during the first runs
    /// its own exchange inside that stack frame. The callback route is open, and a state
    /// this edge issued is all it takes to get past the first check, so without a ceiling
    /// the nesting depth follows the request rate and the stack is what decides.
    ///
    /// The ceiling covers both paths, and this drives the one that does not delegate to an
    /// auth entity. Identity in process, a provider whose token endpoint accepts and
    /// answers nothing, and more callbacks at once than the ceiling allows. What it looks
    /// for is a callback answered while the others are still waiting, which is the one thing
    /// a ceiling produces and the one thing its absence rules out.
    ///
    /// The edge runs on a small stack (SmallStackEdge) because a count of sixty-four is not
    /// on its own an answer to how much stack sixty-four levels cost. Run against a roomy
    /// stack this passed while the ceiling it was testing sat above what a Windows edge can
    /// hold; run against a small one it stops rather than crashes, which is the whole claim.
    void concurrentCallbacksAreBoundedWithIdentityInProcess()
    {
        // Above both ceilings in identityprovider.cpp -- the count (kMaxConcurrentWaits,
        // 64) and the quarter of the thread's stack the nesting may spend -- so some of
        // these have to be refused rather than nested, whichever of the two decides.
        constexpr int kInFlight{80};
        constexpr int kStallMs{3000};
        // Comfortably inside the stall. An answer this early is one that did not wait for
        // the token endpoint, and there is no other way to get one.
        constexpr qint64 kAnsweredWithoutWaitingMs{1200};

        StallingTokenEndpoint stall{kStallMs};
        QVERIFY(stall.isListening());

        IdentityProviderConfig slow;
        slow.name = QStringLiteral("slow");
        slow.devStub = true;
        slow.authorizeUrl = QUrl{m_stub->baseUrl() + QStringLiteral("/authorize")};
        slow.tokenUrl = stall.tokenUrl();
        slow.userinfoUrl = QUrl{m_stub->baseUrl() + QStringLiteral("/userinfo")};
        slow.clientId = QStringLiteral("stub-client");
        slow.clientSecret = QStringLiteral("stub-secret");

        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.providers = {slow};

        SmallStackEdge edge{config};
        QVERIFY2(edge.startAndWait(), qPrintable(edge.errorString()));
        const QString base{QStringLiteral("http://127.0.0.1:%1").arg(edge.port())};

        // One browser per login, because each holds its own state and its own
        // login-binding cookie, and the callback is refused without the matching pair.
        QObject browserScope;
        QList<QNetworkAccessManager *> browsers;
        QStringList states;
        for (int index{0}; index < kInFlight; ++index) {
            auto *browser{new QNetworkAccessManager{&browserScope}};
            browser->setCookieJar(new QNetworkCookieJar{browser});
            const Response begun{
                hopWith(*browser, base + QStringLiteral("/auth/login?provider=slow"))};
            QCOMPARE(begun.status, 302);
            const QString state{QUrlQuery{QUrl{begun.location}.query()}
                                    .queryItemValue(QStringLiteral("state"))};
            QVERIFY(!state.isEmpty());
            browsers.append(browser);
            states.append(state);
        }

        // Fired without waiting for any of them, which is the whole point: they have to be
        // in flight together for the nesting to happen at all.
        QElapsedTimer clock;
        QList<qint64> answeredAtMs;
        clock.start();
        for (int index{0}; index < kInFlight; ++index) {
            QUrl callback{base + QStringLiteral("/auth/callback")};
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("code"), QStringLiteral("a-code"));
            query.addQueryItem(QStringLiteral("state"), states.at(index));
            callback.setQuery(query);
            QNetworkRequest request{callback};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
            QNetworkReply *reply{browsers.at(index)->get(request)};
            connect(reply, &QNetworkReply::finished, reply,
                    [&answeredAtMs, &clock]() { answeredAtMs.append(clock.elapsed()); });
        }

        // Long enough for the stalls to expire and every nested loop to unwind. It cannot
        // return before they do: this loop is underneath them on the stack.
        QTRY_VERIFY_WITH_TIMEOUT(answeredAtMs.size() == kInFlight, 30000);

        int answeredWithoutWaiting{0};
        for (const qint64 elapsed : std::as_const(answeredAtMs)) {
            if (elapsed < kAnsweredWithoutWaitingMs) {
                ++answeredWithoutWaiting;
            }
        }
        QVERIFY2(answeredWithoutWaiting > 0,
                 "every callback waited for the token endpoint, so nothing bounded the "
                 "nesting: the ceiling covers the auth-entity path only");
        QVERIFY2(answeredWithoutWaiting < kInFlight,
                 "no callback waited at all, so none of them nested and this proves "
                 "nothing about the ceiling");
    }

    /// A correctly signed token that leaves out a claim the verifier depends on.
    ///
    /// Kept apart from idTokenRefusals because these two cannot be built by editing a good
    /// token. The payload is what is signed, so a token with `exp` cut out of it fails on
    /// the signature and proves nothing about the claim check. The stub signs them instead
    /// (StubIdentityServer::omitIdTokenClaim), which is also what a real provider doing
    /// this would look like from here.
    ///
    /// `exp` mattered because the check used to run only when the claim was present, so a
    /// token with none was a sign-in that never expired: a copy taken today would still
    /// open a session years from now. `sub` mattered because everything downstream keys on
    /// it -- the scope mapping reads it and a device credential is enrolled against it --
    /// so a token with none signed the visitor in as the empty subject, and every visitor
    /// arriving that way was the same one.
    void anIdTokenMissingARequiredClaimIsRefused()
    {
        const IdentityProviderConfig provider{
            stubOidcProvider(m_stub->baseUrl(), QStringLiteral("verifier"), m_stub->baseUrl())};

        for (const auto &[claim, reason] :
             {std::pair<QString, QString>{QStringLiteral("exp"),
                                          QStringLiteral("carries no expiry")},
              std::pair<QString, QString>{QStringLiteral("sub"),
                                          QStringLiteral("carries no subject")}}) {
            StubIdentityServer stub{StubIdentityServer::DevOnly{}};
            stub.setClientCredentials(QStringLiteral("stub-client"),
                                      QStringLiteral("stub-secret"));
            QVERIFY(stub.start());
            stub.setIssuer(stub.baseUrl());
            stub.omitIdTokenClaim(claim);

            const QString nonce{QStringLiteral("nonce-for-%1").arg(claim)};
            const QString token{mintIdTokenFrom(&stub, nonce)};
            QVERIFY2(!token.isEmpty(), "the stub provider issued no ID token");

            IdentityProviderConfig against{provider};
            against.issuer = stub.baseUrl();
            against.jwksUrl = QUrl{stub.baseUrl() + QStringLiteral("/jwks")};

            QNetworkAccessManager network;
            JwksVerifier verifier{&network};
            QString why;
            QVERIFY2(verifier.verify(token, against, nonce, &why).isEmpty(),
                     qPrintable(QStringLiteral("a token with no %1 verified").arg(claim)));
            QVERIFY2(why.contains(reason),
                     qPrintable(QStringLiteral("refused with '%1', expected '%2'")
                                    .arg(why, reason)));
        }
    }

    /// Another site must not be able to sign a visitor out by navigating them here.
    ///
    /// Logout is reached by a GET, because that is what `Session.logout()` does on both
    /// clients: the browser navigates to the route and the desktop client fetches it. That
    /// makes it a state change any page can cause, and the cookie is no defense --
    /// SameSite=Lax is sent on exactly this, a top-level navigation, and in `split_origin`
    /// the cookie is SameSite=None and is sent on everything. What it costs the visitor is
    /// not only the session: signing out is also the one thing that ends a device
    /// credential, so a stray link would take their stored sign-in with it.
    ///
    /// `Sec-Fetch-Site` is what separates the app's own navigation from somebody else's,
    /// and the browser sets it rather than the page. A caller that is not a browser sends
    /// none, which is why the desktop client's plain GET still works.
    void logoutFromAnotherSiteIsRefused()
    {
        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());
        QVERIFY(m_edge->sessionManager()->isLive(token));

        // Refused, and the session is untouched. Both shapes. This edge is same-origin, so
        // a sign-out from a sibling subdomain is not one of its own either.
        for (const QByteArray &site : {QByteArrayLiteral("cross-site"),
                                       QByteArrayLiteral("same-site")}) {
            const Response hostile{getAs(QUrl{edgeUrl(QStringLiteral("/auth/logout"))}, site)};
            QCOMPARE(hostile.status, 403);
            QVERIFY2(m_edge->sessionManager()->isLive(token),
                     qPrintable(QStringLiteral("a %1 request ended the visitor's session")
                                    .arg(QString::fromUtf8(site))));
        }

        // And the app's own sign-out still works, or the check above would be satisfied by
        // a logout route that refuses everybody.
        const Response own{getAs(QUrl{edgeUrl(QStringLiteral("/auth/logout"))},
                                 QByteArrayLiteral("same-origin"))};
        QCOMPARE(own.status, 302);
        QVERIFY2(!m_edge->sessionManager()->isLive(token),
                 "the visitor's own sign-out did not end the session");
    }

    void loginCsrfRejected()
    {
        // Login is bound to the browser that started it. The login redirect sets a state
        // cookie the callback must present. A different browser cannot complete the login,
        // even with a valid state and code. The defense against login CSRF / fixation.
        const Response login{get(QUrl{edgeUrl(QStringLiteral("/auth/login?provider=stub"))})};
        QCOMPARE(login.status, 302);
        QVERIFY2(login.setCookie.contains("synqt_oauth_state="),
                 "login must bind the flow to the browser with a state cookie");
        QVERIFY(login.setCookie.contains("HttpOnly"));

        const Response authorize{get(QUrl{login.location})};
        QCOMPARE(authorize.status, 302);
        QVERIFY(authorize.location.contains(QStringLiteral("/auth/callback")));

        // A different browser (fresh cookie jar) tries to finish the login. refused.
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

    // identity.provider_entity. A dedicated auth entity owns the authoritative session
    // store behind a per-caller Session connect point. Edges consume it over the mesh. A
    // session created (as login does) on one edge is validated on another.
    void providerEntityDistributedSessions()
    {
        SessionManager authStore{QStringLiteral("anonymous"), 720};

        ConnectPointConfig config;
        config.name = QStringLiteral("sessions");
        config.contract = QStringLiteral("SessionStore");
        config.owner = QStringLiteral("auth");
        config.consumers = {QStringLiteral("web"), QStringLiteral("web2")};
        config.serverFile = QStringLiteral(M8_SRCDIR "/auth/SessionStore.qml");
        config.shared = false;
        config.endpoint.mode = MeshTransportMode::MutualTls;
        config.endpoint.host = QStringLiteral("127.0.0.1");
        config.endpoint.port = 0;

        QQmlEngine authEngine;
        ConnectPointHost authHost{config, credsFor(QStringLiteral("auth")), &authEngine};
        authHost.setContextObject(QStringLiteral("Sessions"), &authStore);
        QVERIFY2(authHost.start(), qPrintable(authHost.errorString()));
        const quint16 port{authHost.serverPort()};

        // Own the edges' mesh objects in a scope declared after the auth host, so at method
        // end they tear down first, while the host and store they are connected to are
        // still alive, instead of at the test object's destruction in an undefined order.
        QObject meshScope;

        // Bring up an edge's session cache. A SessionManager in remote mode, fed by the
        // auth entity's SessionStore Replica over mutual TLS.
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
                // Owned by the node, which meshScope destroys after the cache above: the
                // receiver goes first, the Replica second, which is the order this Replica
                // needs (it frees a metaobject the receiver is connected through).
                replica->setParent(node);
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

    // An elevation rotates the session credential (SessionManager::setScope, which
    // Caller.setScope calls on every sign-in that raises somebody's scope), and the provider
    // tokens are keyed on that credential. Nothing moved them, so after any elevation the
    // tokens for the live session could not be found and the entry under the replaced id was
    // unreachable. The refresh sweep went on spending its refresh token against the provider
    // on behalf of a session that no longer existed, forever.
    void elevatingASessionCarriesItsProviderTokens()
    {
        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        const QByteArray before{sessionToken(callback.setCookie)};
        QVERIFY(!before.isEmpty());

        OAuthBackend *backend{m_edge->identityProvider()->backend()};
        QVERIFY(backend != nullptr);
        const QString access{backend->tokens(QString::fromLatin1(before))
                                 .value(QStringLiteral("access_token")).toString()};
        QVERIFY2(!access.isEmpty(), "the login must have left tokens under the session id");

        const QByteArray after{
            m_edge->sessionManager()->setScope(before, QStringLiteral("moderator"))};
        QVERIFY2(!after.isEmpty(), "the elevation must rotate the credential");
        QVERIFY(after != before);

        QCOMPARE(backend->tokens(QString::fromLatin1(after))
                     .value(QStringLiteral("access_token")).toString(), access);
        QVERIFY2(backend->tokens(QString::fromLatin1(before)).isEmpty(),
                 "nothing may be left under the credential the elevation replaced");
    }

    // Revocation is not a rare path. It is what a detected device-credential reuse does to
    // every session that credential opened. Only expiry released the provider tokens, so a
    // revoked session kept its live access and refresh tokens on the edge, which is most of
    // what the revocation was for.
    void revokingASessionReleasesItsProviderTokens()
    {
        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());

        OAuthBackend *backend{m_edge->identityProvider()->backend()};
        QVERIFY(backend != nullptr);
        QVERIFY(!backend->tokens(QString::fromLatin1(token)).isEmpty());

        m_edge->sessionManager()->revoke(token);
        QVERIFY2(backend->tokens(QString::fromLatin1(token)).isEmpty(),
                 "revoking a session must take its provider tokens with it");
    }

    // AUTH-1: with identity.provider_entity set, the client secret and the tokens live only
    // on a dedicated auth entity. The edge delegates begin/exchange over the Identity mesh
    // connect point, holds no OAuth backend, no secret and no token, and only issues the
    // session cookie.
    void providerEntityCentralizedLogin()
    {
        // The auth entity owns the OAuth engine, with the FULL provider (secret included),
        // behind a per-caller Identity Source over mutual TLS.
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
        cp.shared = false;
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
            // Owned by the node, which outlives the edge holding the receiver (edge is a
            // later stack object than meshScope, so it is destroyed first).
            replica->setParent(node);
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

        // The tokens live ONLY on the auth entity, bound to the session. The edge holds none.
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

    // A replicated edge is N interchangeable processes behind a balancer, and the OAuth
    // callback is a fresh top-level navigation from the provider. Nothing steers it back to
    // the process that began the login, so with N replicas N-1 of every N logins land
    // somewhere else. These three cases are that whole story, and they only work because the
    // pending record (the CSRF binding and the desktop context) is held by the auth entity
    // with the state rather than in the memory of whichever edge answered first.
    //
    // What makes it work is worth naming exactly, because the obvious answer is wrong. It is
    // not that the replicas share a Source: this point is per-caller, and they would still
    // work if each held its own. It is that every Source on the auth entity bridges to the
    // SAME engine, so the record is one record however many Sources are in front of it.
    //
    // The first of these three is the load-bearing one. Against the old code the other two
    // pass for the wrong reason: the edge refused every cross-replica callback outright, so
    // a test that only ever asserts a refusal is green whether the gate works or is stuck
    // shut. Only an accept that must succeed can tell those apart.
    void aLoginBegunOnOneEdgeCompletesOnAnother()
    {
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);
        QVERIFY(first->port != second->port);

        QNetworkAccessManager browser;
        browser.setCookieJar(new QNetworkCookieJar{&browser});

        // Begin at one replica, walk the provider, and come back to the OTHER one. The
        // cookie jar sends the state cookie to both, which is not a convenience of the test:
        // cookies are not keyed by port, so a real balancer in front of one hostname
        // behaves exactly this way.
        const Response login{hopWith(browser, edgeBase(*first)
                                     + QStringLiteral("/auth/login?provider=stub"))};
        QCOMPARE(login.status, 302);
        const Response authorize{hopWith(browser, login.location)};
        QCOMPARE(authorize.status, 302);
        const QString callbackUrl{redirectedTo(authorize.location, *second)};

        const Response callback{hopWith(browser, callbackUrl)};
        QCOMPARE(callback.status, 302);
        QVERIFY2(callback.setCookie.contains("synqt_session="),
                 "the replica that received the callback must be able to finish the login");

        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());
        const SessionRecord *record{second->edge->sessionManager()->lookup(token)};
        QVERIFY2(record != nullptr, "the session belongs to the replica that minted it");
        QCOMPARE(record->identity.value(QStringLiteral("login")).toString(),
                 QStringLiteral("octocat"));
    }

    void aCallbackWithTheWrongBindingIsRefusedOnEveryEdge()
    {
        // The defence a state alone does not provide. An attacker hands a victim a state
        // they began and their own authorization code, and the victim's browser completes
        // it. Moving the record to the auth entity must not have moved this check away
        // with it, and it must hold on a replica that saw none of the login.
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);

        QNetworkAccessManager attacker;
        attacker.setCookieJar(new QNetworkCookieJar{&attacker});
        const Response login{hopWith(attacker, edgeBase(*first)
                                     + QStringLiteral("/auth/login?provider=stub"))};
        QCOMPARE(login.status, 302);
        const Response authorize{hopWith(attacker, login.location)};
        QCOMPARE(authorize.status, 302);
        const QString callbackUrl{redirectedTo(authorize.location, *second)};

        // A different browser. It holds no state cookie, so it presents no binding.
        QNetworkAccessManager victim;
        victim.setCookieJar(new QNetworkCookieJar{&victim});
        const Response forged{hopWith(victim, callbackUrl)};
        QCOMPARE(forged.status, 400);
        QVERIFY2(!forged.setCookie.contains("synqt_session="),
                 "a callback without the binding must not mint a session anywhere");
    }

    void aReplayedCallbackIsRefusedOnEveryEdge()
    {
        // Single use, and single use across the whole deployment rather than once per
        // process. A record consumed on one replica has to be gone for all of them, or a
        // replayed callback buys a second session on the next replica along.
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);

        QNetworkAccessManager browser;
        browser.setCookieJar(new QNetworkCookieJar{&browser});
        const Response login{hopWith(browser, edgeBase(*first)
                                     + QStringLiteral("/auth/login?provider=stub"))};
        QCOMPARE(login.status, 302);
        const Response authorize{hopWith(browser, login.location)};
        QCOMPARE(authorize.status, 302);

        const QString atFirst{redirectedTo(authorize.location, *first)};
        const Response accepted{hopWith(browser, atFirst)};
        QCOMPARE(accepted.status, 302);
        QVERIFY(accepted.setCookie.contains("synqt_session="));

        const QString atSecond{redirectedTo(authorize.location, *second)};
        const Response replayed{hopWith(browser, atSecond)};
        QCOMPARE(replayed.status, 400);
        QVERIFY2(!replayed.setCookie.contains("synqt_session="),
                 "a callback already spent on one replica must be spent for all of them");
    }

    // The desktop half of the same problem, and a sharper version of it: the browser that
    // signs in and the native client that collects are two different programs opening two
    // different connections, so a balancer places them independently even when the visitor
    // does everything in one sitting.
    void aDesktopClaimMintedOnOneEdgeIsRedeemedOnAnother()
    {
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);

        const QByteArray verifier{QByteArrayLiteral("a-verifier-only-this-process-has")};
        const QString code{desktopClaimFrom(*first, verifier)};
        QVERIFY2(!code.isEmpty(), "the desktop login must end at a loopback claim code");

        // Collected against the replica that saw none of it.
        const Response collected{claimAt(*second, code, QString::fromLatin1(verifier))};
        QCOMPARE(collected.status, 200);
        QVERIFY2(collected.body.contains("\"session\""),
                 "the replica that received the claim must be able to answer it");
    }

    void aDesktopClaimIsSpentOnceAcrossReplicas()
    {
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);

        const QByteArray verifier{QByteArrayLiteral("another-verifier-entirely")};
        const QString code{desktopClaimFrom(*first, verifier)};
        QVERIFY(!code.isEmpty());

        QCOMPARE(claimAt(*first, code, QString::fromLatin1(verifier)).status, 200);
        // Not "spent on this process": spent, full stop. A code that buys a second session
        // from the next replica along is not single use in any sense that matters.
        QCOMPARE(claimAt(*second, code, QString::fromLatin1(verifier)).status, 404);
    }

    void aDesktopClaimWithTheWrongVerifierIsRefusedAndSpent()
    {
        // Both halves, because the second is the surprising one and is deliberate: the
        // code is taken out before the verifier is checked, so a wrong guess spends it.
        // A code read out of a browser history must not be something a guesser can sit and
        // try verifiers against.
        AuthEntity auth;
        QVERIFY2(auth.start(this), qPrintable(auth.error));
        std::unique_ptr<EdgeProcess> first{startEdge(auth.port())};
        std::unique_ptr<EdgeProcess> second{startEdge(auth.port())};
        QVERIFY(first && second);

        const QByteArray verifier{QByteArrayLiteral("the-real-verifier-for-this-one")};
        const QString code{desktopClaimFrom(*first, verifier)};
        QVERIFY(!code.isEmpty());

        QCOMPARE(claimAt(*second, code, QStringLiteral("not-the-verifier")).status, 404);
        QCOMPARE(claimAt(*first, code, QString::fromLatin1(verifier)).status, 404);
    }

    void unknownStateRejected()
    {
        // The framework accepts only a state it issued. A forged/expired state is refused
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

    void brokenScopeMappingHookIsReported()
    {
        // A hook that does not compile silently means "no mapping", and no mapping means
        // every authenticated session gets the default scope. That is a permissions change,
        // so it has to be said out loud rather than left to a stray Qt warning. The edge
        // still starts: a login that lands on the default scope beats an edge that is down.
        QQmlEngine engine;
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.mappingHook = QStringLiteral(M8_SRCDIR "/web/identity/broken.qml");
        config.identity.providers = {stubProvider(m_stub->baseUrl())};

        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral(
                                 "identity mapping hook .*broken\\.qml failed to load")});
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));
    }
};

QTEST_MAIN(TestM8)
#include "tst_m8.moc"
