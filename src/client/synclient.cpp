// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "synclient.h"

#include "browserhistory.h"
#include "clientupdate.h"
#include "consumerbase.h"
#include "deletesoon.h"
#include "promise.h"
#include "qmlpalette.h"
#include "remotepageloader.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "websockettransport.h"

#include <QJSEngine>
#include <QJSValue>
#include <QJSValueList>
#include <QMetaObject>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>
#include <QWebSocket>

#ifndef Q_OS_WASM
#  include "loopbackreceiver.h"

#  include <QCryptographicHash>
#  include <QDesktopServices>
#  include <QJsonDocument>
#  include <QJsonObject>
#  include <QNetworkAccessManager>
#  include <QNetworkCookie>
#  include <QNetworkCookieJar>
#  include <QNetworkReply>
#  include <QNetworkRequest>
#  include <QRandomGenerator>
#  include <QSslCertificate>
#  include <QSslConfiguration>
#  include <QSslSocket>
#endif

#include <algorithm>
#include <utility>

namespace SynQt {

namespace {

/// Promise::then()/catchError() are the QML-facing API ("slot(args).then(value =>
/// ...)"). Both only accept a callable QJSValue, because a consumer facade's returning
/// slot is meant to settle into a QML callback. SynClient's own Pages wiring needs the
/// same reply from plain C++, so this bridges the two. A throwaway QObject exposes the
/// two calls the reply can drive, and the app's own QML engine wraps each into a JS
/// closure (there being no C++-native way to construct a callable QJSValue). It has no
/// parent, so once the settled Promise (or its chained rejection handler) drops its
/// handler list, the engine's garbage collector is free to reclaim it like any other
/// unreachable JS-owned QObject.
class PageReplyBridge : public QObject
{
    Q_OBJECT

public:
    PageReplyBridge(Router *router, QString route)
        : m_router{router}
        , m_route{std::move(route)}
    {
    }

    Q_INVOKABLE void deliver(const QVariant &value)
    {
        const QVariantMap fields{value.toMap()};
        m_router->onPageDelivered(m_route, fields.value(QStringLiteral("qml")).toString(),
                                  fields.value(QStringLiteral("hash")).toString(),
                                  fields.value(QStringLiteral("seed")).toString(),
                                  fields.value(QStringLiteral("status")).toString());
    }

    /// A rejected promise (the connect point not yet live, or the remote call itself
    /// failing) must still resolve the route, or it is left in Loading forever.
    Q_INVOKABLE void fail(const QVariant &reason)
    {
        Q_UNUSED(reason);
        m_router->onPageDelivered(m_route, QString{}, QString{}, QString{},
                                  QStringLiteral("error"));
    }

private:
    Router *m_router;
    QString m_route;
};

} // namespace

#ifndef Q_OS_WASM
namespace {

// The native client verifies the edge's certificate: VerifyPeer against the OS trust
// store (and the hostname), plus any pinned/self-hosted certificate from config. It
// never disables verification.
/// The session this client is holding for `origin`, in the form the handshake wants
/// ("name=value; name=value"), read out of the network manager's own cookie jar.
///
/// The jar rather than the reply's Set-Cookie header. Two ordinary things break a read of
/// the header. The edge withholds Set-Cookie from a request that
/// already presents a live session, and a redirect is followed by default, so the response
/// that carries the cookie is not the response this code sees. Either way the header is
/// empty, the credential is overwritten with nothing, and the wss handshake is refused
/// for having no session while the jar holds a good one. The jar is where the answer is,
/// and reading it is also exactly what a browser does.
QByteArray heldCredential(QNetworkAccessManager *network, const QUrl &origin)
{
    if (!network || !network->cookieJar()) {
        return QByteArray{};
    }
    QList<QByteArray> pairs;
    const QList<QNetworkCookie> held{network->cookieJar()->cookiesForUrl(origin)};
    for (const QNetworkCookie &cookie : held) {
        pairs.append(cookie.name() + '=' + cookie.value());
    }
    return pairs.join("; ");
}

QSslConfiguration nativeTlsConfiguration(const SynClientConfig &config)
{
    QSslConfiguration tls{QSslConfiguration::defaultConfiguration()};
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (!config.pinnedCaCertPath.isEmpty()) {
        tls.setCaCertificates(tls.caCertificates()
                              + QSslCertificate::fromPath(config.pinnedCaCertPath));
    }
    return tls;
}

/// A cryptographically random opaque value, hex-encoded. The system generator, not the
/// default one: these are the two values a desktop sign-in rests on, and a predictable
/// nonce is a sign-in somebody else can finish.
QByteArray randomToken()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / static_cast<int>(sizeof(quint32)));
    return raw.toHex();
}

