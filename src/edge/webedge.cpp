// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "webedge.h"

#include "caller.h"
#include "identityprovider.h"
#include "pageseed.h"
#include "pagesedgesource.h"
#include "pagesservice.h"
#include "pagestore.h"
#include "ratewindow.h"
#include "sessionmanager.h"
#include "sessionstatesource.h"
#include "sourcefactory.h"
#include "topology.h"           // loadCertificate / loadPrivateKey
#include "tracer.h"
#include "iothreadpool.h"       // reused host-side (from src/transport)
#include "socketchannel.h"      // reused host-side (from src/transport)
#include "socketoptions.h" // reused host-side (from src/transport)
#include "websockettransport.h" // reused host-side (from src/transport)

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerConfiguration>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QJSValue>
#include <QJSValueIterator>
#include <QJsonDocument>
#include <QMetaMethod>
#include <QNetworkRequest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace SynQt {

namespace {

// How many full-size frames one browser connection may have buffered but unread before
// the edge stops paying for it. A frame is already capped at max_message_bytes; this
// caps their sum, which that cap alone does not. Four is far above anything QtRO
// produces (it drains the buffer synchronously on readyRead, so the steady state is one
// frame) and it keeps the per-connection ceiling tied to the knob an operator already
// tunes: with max_connections_global, the two bound the edge's total read memory.
constexpr qint64 ReadBufferFrames{4};

// The content type for a bundle file the build precompresses, or empty for anything
// else. Empty means "no encoded variant to consider": the response falls through to
// fromFile(), which determines the type itself.
QByteArray bundleContentType(const QString &path)
{
    if (path.endsWith(QLatin1String(".wasm"))) {
        return QByteArrayLiteral("application/wasm");
    }
    if (path.endsWith(QLatin1String(".js"))) {
        return QByteArrayLiteral("text/javascript");
    }
    if (path.endsWith(QLatin1String(".html"))) {
        return QByteArrayLiteral("text/html");
    }
    if (path.endsWith(QLatin1String(".json"))) {
        return QByteArrayLiteral("application/json");
    }
    if (path.endsWith(QLatin1String(".svg"))) {
        return QByteArrayLiteral("image/svg+xml");
    }
    return {};
}

// The conditional-GET reply, when the caller already holds this exact resource. Returns
// nothing when the body must be sent. Lives here rather than on WebEdge because the
// header only forward-declares QHttpServerResponse, and std::optional needs it complete.
std::optional<QHttpServerResponse> notModifiedFor(const QHttpServerRequest &request,
                                                  const QByteArray &etag)
{
    if (etag.isEmpty() || request.value("If-None-Match") != etag) {
        return std::nullopt;
    }
    QHttpServerResponse response{QHttpServerResponse::StatusCode::NotModified};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
    response.setHeaders(std::move(headers));
    return response;
}

/// Whether `candidate` already sits somewhere under `ancestor`, so adopting it would take
/// it away from an owner that is counting on having it. Same question SocketChannel asks
/// before adopting a raw socket, asked here for the link that has no channel.
bool isUnder(const QObject *candidate, const QObject *ancestor)
{
    for (const QObject *walk{candidate}; walk != nullptr; walk = walk->parent()) {
        if (walk == ancestor) {
            return true;
        }
    }
    return false;
}

// A plaintext (dev) transport server that surfaces every accepted socket so the edge
// can start a handshake-timeout timer for it, then hands it to QHttpServer's queue.
class EdgeTcpServer : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;
    std::function<void(QTcpSocket *)> onAccepted;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        disableNagle(socket);
        if (onAccepted) {
            onAccepted(socket);
        }
        addPendingConnection(socket);
    }
};

} // namespace

