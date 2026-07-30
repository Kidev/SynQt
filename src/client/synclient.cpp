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
#include "sessionstate_rep.h"  // the generated SessionStateReplica
#include "websockettransport.h"

#include <QJSEngine>
#include <QJSValue>
#include <QJSValueList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>
#include <QWebSocket>

#ifndef Q_OS_WASM
#  include "constanttime.h"
#  include "desktoproutes.h"
#  include "loopbackreceiver.h"
#  include "secrets.h"

#  include <QDesktopServices>
#  include <QNetworkAccessManager>
#  include <QNetworkCookie>
#  include <QNetworkCookieJar>
#  include <QNetworkReply>
#  include <QNetworkRequest>
#  include <QSslCertificate>
#  include <QSslConfiguration>
#  include <QSslSocket>
#endif

#include <algorithm>
#include <utility>

namespace SynQt {

namespace {

/// The fields of a reply, whatever shape it has in C++. A returning slot's record arrives
/// as the gadget repc generated, and a QVariant holding a gadget converts to an empty map.
/// So a gadget is read through its properties (one per record field), a script value
/// through its own conversion, and anything else as a map.
QVariantMap fieldsOf(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant().toMap();
    }
    const QMetaObject *gadget{value.metaType().metaObject()};
    if (gadget == nullptr || (value.metaType().flags() & QMetaType::IsGadget) == 0) {
        return value.toMap();
    }
    QVariantMap fields;
    for (int index{gadget->propertyOffset()}; index < gadget->propertyCount(); ++index) {
        const QMetaProperty property{gadget->property(index)};
        fields.insert(QString::fromLatin1(property.name()),
                      property.readOnGadget(value.constData()));
    }
    return fields;
}

/// Promise::then()/catchError() are the QML-facing API ("slot(args).then(value => ...)")
/// and accept only a callable QJSValue. SynClient's Pages wiring needs the reply from C++,
/// so this bridges the two: a throwaway QObject exposes the two calls, and the app's QML
/// engine wraps each in a JS closure (C++ cannot construct a callable QJSValue directly).
/// It has no parent, so the engine's garbage collector reclaims it once the settled Promise
/// drops its handlers.
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
        const QVariantMap fields{fieldsOf(value)};
        m_router->onPageDelivered(m_route, fields.value(QStringLiteral("qml")).toString(),
                                  fields.value(QStringLiteral("hash")).toString(),
                                  fields.value(QStringLiteral("seed")).toString(),
                                  fields.value(QStringLiteral("status")).toString());
    }

    /// A rejected promise (the point not live yet, or the call failing) must still resolve
    /// the route, or it stays in Loading.
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

// The native client verifies the edge certificate: VerifyPeer against the OS trust store
// and the hostname, plus any pinned certificate from config. Verification is never
// disabled.
/// The session this client holds for `origin`, in handshake form ("name=value;
/// name=value"), read from the network manager's cookie jar.
///
/// From the jar, not the reply's Set-Cookie header: the edge withholds Set-Cookie from a
/// request that already has a live session, and a followed redirect hides the response that
/// set it. Reading the header would overwrite a good credential with nothing. The jar is
/// also what a browser uses.
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

} // namespace
#endif