/// The two desktop routes hang off the login route, exactly as the edge hangs them off it
/// (IdentityProvider::claimRoute and deviceRoute), so a project that renamed its login route
/// renamed these with it.
QString desktopRoute(const QString &loginRoute, const QString &leaf)
{
    QString route{loginRoute};
    while (route.endsWith(QLatin1Char('/'))) {
        route.chop(1);
    }
    return route + leaf;
}

/// The S256 challenge for a verifier, in the form the edge registers it.
QByteArray challengeFor(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

/// Length-constant comparison, so a mismatch does not leak position via timing.
///
/// It matters here and not only on the edge: any local process can connect to the loopback
/// port and offer this client a code of its own, and the nonce is the only thing that
/// refuses it. A comparison that gives up at the first wrong byte is a nonce that can be
/// walked one byte at a time by a process that is already on the machine.
bool constantTimeEquals(const QByteArray &lhs, const QByteArray &rhs)
{
    if (lhs.isEmpty() || lhs.size() != rhs.size()) {
        return false;
    }
    quint8 difference{0};
    for (qsizetype i{0}; i < lhs.size(); ++i) {
        difference |= static_cast<quint8>(lhs.at(i)) ^ static_cast<quint8>(rhs.at(i));
    }
    return difference == 0;
}

} // namespace
#endif

SynClient::SynClient(SynClientConfig config, QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_server{new ServerAccessor{m_config.connectPoints, this}}
    , m_session{new Session{m_config, this}}
    , m_router{new Router{m_config, m_session, engine, this}}
    , m_update{new ClientUpdate{this}}
    , m_engine{engine}
    , m_reconnectTimer{new QTimer{this}}
    , m_backoffMs{m_config.reconnectBaseMs}
{
    m_reconnectTimer->setSingleShot(true);
    // Reconnect through start() so a native client re-bootstraps its session (the edge
    // may have restarted); on WASM start() just reconnects (the browser holds the cookie).
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() { start(); });

    // The two actions Session offers QML. Session reports them and this answers them,
    // because ending a session is a thing done to the edge over the network and Session
    // holds no network. Nothing was connected to either for a long time, so
    // `Session.logout()` reset the client's own idea of who it was and left the session
    // and its cookie alive at the edge. The next page load signed the visitor back in.
    connect(m_session, &Session::loginRequested, this, &SynClient::beginLogin);
    connect(m_session, &Session::logoutRequested, this, &SynClient::endSession);

    // An empty palette means the app uses no remote pages. Give it no loader, so
    // resolveRemote() falls through to Error rather than silently going Loading forever.
    if (!m_config.remotePalette.isEmpty()) {
        m_pageLoader = new RemotePageLoader{engine, QmlPalette{m_config.remotePalette}, this};
        m_router->setRemotePageLoader(m_pageLoader);
    }
}

SynClient::~SynClient()
{
    teardown();
}

QString SynClient::state() const
{
    return m_state;
}

ServerAccessor *SynClient::server() const
{
    return m_server;
}

Session *SynClient::session() const
{
    return m_session;
}

Router *SynClient::router() const
{
    return m_router;
}

ClientUpdate *SynClient::update() const
{
    return m_update;
}

QByteArray SynClient::edgeHttpOrigin() const
{
    QUrl origin{m_config.edgeUrl};
    origin.setScheme(origin.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                             : QStringLiteral("http"));
    origin.setPath(QString{});
    return origin.toString(QUrl::RemovePath).toUtf8();
}

void SynClient::beginLogin(const QString &provider)
{
    if (m_config.loginRoute.isEmpty()) {
        qWarning("SynQt: Session.login() was called and this project configures no "
                 "identity, so there is no login route to go to. Add an identity: block "
                 "with a provider (synqt add auth <provider>).");
        return;
    }
#ifdef Q_OS_WASM
    QUrl target{QString::fromUtf8(edgeHttpOrigin()) + m_config.loginRoute};
    if (!provider.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("provider"), provider);
        target.setQuery(query);
    }
    leaveForUrl(target.toString());