WebEdge::WebEdge(WebEdgeConfig config, QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_engine{engine}
    , m_connections{new QObject{this}}
    , m_sessionManager{new SessionManager{m_config.defaultScope,
                                          m_config.sessionTtlMinutes, this}}
    , m_clientAddress{m_config.trustedProxies}
{
    // The single-bundle shorthand, folded in once. Everything downstream reads `bundles`
    // and only `bundles`, so `bundleDir` cannot disagree with it later.
    if (m_config.bundles.isEmpty() && !m_config.bundleDir.isEmpty()) {
        m_config.bundles.insert(m_config.defaultScope, m_config.bundleDir);
    }

    // Every upgrade the edge decides on, recorded once. Connected to the signals rather
    // than written at each `emit`, so a refusal added later is traced by existing here
    // and not by someone remembering to add a line beside it.
    //
    // What is recorded is the decision and its reason, never the credential behind it: a
    // session cookie, a bearer token or an Authorization header must not reach a monitor,
    // because a record of a credential is a copy of it (docs/security.md).
    connect(this, &WebEdge::upgradeRejected, this, [](const QString &reason) {
        trace(Category::Authorization, Severity::Warning, QStringLiteral("upgrade refused"),
              {{QStringLiteral("reason"), reason}});
    });
    connect(this, &WebEdge::upgradeAccepted, this, [](const QString &peer) {
        trace(Category::Transport, Severity::Info, QStringLiteral("upgrade accepted"),
              {{QStringLiteral("peer"), peer}});
    });
}

/// Put the connections down before the threads their sockets are on.
///
/// Destroying a connection destroys its device, and a device on a threaded edge answers
/// that by asking its channel to delete itself on the thread it lives on. Stopping the
/// pool first would leave those deletions posted to threads that had already gone. Doing
/// it after works because quitting a thread's event loop delivers what is still queued
/// for it, including the deferred deletes.
WebEdge::~WebEdge()
{
    delete m_connections;
    m_connections = nullptr;
    delete m_ioThreads;
    m_ioThreads = nullptr;
}

QString WebEdge::errorString() const
{
    return m_errorString;
}

SessionManager *WebEdge::sessionManager() const
{
    return m_sessionManager;
}

IdentityProvider *WebEdge::identityProvider() const
{
    return m_identity;
}

PagesService *WebEdge::pagesService() const
{
    return m_pagesService;
}

namespace {

// How deep a seed may nest, and how large its JSON may get. A seed is one page's first
// frame of data rather than a feed, so both bounds are generous for every legitimate use. They exist
// because QJSValue::toVariant() and QJsonDocument::fromVariant() recurse without a bound
// of their own and take the process down on a deep enough structure.
constexpr int kMaxSeedDepth{32};
constexpr int kMaxSeedBytes{64 * 1024};

/// How, if at all, a hook exposes the seedFor(route, parameters, caller) the edge calls.
enum class SeedForSupport {
    None,     ///< no seedFor(route, parameters, caller) at all.
    Untyped,  ///< seedFor with three untyped (QVariant) parameters: the edge can call it.
    Typed,    ///< seedFor with three parameters, at least one annotated: not callable.
};

/// Classify a hook's seedFor. The edge invokes it with three QVariant arguments, so only an
/// untyped seedFor matches. Annotating a parameter type (`route: string`) changes the method
/// signature so the invoke can never bind to it. The probe therefore mirrors the invoke
/// exactly, rather than accepting any three-argument seedFor and letting it fail per request.
SeedForSupport seedForSupport(const QObject *hook)
{
    const QMetaObject *meta{hook->metaObject()};
    for (int index{0}; index < meta->methodCount(); ++index) {
        const QMetaMethod method{meta->method(index)};
        if (method.name() != QByteArrayLiteral("seedFor") || method.parameterCount() != 3) {
            continue;
        }
        for (int argument{0}; argument < 3; ++argument) {
            if (method.parameterMetaType(argument).id() != QMetaType::QVariant) {
                return SeedForSupport::Typed;
            }
        }
        return SeedForSupport::Untyped;
    }
    return SeedForSupport::None;
}

/// value converted to a QVariant, refusing anything nested deeper than kMaxSeedDepth.
///
/// This exists instead of QJSValue::toVariant() because that function (and
/// QJsonDocument::fromVariant() after it) recurses once per level with no limit: a
/// structure whose depth follows its data, like a category tree, overflows the stack and
/// kills the edge for every connected browser. The walk here recurses too, but only ever
/// to kMaxSeedDepth. Sets ok to false when the bound is hit.
QVariant boundedSeedVariant(const QJSValue &value, int depth, bool *ok)
{
    if (depth > kMaxSeedDepth) {
        *ok = false;
        return QVariant{};
    }
    // A QObject reached through a seed is not data to serialize (a hook handed the
    // caller, say). It converts to nothing, which the object check downstream reports.
    if (value.isQObject() || value.isCallable()) {
        return QVariant{};
    }
    if (value.isArray()) {
        QVariantList list{};
        const int length{value.property(QStringLiteral("length")).toInt()};
        for (int index{0}; index < length; ++index) {
            list.append(boundedSeedVariant(value.property(static_cast<quint32>(index)),
                                           depth + 1, ok));
            if (!*ok) {
                return QVariant{};
            }
        }
        return list;
    }
    if (value.isObject()) {
        QVariantMap map{};
        QJSValueIterator iterator{value};
        while (iterator.hasNext()) {
            iterator.next();
            map.insert(iterator.name(), boundedSeedVariant(iterator.value(), depth + 1, ok));
            if (!*ok) {
                return QVariant{};
            }
        }
        return map;
    }
    return value.toVariant();
}

} // namespace

QString WebEdge::seedFor(const QString &route, const QVariantMap &parameters, Caller *caller)
{
    // Installed once on the shared service, but handed the calling connection's own
    // Caller on every call: read the argument, never capture or cache one, or one
    // browser's authorization would answer for another's.
    const auto entry{m_pageSeedHooks.constFind(route)};
    if (entry == m_pageSeedHooks.constEnd()) {
        return QString{};
    }
    QVariant result{};
    if (!QMetaObject::invokeMethod(entry->object, "seedFor", Qt::DirectConnection,
                                   Q_RETURN_ARG(QVariant, result),
                                   Q_ARG(QVariant, QVariant{route}),
                                   Q_ARG(QVariant, QVariant{parameters}),
                                   Q_ARG(QVariant, QVariant::fromValue(
                                       static_cast<QObject *>(caller))))) {
        warnAboutSeedOnce(route, "could not be called");
        return QString{};
    }
    // A QML function returning an object literal comes back wrapped in a QJSValue, which
    // QJsonDocument::fromVariant() knows nothing about. Unwrap it to plain containers
    // first (bounded, see boundedSeedVariant), or every hook would silently seed nothing.
    if (result.canConvert<QJSValue>()) {
        bool withinDepth{true};
        result = boundedSeedVariant(result.value<QJSValue>(), 0, &withinDepth);
        if (!withinDepth) {
            warnAboutSeedOnce(route, "returned a seed nested deeper than a seed may be");
            return QString{};
        }
    }
    // The client reads a seed as a JSON object. Anything else (an array, a string, a
    // number, nothing at all) would arrive as an empty one, so say so rather than let an
    // author debug a page that paints blank for no stated reason.
    const QJsonDocument document{QJsonDocument::fromVariant(result)};
    if (!document.isObject()) {
        warnAboutSeedOnce(route, "did not return an object");
        return QString{};
    }
    const QByteArray json{document.toJson(QJsonDocument::Compact)};
    if (json.size() > kMaxSeedBytes) {
        warnAboutSeedOnce(route, "returned a seed larger than a seed may be");
        return QString{};
    }
    // Whatever the hook returns goes to the browser, verbatim.
    return QString::fromUtf8(json);
}

void WebEdge::warnAboutSeedOnce(const QString &route, const char *reason)
{
    // Once per route, never once per request. A browser chooses how often it asks for a
    // page, so a per-request diagnostic is an unbounded log it can grow on demand.
    const auto entry{m_pageSeedHooks.find(route)};
    if (entry == m_pageSeedHooks.end() || entry->warned) {
        return;
    }
    entry->warned = true;
    qWarning("SynQt: page seed hook %s (route %s) %s; the page is delivered with no seed",
             qUtf8Printable(entry->file), qUtf8Printable(route), reason);
}

void WebEdge::buildPageSeedHooks()
{
    // The app-facing page seed hook, built the way the identity mapping hook is
    // (identityprovider.cpp), the app writes a PageSeed QML object carrying
    // `function seedFor(route, parameters, caller)`, and the edge calls it after the
    // route's scope check. Each hook is built once here, never per request and never per
    // connection. A project whose routes declare no seed builds nothing at all.
    qmlRegisterType<PageSeed>("SynQt", 1, 0, "PageSeed");
    for (const WebEdgePage &page : m_config.pages) {
        if (page.seed.isEmpty()) {
            continue;
        }
        if (!m_engine) {
            qWarning("SynQt: no QML engine, so the page seed hook %s is not loaded",
                     qUtf8Printable(page.seed));
            continue;
        }
        QQmlComponent *component{
            new QQmlComponent{m_engine, QUrl::fromLocalFile(page.seed), this}};
        // Asked before creating: create() on a component that failed to compile prints its
        // own "Component is not ready" first, which names neither the file nor the reason
        // the message below does.
        QObject *hook{component->isReady() ? component->create() : nullptr};
        if (!hook) {
            // The hook's own file and QML diagnostic, which the developer wrote. Never
            // the page's source, and never anything the hook could have read.
            qWarning("SynQt: page seed hook %s failed to load: %s",
                     qUtf8Printable(page.seed), qUtf8Printable(component->errorString()));
            continue;
        }
        // Probed here, once, rather than left to fail per request: Qt logs its own "no
        // such method" complaint on every failed invokeMethod, and a browser decides how
        // often it asks for a page. A hook that cannot answer is not kept.
        const SeedForSupport support{seedForSupport(hook)};
        if (support == SeedForSupport::Typed) {
            // The single most likely mistake. The edge calls seedFor with untyped
            // (QVariant) arguments, so a hook that annotates a parameter can never be
            // reached. Say exactly that, once, instead of leaving Qt to log a generic
            // "no such method" on every request the browser makes.
            qWarning("SynQt: page seed hook %s declares seedFor with typed parameters; the "
                     "edge calls it with untyped (QVariant) arguments, so leave seedFor's "
                     "parameters untyped or the page is delivered with no seed",
                     qUtf8Printable(page.seed));
            delete hook;
            continue;
        }
        if (support == SeedForSupport::None) {
            qWarning("SynQt: page seed hook %s declares no seedFor(route, parameters, "
                     "caller); the page is delivered with no seed",
                     qUtf8Printable(page.seed));
            delete hook;
            continue;
        }
        hook->setParent(this);
        m_pageSeedHooks.insert(page.path, PageSeedHook{hook, page.seed, false});
    }
    if (m_pageSeedHooks.isEmpty()) {
        return;
    }
    m_pagesService->setSeedProvider([this](const QString &route,
                                           const QVariantMap &parameters,
                                           Caller *caller) -> QString {
        return seedFor(route, parameters, caller);
    });
}

void WebEdge::setContextObject(const QString &name, QObject *object)
{
    m_contextObjects.insert(name, object);
}

quint16 WebEdge::serverPort() const
{
    return m_port;
}

QString WebEdge::originHost() const
{
    // A bind address is not a name. "0.0.0.0" and "::" mean every interface, which is an
    // instruction to the socket and not somewhere a browser can be. An edge that took its
    // default bind for its identity would build "https://0.0.0.0:8443" and then refuse the
    // only origin a visitor can arrive with. localhost is the answer that is true of a
    // wildcard bind on the machine the browser is on, which is what a development run is.
    // A deployment reached at a name says so in `public.origin`, and never gets here.
    static const QStringList wildcards{QStringLiteral("0.0.0.0"), QStringLiteral("::"),
                                       QStringLiteral("0:0:0:0:0:0:0:0")};
    if (m_config.host.isEmpty() || wildcards.contains(m_config.host)) {
        return QStringLiteral("localhost");
    }
    // A literal IPv6 address is bracketed in a URL, and only there. The same string is a
    // bare address everywhere else, so the brackets are added here rather than stored.
    if (m_config.host.contains(QLatin1Char(':'))) {
        return QLatin1Char('[') + m_config.host + QLatin1Char(']');
    }
    return m_config.host;
}

QString WebEdge::httpOrigin() const
{
    if (!m_config.origin.isEmpty()) {
        return m_config.origin;
    }
    const QString scheme{m_config.usesTls() ? QStringLiteral("https") : QStringLiteral("http")};
    return QStringLiteral("%1://%2:%3").arg(scheme, originHost()).arg(m_port);
}

QString WebEdge::wssOrigin() const
{
    const QString scheme{m_config.usesTls() ? QStringLiteral("wss") : QStringLiteral("ws")};
    if (!m_config.origin.isEmpty()) {
        // One origin, said once. The sync endpoint is the same host and port as the page,
        // so the WebSocket origin is the declared one with its scheme swapped rather than
        // a second value that can disagree with it.
        QString sync{m_config.origin};
        const qsizetype separator{sync.indexOf(QLatin1String("://"))};
        return separator < 0 ? sync : scheme + sync.mid(separator);
    }
    return QStringLiteral("%1://%2:%3").arg(scheme, originHost()).arg(m_port);
}

QString WebEdge::peerKey(const QString &address, quint16 port)
{
    return address + QLatin1Char(':') + QString::number(port);
}

QStringList WebEdge::expandedAllowedOrigins() const
{
    QStringList result;
    for (const QString &origin : m_config.allowedOrigins) {
        result.append(origin == QLatin1String("self") ? httpOrigin() : origin);
    }
    return result;
}

QByteArray WebEdge::computeCsp() const
{
    // Compute rather than emit raw: append the sync endpoint's explicit wss origin to
    // connect-src (some browsers do not extend 'self' to WebSocket schemes), and add
    // worker-src 'self' blob: under cross-origin isolation (see the blob: note below).
    const QByteArray syncOrigin{wssOrigin().toUtf8()};
    QList<QByteArray> directives;
    bool sawConnectSrc{false};
    bool sawWorkerSrc{false};
    const QList<QByteArray> parts{m_config.csp.toUtf8().split(';')};
    for (QByteArray directive : parts) {
        directive = directive.trimmed();
        if (directive.isEmpty()) {
            continue;
        }
        if (directive.startsWith("connect-src")) {
            directive += ' ' + syncOrigin;
            sawConnectSrc = true;
        } else if (directive.startsWith("worker-src")) {
            sawWorkerSrc = true;
        } else if (directive.startsWith("script-src")) {
            // Allow the bundle's inline loader scripts by hash, keeping the strict CSP
            // (no 'unsafe-inline'). The Qt WebAssembly loader ships an inline bootstrap.
            for (const QByteArray &hash : m_scriptHashes) {
                directive += " 'sha256-" + hash + '\'';
            }
        }
        directives.append(directive);
    }
    if (!sawConnectSrc) {
        directives.append("connect-src 'self' " + syncOrigin);
    }
    if (!sawWorkerSrc) {
        // 'self' is what the shell cache's service worker needs, and what the pinned kit's
        // pthread workers are spawned from. Measured on the real threaded bundle
        // (Qt 6.11.1, Emscripten 4.0.7), a strict worker-src 'self' with no blob: kept the
        // page isolated, spawned every pthread worker, and logged no CSP violation in
        // Chromium, Firefox, or WebKit. blob: is kept as a margin for a future emsdk that
        // goes back to blob: workers, and it costs close to nothing (constructing a blob:
        // worker already needs script execution, which script-src governs). See
        // <https://synqt.org/csp/>.
        //
        // Naming the directive at all is for explicitness rather than permission (worker-src
        // already falls back through child-src to script-src 'self'), but it documents the
        // policy and survives a project that narrows child-src.
        if (m_config.crossOriginIsolation) {
            directives.append(QByteArrayLiteral("worker-src 'self' blob:"));
        } else if (m_config.serviceWorker) {
            directives.append(QByteArrayLiteral("worker-src 'self'"));
        }
    }
    return directives.join("; ");
}

namespace {

/// The password gate's budget: attempts per visitor address, and how long a window lasts.
/// Generous for somebody typing a password and mistyping it, far below what a guesser
/// needs, and low enough that the derivations behind them cannot fill the event loop.
constexpr int kMaxSignInsPerWindow{10};
constexpr qint64 kSignInWindowMs{60 * 1000};
/// How many addresses the window table may name before it is dropped and started again.
constexpr int kMaxRateEntries{4096};

} // namespace

/// The password gate an entity serves for its own people (`signInPath`).
///
/// On success the caller's existing session is elevated rather than replaced, which is what
/// makes the delivery gate work. They already hold a session cookie from fetching the
/// sign-in page, and raising its scope means the next request for the same URL resolves to
/// a different bundle. `setScope` rotates the credential, so the new one is handed back.
///
/// One answer for every failure. Saying which half was wrong tells whoever is guessing
/// which names exist.
QHttpServerResponse WebEdge::handleSignIn(const QHttpServerRequest &request)
{
    // Budgeted before the password is so much as read, so a refusal here says nothing about
    // the credential and costs nothing to give. See m_signInRate: what is being rationed is
    // the PBKDF2 below as much as the guess in front of it.
    const QString visitor{m_clientAddress.resolve(request.remoteAddress(),
                                                  request.value("X-Forwarded-For"))};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const auto refuse{[this](qint64 retryAfterMs) {
        QHttpServerResponse response{QByteArrayLiteral("text/plain"),
                                     QByteArrayLiteral("slow down"),
                                     QHttpServerResponder::StatusCode::TooManyRequests};
        QHttpHeaders headers{response.headers()};
        headers.append(QHttpHeaders::WellKnownHeader::RetryAfter,
                       QByteArray::number(qMax(qint64{1}, (retryAfterMs + 999) / 1000)));
        response.setHeaders(std::move(headers));
        emit signInRefused(QString{});
        return response;
    }};

    // The table's own ceiling, before this request is counted into it and before anything
    // holds a reference into it. Both halves of that matter. QHash::erase moves the entries
    // that follow the one it removes, so a reference taken from operator[] does not survive
    // a prune, and a gate is a poor place to leave that waiting for somebody.
    //
    // What is dropped is only the windows that have run out. When that frees nothing the
    // gate refuses for the rest of the minute. That has an availability cost and it is the
    // right way round. Emptying the table instead would let a flood of throwaway addresses
    // hand the address doing the guessing a fresh ten attempts, and a password gate that can
    // be brute-forced is worse than one that four thousand simultaneous visitors can make
    // briefly unavailable.
    if (pruneRateWindows(m_signInRate, now, kSignInWindowMs, kMaxRateEntries)) {
        return refuse(kSignInWindowMs);
    }

    RateWindow &window{m_signInRate[visitor]};
    if (now - window.startedMs > kSignInWindowMs) {
        window.startedMs = now;
        window.count = 0;
    }
    if (++window.count > kMaxSignInsPerWindow) {
        return refuse(window.startedMs + kSignInWindowMs - now);
    }

    const QUrlQuery form{QString::fromUtf8(request.body())};
    const QString name{form.queryItemValue(QStringLiteral("name"),
                                           QUrl::FullyDecoded)};
    const QString password{form.queryItemValue(QStringLiteral("password"),
                                               QUrl::FullyDecoded)};
    if (name.isEmpty() || password.isEmpty() || !m_config.signIn(name, password)) {
        emit signInRefused(name);
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("no"),
                                   QHttpServerResponder::StatusCode::Unauthorized};
    }
    const QByteArray presented{sessionIdFromCookie(request.value("Cookie"))};
    QByteArray elevated{m_sessionManager->setScope(presented, m_config.signInScope)};
    if (elevated.isEmpty()) {
        // No live session to raise: they arrived without one, which a browser that fetched
        // the page would not have done, but a script might. Give them one at the scope they
        // just proved they hold.
        elevated = m_sessionManager->createSession(m_config.signInScope);
    }
    QHttpServerResponse response{QByteArrayLiteral("text/plain"), QByteArrayLiteral("ok")};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookieFor(elevated));
    response.setHeaders(std::move(headers));
    emit signInAccepted(name);
    return response;
}