SynClient::SynClient(SynClientConfig config, QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_server{new ServerAccessor{m_config.connectPoints, this}}
    , m_session{new Session{m_config, engine, this}}
    , m_router{new Router{m_config, m_session, engine, this}}
    , m_update{new ClientUpdate{this}}
    , m_engine{engine}
    , m_reconnectTimer{new QTimer{this}}
    , m_handshakeTimer{new QTimer{this}}
    , m_backoffMs{m_config.reconnectBaseMs}
{
    m_reconnectTimer->setSingleShot(true);
    m_handshakeTimer->setSingleShot(true);
    // An edge that accepted the socket and then said nothing is treated as a dropped
    // socket: back off and retry. Without this the client would wait on the handshake
    // forever.
    //
    // The socket is aborted so the stale handshake cannot complete later behind the
    // reconnect that replaced it. The next connectToEdge() retires the node and its
    // replicas as usual.
    connect(m_handshakeTimer, &QTimer::timeout, this, [this]() {
        if (m_socket) {
            m_socket->abort();
        }
        onDisconnected();
    });
    // Reconnect through start(), so a native client re-bootstraps its session (the edge may
    // have restarted). On WASM start() just reconnects, since the browser holds the cookie.
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() { start(); });

    // The two actions Session offers QML. Session reports them and this answers them,
    // because ending a session goes over the network to the edge; otherwise
    // `Session.logout()` would leave the session and its cookie alive at the edge.
    connect(m_session, &Session::loginRequested, this, &SynClient::beginLogin);
    connect(m_session, &Session::logoutRequested, this, &SynClient::endSession);

    // An empty palette means no remote pages: no loader, so resolveRemote() reports Error
    // instead of staying in Loading.
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
    // A native window cannot navigate, so the sign-in happens in the system browser, and
    // the answer returns on a port this process holds meanwhile. The answer is a claim
    // code, not a session: the browser's history keeps the URL, on a machine that may be
    // shared.
    if (m_loopback) {
        // Already waiting on a sign-in; a second would open a second port while the first
        // still listens.
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
    m_loginState = randomSecret();
    m_loginVerifier = randomSecret();

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
    // Any local process can reach the port, so only an answer carrying the nonce this
    // client just generated belongs to its sign-in. Otherwise another local process could
    // hand over a code for its own account and sign the visitor in as someone else (session
    // fixation).
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
    QNetworkRequest request{QUrl{QString::fromUtf8(edgeHttpOrigin())
                                 + desktopClaimRoute(m_config.loginRoute)}};
    // The exchange happens over this client's own verified TLS connection to the edge, not
    // through a browser, which is why the loopback carried a code.
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));

    QUrlQuery body;
    body.addQueryItem(QStringLiteral("code"), code);
    body.addQueryItem(QStringLiteral("verifier"), QString::fromLatin1(m_loginVerifier));
    // Enrolment rides on the claim, the one moment the session is proven to belong to this
    // process. The binding is what this machine's store offers. If the deployment requires
    // more, no credential is issued and the visitor stays signed in.
    DeviceCredential *store{deviceStore()};
    if (store != nullptr && store->isAvailable()) {
        body.addQueryItem(QStringLiteral("device"), QStringLiteral("1"));
        body.addQueryItem(QStringLiteral("binding"), store->bindingName());
        body.addQueryItem(QStringLiteral("label"), DeviceCredential::machineLabel());
    }
    const QByteArray payload{body.toString(QUrl::FullyEncoded).toUtf8()};
    // The verifier is no longer needed, so it is cleared now rather than at the end of the
    // sign-in.
    m_loginVerifier.fill('\0');
    m_loginVerifier.clear();

    QNetworkReply *reply{network()->post(request, payload)};
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
            // The edge answers 404 to an unknown, expired, spent or mismatched code alike,
            // so "refused" is all there is to report. Not reaching the edge is reported
            // differently, because it is worth retrying.
            if (status == 0) {
                qWarning("SynQt: the edge did not answer the sign-in claim, so this client "
                         "is still signed out. Signing in again is worth a try.");
            } else {
                qWarning("SynQt: the edge refused the sign-in claim, so this client is "
                         "still signed out.");
            }
            return;
        }
        DeviceCredential::Held enrolled;
        enrolled.id = fields.value(QStringLiteral("device_id")).toString();
        enrolled.secret = fields.value(QStringLiteral("device_secret")).toString().toLatin1();
        if (enrolled.isValid() && m_device) {
            m_device->save(enrolled);
            m_held = enrolled;
            // It was issued with the session below and has paid for it. If the socket then
            // fails, retry with that session instead of spending the new credential again.
            m_credentialSpent = true;
        }

        m_sessionCookie = cookieName.toUtf8() + '=' + session.toUtf8();
        // Also stored on the config, which a reconnect reads; otherwise start() would
        // bootstrap a new anonymous session and sign the visitor out.
        m_config.sessionCookie = m_sessionCookie;
        // Directly to the socket, not through start(): openSession() would spend the new
        // credential again.
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
    // Signing out while a browser sign-in is open: the port closes, and any later answer is
    // ignored.
    endDesktopLogin();
    // The stored credential is deleted first, before the request: a logout that leaves
    // something redeemable on disk is worse than none. The edge deletes its half when it
    // revokes the session, but this half must not depend on that request.
    m_held = DeviceCredential::Held{};
    if (m_device) {
        m_device->erase();
    }
    // The native client presents the credential once more, to have it revoked. The edge
    // closes this session's connections as it revokes it, and the reconnect below returns
    // as a fresh anonymous visitor.
    QNetworkRequest request{QUrl{target}};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie);
    }
    QNetworkReply *reply{network()->get(request)};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        deleteSoon(reply);
        // Dropped whatever the edge answered: the visitor asked to be signed out.
        m_sessionCookie.clear();
        m_config.sessionCookie.clear();
        start();
    });