#else
    beginDesktopLogin(provider);
#endif
}

#ifndef Q_OS_WASM

void SynClient::beginDesktopLogin(const QString &provider)
{
    // A native window cannot navigate, so the sign-in happens in the system browser and
    // the answer comes back over a port this process holds for the length of it. What
    // comes back is a claim code. The URL the browser was sent to is in that browser's
    // history, and a session there would outlive the sign-in on a machine that may not be
    // this visitor's alone.
    if (m_loopback) {
        // Already waiting on one. Starting a second would take a second port and leave the
        // first listening, and the visitor already has a browser open on the first.
        qWarning("SynQt: a sign-in is already in progress.");
        return;
    }
    auto *loopback{new LoopbackReceiver{this}};
    if (!loopback->listen()) {
        delete loopback;
        qWarning("SynQt: could not take a loopback port for the sign-in, so there is "
                 "nowhere for the answer to come back to. Not opening a browser.");
        return;
    }
    m_loopback = loopback;
    m_loginState = randomToken();
    m_loginVerifier = randomToken();

    QUrl target{QString::fromUtf8(edgeHttpOrigin()) + m_config.loginRoute};
    QUrlQuery query;
    if (!provider.isEmpty()) {
        query.addQueryItem(QStringLiteral("provider"), provider);
    }
    query.addQueryItem(QStringLiteral("return"), loopback->returnUrl());
    query.addQueryItem(QStringLiteral("return_state"),
                       QString::fromLatin1(m_loginState));
    query.addQueryItem(QStringLiteral("return_challenge"),
                       QString::fromLatin1(challengeFor(m_loginVerifier)));
    target.setQuery(query);

    connect(loopback, &LoopbackReceiver::received, this, &SynClient::onLoginAnswer);
    connect(loopback, &LoopbackReceiver::timedOut, this, [this]() {
        qWarning("SynQt: the sign-in was not finished in time, so this client stopped "
                 "waiting for it.");
        endDesktopLogin();
    });
    QDesktopServices::openUrl(target);
}

void SynClient::onLoginAnswer(const QString &code, const QString &state, const QString &error)
{
    // Any local process can reach that port, so nothing that arrives on it is trusted for
    // being there. Only an answer carrying the nonce this client generated a moment ago
    // belongs to the sign-in it started. Without this check a process on the same machine
    // could hand over a code for an account it controls and have the visitor signed in as
    // somebody else, which is session fixation with extra steps.
    if (!constantTimeEquals(state.toUtf8(), m_loginState)) {
        qWarning("SynQt: an answer arrived on the sign-in port that this client did not "
                 "ask for. Refused; no session was claimed.");
        endDesktopLogin();
        return;
    }
    if (!error.isEmpty() || code.isEmpty()) {
        qWarning("SynQt: the sign-in did not complete (%s).",
                 error.isEmpty() ? "no code was returned" : qUtf8Printable(error));
        endDesktopLogin();
        return;
    }
    claimSession(code);
}