QByteArray WebEdge::issueSessionCookie()
{
    return cookieFor(m_sessionManager->createSession());
}

QByteArray WebEdge::sessionCookieFor(const QHttpServerRequest &request)
{
    const QByteArray presented{sessionIdFromCookie(request.value("Cookie"))};
    if (m_sessionManager->isLive(presented)) {
        return QByteArray{};  // it holds a live session; leave the one it has alone
    }
    // The one case where a browser holding a dead id is not a browser starting over: its
    // session was rotated under it by a scope change (Caller.setScope in a slot), which no
    // slot call can put in a cookie. Hand it the id its session became, rather than a new
    // anonymous session, which would sign a signed-in visitor out on their next reload.
    if (const QByteArray rotated{m_sessionManager->rotationOf(presented)}; !rotated.isEmpty()) {
        return cookieFor(rotated);
    }
    return issueSessionCookie();
}

QByteArray WebEdge::cookieFor(const QByteArray &token)
{
    QByteArray cookie{m_config.cookieName.toUtf8() + "=" + token + "; HttpOnly; Path=/"};
    if (m_config.originModel == QLatin1String("split_origin")) {
        // No `Partitioned` (CHIPS), deliberately, and this is measured rather than assumed:
        // tests/split-origin proves that a partitioned cookie survives third-party cookie
        // restriction but loses the login, because the OAuth callback is a top-level
        // navigation onto the edge and the cookie lands under the edge's own partition, which
        // the client site cannot read. Adding the attribute would trade a path that works
        // today for one that fails today. See tests/split-origin/README.md for the table and
        // for the callback redesign that would make CHIPS adoptable.
        cookie += "; SameSite=None; Secure";
    } else {
        cookie += "; SameSite=Lax";
        if (m_config.usesTls()) {
            cookie += "; Secure";
        }
    }
    return cookie;
}

QByteArray WebEdge::sessionIdFromCookie(const QByteArray &cookieHeader) const
{
    const QByteArray prefix{m_config.cookieName.toUtf8() + "="};
    const QList<QByteArray> parts{cookieHeader.split(';')};
    for (QByteArray part : parts) {
        part = part.trimmed();
        if (part.startsWith(prefix)) {
            return part.mid(prefix.size());
        }
    }
    return QByteArray{};
}

void WebEdge::stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response)
{
    QHttpHeaders headers{response.headers()};
    headers.append(QByteArrayLiteral("Content-Security-Policy"), computeCsp());
    if (m_config.crossOriginIsolation) {
        headers.append(QByteArrayLiteral("Cross-Origin-Opener-Policy"),
                       QByteArrayLiteral("same-origin"));
        headers.append(QByteArrayLiteral("Cross-Origin-Embedder-Policy"),
                       QByteArrayLiteral("require-corp"));
    }
    if (m_config.usesTls()) {
        headers.append(QByteArrayLiteral("Strict-Transport-Security"),
                       QByteArrayLiteral("max-age=63072000"));
    }
    headers.append(QByteArrayLiteral("X-Content-Type-Options"), QByteArrayLiteral("nosniff"));
    headers.append(QByteArrayLiteral("Referrer-Policy"), QByteArrayLiteral("same-origin"));

    // Cache headers for the bundle. no-cache is "revalidate", not "do not store": the
    // browser keeps the bytes and spends one conditional GET to confirm them, which is
    // what turns a repeat visit into a 304 instead of a full download. It is also what
    // stops a browser pinning a stale service worker.
    const QString requested{bundlePathFor(bundleFor(request), request.url().path())};
    if (!requested.isEmpty()) {
        const QByteArray etag{etagFor(requested)};
        if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
            headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
        }
        headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                       QByteArrayLiteral("no-cache"));
    }

    // Issue a session on the page load, so the browser has a credential to present at the
    // wss upgrade. Only for a browser that arrives without a live one: a page load is not
    // a new visitor. Re-issuing unconditionally would replace the credential a visitor
    // has signed in with (the OAuth callback redirects onto this very route, so the
    // landing load would sign them straight back out), and would let one browser mint an
    // unbounded number of sessions by reloading.
    if (request.url().path() == m_config.clientRoute) {
        const QByteArray cookie{sessionCookieFor(request)};
        if (!cookie.isEmpty()) {
            headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
        }
    }
    response.setHeaders(std::move(headers));
}

void WebEdge::cacheBundle()
{
    m_etags.clear();
    // Every bundle this edge may serve, not just one: the table is keyed by canonical
    // absolute path, so two roots holding a file of the same name never collide.
    for (const QString &bundle : std::as_const(m_config.bundles)) {
        const QDir root{bundle};
        const QFileInfoList entries{root.entryInfoList(QDir::Files | QDir::NoSymLinks)};
        for (const QFileInfo &entry : entries) {
        // A precompressed variant is the same resource under a different encoding, so it
        // shares the identity of the file it encodes and is never requested directly.
        if (entry.fileName().endsWith(QLatin1String(".br"))
            || entry.fileName().endsWith(QLatin1String(".gz"))) {
            continue;
        }
        QFile file{entry.absoluteFilePath()};
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QCryptographicHash hash{QCryptographicHash::Sha256};
        if (!hash.addData(&file)) {
            continue;
        }
            m_etags.insert(entry.canonicalFilePath(),
                           '"' + hash.result().toHex().left(32) + '"');
        }
    }
}

QByteArray WebEdge::etagFor(const QString &path) const
{
    return m_etags.value(QFileInfo{path}.canonicalFilePath());
}