#endif
}

void SynClient::start()
{
#ifdef Q_OS_WASM
    // The browser served the page and holds the session cookie, which it attaches to the
    // wss handshake.
    connectToEdge();
#else
    openSession();
#endif
}

#ifndef Q_OS_WASM

DeviceCredential *SynClient::deviceStore()
{
    if (!m_config.deviceSession) {
        // Built here, so a project that does not persist desktop sessions never touches a
        // keyring.
        return nullptr;
    }
    if (m_device == nullptr) {
        m_device = new DeviceCredential{m_config.edgeUrl, this};
    }
    return m_device;
}

QNetworkAccessManager *SynClient::network()
{
    if (m_network == nullptr) {
        m_network = new QNetworkAccessManager{this};
        // Every request here is something else waits on (the session before a socket, the
        // claim that ends a sign-in, a redemption), so each has a timeout. A peer that
        // accepts and never answers is not an error, and would otherwise block for the life
        // of the process.
        m_network->setTransferTimeout(m_config.requestTimeoutMs);
    }
    return m_network;
}

void SynClient::openSession()
{
    // Native desktop. A client already holding a session (signed in, or configured)
    // presents it. Otherwise it spends a device credential stored at a previous launch, or
    // gets an anonymous session from the edge and stays anonymous until Session.login().
    //
    // The second condition makes an edge restart survivable. A reconnect after an accepted
    // connection retries with the same session, since a dropped socket is usually a network
    // blip. Otherwise this is a fresh launch or an edge that lost its session table, and
    // the stored credential signs the visitor back in.
    //
    // The credential is spent at most once per session it buys (m_credentialSpent). A
    // failing socket says nothing about the session, and spending the credential per
    // reconnect would retire a generation each time, hit the edge's rate limit, and end
    // with the credential deleted. So a failed session is retried with the same session,
    // and the credential waits until a connection has been accepted.
    if (!m_config.sessionCookie.isEmpty() && m_sessionAccepted) {
        // Cleared as the attempt starts and set again only on connecting, so this retries
        // only once.
        m_sessionAccepted = false;
        m_sessionCookie = m_config.sessionCookie;
        connectToEdge();
        return;
    }
    DeviceCredential *store{deviceStore()};
    if (store != nullptr && !m_redeeming && !m_credentialSpent
        && m_redeemNotBefore.hasExpired()) {
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
    m_redeeming = true;

    QNetworkRequest request{QUrl{QString::fromUtf8(edgeHttpOrigin())
                                 + desktopDeviceRoute(m_config.loginRoute)}};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/x-www-form-urlencoded"));
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("device_id"), m_held.id);
    body.addQueryItem(QStringLiteral("device_secret"), QString::fromLatin1(m_held.secret));

    QNetworkReply *reply{network()->post(request,
                                        body.toString(QUrl::FullyEncoded).toUtf8())};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray answer{reply->readAll()};
        const int status{
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()};
        const QByteArray retryAfter{reply->rawHeader("Retry-After")};
        deleteSoon(reply);
        m_redeeming = false;

        const QJsonObject fields{QJsonDocument::fromJson(answer).object()};
        const QString session{fields.value(QStringLiteral("session")).toString()};
        const QString cookieName{fields.value(QStringLiteral("cookie_name")).toString()};
        if (status == 200 && !session.isEmpty() && !cookieName.isEmpty()) {
            DeviceCredential::Held next;
            next.id = fields.value(QStringLiteral("device_id")).toString();
            next.secret = fields.value(QStringLiteral("device_secret")).toString().toLatin1();
            // Stored before the session is used, because the presented generation is
            // already retired at the edge. Losing this write is what the edge's overlap
            // window covers.
            if (next.isValid() && m_device) {
                m_device->save(next);
                m_held = next;
            }
            // The credential paid for this session and buys no other until this one has
            // been accepted and later fails. See openSession().
            m_credentialSpent = true;
            m_sessionCookie = cookieName.toUtf8() + '=' + session.toUtf8();
            m_config.sessionCookie = m_sessionCookie;
            connectToEdge();
            return;
        }
        if (status == 404) {
            // The edge refused the credential, so it can never become a session again and
            // is deleted.
            m_held = DeviceCredential::Held{};
            if (m_device) {
                m_device->erase();
            }
            qInfo("SynQt: the stored sign-in is no longer valid, so this launch starts "
                  "signed out.");
        } else if (status != 0) {
            // Any other answer (429 from the edge's rate window, or a proxy error) says
            // nothing about the credential, so it is kept, and the client waits instead of
            // retrying at reconnect pace. The wait is what the edge named, or a minute, the
            // window length.
            constexpr qint64 kDefaultHoldMs{60 * 1000};
            constexpr qint64 kMaxHoldMs{5 * 60 * 1000};
            bool numeric{false};
            // Bounded: a proxy can set this value, and must not stop the client for a week.
            const qint64 asked{static_cast<qint64>(retryAfter.toInt(&numeric)) * 1000};
            m_redeemNotBefore.setRemainingTime(
                numeric && asked > 0 ? qMin(asked, kMaxHoldMs) : kDefaultHoldMs);
        }
        // A transport failure keeps the credential and adds no wait: nothing is known about
        // it, and the next attempt may reach a recovered edge.
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
    QUrl httpUrl{m_config.edgeUrl};
    httpUrl.setScheme(httpUrl.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                               : QStringLiteral("http"));
    httpUrl.setPath(QStringLiteral("/"));
    QNetworkRequest request{httpUrl};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    QNetworkReply *reply{network()->get(request)};
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
    // Parented like the node and the transport: teardown() retires these on every
    // reconnect, but deleteLater needs a running event loop and the destructor may run
    // after exec() returns.
    m_socket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
    m_transport = new WebSocketTransport{m_socket, this};
    // Started before open(), so it covers connecting as well as upgrading, both of which
    // can stall silently. Stopped by onConnected() and by teardown().
    m_handshakeTimer->start(m_config.requestTimeoutMs);

    connect(m_socket, &QWebSocket::connected, this, [this]() { onConnected(); });
    connect(m_socket, &QWebSocket::disconnected, this, [this]() { onDisconnected(); });
    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onDisconnected(); });