void SynClient::claimSession(const QString &code)
{
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QNetworkRequest request{QUrl{QString::fromUtf8(edgeHttpOrigin())
                                 + desktopRoute(m_config.loginRoute,
                                                QStringLiteral("/claim"))}};
    // This client's own verified connection to the edge, which is the whole reason the
    // loopback carried a code and not a session: the exchange happens here, over TLS this
    // process terminates, and not through a browser.
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));

    QUrlQuery body;
    body.addQueryItem(QStringLiteral("code"), code);
    body.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(m_loginVerifier));
    // Enrolment rides on the claim rather than on a route of its own, because this is the
    // one moment where the session has been proved to belong to this process. The
    // binding is what this machine's store gives, reported as it is. A deployment
    // that asked for more than this machine can give gets no credential, and the visitor is
    // still signed in.
    DeviceCredential *store{deviceStore()};
    if (store != nullptr && store->isAvailable()) {
        body.addQueryItem(QStringLiteral("device"), QStringLiteral("1"));
        body.addQueryItem(QStringLiteral("binding"), store->bindingName());
        body.addQueryItem(QStringLiteral("label"), DeviceCredential::machineLabel());
    }
    const QByteArray payload{body.toString(QUrl::FullyEncoded).toUtf8()};
    // The verifier has done its work. It is of no further use to this client and of every
    // use to anything reading this process, so it stops existing here rather than at the
    // end of the sign-in.
    m_loginVerifier.fill('\0');
    m_loginVerifier.clear();

    QNetworkReply *reply{m_network->post(request, payload)};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray answer{reply->readAll()};
        const int status{
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()};
        deleteSoon(reply);
        endDesktopLogin();

        const QJsonObject fields{QJsonDocument::fromJson(answer).object()};
        const QString session{fields.value(QStringLiteral("session")).toString()};
        const QString cookieName{fields.value(QStringLiteral("cookie_name")).toString()};
        if (status != 200 || session.isEmpty() || cookieName.isEmpty()) {
            // Every refusal looks the same from here on purpose (the edge answers 404 to an
            // unknown, expired, spent or mismatched code alike), so there is nothing more
            // specific to report than that it was refused.
            qWarning("SynQt: the edge refused the sign-in claim, so this client is still "
                     "signed out.");
            return;
        }
        DeviceCredential::Held enrolled;
        enrolled.id = fields.value(QStringLiteral("device_id")).toString();
        enrolled.secret = fields.value(QStringLiteral("device_secret")).toString().toLatin1();
        if (enrolled.isValid() && m_device) {
            m_device->save(enrolled);
            m_held = enrolled;
        }

        m_sessionCookie = cookieName.toUtf8() + '=' + session.toUtf8();
        // Kept on the config too, because that is what a reconnect reads: without it the
        // next start() would bootstrap a fresh anonymous session and quietly sign the
        // visitor back out.
        m_config.sessionCookie = m_sessionCookie;
        // Straight to the socket, and not through start(): the session is in hand, and
        // openSession() would spend the credential that was enrolled to get another.
        connectToEdge();
    });
}

void SynClient::endDesktopLogin()
{
    if (m_loopback) {
        m_loopback->stop();
        deleteSoon(m_loopback);
        m_loopback = nullptr;
    }
    m_loginState.fill('\0');
    m_loginState.clear();
    m_loginVerifier.fill('\0');
    m_loginVerifier.clear();
}

#endif // !Q_OS_WASM

void SynClient::endSession()
{
    if (m_config.logoutRoute.isEmpty()) {
        qWarning("SynQt: Session.logout() was called and this project configures no "
                 "identity, so there is no logout route to go to.");
        return;
    }
    const QString target{QString::fromUtf8(edgeHttpOrigin()) + m_config.logoutRoute};
#ifdef Q_OS_WASM
    // The cookie is the browser's, and only the route that expires it can take it away.
    leaveForUrl(target);
#else
    // Signing out while a sign-in is still open in the browser. The port goes, and an
    // answer arriving after this is answering a request that no longer exists.
    endDesktopLogin();
    // And the stored credential goes with it, before the request rather than after: a logout
    // that leaves something redeemable on the disk is worse than no logout at all, because
    // the visitor believes it worked. The edge deletes its half of the same pair when it
    // revokes the session. This half must not depend on that request arriving.
    m_held = DeviceCredential::Held{};
    if (m_device) {
        m_device->erase();
    }
    // The native client is holding the credential, so it presents it once, to be told to
    // stop holding it. The reconnect below is what makes the rest of the client agree:
    // the edge closes this session's connections as it revokes it, and start() comes back
    // with no cookie and therefore as a fresh anonymous visitor.
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QNetworkRequest request{QUrl{target}};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie);
    }
    QNetworkReply *reply{m_network->get(request)};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        deleteSoon(reply);
        // Dropped whatever the edge answered: a logout that the edge refused is still a
        // logout as far as this client is concerned, and going on holding a credential
        // the visitor asked to be rid of is the one outcome that would be wrong.
        m_sessionCookie.clear();
        m_config.sessionCookie.clear();
        start();
    });
#endif
}

void SynClient::start()
{
#ifdef Q_OS_WASM
    // The browser served the page and holds the session cookie. It attaches it to the
    // wss handshake automatically.
    connectToEdge();
#else
    openSession();
#endif
}