QString WebEdge::bundleForScope(const QString &scope) const
{
    const QString fallback{m_config.bundles.value(m_config.defaultScope)};
    if (scope.isEmpty()) {
        return fallback;
    }
    const QString exact{m_config.bundles.value(scope)};
    if (!exact.isEmpty()) {
        return exact;
    }
    // Hierarchical scopes rank, so a scope with no bundle of its own is served the nearest
    // one below it. That is what lets a project declare two bundles instead of one per
    // scope. Set-based scopes do not rank at all, so there is no "below" to walk and an
    // unmapped scope takes the default scope's bundle.
    if (!m_config.scopesHierarchical) {
        return fallback;
    }
    const qsizetype rank{m_config.scopeOrder.indexOf(scope)};
    if (rank < 0) {
        return fallback;
    }
    for (qsizetype index{rank - 1}; index >= 0; --index) {
        const QString candidate{m_config.bundles.value(m_config.scopeOrder.at(index))};
        if (!candidate.isEmpty()) {
            return candidate;
        }
    }
    return fallback;
}

QString WebEdge::bundleFor(const QHttpServerRequest &request) const
{
    const QByteArray sessionId{sessionIdFromCookie(request.value("Cookie"))};
    const SessionRecord *record{m_sessionManager->lookup(sessionId)};
    return bundleForScope(record ? record->scope : QString{});
}

QString WebEdge::bundlePathFor(const QString &root, const QString &urlPath) const
{
    if (urlPath == m_config.clientRoute) {
        return QDir{root}.filePath(QStringLiteral("index.html"));
    }
    const QString name{urlPath.mid(1)};
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        return {};
    }
    const QString resolved{QFileInfo{QDir{root}, name}.canonicalFilePath()};
    // Membership of the ETag table is no longer enough: it now holds every bundle's
    // files, so a path has to be inside the bundle this caller was served as well.
    const QString canonicalRoot{QDir{root}.canonicalPath()};
    if (canonicalRoot.isEmpty() || !resolved.startsWith(canonicalRoot + QLatin1Char('/'))) {
        return {};
    }
    return m_etags.contains(resolved) ? resolved : QString{};
}

QHttpServerResponse WebEdge::shellOrNotFound(const QString &root, const QString &path,
                                             const QHttpServerRequest &request)
{
    // Only a navigation gets the shell. A POST or a DELETE to an unknown URL is a
    // client bug or a probe, and answering it with HTML would hide that.
    if (request.method() != QHttpServerRequest::Method::Get
        && request.method() != QHttpServerRequest::Method::Head) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
    }
    // An asset request (its last segment has an extension) must fail as a 404 rather
    // than receive HTML with a 200, which would surface as a confusing module-load
    // error instead of a missing file.
    const qsizetype lastSlash{path.lastIndexOf(QLatin1Char('/'))};
    if (path.mid(lastSlash + 1).contains(QLatin1Char('.'))) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
    }
    const QString index{QDir{root}.filePath(QStringLiteral("index.html"))};
    if (auto notModified{notModifiedFor(request, etagFor(index))}) {
        stampShell(root, *notModified, request);
        return std::move(*notModified);
    }
    QHttpServerResponse response{QHttpServerResponse::fromFile(index)};
    stampShell(root, response, request);
    return response;
}

void WebEdge::stampShell(const QString &root, QHttpServerResponse &response,
                         const QHttpServerRequest &request)
{
    // A deep link is a cold visitor's first page load just as often as "/" is, so it
    // has to leave with the same two things the client route's response leaves with.
    // stampResponse() cannot do it: it sees only the request, and by URL a deep link is
    // indistinguishable from a 404. Here the response IS index.html, by construction.
    //
    // Without the cookie the client has no credential at the wss upgrade, verifyUpgrade
    // answers 401, and the app reconnects forever on a page that loaded perfectly.
    // Without the cache terms an intermediary may pin a loader the deploy replaced.
    //
    // Never reached for m_config.clientRoute (that route is registered first and
    // answers it), so this cannot double the Set-Cookie stampResponse() issues there.
    const QString index{QDir{root}.filePath(QStringLiteral("index.html"))};
    const QByteArray etag{etagFor(index)};
    QHttpHeaders headers{response.headers()};
    if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
        headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
    }
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                   QByteArrayLiteral("no-cache"));
    // On the same terms as the client route (see stampResponse), only a browser arriving
    // without a live session is given one, so a refresh deep in the app never replaces
    // the credential the visitor signed in with.
    const QByteArray cookie{sessionCookieFor(request)};
    if (!cookie.isEmpty()) {
        headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
    }
    response.setHeaders(std::move(headers));
}

void WebEdge::computeScriptHashes()
{
    m_scriptHashes.clear();
    // The union across bundles, not a set per bundle. Response stamping runs as an
    // after-request handler, where the bundle that produced the response is no longer in
    // hand, so a per-bundle policy would cost per-request state on the path of every
    // response. Every hash here is of a loader script this build generated, and injecting
    // an inline script matching one would already require controlling a bundle, so the
    // union buys an attacker nothing.
    for (const QString &bundle : std::as_const(m_config.bundles)) {
        collectScriptHashes(QDir{bundle}.filePath(QStringLiteral("index.html")));
    }
}

