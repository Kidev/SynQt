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
#include <QFile>
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
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSslCertificate>
#include <QSslKey>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <jwt-cpp/jwt.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>

#include <chrono>
#include <memory>
#include <string>

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

/// ID tokens this test signs itself, and the key set that verifies them.
///
/// The stub provider decides every claim it issues, and the claims worth refusing are the
/// ones it never gets wrong: an expiry in the past, an audience listed among others, a key
/// that is not RSA. So the test signs with a key of its own, the RSA key the configure step
/// issued for the `auth` entity, and publishes that key's modulus and exponent itself.
class TokenSigner
{
public:
    TokenSigner()
    {
        QFile file{QStringLiteral(M8_CERT_DIR "/auth.key")};
        if (file.open(QIODevice::ReadOnly)) {
            m_privatePem = file.readAll().toStdString();
        }
    }

    bool isReady() const { return !m_privatePem.empty() && !modulus().isEmpty(); }

    /// The key as a JWKS entry named \a kid, of key type \a kty.
    QJsonObject jwk(const QString &kid, const QString &kty = QStringLiteral("RSA")) const
    {
        QJsonObject key;
        key.insert(QStringLiteral("kty"), kty);
        key.insert(QStringLiteral("kid"), kid);
        key.insert(QStringLiteral("alg"), QStringLiteral("RS256"));
        key.insert(QStringLiteral("use"), QStringLiteral("sig"));
        key.insert(QStringLiteral("n"), QString::fromLatin1(modulus()));
        key.insert(QStringLiteral("e"), QString::fromLatin1(parameter(OSSL_PKEY_PARAM_RSA_E)));
        return key;
    }

    /// An RS256 token under \a kid carrying \a audience, expiring \a expiresIn from now.
    /// One audience is written as a string, more than one as an array, as providers do.
    QString sign(const QString &kid, const QStringList &audience,
                 std::chrono::seconds expiresIn, const QString &nonce) const
    {
        using Json = jwt::traits::kazuho_picojson;
        Json::array_type audiences;
        for (const QString &entry : audience) {
            audiences.push_back(Json::value_type{entry.toStdString()});
        }
        const auto now{std::chrono::system_clock::now()};
        auto builder{jwt::create()};
        if (audience.size() == 1) {
            builder.set_audience(audience.first().toStdString());
        } else {
            builder.set_audience(audiences);
        }
        return QString::fromStdString(
            builder
                .set_key_id(kid.toStdString())
                .set_type("JWT")
                .set_issuer("https://own.example")
                .set_subject("ada")
                .set_issued_at(now)
                .set_expires_at(now + expiresIn)
                .set_payload_claim("nonce", jwt::claim(nonce.toStdString()))
                .sign(jwt::algorithm::rs256{"", m_privatePem, "", ""}));
    }

private:
    QByteArray modulus() const { return parameter(OSSL_PKEY_PARAM_RSA_N); }

    QByteArray parameter(const char *name) const
    {
        std::error_code ec;
        const auto key{jwt::helper::load_private_key_from_string(m_privatePem, "", ec)};
        if (ec) {
            return QByteArray{};
        }
        BIGNUM *number{nullptr};
        if (EVP_PKEY_get_bn_param(key.get(), name, &number) != 1) {
            return QByteArray{};
        }
        QByteArray bytes(BN_num_bytes(number), Qt::Uninitialized);
        BN_bn2bin(number, reinterpret_cast<unsigned char *>(bytes.data()));
        BN_free(number);
        return bytes.toBase64(QByteArray::Base64UrlEncoding
                              | QByteArray::OmitTrailingEquals);
    }

    std::string m_privatePem;
};

/// A loopback endpoint that answers every request with the same raw bytes and then says
/// nothing more, so a test can serve an answer no well-behaved server would: a length far
/// past a ceiling, followed by a stall.
class RawAnswerHost : public QTcpServer
{
public:
    explicit RawAnswerHost(QByteArray answer)
        : m_answer{std::move(answer)}
    {
        listen(QHostAddress::LocalHost, 0);
    }

    QUrl url(const QString &path) const
    {
        return QUrl{QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path)};
    }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(descriptor)) {
            delete socket;
            return;
        }
        const QByteArray answer{m_answer};
        connect(socket, &QTcpSocket::readyRead, socket, [socket, answer]() {
            socket->readAll();
            socket->write(answer);
            socket->flush();
        });
    }

private:
    QByteArray m_answer;
};

/// A whole HTTP answer carrying \a body as JSON, for a RawAnswerHost.
QByteArray jsonAnswer(const QByteArray &body)
{
    return QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                             "Content-Length: ")
           + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
}