#ifndef Q_OS_WASM

DeviceCredential *SynClient::deviceStore()
{
    if (!m_config.deviceSession) {
        // A project that does not persist desktop sessions never touches a keyring, which is
        // also why this is built here and not in the constructor.
        return nullptr;
    }
    if (m_device == nullptr) {
        m_device = new DeviceCredential{m_config.edgeUrl, this};
    }
    return m_device;
}

void SynClient::openSession()
{
    // Native desktop: a client that already holds a session (one it just signed in for, or
    // one it was configured with) presents it directly; otherwise it either spends a device
    // credential stored at a previous launch or obtains an anonymous session from the edge,
    // and in the anonymous case stays anonymous until somebody calls Session.login().
    //
    // The second condition is what makes an edge restart survivable. A reconnect that has
    // already been accepted once retries with the same session, because a dropped socket is
    // usually a network blip. One that has not is either a fresh launch or an edge that came
    // back without the session table it had, and there the stored credential is exactly the
    // thing that gets the visitor back in without a sign-in.
    if (!m_config.sessionCookie.isEmpty() && m_sessionAccepted) {
        // Cleared as the attempt starts, and set again only by connecting: that is what
        // makes this one retry rather than a loop against a session the edge has forgotten.
        m_sessionAccepted = false;
        m_sessionCookie = m_config.sessionCookie;
        connectToEdge();
        return;
    }
    DeviceCredential *store{deviceStore()};
    if (store != nullptr && !m_redeeming) {
        if (!m_held.isValid()) {
            m_held = store->load();
        }
        if (m_held.isValid()) {
            redeemDeviceCredential();
            return;
        }
    }
    if (!m_config.sessionCookie.isEmpty()) {
        m_sessionCookie = m_config.sessionCookie;
        connectToEdge();
        return;
    }
    bootstrapAnonymousSession();
}

void SynClient::redeemDeviceCredential()
{
    setState(QStringLiteral("connecting"));
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    m_redeeming = true;

    QNetworkRequest request{QUrl{QString::fromUtf8(edgeHttpOrigin())
                                 + desktopRoute(m_config.loginRoute,
                                                QStringLiteral("/device"))}};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("device_id"), m_held.id);
    body.addQueryItem(QStringLiteral("device_secret"), QString::fromLatin1(m_held.secret));

    QNetworkReply *reply{m_network->post(request, body.toString(QUrl::FullyEncoded).toUtf8())};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray answer{reply->readAll()};
        const int status{
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()};
        deleteSoon(reply);
        m_redeeming = false;

        const QJsonObject fields{QJsonDocument::fromJson(answer).object()};
        const QString session{fields.value(QStringLiteral("session")).toString()};
        const QString cookieName{fields.value(QStringLiteral("cookie_name")).toString()};
        if (status == 200 && !session.isEmpty() && !cookieName.isEmpty()) {
            DeviceCredential::Held next;
            next.id = fields.value(QStringLiteral("device_id")).toString();
            next.secret = fields.value(QStringLiteral("device_secret")).toString().toLatin1();
            // Stored before the session is used, because the generation presented is
            // already retired at the edge. Losing this write is the case the edge's overlap
            // window exists for, and it is not one to walk into on purpose.
            if (next.isValid() && m_device) {
                m_device->save(next);
                m_held = next;
            }
            m_sessionCookie = cookieName.toUtf8() + '=' + session.toUtf8();
            m_config.sessionCookie = m_sessionCookie;
            connectToEdge();
            return;
        }
        if (status != 0 && status != 200) {
            // The edge answered, and its answer was no. Whatever is stored cannot become a
            // session again, so it goes: keeping it would mean presenting a dead credential
            // at every launch for the rest of the installation's life.
            m_held = DeviceCredential::Held{};
            if (m_device) {
                m_device->erase();
            }
            qInfo("SynQt: the stored sign-in is no longer valid, so this launch starts "
                  "signed out.");
        }
        // A transport failure keeps it: the edge said nothing, so nothing is known about
        // whether the credential is still good.
        if (!m_config.sessionCookie.isEmpty()) {
            m_sessionCookie = m_config.sessionCookie;
            connectToEdge();
            return;
        }
        bootstrapAnonymousSession();
    });
}