void WebEdge::collectScriptHashes(const QString &indexPath)
{
    QFile index{indexPath};
    if (!index.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray html{index.readAll()};
    // Inline <script>...</script> blocks (those without a src attribute) need their
    // sha256 in the CSP's script-src so the strict policy still runs the loader.
    static const QRegularExpression scriptTag{
        QStringLiteral("<script(?![^>]*\\bsrc=)[^>]*>(.*?)</script>"),
        QRegularExpression::DotMatchesEverythingOption
            | QRegularExpression::CaseInsensitiveOption};
    QRegularExpressionMatchIterator it{scriptTag.globalMatch(QString::fromUtf8(html))};
    while (it.hasNext()) {
        const QByteArray body{it.next().captured(1).toUtf8()};
        m_scriptHashes.append(
            QCryptographicHash::hash(body, QCryptographicHash::Sha256).toBase64());
    }
}

bool WebEdge::start()
{
    computeScriptHashes();
    cacheBundle();

    // 1. No connect point Source is built here. Every one of them is instantiated per
    //    connection in hostConnection(), so each carries a Caller for its one user, and
    //    anything the connections share belongs to the entity's own singleton, which
    //    outlives all of them (the PageStore below is the framework's own example).
    //
    // 1.5. The framework's own Pages connect point (edge-delivered pages): one
    //      PageStore/PagesService shared by every connection, since the page table
    //      is the same for everyone. Built once, here, and never rebuilt per
    //      connection. A per-connection PagesEdgeSource is created in
    //      hostConnection() so each carries its own Caller. Nothing is created when
    //      the project configures no pages, so that app pays nothing.
    if (!m_config.pages.isEmpty()) {
        m_pageStore = new PageStore{m_config.pagesDir, this};
        for (const WebEdgePage &page : m_config.pages) {
            m_pageStore->addPage(page.path, page.file, page.scope, page.graphics);
        }
        // Development-only watching, keyed to an explicit dev flag. Only the
        // "synqt dev" launch path (dev_command() in tools/synqt/synqt/run.py, via the
        // generated edge's --dev option) sets devWatch. A built or served edge leaves
        // it false and never watches. Deriving "development" from the absence of local
        // TLS would be wrong. A production edge that terminates TLS at a reverse proxy
        // and speaks plaintext on the loopback hop has no local cert yet is not dev.
        if (m_config.devWatch) {
            m_pageStore->setWatching(true);
        }
        m_pagesService = new PagesService{m_pageStore, this};
        buildPageSeedHooks();
    }

    // 2. The HTTP server: serve the bundle, stamp headers, and verify upgrades.
    m_httpServer = new QHttpServer{this};

    // Qt's own limits on the request, applied before anything of ours runs. Set here rather
    // than left at their defaults because Qt picks for a general-purpose server: a 32 MiB
    // body ceiling is right for one that receives uploads and generous for one whose own
    // routes carry a token and a password field. The idle timeout is load-bearing rather
    // than housekeeping, since it is what closes a peer that sends half a request and stops
    // (docs/security.md says which of these covers what). Rate limiting stays off unless a
    // project asks: Qt counts the peer address, which is the balancer's on every deployment
    // that has one.
    QHttpServerConfiguration httpConfiguration;
    httpConfiguration.setKeepAliveTimeout(
        std::chrono::seconds{m_config.keepAliveTimeoutSeconds});
    httpConfiguration.setMaximumBodySize(m_config.maxBodyBytes);
    if (m_config.maxRequestsPerSecond > 0) {
        httpConfiguration.setRateLimitPerSecond(m_config.maxRequestsPerSecond);
    }
    m_httpServer->setConfiguration(httpConfiguration);
    if (m_config.serveClient) {
        m_httpServer->route(m_config.clientRoute, [this](const QHttpServerRequest &request) {
            const QString index{QDir{bundleFor(request)}
                                    .filePath(QStringLiteral("index.html"))};
            if (auto notModified{notModifiedFor(request, etagFor(index))}) {
                return std::move(*notModified);
            }
            return QHttpServerResponse::fromFile(index);
        });
    } else {
        // A CDN delivers the bundle, so this route delivers the one thing only this origin
        // can. The session. Without it a browser that loaded the app elsewhere reaches the
        // upgrade with no credential and is refused, which looks like a broken app rather
        // than a missing request.
        m_httpServer->route(m_config.clientRoute, [this](const QHttpServerRequest &request) {
            return credentialResponse(request);
        });
    }

    // Login/callback/logout. The whole OAuth flow runs here, on the edge. The browser
    // ends with only a session cookie. The client secret and tokens never leave.
    if (m_config.identity.enabled) {
        CookiePolicy cookie;
        cookie.name = m_config.cookieName;
        cookie.sameSiteNone = (m_config.originModel == QLatin1String("split_origin"));
        cookie.secure = m_config.usesTls();
        m_identity = new IdentityProvider{m_config.identity, m_sessionManager, m_engine,
                                          httpOrigin(), cookie, this};
        // One notion of "which address is the visitor" for the whole edge. The device
        // route rate-limits on it exactly as the upgrade verifier caps on it, and two
        // answers to that question is how one of them ends up being the balancer's.
        m_identity->setClientAddress(&m_clientAddress);
        // A session that runs out of time takes its server-side tokens with it. Logging
        // out already released them; almost nobody logs out.
        connect(m_sessionManager, &SessionManager::sessionExpired, m_identity,
                [this](const QString &token) { m_identity->forgetSession(token.toLatin1()); });
        m_httpServer->route(m_config.identity.loginRoute,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleLogin(request);
        });
        m_httpServer->route(m_config.identity.callbackRoute,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleCallback(request);
        });
        m_httpServer->route(m_config.identity.logoutRoute,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleLogout(request);
        });
        // The desktop half. POST only, so the code and its verifier stay out of request
        // logs, out of the browser's address bar, and out of any cache. The handler itself
        // refuses everything unless the project builds a desktop client.
        m_httpServer->route(m_identity->claimRoute(), QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleClaim(request);
        });
        // Staying signed in. The same shape, spending a credential the client stored at its
        // last launch instead of a code its browser carried. Registered unconditionally
        // and refused inside, so a project that persists nothing answers it the way it
        // answers any other path it does not serve.
        m_httpServer->route(m_identity->deviceRoute(), QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleDevice(request);
        });
    }
    // The monitor's operator gate. POST only, so a password never lands in a request log,
    // in an address bar, or in a cache.
    if (!m_config.signInPath.isEmpty() && m_config.signIn) {
        m_httpServer->route(m_config.signInPath, QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return handleSignIn(request);
        });
    }
    // Delivery of the bundle itself, only when this edge is the app's origin.
    if (m_config.serveClient) {
        registerBundleRoutes();
    }

    // Ending a session ends the connections it authorized. Both signals, because a session
    // ends in two ways and only one of them is somebody pressing sign out: `sessionRemoved`
    // is revocation (and the rotation that dropSession reads and ignores), `sessionExpired`
    // is the TTL sweep. Wired here rather than beside the identity provider, because a
    // project with no sign-in at all still revokes and still expires.
    connect(m_sessionManager, &SessionManager::sessionRemoved, this,
            [this](const QString &token) { dropSession(token.toLatin1()); });
    connect(m_sessionManager, &SessionManager::sessionExpired, this,
            [this](const QString &token) { dropSession(token.toLatin1()); });

    m_httpServer->addAfterRequestHandler(
        this, [this](const QHttpServerRequest &request, QHttpServerResponse &response) {
            stampResponse(request, response);
        });
    m_httpServer->addWebSocketUpgradeVerifier(this, &WebEdge::verifyUpgrade);
    connect(m_httpServer, &QHttpServer::newWebSocketConnection,
            this, &WebEdge::onNewWebSocketConnection);

    // 3. The public transport: TLS by default (a QSslServer bound to QHttpServer), with
    //    connection tracking for the handshake timeout.
    if (m_config.usesTls()) {
        QSslServer *sslServer{new QSslServer{this}};
        QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
        configuration.setLocalCertificate(loadCertificate(m_config.certFile));
        configuration.setPrivateKey(loadPrivateKey(m_config.keyFile));
        // The browser presents no client certificate; only the server is authenticated.
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        sslServer->setSslConfiguration(configuration);
        connect(sslServer, &QSslServer::startedEncryptionHandshake, this,
                [this](QSslSocket *socket) { trackPendingUpgrade(socket); });
        m_transportServer = sslServer;
    } else {
        EdgeTcpServer *tcpServer{new EdgeTcpServer{this}};
        tcpServer->onAccepted = [this](QTcpSocket *socket) { trackPendingUpgrade(socket); };
        m_transportServer = tcpServer;
    }

    // The threads accepted sockets are spread across, on an edge that asked for more than
    // one. Built before anything is listening, so no connection can arrive and find the
    // pool half there.
    if (m_config.socketThreads > 1) {
        m_ioThreads = new IoThreadPool{m_config.socketThreads, this};
    }

    if (!m_transportServer->listen(QHostAddress{m_config.host}, m_config.port)) {
        m_errorString = m_transportServer->errorString();
        return false;
    }
    m_port = m_transportServer->serverPort();
    if (m_identity) {
        // The port is known now, so the callback redirect_uri is well-formed.
        m_identity->setEdgeOrigin(httpOrigin());
    }
    if (!m_httpServer->bind(m_transportServer)) {
        m_errorString = QStringLiteral("failed to bind the HTTP server to the transport");
        return false;
    }

    // A shared point's Source is the entity. It holds what outlives any one session, and
    // its `Component.onCompleted` is where the edge subscribes to what it consumes. Built
    // on the first visitor instead, an edge would miss everything a service announced
    // before somebody happened to open the page.
    for (const WebEdgeConnectPoint &connectPoint : std::as_const(m_config.connectPoints)) {
        if (!connectPoint.shared || connectPoint.serverFile.isEmpty()) {
            continue;
        }
        QString error;
        if (sharedSource(connectPoint, &error) == nullptr) {
            m_errorString = error;
            return false;
        }
    }
    return true;
}

void WebEdge::trackPendingUpgrade(QAbstractSocket *socket)
{
    const QString key{peerKey(socket->peerAddress().toString(), socket->peerPort())};
    QTimer *timer{new QTimer{socket}};
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, socket, key]() {
        m_pendingTimers.remove(key);
        emit upgradeRejected(QStringLiteral("handshake timeout"));
        socket->abort();
    });
    // Only a peer that connects and then says nothing is on this clock. The browser fetches
    // the page, the loader and the bundle over the same connection it would upgrade on, so a
    // deadline that outlived the first byte cut ordinary transfers part-way through and
    // recorded a refusal nobody made. What the window still covers is the socket that
    // arrives and stays silent. A peer that speaks and then stalls is left to QHttpServer's
    // own keep-alive timeout, which ends it once it goes idle. One that keeps dribbling
    // never goes idle and is bounded only by the header ceilings, which docs/security.md
    // spells out. The connection caps are not that bound. They are counted when a
    // connection is hosted, and a request that never completes is never hosted. The
    // observer takes itself off after the first byte, because a request body would
    // otherwise run it once per chunk for nothing.
    connect(socket, &QIODevice::readyRead, timer, [socket, timer]() {
        timer->stop();
        disconnect(socket, &QIODevice::readyRead, timer, nullptr);
    });
    // The entry has to go when the socket does (a peer that hangs up mid-handshake never
    // reaches verifyUpgrade), but watching the socket's own destroyed() is what made every
    // closed connection print "wildcard call disconnects from destroyed signal of
    // QTcpSocket": QWebSocketPrivate::releaseConnections() wildcard-disconnects that very
    // socket and Qt warns about any destroyed() connection it finds there. The timer is a
    // child of the socket, so it dies with it and nothing wildcard-disconnects the timer.
    connect(timer, &QObject::destroyed, this, [this, key]() { m_pendingTimers.remove(key); });
    m_pendingTimers.insert(key, timer);
    // Caught here because this is the last place it can be. Once the upgrade is accepted,
    // the QWebSocket on top of this socket does not lead back to it (it is not its child)
    // and there is no other way to ask. A threaded edge has to move both or the connection
    // ends up read on one thread and written on another.
    // A tag whose whole job is to say when this socket is gone. The timeout timer above
    // cannot serve, however tempting. It is deleted the moment a valid upgrade request
    // arrives, which is exactly when the raw socket is still wanted, so hanging the entry
    // off it means every threaded connection quietly falls back to this thread. A plain
    // QObject child dies with the socket instead, and nothing wildcard-disconnects it
    // (see the note on the timer above for why that matters).
    //
    // Remembered on every link and not only a threaded one. A threaded edge needs it to
    // move the connection; every edge needs it to own the connection, because once the
    // upgrade is accepted nothing else does (see carry()).
    QObject *tag{new QObject{socket}};
    connect(tag, &QObject::destroyed, this,
            [this, key]() { m_pendingRawSockets.remove(key); });
    m_pendingRawSockets.insert(key, socket);
    timer->start(m_config.handshakeTimeoutMs);
}

QHttpServerResponse WebEdge::credentialResponse(const QHttpServerRequest &request)
{
    // A CDN delivered the app, so this is the browser's first and only HTTP request to
    // this origin, and the one thing it needs from here is a session. The cookie itself is
    // stamped by stampResponse() on the client route, exactly as it is when this edge
    // serves the page. The body is empty because there is nothing else to say.
    QHttpServerResponse response{QHttpServerResponse::StatusCode::NoContent};

    // The fetch that asks for this carries credentials, so it is only honored for an origin
    // the project listed. `Allow-Origin` echoes the one asking rather than answering `*`,
    // which a credentialed request refuses anyway, and `Vary` keeps a cache from handing
    // one client origin's answer to another.
    const QString origin{QString::fromUtf8(request.value("Origin"))};
    QHttpHeaders headers{response.headers()};
    if (!origin.isEmpty() && expandedAllowedOrigins().contains(origin)) {
        headers.append(QByteArrayLiteral("Access-Control-Allow-Origin"), origin.toUtf8());
        headers.append(QByteArrayLiteral("Access-Control-Allow-Credentials"),
                       QByteArrayLiteral("true"));
    }
    headers.append(QHttpHeaders::WellKnownHeader::Vary, QByteArrayLiteral("Origin"));
    response.setHeaders(std::move(headers));
    return response;
}