/// One web edge, on a thread with a quarter of a megabyte of stack.
///
/// What the nesting below spends is stack, and how much a level of it costs is decided by
/// the compiler. About 3.5 KB with GCC on Linux, several times that with MSVC. So the same
/// sixty-four levels that fit comfortably in the eight megabytes Linux and macOS give the
/// main thread overflowed the one megabyte Windows gives it, and the first Windows run of
/// this test is where that showed up: as a stack overflow, in CI, on a ceiling written to
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
        // Created here rather than handed in, so the whole request path (routing, the
        // callback handler and the nested exchange loops under it) runs on this stack
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
        // The vocabulary the hook's Scope members were generated from. Required
        // beside the hook, not optional. The edge resolves the answer as an index into
        // this list, so an edge that has the hook and not the list refuses every login.
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
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

    /// The verifier's answer, waited for.
    ///
    /// The verifier itself offers no form that waits, because its caller is a connect
    /// point slot, and a slot that waits holds its entity's event loop. A test is the one
    /// place where waiting is the right thing to do, so the wait is composed here out of
    /// the same asynchronous call production makes, rather than kept in the class where
    /// production could reach it.
    static QVariantMap verified(JwksVerifier *verifier, const QString &idToken,
                                const IdentityProviderConfig &provider,
                                const QString &expectedNonce, QString *error)
    {
        QVariantMap claims;
        QString failure;
        bool answered{false};
        QEventLoop loop;
        verifier->verifyAsync(idToken, provider, expectedNonce,
                              [&](const QVariantMap &result, const QString &why) {
            claims = result;
            failure = why;
            answered = true;
            loop.quit();
        });
        if (!answered) {
            loop.exec();
        }
        if (error != nullptr) {
            *error = failure;
        }
        return claims;
    }

    /// One whole login on `backend`, from begin to a finished exchange, answering with the
    /// state key its tokens are stored under. False if any step of it did not work.
    bool exchangeOn(OAuthBackend *backend, QString *tokenKey)
    {
        const OAuthBackend::BeginResult begun{
            backend->begin(QStringLiteral("stub"), edgeUrl(QStringLiteral("/auth/callback")))};
        if (begun.state.isEmpty()) {
            return false;
        }
        const Response redirected{get(begun.authorizeUrl)};
        if (redirected.status != 302) {
            return false;
        }
        const QString code{QUrlQuery{QUrl{redirected.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        const OAuthBackend::ExchangeResult result{
            backend->exchange(begun.state, code,
                              edgeUrl(QStringLiteral("/auth/callback")), QString{})};
        *tokenKey = result.tokenKey;
        return result.error.isEmpty() && !result.tokenKey.isEmpty();
    }

    /// One whole login on `backend` through the provider named \a provider, answering with
    /// what the exchange produced: the identity, or the reason there is none.
    OAuthBackend::ExchangeResult loginThrough(OAuthBackend *backend, const QString &provider)
    {
        const OAuthBackend::BeginResult begun{
            backend->begin(provider, edgeUrl(QStringLiteral("/auth/callback")))};
        OAuthBackend::ExchangeResult failed;
        if (begun.state.isEmpty()) {
            failed.error = QStringLiteral("the login did not begin: %1").arg(begun.error);
            return failed;
        }
        const Response redirected{get(begun.authorizeUrl)};
        const QString code{QUrlQuery{QUrl{redirected.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        if (code.isEmpty()) {
            failed.error = QStringLiteral("the provider issued no code");
            return failed;
        }
        return backend->exchange(begun.state, code, edgeUrl(QStringLiteral("/auth/callback")),
                                 QString{});
    }

    /// One pending login on `service`, taken as far as a browser takes it. Begin, then the
    /// provider's /authorize, which redirects carrying the code.
    ///
    /// Both halves are the engine's own, so the state, the PKCE verifier behind it and the
    /// code are the ones an exchange is checked against. A pair assembled here by
    /// hand would be refused before it reached anything worth measuring.
    bool beginAndAuthorize(IdentityService *service, QString *state, QString *code)
    {
        const QVariantMap begun{
            service->beginLogin(QStringLiteral("stub"),
                                edgeUrl(QStringLiteral("/auth/callback")))};
        *state = begun.value(QStringLiteral("state")).toString();
        const QString authorizeUrl{begun.value(QStringLiteral("authorizeUrl")).toString()};
        if (state->isEmpty() || authorizeUrl.isEmpty()) {
            return false;
        }
        const Response redirected{get(QUrl{authorizeUrl})};
        if (redirected.status != 302) {
            return false;
        }
        *code = QUrlQuery{QUrl{redirected.location}.query()}
                    .queryItemValue(QStringLiteral("code"));
        return !code->isEmpty();
    }

    /// One real ID token from the stub provider, for a nonce the test chooses.
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
        return urlFor(m_edgePort, path);
    }

    static QString urlFor(quint16 port, const QString &path)
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path);
    }

    // Run the whole browser round trip (login -> provider -> callback) and return the
    // callback response. Capture the authorization request query if asked.
    Response completeLogin(const QString &providerQuery, QUrlQuery *authQuery = nullptr)
    {
        return completeLoginOn(m_edgePort, providerQuery, authQuery);
    }

    // The same round trip against an edge other than the fixture's. A test that needs a
    // different mapping hook needs a different edge, because the hook is read once when the
    // provider is built. The cookie jar is shared and does not need separating, since the
    // login-state cookie is scoped to the host and port the second edge bound.
    Response completeLoginOn(quint16 port, const QString &providerQuery,
                             QUrlQuery *authQuery = nullptr)
    {
        const Response login{get(QUrl{urlFor(port, QStringLiteral("/auth/login") + providerQuery)})};
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
        // The vocabulary map.qml's Scope enum was generated from, in the same order,
        // because the hook's answer is resolved as an index into this list. A real project
        // gets both from scopes.order (maingen writes this line, scopegen writes the enum).
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
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

        // The control. This token, this JWKS, this nonce, and the claims come back.
        QString error;
        const QVariantMap claims{verified(&verifier, idToken, good, nonce, &error)};
        QVERIFY2(!claims.isEmpty(), qPrintable(error));
        QCOMPARE(claims.value(QStringLiteral("iss")).toString(), m_stub->baseUrl());
        QCOMPARE(claims.value(QStringLiteral("nonce")).toString(), nonce);

        const auto refuses{[&](const QString &token, const IdentityProviderConfig &provider,
                               const QString &expectedNonce, const QString &reason) {
            QString why;
            const QVariantMap result{verified(&verifier, token, provider, expectedNonce, &why)};
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

        // And nothing above quietly broke the verifier. The good token still verifies.
        error.clear();
        QVERIFY2(!verified(&verifier, idToken, good, nonce, &error).isEmpty(), qPrintable(error));
    }

    /// What an ID token is held to beyond its signature, on tokens this test signs itself
    /// so each claim can be wrong on its own. A token past its expiry is refused, one inside
    /// the minute of clock skew is not; an audience listed among others is this client's
    /// when it is in the list and nobody's when it is not; and a key the provider publishes
    /// as anything but RSA is not used to verify an RS256 signature.
    void anIdTokenIsHeldToItsExpiryItsAudienceAndAnRsaKey()
    {
        const TokenSigner signer;
        QVERIFY2(signer.isReady(), "the configure step issued no RSA key to sign with");
        JwksHost rsa{QJsonArray{signer.jwk(QStringLiteral("own"))}};
        JwksHost ec{QJsonArray{signer.jwk(QStringLiteral("own"), QStringLiteral("EC"))}};

        IdentityProviderConfig provider;
        provider.name = QStringLiteral("own");
        provider.clientId = QStringLiteral("own-client");
        provider.useIdToken = true;
        provider.jwksUrl = rsa.url();
        provider.issuer = QStringLiteral("https://own.example");
        const QString nonce{QStringLiteral("own-nonce")};
        const QStringList ours{QStringLiteral("own-client")};
        const std::chrono::seconds minute{60};

        QNetworkAccessManager network;
        JwksVerifier verifier{&network};
        QString error;
        const auto verifies{[&](const QString &token, const IdentityProviderConfig &config) {
            error.clear();
            return !verified(&verifier, token, config, nonce, &error).isEmpty();
        }};

        QVERIFY2(verifies(signer.sign(QStringLiteral("own"), ours, 5 * minute, nonce), provider),
                 qPrintable(error));
        QVERIFY2(verifies(signer.sign(QStringLiteral("own"), {QStringLiteral("another-client"), QStringLiteral("own-client")},
                                      5 * minute, nonce), provider),
                 qPrintable(error));
        QVERIFY(!verifies(signer.sign(QStringLiteral("own"), {QStringLiteral("another-client"), QStringLiteral("a-third")},
                                      5 * minute, nonce), provider));
        QCOMPARE(error, QStringLiteral("ID-token audience mismatch"));

        QVERIFY(!verifies(signer.sign(QStringLiteral("own"), ours, -60 * minute, nonce),
                          provider));
        QCOMPARE(error, QStringLiteral("ID token expired"));
        QVERIFY2(verifies(signer.sign(QStringLiteral("own"), ours, std::chrono::seconds{-30},
                                      nonce), provider),
                 qPrintable(error));

        IdentityProviderConfig ellipticKeys{provider};
        ellipticKeys.name = QStringLiteral("own-ec");
        ellipticKeys.jwksUrl = ec.url();
        QVERIFY(!verifies(signer.sign(QStringLiteral("own"), ours, 5 * minute, nonce),
                          ellipticKeys));
        QCOMPARE(error, QStringLiteral("the ID token's signing key is not RSA"));
    }

    /// The key set every ID token is trusted against is fetched only when it can be one. Over
    /// plaintext from anywhere but this machine it is not fetched at all; an answer too large
    /// to be a key set is refused as it arrives, well inside the fetch deadline, although the
    /// sender then stalls; and a key set with no keys is refused rather than cached, which
    /// would hold the refetch floor in front of the real one.
    void aKeySetIsFetchedOnlyWhenItCanBeOne()
    {
        const TokenSigner signer;
        QVERIFY2(signer.isReady(), "the configure step issued no RSA key to sign with");
        const QString nonce{QStringLiteral("fetch-nonce")};
        const QString token{signer.sign(QStringLiteral("own"), {QStringLiteral("own-client")},
                                        std::chrono::minutes{5}, nonce)};
        IdentityProviderConfig provider;
        provider.name = QStringLiteral("fetch");
        provider.clientId = QStringLiteral("own-client");
        provider.useIdToken = true;
        provider.issuer = QStringLiteral("https://own.example");

        QNetworkAccessManager network;
        JwksVerifier verifier{&network};
        QString error;

        provider.jwksUrl = QUrl{QStringLiteral("http://idp.example/jwks")};
        QVERIFY(verified(&verifier, token, provider, nonce, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("plaintext")), qPrintable(error));

        RawAnswerHost oversized{QByteArrayLiteral("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: application/json\r\n"
                                                  "Content-Length: 2097152\r\n\r\n{\"keys\":[")};
        provider.jwksUrl = oversized.url(QStringLiteral("/jwks"));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(verified(&verifier, token, provider, nonce, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("larger than a key set can be")),
                 qPrintable(error));
        QVERIFY2(clock.elapsed() < 3000, "refused by the deadline, not by its size");

        JwksHost empty{QJsonArray{}};
        provider.jwksUrl = empty.url();
        QVERIFY(verified(&verifier, token, provider, nonce, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("carried no keys")), qPrintable(error));
    }

    /// How an OAuth2 profile becomes the identity object, on the provider shape GitHub
    /// has: a numeric `id` for the subject, and a null `email` when the address is private,
    /// with the addresses on an endpoint of their own. The primary address is taken only
    /// when it is verified, because an unverified primary is one anybody could have typed;
    /// a profile with no subject, one that is not an object, and one too large to be a
    /// profile are refused, the last as it arrives rather than when the request times out;
    /// and a provider with no way to say who signed in cannot sign anyone in.
    void aProfileIsNormalizedOrTheLoginIsRefused()
    {
        const QByteArray privateEmail{
            R"({"id": 1729, "login": "ada", "name": "Ada Lovelace", "email": null})"};
        RawAnswerHost profile{jsonAnswer(privateEmail)};
        RawAnswerHost emails{jsonAnswer(
            R"([{"email": "old@example.org", "primary": false, "verified": true},)"
            R"( {"email": "ada@example.org", "primary": true, "verified": true}])")};
        RawAnswerHost unverifiedEmails{jsonAnswer(
            R"([{"email": "typed@example.org", "primary": true, "verified": false}])")};
        RawAnswerHost noSubject{jsonAnswer(R"({"login": "ada"})")};
        RawAnswerHost notAnObject{jsonAnswer(R"(["ada"])")};
        RawAnswerHost oversized{QByteArrayLiteral("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: application/json\r\n"
                                                  "Content-Length: 4194304\r\n\r\n{")};

        const auto through{[this](const QString &name, const QUrl &userinfo,
                                  const QUrl &emailsUrl) {
            IdentityProviderConfig provider{stubProvider(m_stub->baseUrl())};
            provider.name = name;
            provider.userinfoUrl = userinfo;
            provider.emailsUrl = emailsUrl;
            return provider;
        }};
        IdentityConfig config;
        config.enabled = true;
        config.allowDevStub = true;
        config.providers = {
            through(QStringLiteral("github"), profile.url(QStringLiteral("/user")),
                    emails.url(QStringLiteral("/user/emails"))),
            through(QStringLiteral("unverified"), profile.url(QStringLiteral("/user")),
                    unverifiedEmails.url(QStringLiteral("/user/emails"))),
            through(QStringLiteral("no-subject"), noSubject.url(QStringLiteral("/user")), {}),
            through(QStringLiteral("not-an-object"), notAnObject.url(QStringLiteral("/user")),
                    {}),
            through(QStringLiteral("oversized"), oversized.url(QStringLiteral("/user")), {}),
            through(QStringLiteral("no-userinfo"), {}, {}),
        };
        OAuthBackend backend{config};

        const OAuthBackend::ExchangeResult github{
            loginThrough(&backend, QStringLiteral("github"))};
        QVERIFY2(github.error.isEmpty(), qPrintable(github.error));
        QCOMPARE(github.identity.value(QStringLiteral("sub")).toString(), QStringLiteral("1729"));
        QCOMPARE(github.identity.value(QStringLiteral("login")).toString(), QStringLiteral("ada"));
        QCOMPARE(github.identity.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Ada Lovelace"));
        QCOMPARE(github.identity.value(QStringLiteral("email")).toString(),
                 QStringLiteral("ada@example.org"));

        const OAuthBackend::ExchangeResult unverified{
            loginThrough(&backend, QStringLiteral("unverified"))};
        QVERIFY2(unverified.error.isEmpty(), qPrintable(unverified.error));
        QVERIFY2(unverified.identity.contains(QStringLiteral("email"))
                     && unverified.identity.value(QStringLiteral("email")).isNull(),
                 "an unverified primary address is not the user's address");

        const auto refused{[&](const QString &name, const QString &reason) {
            const OAuthBackend::ExchangeResult result{loginThrough(&backend, name)};
            QVERIFY2(result.identity.isEmpty(), qPrintable(name + QStringLiteral(" signed in")));
            QVERIFY2(result.error.contains(reason),
                     qPrintable(QStringLiteral("%1: '%2', expected '%3'")
                                    .arg(name, result.error, reason)));
        }};
        refused(QStringLiteral("no-subject"), QStringLiteral("carried no 'id'"));
        refused(QStringLiteral("not-an-object"), QStringLiteral("was not an object"));
        QElapsedTimer clock;
        clock.start();
        refused(QStringLiteral("oversized"), QStringLiteral("larger than a provider's can be"));
        QVERIFY2(clock.elapsed() < 5000, "refused by the deadline, not by its size");
        refused(QStringLiteral("no-userinfo"), QStringLiteral("no userinfo endpoint"));
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

        // One key published. The token names none and there is only one it could be, so
        // selection succeeds and the token is refused on its signature instead.
        QString why;
        QVERIFY(verified(&verifier, noKid, soleKey, nonce, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("signature invalid")), qPrintable(why));

        // Two keys published. There is no telling which of them signed it, and guessing by
        // position is exactly what must not happen.
        why.clear();
        QVERIFY(verified(&verifier, noKid, rotating, nonce, &why).isEmpty());
        QVERIFY2(why.contains(QStringLiteral("cannot be told")), qPrintable(why));

        // And a token that does name its key still verifies against the rotating set, which
        // is the whole reason a provider publishes two.
        why.clear();
        QVERIFY2(!verified(&verifier, idToken, rotating, nonce, &why).isEmpty(), qPrintable(why));
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
    /// hold. Run against a small one it stops rather than crashes, which is the whole claim.
    void concurrentCallbacksAreBoundedWithIdentityInProcess()
    {
        // Above both ceilings in identityprovider.cpp, the count (kMaxConcurrentWaits,
        // 64) and the quarter of the thread's stack the nesting may spend, so some of
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

        // Fired without waiting for any of them. They have to be
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
    /// `exp` matters because a check that runs only when the claim is present makes a
    /// token with none a sign-in that never expires. A copy taken today would still
    /// open a session years from now. `sub` matters because everything downstream keys on
    /// it (the scope mapping reads it and a device credential is enrolled against it),
    /// so a token with none would sign the visitor in as the empty subject, and every visitor
    /// arriving that way would be the same one.
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
            QVERIFY2(verified(&verifier, token, against, nonce, &why).isEmpty(),
                     qPrintable(QStringLiteral("a token with no %1 verified").arg(claim)));
            QVERIFY2(why.contains(reason),
                     qPrintable(QStringLiteral("refused with '%1', expected '%2'")
                                    .arg(why, reason)));
        }
    }

    /// Another site must not be able to sign a visitor out by navigating them here.
    ///
    /// Logout is reached by a GET, because that is what `Session.logout()` does on both
    /// clients. The browser navigates to the route and the desktop client fetches it. That
    /// makes it a state change any page can cause, and the cookie is no defense:
    /// SameSite=Lax is sent on exactly this, a top-level navigation, and in `split_origin`
    /// the cookie is SameSite=None and is sent on everything. What it costs the visitor is
    /// not only the session. Signing out is also the one thing that ends a device
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

        // An elevation on one edge rotates the credential, and the browser goes on holding
        // the old one in a cookie no slot call can rewrite. The next page load may land on
        // any replica, so the hand-off from the old id to the new has to be known on every
        // edge. Without it edge B saw the old id removed, found no hand-off for it, and
        // minted a fresh anonymous session in its place, which signed a visitor out for
        // having been signed in on the other replica.
        const QByteArray rotated{edgeA->setScope(token, QStringLiteral("admin"))};
        QVERIFY(!rotated.isEmpty());
        QTRY_VERIFY(edgeB->isLive(rotated));
        QTRY_COMPARE(edgeB->rotationOf(token), rotated);
        QCOMPARE(edgeB->lookup(rotated)->scope, QStringLiteral("admin"));

        // Revocation on one edge propagates everywhere.
        edgeA->revoke(rotated);
        QTRY_VERIFY(!authStore.isLive(rotated));
        QTRY_VERIFY(!edgeB->isLive(rotated));
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

    // A provider that names no lifetime is a conforming provider: `expires_in` is
    // RECOMMENDED and not REQUIRED (RFC 6749 section 5.1). Keeping the expiry the refresh
    // replaced left the entry permanently inside the sweep's margin, so the next pass
    // refreshed it again, and the one after that, spending a refresh token against a third
    // party once per interval for the life of the session and getting nothing back.
    void aRefreshThatNamesNoLifetimeIsNotSweptAgain()
    {
        m_stub->setRefreshOmitsExpiry(true);
        const auto restore{qScopeGuard([this]() { m_stub->setRefreshOmitsExpiry(false); })};

        const Response callback{completeLogin(QStringLiteral("?provider=stub"))};
        QCOMPARE(callback.status, 302);
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());

        OAuthBackend *backend{m_edge->identityProvider()->backend()};
        QVERIFY(backend != nullptr);
        const auto accessToken{[&]() {
            return backend->tokens(QString::fromLatin1(token))
                .value(QStringLiteral("access_token")).toString();
        }};

        // Asserted on this entry rather than on what the sweep returns. A sweep walks every
        // session the backend holds, and the suite has signed in several by here.
        const QString issued{accessToken()};
        QVERIFY(!issued.isEmpty());

        // The exchange named 3600 seconds, so a wide margin makes this entry due once.
        backend->refreshExpiring(4000);
        const QString refreshed{accessToken()};
        QVERIFY2(refreshed != issued, "the entry was due and should have been refreshed");

        // And the answer named no lifetime, so there is nothing left for a timer to act on.
        backend->refreshExpiring(4000);
        QCOMPARE(accessToken(), refreshed);
        QVERIFY2(m_edge->sessionManager()->isLive(token),
                 "leaving a token alone must not disturb the session");
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

    // A connect point slot may not wait, and this is the one that did.
    //
    // The auth entity answers `exchangeCode` from its Identity Source. An exchange
    // that drives the provider round trip (the /token POST, then /userinfo)
    // in a nested event loop and returns when it has an answer looks healthy, because a
    // nested loop keeps serving, but it puts every later
    // caller under the earlier one on the stack, and none of them can be answered until
    // the slowest one below them is. Two people signing in at once is not a rare event,
    // and the second one's provider being slow would hold the first one's sign-in hostage.
    //
    // Measured as ordering, which is what makes it a fact about the entity rather than
    // about a timer. The first login's provider answers in 150 ms and the second's in
    // 800, so the first login must finish first. Nested, it cannot. Its own event loop
    // has the second one's on top of it and cannot return until that one has.
    //
    // The same wait is why an edge dropping mid-login could take the entity down with it;
    // the case below this one is that half.
    void aSlowLoginDoesNotHoldUpTheOneBehindIt()
    {
        IdentityConfig authConfig;
        authConfig.enabled = true;
        authConfig.allowDevStub = true;
        authConfig.providers = {stubProvider(m_stub->baseUrl())};
        IdentityService service{authConfig};

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("IdentityEngine"), &service);
        QQmlComponent component{&engine,
                                QUrl::fromLocalFile(QStringLiteral(M8_SRCDIR "/auth/Identity.qml"))};
        QScopedPointer<QObject> source{component.create()};
        QVERIFY2(!source.isNull(), qPrintable(component.errorString()));

        QString firstState;
        QString firstCode;
        QVERIFY(beginAndAuthorize(&service, &firstState, &firstCode));
        QString secondState;
        QString secondCode;
        QVERIFY(beginAndAuthorize(&service, &secondState, &secondCode));

        QSignalSpy answered{source.data(),
                            SIGNAL(exchangeResult(QString, QString, QString, QString))};

        // The second login is started from inside the first, which is the only way it can
        // be. A real one arrives as a QtRemoteObjects packet the entity reads while the
        // first slot is still running, and a nested loop is precisely what reads it.
        QTimer::singleShot(50, source.data(), [this, &source, secondState, secondCode]() {
            m_stub->setTokenDelayMs(800);
            QMetaObject::invokeMethod(source.data(), "exchangeCode", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("second")),
                                      Q_ARG(QString, secondState),
                                      Q_ARG(QString, secondCode),
                                      Q_ARG(QString, edgeUrl(QStringLiteral("/auth/callback"))),
                                      Q_ARG(QString, QString{}));
        });
        m_stub->setTokenDelayMs(150);
        QMetaObject::invokeMethod(source.data(), "exchangeCode", Qt::DirectConnection,
                                  Q_ARG(QString, QStringLiteral("first")),
                                  Q_ARG(QString, firstState),
                                  Q_ARG(QString, firstCode),
                                  Q_ARG(QString, edgeUrl(QStringLiteral("/auth/callback"))),
                                  Q_ARG(QString, QString{}));

        QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 2, 20000);
        m_stub->setTokenDelayMs(0);
        QCOMPARE(answered.at(0).at(0).toString(), QStringLiteral("first"));
        QCOMPARE(answered.at(1).at(0).toString(), QStringLiteral("second"));
        // And both signed in. An ordering that came from two failures would prove
        // nothing about either.
        for (int index{0}; index < 2; ++index) {
            QVERIFY2(answered.at(index).at(3).toString().isEmpty(),
                     qPrintable(answered.at(index).at(3).toString()));
            QVERIFY(!answered.at(index).at(1).toString().isEmpty());
        }
    }

    // The other half of the same wait. What the entity was standing on while it waited.
    //
    // A slot runs on a Source, and that Source belongs to the link the call arrived on.
    // While the exchange waited in its nested loop the entity kept serving events, so an
    // edge that went away in the meantime was noticed right there: the socket's
    // `disconnected` ran, the link was deleted, and `releasePeerSource` deleted the very
    // Source whose slot was on the stack. The wait then returned into a destroyed object
    // and into QtRemoteObjects' own bookkeeping for a connection that no longer existed.
    // A login takes a provider round trip, an edge restart or a network blip is ordinary,
    // so this needed no attacker at all.
    //
    // A normal build may or may not fall over on that. The tree the leak job configures
    // with AddressSanitizer reports it as a heap-use-after-free either way. What is
    // asserted here is what can be asserted in every build. The entity is still there and
    // still answers the next caller.
    void anEdgeThatDropsMidLoginLeavesTheAuthEntityServing()
    {
        IdentityConfig authConfig;
        authConfig.enabled = true;
        authConfig.allowDevStub = true;
        authConfig.providers = {stubProvider(m_stub->baseUrl())};
        IdentityService service{authConfig};

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

        QQmlEngine authEngine;
        ConnectPointHost host{point, credsFor(QStringLiteral("auth")), &authEngine};
        host.setContextObject(QStringLiteral("IdentityEngine"), &service);
        QVERIFY2(host.start(), qPrintable(host.errorString()));
        const quint16 authPort{host.serverPort()};

        QString state;
        QString code;
        QVERIFY(beginAndAuthorize(&service, &state, &code));

        {
            QRemoteObjectNode node;
            MeshClient client;
            connect(&client, &MeshClient::connected, &node, [&node](QIODevice *device) {
                node.addClientSideConnection(device);
            });
            client.connectMutualTls(QHostAddress::LocalHost, authPort, QStringLiteral("auth"),
                loadCertificate(QStringLiteral(M8_CERT_DIR "/ca.crt")),
                loadCertificate(QStringLiteral(M8_CERT_DIR "/web.crt")),
                loadPrivateKey(QStringLiteral(M8_CERT_DIR "/web.key")));
            QScopedPointer<QRemoteObjectDynamicReplica> replica{
                node.acquireDynamic(QStringLiteral("identity"))};
            QVERIFY2(replica->waitForSource(8000), "the auth entity never came up");

            // Long enough that the link below is certainly gone before the provider
            // answers, so the slot is standing on a Source nobody owns any more.
            m_stub->setTokenDelayMs(1500);
            QMetaObject::invokeMethod(replica.data(), "exchangeCode",
                                      Q_ARG(QString, QStringLiteral("dropped")),
                                      Q_ARG(QString, state), Q_ARG(QString, code),
                                      Q_ARG(QString, edgeUrl(QStringLiteral("/auth/callback"))),
                                      Q_ARG(QString, QString{}));
            QTest::qWait(300);   // the call is on its way to the provider
        }
        // The edge is gone, mid-login. Past the provider's answer, and then some.
        QTest::qWait(2000);
        m_stub->setTokenDelayMs(0);

        // Still serving: a second edge connects, acquires, and is answered.
        QString nextState;
        QString nextCode;
        QVERIFY(beginAndAuthorize(&service, &nextState, &nextCode));

        QRemoteObjectNode node;
        MeshClient client;
        connect(&client, &MeshClient::connected, &node, [&node](QIODevice *device) {
            node.addClientSideConnection(device);
        });
        client.connectMutualTls(QHostAddress::LocalHost, authPort, QStringLiteral("auth"),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(M8_CERT_DIR "/web.crt")),
            loadPrivateKey(QStringLiteral(M8_CERT_DIR "/web.key")));
        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("identity"))};
        QVERIFY2(replica->waitForSource(8000),
                 "the auth entity stopped serving after an edge dropped mid-login");

        QSignalSpy answered{replica.data(),
                            SIGNAL(exchangeResult(QString, QString, QString, QString))};
        QMetaObject::invokeMethod(replica.data(), "exchangeCode",
                                  Q_ARG(QString, QStringLiteral("after")),
                                  Q_ARG(QString, nextState), Q_ARG(QString, nextCode),
                                  Q_ARG(QString, edgeUrl(QStringLiteral("/auth/callback"))),
                                  Q_ARG(QString, QString{}));
        QTRY_COMPARE_WITH_TIMEOUT(answered.count(), 1, 20000);
        QCOMPARE(answered.at(0).at(0).toString(), QStringLiteral("after"));
        QVERIFY2(answered.at(0).at(3).toString().isEmpty(),
                 qPrintable(answered.at(0).at(3).toString()));
    }

    // A login that got as far as the provider and then had nobody to hand the session to.
    //
    // The exchange stores what the provider issued under the login's state key and waits
    // for the caller to bind a session to it, which is normally the very next thing that
    // happens. When it is not, because the edge that asked went away in between (its link
    // dropped, or the process restarted), an entry kept for the life of the
    // process is an access token and a refresh token belonging to somebody who was never
    // signed in, and with the refresh sweep on, a refresh token spent against the provider
    // once an interval, forever, on their behalf.
    //
    // A session bound to it is the other half, and it must not be swept. That one is a
    // signed-in visitor, and it lives and dies with their session.
    void aLoginNoSessionWasBoundToDoesNotKeepItsTokens()
    {
        IdentityConfig config;
        config.enabled = true;
        config.allowDevStub = true;
        config.providers = {stubProvider(m_stub->baseUrl())};
        OAuthBackend backend{config};
        // Nothing may go unclaimed, so one sweep decides. The window is what an operator
        // sets. Zero is the end of the range and is what makes this observable in a test
        // rather than in five minutes.
        backend.setUnclaimedWindow(0);

        QString claimed;
        QVERIFY(exchangeOn(&backend, &claimed));
        QCOMPARE(backend.heldTokenCount(), 1);
        // Claimed by a session, the way the callback claims one.
        backend.rekeyTokens(claimed, QStringLiteral("session-1"));

        QString abandoned;
        QVERIFY(exchangeOn(&backend, &abandoned));
        QCOMPARE(backend.heldTokenCount(), 2);

        // One sweep later. The login nobody bound a session to is gone, and the one
        // somebody did is still there.
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral("no session was bound to")});
        QTRY_COMPARE_WITH_TIMEOUT(backend.heldTokenCount(), 1, 5000);
        QVERIFY(!backend.tokens(QStringLiteral("session-1")).isEmpty());
        QVERIFY(backend.tokens(abandoned).isEmpty());
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
        // The vocabulary the hook's Scope members were generated from. Required
        // beside the hook, not optional. The edge resolves the answer as an index into
        // this list, so an edge that has the hook and not the list refuses every login.
        edgeConfig.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                                 QStringLiteral("moderator"), QStringLiteral("admin")};
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
    // The first of these three is the one that matters. Against the old code the other two
    // pass for the wrong reason. The edge refused every cross-replica callback outright, so
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
        // Both halves, because the second is the surprising one: the
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

    /// With more than one person configured, /authorize asks which, and signs in the one
    /// that was picked.
    ///
    /// The reason to configure a second dev user is to reach a second scope, and a scope
    /// is what the mapping hook returns for an identity. So the whole feature comes down
    /// to this. The identity the edge ends up holding is the one the browser chose, and
    /// not the first one in the list.
    void theDevSignInSignsInWhoeverWasPicked()
    {
        StubIdentityServer stub{StubIdentityServer::DevOnly{}};
        stub.setClientCredentials(QStringLiteral("stub-client"), QStringLiteral("stub-secret"));
        QVariantMap first;
        first.insert(QStringLiteral("sub"), QStringLiteral("ada"));
        first.insert(QStringLiteral("login"), QStringLiteral("ada"));
        first.insert(QStringLiteral("name"), QStringLiteral("Ada"));
        first.insert(QStringLiteral("email"), QStringLiteral("ada@localhost"));
        QVariantMap second;
        second.insert(QStringLiteral("sub"), QStringLiteral("grace"));
        second.insert(QStringLiteral("login"), QStringLiteral("grace"));
        second.insert(QStringLiteral("name"), QStringLiteral("Grace"));
        second.insert(QStringLiteral("email"), QStringLiteral("grace@localhost"));
        stub.setUser(first);
        stub.addUser(second);
        QVERIFY(stub.start());
        stub.setIssuer(stub.baseUrl());
        QCOMPARE(stub.userCount(), 2);

        QUrl authorize{stub.baseUrl() + QStringLiteral("/authorize")};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client_id"), QStringLiteral("stub-client"));
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("redirect_uri"),
                           edgeUrl(QStringLiteral("/auth/callback")));
        authorize.setQuery(query);

        // No choice on the request, so it is a page and not a redirect: with two people
        // configured the stub cannot know which one you are, and guessing would make the
        // second entry decoration.
        const Response asked{get(authorize)};
        QCOMPARE(asked.status, 200);
        QVERIFY(asked.body.contains("Ada"));
        QVERIFY(asked.body.contains("Grace"));

        // Picking the second one is a redirect back with a code, and the identity that
        // code buys is that person's.
        QUrl picked{authorize};
        QUrlQuery chosen{query};
        chosen.addQueryItem(QStringLiteral("synqt_user"), QStringLiteral("1"));
        picked.setQuery(chosen);
        const Response redirected{get(picked)};
        QCOMPARE(redirected.status, 302);
        const QString code{QUrlQuery{QUrl{redirected.location}.query()}
                               .queryItemValue(QStringLiteral("code"))};
        QVERIFY(!code.isEmpty());

        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("client_id"), QStringLiteral("stub-client"));
        form.addQueryItem(QStringLiteral("client_secret"), QStringLiteral("stub-secret"));
        QNetworkRequest request{QUrl{stub.baseUrl() + QStringLiteral("/token")}};
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        QNetworkReply *reply{m_browser.post(request,
                                            form.toString(QUrl::FullyEncoded).toUtf8())};
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QJsonObject tokens{QJsonDocument::fromJson(reply->readAll()).object()};
        reply->deleteLater();

        const QString access{tokens.value(QStringLiteral("access_token")).toString()};
        QVERIFY(!access.isEmpty());

        // Both halves of what a provider answers, because a project may read either.
        QNetworkRequest profile{QUrl{stub.baseUrl() + QStringLiteral("/userinfo")}};
        profile.setRawHeader("Authorization", ("Bearer " + access).toUtf8());
        QNetworkReply *userinfo{m_browser.get(profile)};
        QEventLoop second_loop;
        connect(userinfo, &QNetworkReply::finished, &second_loop, &QEventLoop::quit);
        second_loop.exec();
        const QJsonObject who{QJsonDocument::fromJson(userinfo->readAll()).object()};
        userinfo->deleteLater();
        QCOMPARE(who.value(QStringLiteral("sub")).toString(), QStringLiteral("grace"));

        const QString idToken{tokens.value(QStringLiteral("id_token")).toString()};
        QVERIFY(!idToken.isEmpty());
        const QJsonObject claims{
            QJsonDocument::fromJson(
                QByteArray::fromBase64(idToken.split(QLatin1Char('.')).at(1).toUtf8(),
                                       QByteArray::Base64UrlEncoding))
                .object()};
        QCOMPARE(claims.value(QStringLiteral("sub")).toString(), QStringLiteral("grace"));
        QCOMPARE(claims.value(QStringLiteral("email")).toString(),
                 QStringLiteral("grace@localhost"));
    }

    /// One person configured, and /authorize does not ask.
    void oneDevUserIsSignedInWithoutBeingAsked()
    {
        StubIdentityServer stub{StubIdentityServer::DevOnly{}};
        stub.setClientCredentials(QStringLiteral("stub-client"), QStringLiteral("stub-secret"));
        QVERIFY(stub.start());
        QCOMPARE(stub.userCount(), 1);

        QUrl authorize{stub.baseUrl() + QStringLiteral("/authorize")};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client_id"), QStringLiteral("stub-client"));
        query.addQueryItem(QStringLiteral("redirect_uri"),
                           edgeUrl(QStringLiteral("/auth/callback")));
        authorize.setQuery(query);
        QCOMPARE(get(authorize).status, 302);
    }

    /// One nonce in the authorization request, not two.
    ///
    /// Qt's own `NonceMode::Automatic` already puts a nonce in the request whenever the
    /// scope contains `openid`, so a second one added beside it went into the same
    /// multi-map and the request carried the parameter twice with two different values.
    /// RFC 6749 section 3.1 says a parameter MUST NOT appear more than once: a provider
    /// that enforces it answers invalid_request and nobody signs in, and one that does
    /// not picks whichever value it reads first, which decides by coin toss whether the
    /// ID token's nonce matches the one the edge recorded.
    void anAuthorizationRequestCarriesOneNonce()
    {
        const Response login{
            get(QUrl{edgeUrl(QStringLiteral("/auth/login?provider=stub-oidc"))})};
        QCOMPARE(login.status, 302);

        const QUrlQuery query{QUrl{login.location}.query()};
        const QStringList nonces{query.allQueryItemValues(QStringLiteral("nonce"))};
        QCOMPARE(nonces.size(), 1);
        QVERIFY(!nonces.first().isEmpty());

        // And the one that is there is the one the framework minted, because that is the
        // value the returned ID token is checked against.
        QCOMPARE(nonces.first().size(), 64);  // randomToken(): 32 bytes as hex
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
        // A hook that does not compile silently means "no mapping", and no mapping now means
        // every login is refused rather than every session getting the default scope. Either
        // way it is a permissions change, so it has to be said out loud rather than left to
        // a stray Qt warning. The edge still starts. An edge that refuses logins and serves
        // everything else beats an edge that is down, and the warning names the file.
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

    // An edge exactly like the fixture's, but reading the named mapping hook, so a test can
    // ask what a different hook does to a real login. Returned by pointer because WebEdge is
    // not movable and the caller needs it alive for the round trip.
    std::unique_ptr<WebEdge> edgeWithHook(QQmlEngine *engine, const QString &hook)
    {
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M8_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.identity.enabled = true;
        config.identity.allowDevStub = true;
        config.identity.mappingHook = hook;
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        config.identity.providers = {stubProvider(m_stub->baseUrl())};
        auto edge = std::make_unique<WebEdge>(config, engine);
        return edge;
    }

    void aHookAnswerOutsideTheVocabularyFailsTheLoginClosed()
    {
        // The vocabulary has four scopes, so 4 is one past the end. Before the answer was an
        // index into a declared list, mapScope turned whatever the hook returned into a
        // string and used it, so a hook out of step with scopes.order minted a session
        // holding a scope no check could satisfy. The visitor was locked out of everything
        // and nothing anywhere reported why.
        QQmlEngine engine;
        std::unique_ptr<WebEdge> edge{
            edgeWithHook(&engine, QStringLiteral(M8_SRCDIR "/web/identity/outofrange.qml"))};
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));

        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral(
                                 "refusing a login the identity mapping hook could not place"
                                 ".*returned 4")});
        const Response callback{completeLoginOn(edge->serverPort(), QString{})};
        QCOMPARE(callback.status, 302);
        QVERIFY2(sessionToken(callback.setCookie).isEmpty(),
                 "a login the hook could not place must set no session cookie");
    }

    // A refused login must leave nothing behind, and it left the tokens. The exchange
    // stores the provider's access, refresh and ID tokens under the state key before the
    // hook is asked, and a refusal returned without releasing them. One entry per refused
    // attempt for the life of the process, each with a refresh token the sweep went on
    // spending against the provider on behalf of a visitor who was never signed in. Any
    // signed-in account the hook does not place could grow it, one callback at a time.
    void aLoginTheHookRefusesLeavesNoTokensBehind()
    {
        QQmlEngine engine;
        std::unique_ptr<WebEdge> edge{
            edgeWithHook(&engine, QStringLiteral(M8_SRCDIR "/web/identity/outofrange.qml"))};
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));
        QVERIFY(edge->identityProvider()->backend() != nullptr);

        for (int attempt{0}; attempt < 3; ++attempt) {
            QTest::ignoreMessage(QtWarningMsg,
                                 QRegularExpression{QStringLiteral(
                                     "refusing a login the identity mapping hook could not "
                                     "place")});
            const Response callback{completeLoginOn(edge->serverPort(), QString{})};
            QCOMPARE(callback.status, 302);
            QVERIFY(sessionToken(callback.setCookie).isEmpty());
        }
        QCOMPARE(edge->identityProvider()->backend()->heldTokenCount(), 0);
    }

    void aHookThatDoesNotAnswerFailsTheLoginClosed()
    {
        // A `return QStringLiteral("user")` fallback here would let a hook that fails
        // outright hand out an authenticated scope to everybody who signs in.
        QQmlEngine engine;
        std::unique_ptr<WebEdge> edge{
            edgeWithHook(&engine, QStringLiteral(M8_SRCDIR "/web/identity/noanswer.qml"))};
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));

        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral(
                                 "refusing a login the identity mapping hook could not place"
                                 ".*no scopeFor")});
        const Response callback{completeLoginOn(edge->serverPort(), QString{})};
        QCOMPARE(callback.status, 302);
        QVERIFY2(sessionToken(callback.setCookie).isEmpty(),
                 "a hook with no scopeFor must sign nobody in");
    }

    void aHookAnswerInsideTheVocabularyResolvesByIndex()
    {
        // Not padding. A gate tested only by refusals passes when it refuses everything,
        // which is how identity.required refused everybody in this tree for months. This is
        // the case that proves the two above are a bounds check and not an outage: the same
        // edge, a hook that answers in range, and a session that holds that scope.
        QQmlEngine engine;
        std::unique_ptr<WebEdge> edge{
            edgeWithHook(&engine, QStringLiteral(M8_SRCDIR "/web/identity/map.qml"))};
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));

        const Response callback{completeLoginOn(edge->serverPort(), QString{})};
        QCOMPARE(callback.status, 302);
        const QByteArray token{sessionToken(callback.setCookie)};
        QVERIFY(!token.isEmpty());
        const SessionRecord *record{edge->sessionManager()->lookup(token)};
        QVERIFY(record != nullptr);
        // Scope.Moderator is 2, and scopeOrder[2] is "moderator".
        QCOMPARE(record->scope, QStringLiteral("moderator"));
    }
};

QTEST_MAIN(TestM8)
#include "tst_m8.moc"