void SynClient::bootstrapAnonymousSession()
{
    setState(QStringLiteral("connecting"));
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QUrl httpUrl{m_config.edgeUrl};
    httpUrl.setScheme(httpUrl.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                               : QStringLiteral("http"));
    httpUrl.setPath(QStringLiteral("/"));
    QNetworkRequest request{httpUrl};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    QNetworkReply *reply{m_network->get(request)};
    connect(reply, &QNetworkReply::finished, this, [this, reply, httpUrl]() {
        m_sessionCookie = heldCredential(m_network, httpUrl);
        deleteSoon(reply);
        connectToEdge();
    });
}

#endif // !Q_OS_WASM

void SynClient::connectToEdge()
{
    teardown();
    setState(QStringLiteral("connecting"));

    m_node = new QRemoteObjectNode{this};
    // Parented like the node and the transport beside it: teardown() retires these on
    // every reconnect, but deleteLater needs a running event loop, and the destructor can
    // run after exec() has returned. The parent is what makes the last one deterministic.
    m_socket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
    m_transport = new WebSocketTransport{m_socket, this};

    connect(m_socket, &QWebSocket::connected, this, [this]() { onConnected(); });
    connect(m_socket, &QWebSocket::disconnected, this, [this]() { onDisconnected(); });
    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onDisconnected(); });

#ifdef Q_OS_WASM
    // The browser terminates TLS and attaches the session cookie. Mark the device open,
    // add the connection, THEN open the socket. The QtRO handshake is server-initiated,
    // so the connection must be attached before the socket connects.
    m_transport->open(QIODevice::ReadWrite);
    m_node->addClientSideConnection(m_transport);
    m_node->setHeartbeatInterval(m_config.heartbeatMs);
    m_socket->open(m_config.edgeUrl);
#else
    // Native. Mark the device open, then open the socket here so the client terminates
    // its own TLS and presents the session credential and origin on the handshake.
    m_transport->open(QIODevice::ReadWrite);
    m_node->addClientSideConnection(m_transport);
    m_node->setHeartbeatInterval(m_config.heartbeatMs);

    const QSslConfiguration tls{nativeTlsConfiguration(m_config)};
    m_socket->setSslConfiguration(tls);

    QNetworkRequest request{m_config.edgeUrl};
    request.setSslConfiguration(tls);
    request.setRawHeader("Origin", edgeHttpOrigin());
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie);
    }
    m_socket->open(request);
#endif

    m_server->bindNode(m_node);
    if (m_pageLoader) {
        bindPagesConnectPoint();
    }
}

void SynClient::onConnected()
{
    m_backoffMs = m_config.reconnectBaseMs;
#ifndef Q_OS_WASM
    // The edge accepted this credential, so the next dropped socket is a network event and
    // not an edge that came back without the session table it had. See openSession().
    m_sessionAccepted = true;
#endif
    setState(QStringLiteral("connected"));
}

void SynClient::onDisconnected()
{
    if (m_state == QStringLiteral("reconnecting")) {
        return;
    }
    setState(QStringLiteral("reconnecting"));
    scheduleReconnect();
}

void SynClient::scheduleReconnect()
{
    if (m_reconnectTimer->isActive()) {
        return;
    }
    m_reconnectTimer->start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, m_config.reconnectMaxMs);
}

void SynClient::teardown()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        deleteSoon(m_socket);
        m_socket = nullptr;
    }
    if (m_transport) {
        deleteSoon(m_transport);
        m_transport = nullptr;
    }
    if (m_node) {
        deleteSoon(m_node);  // deletes the replicas it parents
        m_node = nullptr;
    }
}