void WebEdge::registerBundleRoutes()
{
    // Serve the rest of the bundle (the loader, the .wasm module, assets). Only files
    // that resolve to a real path INSIDE the bundle directory are reachable: reject
    // absolute paths and NUL/backslash, then verify the canonical (symlink- and
    // ..-resolved) path stays under the canonical bundle root.
    //
    // Skipped entirely when a CDN delivers the bundle: an edge that is not the origin of
    // the app has no business serving files, and every path that is not one of its own
    // routes should be a 404 rather than a second copy of what the CDN is authoritative for.
    m_httpServer->route(QStringLiteral("/<arg>"),
                        [this](const QString &asset,
                               const QHttpServerRequest &request) {
        // Resolved per request rather than once at start: which bundle a caller may read
        // from is a property of their session, not of this edge.
        const QString root{bundleFor(request)};
        const QString bundleRoot{QDir{root}.canonicalPath()};
        if (asset.isEmpty() || QDir::isAbsolutePath(asset)
            || asset.contains(QLatin1Char('\0')) || asset.contains(QLatin1Char('\\'))) {
            return QHttpServerResponse{QHttpServerResponse::StatusCode::Forbidden};
        }
        const QString resolved{QFileInfo{QDir{root}, asset}.canonicalFilePath()};
        if (resolved.isEmpty()) {
            // The bundle holds no such file. This route and the shell fallback below
            // share the "/<arg>" template and this one is registered first, so a
            // single-segment client route ("/about") is matched here and would never
            // reach the fallback. Answer it on the fallback's own terms.
            return shellOrNotFound(root, asset, request);
        }
        if (bundleRoot.isEmpty() || !resolved.startsWith(bundleRoot + QLatin1Char('/'))) {
            // It exists, but outside the bundle. Refuse it, and never dress the attempt
            // up as a client route.
            return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
        }
        if (!QFileInfo{resolved}.isFile()) {
            // A directory inside the bundle serves nothing. This route is one segment
            // deep and the ETag cache indexes top-level files only, so nothing under it
            // is reachable anyway. Treating it as "no such asset" is what keeps a client
            // route named after a bundle directory ("/assets") working on refresh, when
            // its neighbors already do. After the containment test, so an attempt to
            // probe outside the bundle is still refused before anything else looks at
            // the path.
            return shellOrNotFound(root, asset, request);
        }
        if (auto notModified{notModifiedFor(request, etagFor(resolved))}) {
            return std::move(*notModified);
        }
        // Serve a precompressed variant when the client accepts it (the build
        // precompresses with Brotli and gzip). The bytes are smaller and the resource
        // still arrives under its own type via Content-Encoding. The wasm dominates the
        // transfer, but the Emscripten glue .js is the next largest on a first visit, so
        // it earns the same treatment.
        const QByteArray mime{bundleContentType(resolved)};
        if (!mime.isEmpty()) {
            const QByteArray accept{request.value("Accept-Encoding")};
            const auto encoded{[&](const char *suffix,
                                   const char *encoding) -> std::optional<QHttpServerResponse> {
                if (!accept.contains(encoding)) {
                    return std::nullopt;
                }
                QFile file{resolved + QLatin1String(suffix)};
                if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
                    return std::nullopt;
                }
                QHttpServerResponse response{mime, file.readAll()};
                QHttpHeaders headers{response.headers()};
                headers.append(QHttpHeaders::WellKnownHeader::ContentEncoding,
                               QByteArray{encoding});
                headers.append(QHttpHeaders::WellKnownHeader::Vary,
                               QByteArrayLiteral("Accept-Encoding"));
                response.setHeaders(std::move(headers));
                return response;
            }};
            if (auto response{encoded(".br", "br")}) {
                return std::move(*response);
            }
            if (auto response{encoded(".gz", "gzip")}) {
                return std::move(*response);
            }
        }
        return QHttpServerResponse::fromFile(resolved);
    });
    // The application shell for any unmatched path, so a deep link or a refresh on
    // "/c/summer-sale" lands on the app instead of a 404.
    //
    // This is deliberately a route and not setMissingHandler(): a missing handler is
    // answered through a QHttpServerResponder, and Qt does not run after-request
    // handlers for those, so the shell would go out with no CSP, COOP, or COEP.
    // Registered last, so every real route above still wins. The parameter is QUrl
    // rather than QString so the "<arg>" placeholder captures a multi-segment
    // remainder ("/a/b/c"), not only one path component.
    m_httpServer->route(QStringLiteral("/<arg>"), QHttpServerRequest::Method::Get
                                                       | QHttpServerRequest::Method::Head,
                        [this](const QUrl &rest, const QHttpServerRequest &request) {
        return shellOrNotFound(bundleFor(request), rest.path(), request);
    });
}