#ifdef Q_OS_WASM
    // The browser terminates TLS and attaches the session cookie. Mark the device open, add
    // the connection, then open the socket: the QtRO handshake starts from the server, so
    // the connection must be attached first.
    m_transport->open(QIODevice::ReadWrite);
    m_node->addClientSideConnection(m_transport);
    m_node->setHeartbeatInterval(m_config.heartbeatMs);
    m_socket->open(m_config.edgeUrl);
#else
    // Native: mark the device open, then open the socket, which terminates TLS here and
    // presents the session credential and origin on the handshake.
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
    bindSessionState();
    if (m_pageLoader) {
        bindPagesConnectPoint();
    }
}

void SynClient::onConnected()
{
    m_handshakeTimer->stop();
    m_backoffMs = m_config.reconnectBaseMs;
#ifndef Q_OS_WASM
    // The edge accepted this credential, so the next dropped socket is a network event, not
    // an edge that lost its session table. See openSession().
    m_sessionAccepted = true;
    // The session worked, so the stored credential may buy the next one if this one fails.
    // Cleared only here: an accepted connection proves the round trip.
    m_credentialSpent = false;
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
    m_handshakeTimer->stop();
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
    // A child of the retired node (bindSessionState), already gone.
    m_sessionState = nullptr;
}