void SynClient::bindPagesConnectPoint()
{
    // The facade is stable across reconnects (ServerAccessor rebinds the same instance
    // to each fresh Replica. Its own reconnect logic re-notifies routeTableChanged),
    // so wire it up once. Without this guard, connectToEdge() calling this on every
    // reconnect would multiply every connection made below by one more each time.
    if (m_pagesFacade) {
        return;
    }

    // The Pages connect point is framework plumbing, not something an app declares for
    // its own use, but it is still an ordinary consumed connect point: it rides the
    // same acquire-and-bind path as any other name in m_config.connectPoints (populated
    // for a remote-pages app by the generated topology), so no separate acquisition
    // mechanism is introduced here. Nothing to bind yet if that entry has not arrived.
    QString pointName;
    for (const ClientConnectPoint &point : std::as_const(m_config.connectPoints)) {
        if (point.contract == QStringLiteral("Pages")) {
            pointName = point.name;
            break;
        }
    }
    if (pointName.isEmpty()) {
        return;
    }

    auto *facade{qobject_cast<ConsumerBase *>(m_server->point(pointName))};
    if (!facade) {
        // No consumer facade registered for "Pages" in this build. A raw Replica alone
        // cannot answer fetchPage() with a value this class can read generically (its
        // reply type is declared per app by the generated contract). Warn once rather
        // than resolve every remote route to a silent Error.
        qWarning("SynQt: the 'Pages' connect point has no consumer facade; edge-delivered "
                 "pages will not resolve");
        return;
    }
    m_pagesFacade = facade;
    if (m_engine) {
        // The generated facade's own fetchPage() builds the Promise it returns via
        // qjsEngine(this), which is null until the object has been given a JS wrapper at
        // least once. The facade is framework plumbing no app QML ever references
        // directly, so without this it would never get one, every reply would resolve
        // as an undefined value, and every remote page would fail. Discarding the
        // returned QJSValue is fine: the association qjsEngine() reads is recorded on
        // the object itself, not on the wrapper value's own lifetime.
        m_engine->newQObject(facade);
    }

    connect(m_router, &Router::pageRequested, this,
            [this, facade](const QString &route, const QString &haveHash) {
        SynQt::Promise *promise{nullptr};
        QMetaObject::invokeMethod(facade, "fetchPage",
                                  Q_RETURN_ARG(SynQt::Promise *, promise),
                                  Q_ARG(QString, route), Q_ARG(QString, haveHash));
        if (!promise || !m_engine) {
            // No promise (the call could not even be dispatched) or no engine to bridge
            // one through. Either way, resolve to Error now rather than hang in Loading.
            m_router->onPageDelivered(route, QString{}, QString{}, QString{},
                                      QStringLiteral("error"));
            return;
        }
        auto *bridge{new PageReplyBridge{m_router, route}};
        // then()/catchError() only take a callable QJSValue. Wrap the bridge's two
        // invokable methods into JS closures over it, rather than exposing it as a
        // named global. A rejection (the connect point not yet live, or the call
        // itself failing) is handled too, so a failure always reaches onPageDelivered
        // instead of leaving the route in Loading forever.
        QJSValue factory{m_engine->evaluate(QStringLiteral(
            "(function (bridge) { return {"
            "  onFulfilled: function (value) { bridge.deliver(value); },"
            "  onRejected: function (reason) { bridge.fail(reason); }"
            "}; })"))};
        const QJSValue handlers{factory.call(QJSValueList{m_engine->newQObject(bridge)})};
        promise->then(handlers.property(QStringLiteral("onFulfilled")))
            ->catchError(handlers.property(QStringLiteral("onRejected")));
    });

    // Old-style string connects. The facade's concrete type (and so its pageChanged/
    // routeTableChanged signals) is generated per app, so it is only ever held here
    // through the generic ConsumerBase surface.
    connect(facade, SIGNAL(pageChanged(QString, QString)), this,
            SLOT(handlePagesPageChanged(QString, QString)));
    connect(facade, SIGNAL(routeTableChanged()), this, SLOT(handlePagesRouteTableChanged()));
    // Pull whatever the table already holds (a reconnect rebinds the same facade to a
    // fresh Replica, which re-notifies once initialized. The first bind on a plain
    // property read needs no signal to have fired yet).
    handlePagesRouteTableChanged();
}

void SynClient::handlePagesPageChanged(const QString &route, const QString &hash)
{
    m_router->onPageChanged(route, hash);
}

void SynClient::handlePagesRouteTableChanged()
{
    if (!m_pagesFacade) {
        return;
    }
    m_router->applyRemoteRouteTable(m_pagesFacade->property("routeTable").toString());
}

void SynClient::setState(const QString &state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
    m_session->setState(state);
}

} // namespace SynQt

#include "synclient.moc"