QHttpServerWebSocketUpgradeResponse WebEdge::verifyUpgrade(const QHttpServerRequest &request)
{
    // The upgrade request arrived in time: cancel the handshake-timeout timer.
    const QString key{peerKey(request.remoteAddress().toString(), request.remotePort())};
    if (QTimer *timer{m_pendingTimers.take(key)}) {
        timer->stop();
        timer->deleteLater();
    }

    // 1. Origin check: the primary defense against cross-site WebSocket hijacking.
    const QString origin{QString::fromUtf8(request.value("Origin"))};
    if (!expandedAllowedOrigins().contains(origin)) {
        emit upgradeRejected(QStringLiteral("origin not allowed: %1").arg(origin));
        return QHttpServerWebSocketUpgradeResponse::deny(
            403, QByteArrayLiteral("origin not allowed"));
    }

    // 2. Session credential: the cookie must map to a live session.
    const QByteArray sessionId{sessionIdFromCookie(request.value("Cookie"))};
    if (!m_sessionManager->isLive(sessionId)) {
        emit upgradeRejected(QStringLiteral("no valid session"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            401, QByteArrayLiteral("no valid session"));
    }

    // 3. Scope precondition. An anonymous connection is rejected when identity is required.
    //
    // Anonymous means the session carries no identity, which is what a session created for
    // a visitor who has not signed in carries. Refusing the upgrade on the flag
    // alone, without looking at the session, makes `identity.required: true`
    // refuse everybody, signed in or not, so an app that sets it cannot be connected to
    // at all. A test of the flag that covers only the half that is
    // right (an anonymous visitor is refused) cannot see that, because being refused is
    // also what a broken accept looks like from there.
    if (m_config.identityRequired) {
        const SessionRecord *record{m_sessionManager->lookup(sessionId)};
        if (!record || record->identity.isEmpty()) {
            emit upgradeRejected(QStringLiteral("authentication required"));
            return QHttpServerWebSocketUpgradeResponse::deny(
                403, QByteArrayLiteral("authentication required"));
        }
    }

    // 4. Rate and resource checks. Per-IP and global connection caps.
    //
    // The address the cap counts against is the visitor's, which is the peer's until a
    // deployment names a balancer in front of this edge. Counting the peer there would
    // put every visitor in one bucket, so the cap would either refuse the whole site at
    // the twentieth connection or, raised to compensate, limit nobody.
    const QString ip{m_clientAddress.resolve(request.remoteAddress(),
                                             request.value("X-Forwarded-For"))};
    if (m_activeGlobal >= m_config.maxConnectionsGlobal
        || m_activePerIp.value(ip) >= m_config.maxConnectionsPerIp) {
        emit upgradeRejected(QStringLiteral("connection cap reached"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            503, QByteArrayLiteral("too many connections"));
    }

    // Accepted: stash the verified id by peer so the accepted socket (whose headers are
    // not re-readable) can be bound to its session when it is hosted. Last, after every
    // check, so a refused upgrade leaves nothing behind.
    rememberVerifiedSession(key, sessionId, ip);
    emit upgradeAccepted(key);
    return QHttpServerWebSocketUpgradeResponse::accept();
}

void WebEdge::rememberVerifiedSession(const QString &peer, const QByteArray &sessionId,
                                      const QString &clientIp)
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    // An accepted upgrade is hosted in the same event-loop turn it is accepted in, so
    // anything still here after the handshake window belongs to a socket that never
    // arrived (the peer hung up between the 101 and the first frame). Nothing else would
    // ever remove it, and a peer can repeat that as often as it likes, so the sweep is
    // what keeps the map bounded by the accept rate rather than by the uptime.
    const qint64 staleAfter{qMax(m_config.handshakeTimeoutMs, 1000)};
    for (auto it{m_pendingSessions.begin()}; it != m_pendingSessions.end();) {
        if (now - it->verifiedMs > staleAfter) {
            it = m_pendingSessions.erase(it);
        } else {
            ++it;
        }
    }
    m_pendingSessions.insert(peer, VerifiedSession{sessionId, now, clientIp});
}

QObject *WebEdge::createSource(const WebEdgeConnectPoint &connectPoint, QObject *caller,
                              QObject *parent, QString *error)
{
    // Each Source gets its own QML context so an instance can see its own Caller
    // (and its Client alias) and the edge's consumed-mesh accessors (Database, ...).
    QQmlContext *context{new QQmlContext{m_engine->rootContext(), parent}};
    if (caller) {
        context->setContextProperty(QStringLiteral("Caller"), caller);
        context->setContextProperty(QStringLiteral("Client"), caller);  // browser-user alias
    }
    for (auto it{m_contextObjects.constBegin()}; it != m_contextObjects.constEnd(); ++it) {
        context->setContextProperty(it.key(), it.value());
    }
    QQmlComponent component{m_engine, QUrl::fromLocalFile(connectPoint.serverFile)};
    // Asked before creating: create() on a component that failed to compile prints its own
    // "Component is not ready" first, which says less than the error built below.
    QObject *source{component.isReady() ? component.create(context) : nullptr};
    if (!source) {
        if (error) {
            *error = QStringLiteral("failed to load %1: %2")
                         .arg(connectPoint.serverFile, component.errorString());
        }
        return nullptr;
    }
    source->setParent(parent);
    context->setParent(source);
    return source;
}

QObject *WebEdge::sharedSource(const WebEdgeConnectPoint &connectPoint, QString *error)
{
    SharedSource &entry{m_sharedSources[connectPoint.name]};
    if (entry.source) {
        return entry.source;
    }
    // The Caller in a shared Source's context starts as nobody and is made to be whoever is
    // calling, one forwarded call at a time (SynQt::Caller::adopt). It is minted here rather
    // than in a mirror so the QML context that names it is built once, with the Source.
    Caller *caller{Caller::forUser(connectPoint.contract, m_sessionManager, QByteArray{},
                                   nullptr, this)};
    caller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
    QObject *source{createSource(connectPoint, caller, this, error)};
    if (!source) {
        delete caller;
        m_sharedSources.remove(connectPoint.name);
        return nullptr;
    }
    caller->setParent(source);
    SourceFactory::bindCaller(source, caller);
    // This one holds the state every mirror publishes from, for every session at once, so
    // it is the one Source that does not apply a `<scope>` gate. The mirrors do, each for
    // the session it answers.
    SourceFactory::holdsSharedState(source);
    entry.source = source;
    entry.caller = caller;
    return source;
}

QObject *WebEdge::sourceForConnection(const WebEdgeConnectPoint &connectPoint,
                                      const QByteArray &sessionId, QObject *connection,
                                      QString *error)
{
    // An anonymous browser holds no session, so there is nothing to key a continuing Source
    // on: it gets one per connection, parented to the connection, and nothing is remembered.
    if (sessionId.isEmpty()) {
        Caller *caller{Caller::forUser(connectPoint.contract, m_sessionManager, sessionId,
                                       nullptr, connection)};
        caller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
        QObject *source{!connectPoint.behind.isEmpty()
                            ? relayFor(connectPoint, caller, connection, error)
                        : connectPoint.shared
                            ? mirrorFor(connectPoint, caller, connection, error)
                            : createSource(connectPoint, caller, connection, error)};
        if (source) {
            caller->setParent(source);
            caller->setSource(source);
            SourceFactory::bindCaller(source, caller);
        } else {
            delete caller;
        }
        return source;
    }

    SessionSources &sources{m_sessionSources[sessionId]};
    if (QObject *existing{sources.byConnectPoint.value(connectPoint.name)}) {
        return existing;
    }
    // Parented to the edge rather than to the socket. It outlives this connection, and
    // releaseSessionSources() is what ends it. The Caller is minted once with it and holds
    // the session, so `Caller.emitSignal` reaches every tab of that session, which is what
    // makes an answer arrive in the tab that did not ask.
    Caller *caller{Caller::forUser(connectPoint.contract, m_sessionManager, sessionId,
                                   nullptr, this)};
    caller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
    QObject *source{!connectPoint.behind.isEmpty()
                        ? relayFor(connectPoint, caller, this, error)
                    : connectPoint.shared
                        ? mirrorFor(connectPoint, caller, this, error)
                        : createSource(connectPoint, caller, this, error)};
    if (!source) {
        delete caller;
        return nullptr;
    }
    caller->setParent(source);
    caller->setSource(source);
    SourceFactory::bindCaller(source, caller);
    sources.byConnectPoint.insert(connectPoint.name, source);
    return source;
}

void WebEdge::setEntityBehind(const QString &entity, QObject *replica)
{
    m_entitiesBehind.insert(entity, replica);
}

QString WebEdge::entityFor(const WebEdgeConnectPoint &connectPoint,
                           const QString &scope) const
{
    if (const QString named{connectPoint.behind.value(scope)}; !named.isEmpty()) {
        return named;
    }
    if (!m_config.scopesHierarchical) {
        // Set-based. A caller holds exactly one scope and there is no order to fall back
        // along, so a scope nobody wrote a line for is served by nobody.
        return QString{};
    }
    const qsizetype held{m_config.scopeOrder.indexOf(scope)};
    if (held < 0) {
        return QString{};
    }
    // The highest tier at or below what they hold, so a scope with no line of its own still
    // lands somewhere, and always below itself rather than above.
    QString best;
    qsizetype highest{-1};
    for (auto tier{connectPoint.behind.constBegin()};
         tier != connectPoint.behind.constEnd(); ++tier) {
        const qsizetype rank{m_config.scopeOrder.indexOf(tier.key())};
        if (rank >= 0 && rank <= held && rank > highest) {
            best = tier.value();
            highest = rank;
        }
    }
    return best;
}

QObject *WebEdge::relayFor(const WebEdgeConnectPoint &connectPoint, Caller *caller,
                           QObject *parent, QString *error)
{
    const QString entity{entityFor(connectPoint, caller->scope())};
    if (entity.isEmpty()) {
        if (error) {
            *error = QStringLiteral("%1: nothing behind it serves scope '%2'")
                         .arg(connectPoint.name, caller->scope());
        }
        return nullptr;
    }
    QObject *behind{m_entitiesBehind.value(entity).data()};
    if (!behind) {
        if (error) {
            *error = QStringLiteral("%1: '%2' is not reachable yet")
                         .arg(connectPoint.name, entity);
        }
        return nullptr;
    }
    // Built from the C++ helper rather than loaded from QML: a front owns this point and
    // implements none of it, so there is no server file to load and nothing for one to say.
    QObject *source{SourceFactory::create(connectPoint.contract, parent)};
    if (!source) {
        if (error) {
            *error = QStringLiteral("no Source registered for contract %1")
                         .arg(connectPoint.contract);
        }
        return nullptr;
    }
    caller->setSource(source);
    SourceFactory::bindCaller(source, caller);
    SourceFactory::relay(source, behind);
    return source;
}

QObject *WebEdge::mirrorFor(const WebEdgeConnectPoint &connectPoint, Caller *caller,
                            QObject *parent, QString *error)
{
    QObject *shared{sharedSource(connectPoint, error)};
    if (!shared) {
        return nullptr;
    }
    QObject *mirror{SourceFactory::create(connectPoint.contract, parent)};
    if (!mirror) {
        if (error) {
            *error = QStringLiteral("no Source registered for contract %1")
                         .arg(connectPoint.contract);
        }
        return nullptr;
    }
    // The Caller is bound before the mirroring, so a call that arrives in the same turn
    // already carries whose it is.
    caller->setSource(mirror);
    SourceFactory::mirror(mirror, shared, caller);
    return mirror;
}

void WebEdge::dropSession(const QByteArray &sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }
    // A scope change is not the end of a session. `SessionManager::setScope` rotates the
    // credential and reports the old id as removed, but the session it names is still there
    // under a new id and the browser is still signed in. The manager remembers the hand-off
    // for exactly that reason, so a rotation is read here rather than acted on. What is left
    // is the three ways a session ends: signed out, revoked, expired.
    if (!m_sessionManager->rotationOf(sessionId).isEmpty()) {
        return;
    }
    // Taken first. Closing a socket runs its disconnected handler, which comes back through
    // releaseSessionSources and would otherwise be walking a container being iterated.
    const QList<WebSocketTransport *> open{m_sessionSockets.values(sessionId)};
    m_sessionSockets.remove(sessionId);
    for (WebSocketTransport *transport : open) {
        if (transport) {
            // Going Away, the code a browser reads as "the server ended this on purpose",
            // which is what the client's reconnect backoff is written against. Asked of
            // the device rather than the socket, so it still arrives when the socket is on
            // another thread. Calling close() on it from here would do nothing at all and
            // report nothing, which is read access outliving the credential all over again.
            transport->shutdown(QWebSocketProtocol::CloseCodeGoingAway,
                                QStringLiteral("session ended"));
        }
    }
}

void WebEdge::followRotation(const QByteArray &from, const QByteArray &to,
                             WebSocketTransport *transport)
{
    // The socket, which belongs to this connection alone.
    m_sessionSockets.remove(from, transport);
    m_sessionSockets.insert(to, transport);

    // The Sources, which belong to the session and are therefore moved once however many
    // tabs it has open. The first connection told finds the old key and moves it, and every
    // other one finds nothing left to move. Copied out before the insert below, because
    // inserting can rehash and leave the iterator pointing at nothing.
    const auto entry{m_sessionSources.find(from)};
    if (entry == m_sessionSources.end()) {
        return;
    }
    const SessionSources leaving{*entry};
    m_sessionSources.erase(entry);
    SessionSources &arriving{m_sessionSources[to]};
    arriving.connections += leaving.connections;
    for (auto point{leaving.byConnectPoint.cbegin()};
         point != leaving.byConnectPoint.cend(); ++point) {
        arriving.byConnectPoint.insert(point.key(), point.value());
    }
}

void WebEdge::releaseSessionSources(const QByteArray &sessionId)
{
    const auto entry{m_sessionSources.find(sessionId)};
    if (entry == m_sessionSources.end()) {
        return;
    }
    if (--entry->connections > 0) {
        return;
    }
    // The last connection of this session is gone, so the state it was holding goes with
    // it. A user who comes back gets a fresh Source, which is the same thing a restart
    // would give them. A Source is live state, not storage. What must survive belongs in
    // the entity singleton or behind a persistence connect point.
    for (QObject *source : std::as_const(entry->byConnectPoint)) {
        delete source;
    }
    m_sessionSources.erase(entry);
}

void WebEdge::onNewWebSocketConnection()
{
    while (std::unique_ptr<QWebSocket> pending{m_httpServer->nextPendingWebSocketConnection()}) {
        // Left unparented: hostConnection() decides what carries it, which on a threaded
        // edge is a channel bound for another thread and not anything of this one's.
        hostConnection(pending.release());
    }
}