void SynClient::bindPagesConnectPoint()
{
    // The facade persists across reconnects (ServerAccessor rebinds it to each new Replica
    // and re-notifies routeTableChanged), so it is wired once. Without this guard every
    // reconnect would add another copy of each connection below.
    if (m_pagesFacade) {
        return;
    }

    // The Pages point is framework plumbing but an ordinary consumed point: it uses the
    // same acquire-and-bind path as any name in m_config.connectPoints (populated for a
    // remote-pages app by the generated topology). Nothing to bind until that entry
    // arrives.
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
        // No consumer facade is registered for "Pages" in this build, and a raw Replica
        // cannot answer fetchPage() in a form this class can read (the reply type is
        // generated per app). Warn once instead of resolving every remote route to a silent
        // Error.
        qWarning("SynQt: the 'Pages' connect point has no consumer facade; edge-delivered "
                 "pages will not resolve");
        return;
    }
    m_pagesFacade = facade;
    if (m_engine) {
        // The generated facade's fetchPage() builds its Promise through qjsEngine(this),
        // which is null until the object has had a JS wrapper. No app QML references this
        // facade, so give it one here; otherwise every reply resolves as undefined. The
        // returned QJSValue can be discarded: qjsEngine() reads an association stored on
        // the object.
        m_engine->newQObject(facade);
    }

    connect(m_router, &Router::pageRequested, this,
            [this, facade](const QString &route, const QString &haveHash) {
        SynQt::Promise *promise{nullptr};
        QMetaObject::invokeMethod(facade, "fetchPage",
                                  Q_RETURN_ARG(SynQt::Promise *, promise),
                                  Q_ARG(QString, route), Q_ARG(QString, haveHash));
        if (!promise || !m_engine) {
            // No promise (the call could not be dispatched) or no engine: resolve to Error
            // now.
            m_router->onPageDelivered(route, QString{}, QString{}, QString{},
                                      QStringLiteral("error"));
            return;
        }
        auto *bridge{new PageReplyBridge{m_router, route}};
        // then()/catchError() take only a callable QJSValue, so the bridge's two invokable
        // methods are wrapped in JS closures. A rejection is handled too, so every outcome
        // reaches onPageDelivered.
        QJSValue factory{m_engine->evaluate(QStringLiteral(
            "(function (bridge) { return {"
            "  onFulfilled: function (value) { bridge.deliver(value); },"
            "  onRejected: function (reason) { bridge.fail(reason); }"
            "}; })"))};
        const QJSValue handlers{factory.call(QJSValueList{m_engine->newQObject(bridge)})};
        promise->then(handlers.property(QStringLiteral("onFulfilled")))
            ->catchError(handlers.property(QStringLiteral("onRejected")));
    });

    // String-based connects: the facade's concrete type (with its pageChanged and
    // routeTableChanged signals) is generated per app, so it is held only as a
    // ConsumerBase.
    connect(facade, SIGNAL(pageChanged(QString, QString)), this,
            SLOT(handlePagesPageChanged(QString, QString)));
    connect(facade, SIGNAL(routeTableChanged()), this, SLOT(handlePagesRouteTableChanged()));
    // Read what the table already holds. A reconnect rebinds the same facade to a new
    // Replica, which re-notifies once initialized; the first bind needs no signal.
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

void SynClient::bindSessionState()
{
    if (!m_node) {
        return;
    }
    // A compile-time Replica, not a dynamic one: it carries its API instead of exchanging a
    // definition, which is what makes it work under single-threaded WebAssembly. The
    // contract is compiled into this library, so this works even in a project that declares
    // no connect points.
    auto *replica{m_node->acquire<SessionStateReplica>(QStringLiteral("SessionState"))};
    // Parented to the node, because acquire() does not parent a typed replica (`new
    // ObjectType(this, name)` over a `QObject(nullptr)` constructor) and the node holds
    // only a weak reference. teardown() retires the node on every reconnect, and the
    // replica goes with it. tests/memory measures this; EntityRuntime does the same for a
    // mesh link.
    replica->setParent(m_node);
    m_sessionState = replica;
    connect(replica, &SessionStateReplica::sessionChanged, this,
            [this]() { applySessionState(); });
    // A property already at its published value when the replica initializes emits no
    // change, which is the normal case: the edge publishes as soon as the connection is
    // accepted.
    connect(replica, &QRemoteObjectReplica::initialized, this,
            [this]() { applySessionState(); });
}

void SynClient::applySessionState()
{
    if (!m_sessionState) {
        return;
    }
    const QByteArray published{
        m_sessionState->property("session").toString().toUtf8()};
    if (published.isEmpty()) {
        // Not known yet, which differs from anonymous. Leaving Session alone keeps a
        // reconnect from briefly blanking a signed-in visitor.
        return;
    }
    const QJsonObject state{QJsonDocument::fromJson(published).object()};
    const QJsonValue identity{state.value(QLatin1String{"identity"})};
    m_session->setSession(state.value(QLatin1String{"scope"}).toString(),
                          identity.isObject()
                              ? QVariant{identity.toObject().toVariantMap()}
                              : QVariant{});
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