/// Give the accepted socket to whatever carries it, and hand back the device QtRO writes.
///
/// On a one-thread edge that is the socket itself and nothing has changed. On a threaded
/// edge the socket and the raw socket underneath it become a channel's children and go to
/// an IO thread together, leaving the device here, on the thread the QtRO host and every
/// Source live on. The move is deliberately not done here: the caller does it last, once
/// the connection is hosted, so nothing runs on the socket between the two.
WebSocketTransport *WebEdge::carry(QWebSocket *socket, QObject *connection)
{
    QAbstractSocket *raw{m_pendingRawSockets.take(
        peerKey(socket->peerAddress().toString(), socket->peerPort()))};
    if (m_ioThreads && !raw) {
        // Should not happen: every accepted socket is remembered by the same key on the
        // way in. If it ever does, this connection stays on this thread rather than going
        // half way, because a QWebSocket read on one thread and written on another is a
        // data race that presents as a connection which receives and never answers.
        qWarning("SynQt: no raw socket found for an accepted upgrade; serving this "
                 "connection on the main thread instead of an IO thread");
    }
    if (!m_ioThreads || !raw) {
        socket->setParent(connection);
        // And the socket underneath it, which otherwise nobody owns. QHttpServer takes the
        // accepted socket out of the QSslServer's object tree to upgrade it, and the
        // QWebSocket it hands back is not its parent, so an edge destroyed while a browser
        // is still connected left the whole connection behind: 79 KB and 180 allocations
        // per live browser, measured. A threaded edge never had this, because SocketChannel
        // adopts the raw socket in order to carry it to another thread, and owning it was
        // the side effect that mattered. Guarded the same way SocketChannel guards it: a
        // socket already under the QWebSocket has an owner counting on having it.
        if (raw && !isUnder(raw, socket)) {
            raw->setParent(connection);
        }
        return new WebSocketTransport{socket, connection};
    }
    SocketChannel *channel{new SocketChannel{socket, raw}};
    WebSocketTransport *transport{new WebSocketTransport{channel, connection}};
    // What a batch may grow to is what the browser end is allowed to receive, since
    // batching merges the messages written in one pass into a single WebSocket message.
    transport->setWriteBatchLimit(m_config.maxMessageBytes);
    return transport;
}

void WebEdge::hostConnection(QWebSocket *socket)
{
    // Reject oversized frames before buffering (DoS guard).
    socket->setMaxAllowedIncomingMessageSize(static_cast<quint64>(m_config.maxMessageBytes));
    socket->setMaxAllowedIncomingFrameSize(static_cast<quint64>(m_config.maxMessageBytes));

    // Identify the session behind this socket, and the visitor behind the peer: both were
    // stashed by the verifier for this peer, because the accepted socket's handshake
    // headers are not re-readable server-side. The address especially: the forwarding
    // header is gone by now, so recomputing it here would give the balancer every time
    // and the release below would decrement a bucket the accept never incremented.
    const QString key{peerKey(socket->peerAddress().toString(), socket->peerPort())};
    const VerifiedSession verified{m_pendingSessions.take(key)};
    const QByteArray sessionId{verified.id};
    const QString ip{verified.clientIp.isEmpty()
                         ? normalizedAddress(socket->peerAddress()).toString()
                         : verified.clientIp};

    // Everything this connection owns on this thread hangs off one object, so ending it is
    // one deletion and the edge's own teardown can put every connection down before the
    // threads their sockets are on. Its children are deleted in the order they were added,
    // which is why the device is built after the node and not before: QtRO writes a last
    // message to every listener as the node goes, and a device already destroyed by then
    // is a null QIODevice being asked whether it is open.
    QObject *connection{new QObject{m_connections}};

    ++m_activeGlobal;
    ++m_activePerIp[ip];

    // Claimed before any Source is reached for, and released when the socket closes. The
    // count is what keeps a session's shared Sources alive across a tab closing while
    // another tab is still open, and what destroys them when the last one goes.
    if (!sessionId.isEmpty()) {
        ++m_sessionSources[sessionId].connections;
    }

    // One QtRO host node per connection, and one Source per connect point on it, minted
    // fresh with a Caller bound to this session. The node is per connection whatever the
    // Sources do, which is why reusing one Source across connections saved so little.
    QRemoteObjectHost *node{new QRemoteObjectHost{connection}};
    node->setHostUrl(QUrl{QStringLiteral("synqt-edge:///%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                     QRemoteObjectHost::AllowExternalRegistration);

    // A gate Caller reads this session's live scope for the per-connect-point decisions.
    // It hosts no Source and emits nothing, so it needs no typed subclass (empty contract).
    Caller *gate{Caller::forUser(QString{}, m_sessionManager, sessionId, nullptr, node)};
    gate->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);

    for (const WebEdgeConnectPoint &connectPoint : m_config.connectPoints) {
        // Scope gating: never host a scoped connect point for an under-scoped session, so
        // the browser can never even acquire a Replica it is not authorized for.
        if (!connectPoint.scope.isEmpty() && !gate->hasScope(connectPoint.scope)) {
            continue;
        }
        QString error;
        QObject *source{sourceForConnection(connectPoint, sessionId, connection, &error)};
        if (!source) {
            // Said out loud as well as on the signal. Every other rejection here is the
            // policy working and belongs to the connection that earned it, but a Source that
            // will not load is a defect in the entity, the same one on every connection, and
            // the browser's only symptom is a connect point that never arrives. Left to the
            // signal alone it went unreported through three operating systems of CI.
            qWarning("SynQt: connect point %s is not served on this connection: %s",
                     qUtf8Printable(connectPoint.name), qUtf8Printable(error));
            emit upgradeRejected(error);
            continue;
        }
        // Remoted on this connection's own node. A per-caller Source is remoted on one node
        // per tab, which QtRO allows: each host gets its own view of the same object, and
        // every replica tracks it.
        if (!node->enableRemoting(source, connectPoint.name)) {
            qWarning("SynQt: connect point %s loaded but could not be remoted",
                     qUtf8Printable(connectPoint.name));
            emit upgradeRejected(
                QStringLiteral("enableRemoting failed for %1").arg(connectPoint.name));
        }
    }

    // The framework's own SessionState connect point: who this connection's visitor is.
    // Hosted on every accepted connection, unconditionally, and that is deliberate. It is
    // not a feature a project turns on: `Session.scope` and `Session.identity` are what
    // the runtime API says a client may always ask, and every app with a sign-in gates its
    // UI on them. Hosting it only where identity is configured would leave the two of them
    // answering "anonymous, nobody" on exactly the projects that are about to ask.
    {
        Caller *stateCaller{Caller::forUser(QStringLiteral("SessionState"), m_sessionManager,
                                            sessionId, nullptr, connection)};
        stateCaller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
        SessionStateSource *stateSource{new SessionStateSource{stateCaller, connection}};
        stateCaller->setParent(stateSource);
        stateCaller->setSource(stateSource);
        if (!node->enableRemoting(stateSource, QStringLiteral("SessionState"))) {
            emit upgradeRejected(QStringLiteral("enableRemoting failed for SessionState"));
        }
    }

    // The framework's own Pages connect point, hosted the same way as every
    // application connect point above. A fresh Source per connection, carrying this
    // connection's own Caller, over the PageStore/PagesService shared by every
    // connection. Page-level scope gating happens inside PagesService, per request,
    // so there is no single connect-point-level scope to check here.
    if (m_pagesService) {
        Caller *pagesCaller{Caller::forUser(QStringLiteral("Pages"), m_sessionManager,
                                            sessionId, nullptr, connection)};
        pagesCaller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
        PagesEdgeSource *pagesSource{
            new PagesEdgeSource{m_pageStore, m_pagesService, pagesCaller, connection}};
        pagesCaller->setParent(pagesSource);
        pagesCaller->setSource(pagesSource);
        if (!node->enableRemoting(pagesSource, QStringLiteral("Pages"))) {
            emit upgradeRejected(QStringLiteral("enableRemoting failed for Pages"));
        }
    }

    // The device the browser is reached through, built last so it outlives the node above.
    WebSocketTransport *transport{carry(socket, connection)};
    transport->setReadBufferLimit(m_config.maxMessageBytes * ReadBufferFrames);
    transport->open(QIODevice::ReadWrite);
    node->addHostSideConnection(transport);

    // The session this connection is bound to, which is not the constant it looks like:
    // an elevation rotates the credential underneath (SessionManager::setScope, which
    // Caller.setScope calls), and every key the edge keeps this connection under has to
    // move with it. Shared rather than captured by value, so the rotation handler and the
    // disconnect handler below are reading one answer instead of two.
    const auto liveSession{std::make_shared<QByteArray>(sessionId)};
    if (!sessionId.isEmpty()) {
        m_sessionSockets.insert(sessionId, transport);
        // Received on `connection`, so it goes when the connection does.
        connect(m_sessionManager, &SessionManager::sessionRotated, connection,
                [this, liveSession, transport](const QByteArray &from, const QByteArray &to) {
            if (*liveSession != from) {
                return;
            }
            followRotation(from, to, transport);
            *liveSession = to;
        });
    }
    // Watched on the device rather than the socket: the device is on this thread whatever
    // the socket is doing, and it relays the socket's disconnect either way.
    connect(transport, &WebSocketTransport::disconnected, this,
            [this, transport, connection, ip, liveSession]() {
        --m_activeGlobal;
        if (--m_activePerIp[ip] <= 0) {
            m_activePerIp.remove(ip);
        }
        if (!liveSession->isEmpty()) {
            m_sessionSockets.remove(*liveSession, transport);
            releaseSessionSources(*liveSession);
        }
        // Takes the node, the Sources, the Callers and the device with it, and the device
        // in turn puts the socket down on whichever thread the socket is on.
        connection->deleteLater();
    });

    // Last, and only now. The socket goes to its thread with the connection already hosted
    // on it, so nothing runs on it between being wired up and being somewhere else.
    // Anything QtRO has already written is waiting in the device's batch and crosses on
    // the next pass, by which time the channel is where it is going to stay.
    if (m_ioThreads) {
        transport->moveSocketToThread(m_ioThreads->nextThread());
    }
}

} // namespace SynQt
