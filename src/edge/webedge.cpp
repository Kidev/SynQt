// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "webedge.h"

#include "caller.h"
#include "cookies.h"
#include "identityprovider.h"
#ifdef SYNQT_DEV_TOOLS
#include "identitypicker.h"
#endif
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
#include "objecttree.h"         // reused host-side (from src/transport)
#include "socketchannel.h"      // reused host-side (from src/transport)
#include "socketoptions.h" // reused host-side (from src/transport)
#include "websockettransport.h" // reused host-side (from src/transport)
#include "withdrawsource.h"

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

// How many full-size frames one browser connection may buffer, in either direction, before
// the edge stops serving it. max_message_bytes caps one frame; this caps their sum. QtRO
// drains reads synchronously, so the read side holds one frame in the steady state; on the
// write side this is how far a browser may fall behind once the kernel buffers are full.
// With max_connections_global, these bound the total socket memory.
constexpr qint64 BufferFrames{4};

// The content type of a bundle file the build precompresses, or empty. Empty means no
// encoded variant, and the response falls back to fromFile().
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

// The ETag of one encoding of a bundle file: the file's own tag, told apart by the
// encoding (`"abc"` becomes `"abc-br"`). Empty stays empty; no encoding keeps the file's tag.
QByteArray representationTag(const QByteArray &fileTag, const QByteArray &encoding)
{
    if (fileTag.isEmpty() || encoding.isEmpty() || !fileTag.endsWith('"')) {
        return fileTag;
    }
    return fileTag.chopped(1) + '-' + encoding + '"';
}

// The conditional-GET reply when the caller already holds this exact resource, or nothing
// when the body must be sent. Not on WebEdge: the header only forward-declares
// QHttpServerResponse.
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

// A plaintext (dev) transport server that reports each accepted socket, so the edge can
// start its handshake timer, then passes it to QHttpServer.
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
    // Fold the single-bundle shorthand in once; everything downstream reads only `bundles`.
    if (m_config.bundles.isEmpty() && !m_config.bundleDir.isEmpty()) {
        m_config.bundles.insert(m_config.defaultScope, m_config.bundleDir);
    }

    // The session table ceiling. Eviction never takes a session a browser is connected on;
    // the socket table tracks those.
    m_sessionManager->setMaximumSessions(m_config.maxSessions);
    m_sessionManager->setInUseCheck([this](const QByteArray &sessionId) {
        return m_sessionSockets.contains(sessionId);
    });

    // Record every upgrade decision once, from the signals, so a new refusal is traced
    // without extra code.
    //
    // The decision and its reason are recorded, never the credential: no cookie, bearer
    // token or Authorization header reaches a monitor (docs/security.md).
    connect(this, &WebEdge::upgradeRejected, this, [](const QString &reason) {
        trace(Category::Authorization, Severity::Warning, QStringLiteral("upgrade refused"),
              {{QStringLiteral("reason"), reason}});
    });
    connect(this, &WebEdge::upgradeAccepted, this, [](const QString &peer) {
        trace(Category::Transport, Severity::Info, QStringLiteral("upgrade accepted"),
              {{QStringLiteral("peer"), peer}});
    });
}

/// Delete the connections before stopping the threads their sockets are on. On a threaded
/// edge, a device deletes its channel on the channel's thread; quitting a thread's event
/// loop still delivers the deferred deletes queued for it.
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

// Bounds on a seed's nesting depth and JSON size. A seed is one page's first frame of data,
// so both are generous. QJSValue::toVariant() and QJsonDocument::fromVariant() recurse
// without a bound and would crash on deep input.
constexpr int kMaxSeedDepth{32};
constexpr int kMaxSeedBytes{64 * 1024};

/// How, if at all, a hook exposes the seedFor(route, parameters, caller) the edge calls.
enum class SeedForSupport {
    None,     ///< no seedFor(route, parameters, caller) at all.
    Untyped,  ///< seedFor with three untyped (QVariant) parameters: the edge can call it.
    Typed,    ///< seedFor with three parameters, at least one annotated: not callable.
};

/// Classify a hook's seedFor. The edge invokes it with three QVariant arguments, so only an
/// untyped seedFor matches; a typed parameter (`route: string`) changes the signature. The
/// probe mirrors the invoke exactly.
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
/// Used instead of QJSValue::toVariant(), which recurses without a limit: a deep structure
/// such as a category tree would overflow the stack. Sets ok to false when the bound is
/// hit.
QVariant boundedSeedVariant(const QJSValue &value, int depth, bool *ok)
{
    if (depth > kMaxSeedDepth) {
        *ok = false;
        return QVariant{};
    }
    // A QObject in a seed is not data (a hook returning the caller, for example). It
    // converts to nothing, and the object check below reports it.
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
    // Installed once on the shared service and handed the calling connection's own Caller
    // on every call. Read the argument; never capture or cache it.
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
    // A QML function returning an object literal returns a QJSValue, which
    // QJsonDocument::fromVariant() does not handle. Unwrap it first (bounded, see
    // boundedSeedVariant).
    if (result.canConvert<QJSValue>()) {
        bool withinDepth{true};
        result = boundedSeedVariant(result.value<QJSValue>(), 0, &withinDepth);
        if (!withinDepth) {
            warnAboutSeedOnce(route, "returned a seed nested deeper than a seed may be");
            return QString{};
        }
    }
    // The client reads a seed as a JSON object; anything else would arrive empty. Report
    // it.
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
    // Once per route, never per request: the browser decides how often it asks.
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
    // The page seed hooks, built like the identity mapping hook (identityprovider.cpp): a
    // PageSeed QML object with `function seedFor(route, parameters, caller)`, called after
    // the route's scope check. Built once each, and not at all when no route declares a
    // seed.
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
        // Checked before create(), whose own "Component is not ready" names neither the
        // file nor the reason.
        QObject *hook{component->isReady() ? component->create() : nullptr};
        if (!hook) {
            // The hook's own file and QML diagnostic; never the page source or anything the
            // hook read.
            qWarning("SynQt: page seed hook %s failed to load: %s",
                     qUtf8Printable(page.seed), qUtf8Printable(component->errorString()));
            continue;
        }
        // Probed once here: a failed invokeMethod logs "no such method" on every request,
        // and the browser decides how often it asks. A hook that cannot answer is dropped.
        const SeedForSupport support{seedForSupport(hook)};
        if (support == SeedForSupport::Typed) {
            // The most likely mistake: seedFor is called with untyped (QVariant) arguments,
            // so a hook that annotates a parameter is never reached. Say so once.
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
    // A bind address is not a name: "0.0.0.0" and "::" mean every interface. Taking the
    // default bind as the origin would build "https://0.0.0.0:8443" and refuse every
    // visitor. For a wildcard bind the origin is localhost, which is right for a
    // development run. A deployment sets `public.origin` and never gets here.
    static const QStringList wildcards{QStringLiteral("0.0.0.0"), QStringLiteral("::"),
                                       QStringLiteral("0:0:0:0:0:0:0:0")};
    if (m_config.host.isEmpty() || wildcards.contains(m_config.host)) {
        return QStringLiteral("localhost");
    }
    // A literal IPv6 address is bracketed only in a URL, so the brackets are added here.
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
        // The sync endpoint has the page's host and port, so its origin is the declared
        // origin with the scheme swapped.
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

void WebEdge::cachePolicy()
{
    m_csp = computeCsp();
    m_allowedOrigins = expandedAllowedOrigins();
}

QByteArray WebEdge::computeCsp() const
{
    // Compute the policy: append the sync endpoint's wss origin to connect-src (some
    // browsers do not extend 'self' to WebSocket schemes), and add worker-src 'self' blob:
    // under cross-origin isolation.
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
            // without 'unsafe-inline'.
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
        // 'self' covers the shell cache's service worker and the kit's pthread workers.
        // blob: covers an emsdk that spawns blob: workers; those still need script-src. See
        // <https://synqt.org/csp/>.
        //
        // worker-src would fall back through child-src to script-src 'self' anyway; naming
        // it documents the policy and survives a project that narrows child-src.
        if (m_config.crossOriginIsolation) {
            directives.append(QByteArrayLiteral("worker-src 'self' blob:"));
        } else if (m_config.serviceWorker) {
            directives.append(QByteArrayLiteral("worker-src 'self'"));
        }
    }
    return directives.join("; ");
}

namespace {

/// The password gate's budget: attempts per visitor address, and the window length.
/// Generous for a person mistyping, far below what a guesser needs, and low enough that the
/// key derivations cannot flood the event loop.
constexpr int kMaxSignInsPerWindow{10};

/// The maximum per-tab nonce length. The nonce becomes part of a cookie name and is chosen
/// by the caller.
constexpr int kMaxTabNonce{32};
constexpr qint64 kSignInWindowMs{60 * 1000};
/// How many addresses the window table may name. Full, it drops expired windows, and a
/// table still full refuses every sign-in for the rest of the window.
constexpr int kMaxRateEntries{4096};

} // namespace

/// The password gate an entity serves for its own people (`signInPath`).
///
/// On success the caller's existing session is elevated, not replaced: the caller already
/// holds a session from fetching the sign-in page, and a higher scope makes the same URL
/// resolve to another bundle. `setScope` rotates the credential, so the new one is
/// returned.
///
/// Every failure gets the same answer, so a guesser cannot tell which names exist.
QHttpServerResponse WebEdge::handleSignIn(const QHttpServerRequest &request)
{
    // Refuse cross-site requests. A page elsewhere could POST here with chosen credentials:
    // the Lax cookie is not sent on a cross-site POST, so the gate would mint a session at
    // `signInScope` in the visitor's browser and sign them in as someone else's operator.
    // Browsers state the request's origin in `Sec-Fetch-Site`; non-browser callers send
    // none. Same rule as the sign-out route (IdentityProvider::handleLogout).
    const QByteArray site{request.value("Sec-Fetch-Site")};
    // Also check `Origin`, for browsers without `Sec-Fetch-Site` (older Safari): they still
    // send it on every POST. An origin this edge did not name is refused. Non-browser
    // callers send neither.
    const QString origin{QString::fromUtf8(request.value("Origin"))};
    if (site == "cross-site"
        || (site == "same-site" && m_config.originModel != QLatin1String("split_origin"))
        || (!origin.isEmpty() && !m_allowedOrigins.contains(origin))) {
        emit signInRefused(QString{});
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("sign in from the application"),
                                   QHttpServerResponder::StatusCode::Forbidden};
    }

    // Rate-limited before the password is read, so a refusal reveals nothing and costs
    // nothing. m_signInRate also rations the PBKDF2 below.
    const QString visitor{m_clientAddress.resolve(request.remoteAddress(),
                                                  request.value("X-Forwarded-For"))};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const auto refuse{[this](qint64 retryAfterMs) {
        QHttpServerResponse response{QByteArrayLiteral("text/plain"),
                                     QByteArrayLiteral("slow down"),
                                     QHttpServerResponder::StatusCode::TooManyRequests};
        QHttpHeaders headers{response.headers()};
        headers.append(QHttpHeaders::WellKnownHeader::RetryAfter,
                       QByteArray::number(retryAfterSeconds(retryAfterMs)));
        response.setHeaders(std::move(headers));
        emit signInRefused(QString{});
        return response;
    }};

    // Enforce the table ceiling before this request is counted and before any reference
    // into the table is held: QHash::erase moves later entries, so a reference from
    // operator[] does not survive a prune.
    //
    // Only expired windows are dropped. If that frees nothing, the gate refuses for the
    // rest of the minute. Emptying the table instead would let a flood of throwaway
    // addresses reset the guesser's attempts.
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
    const QByteArray presented{sessionIdOf(request)};
    // No hand-off from the old credential. The response carries the new cookie; a hand-off
    // could only let someone else redeem the pre-sign-in id for the operator session.
    QByteArray elevated{m_sessionManager->setScope(presented, m_config.signInScope,
                                                   QVariantMap{},
                                                   SessionManager::Handoff::None)};
    if (elevated.isEmpty()) {
        // No live session to elevate (a script, not a browser that fetched the page).
        // Create one at the proven scope.
        elevated = m_sessionManager->createSession(m_config.signInScope);
    }
    if (elevated.isEmpty()) {
        // The table is full of sessions that cannot be dropped. The password was right, but
        // there is no cookie to give.
        emit signInRefused(name);
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("no session can be issued right now"),
                                   QHttpServerResponder::StatusCode::ServiceUnavailable};
    }
    QHttpServerResponse response{QByteArrayLiteral("text/plain"), QByteArrayLiteral("ok")};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::SetCookie,
                   cookieFor(elevated, tabNonce(request)));
    response.setHeaders(std::move(headers));
    emit signInAccepted(name);
    return response;
}

#ifdef SYNQT_DEV_TOOLS
QHttpServerResponse WebEdge::handlePick(const QHttpServerRequest &request)
{
    // The picker checks that the choice names a declared scope and mints the session. The
    // edge forms the cookie the same way as every other session cookie.
    IdentityPicker::Choice choice;
    QHttpServerResponse refusal{m_picker->choose(request, &choice)};
    if (choice.sessionId.isEmpty()) {
        return refusal;  // refused. The picker said why and minted nothing
    }

    // A shared choice keeps the tab's URL; the new cookie is the whole change. A per-tab
    // choice redirects, because the nonce lives in the URL and the edge needs it on every
    // later request to find the tab's cookie.
    const bool perTab{!choice.tabNonce.isEmpty()};
    const QByteArray target{QByteArrayLiteral("/?s=") + choice.tabNonce};
    QHttpServerResponse response{
        perTab ? QHttpServerResponse{QByteArrayLiteral("text/plain"), target,
                                     QHttpServerResponder::StatusCode::SeeOther}
               : QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                     QByteArrayLiteral("ok")}};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::SetCookie,
                   cookieFor(choice.sessionId, choice.tabNonce));
    if (perTab) {
        headers.append(QHttpHeaders::WellKnownHeader::Location, target);
    }
    response.setHeaders(std::move(headers));
    return response;
}
#endif

QByteArray WebEdge::issueSessionCookie()
{
    const QByteArray minted{m_sessionManager->createSession()};
    return minted.isEmpty() ? QByteArray{} : cookieFor(minted);
}

QByteArray WebEdge::sessionCookieFor(const QHttpServerRequest &request)
{
    // Use this tab's cookie name in both branches, so the tab and the edge read and write
    // the same cookie.
    const QByteArray nonce{tabNonce(request)};
    const QByteArray presented{sessionIdOf(request)};
    if (m_sessionManager->isLive(presented)) {
        return QByteArray{};  // it holds a live session. Leave the one it has alone
    }
    // A dead id that was rotated by a scope change (Caller.setScope in a slot, which cannot
    // set a cookie): return the id the session became, so a reload does not sign the
    // visitor out.
    if (const QByteArray rotated{m_sessionManager->rotationOf(presented)}; !rotated.isEmpty()) {
        return cookieFor(rotated, nonce);
    }
    // Empty when the table is full with nothing to drop. The page is still delivered; the
    // upgrade is refused for lacking a session, and the client keeps retrying.
    const QByteArray minted{m_sessionManager->createSession()};
    return minted.isEmpty() ? QByteArray{} : cookieFor(minted, nonce);
}

QByteArray WebEdge::cookieFor(const QByteArray &token, const QByteArray &nonce)
{
    QByteArray cookie{cookieNameFor(nonce) + "=" + token + "; HttpOnly; Path=/"};
    if (m_config.originModel == QLatin1String("split_origin")) {
        // No `Partitioned` (CHIPS): the OAuth callback would file the cookie under the
        // edge's partition, which the client site cannot read (tests/split-origin).
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
    return cookieValue(cookieHeader, m_config.cookieName.toUtf8());
}

QByteArray WebEdge::tabNonce(const QHttpServerRequest &request)
{
    const QUrlQuery query{request.url().query()};
    const QString value{query.queryItemValue(QStringLiteral("s"), QUrl::FullyDecoded)};
    if (value.isEmpty() || value.size() > kMaxTabNonce) {
        return QByteArray{};
    }
    // Validated because it becomes part of a cookie name in a Set-Cookie header, where a
    // ';' could add attributes and a newline a second header. ASCII letters and digits
    // only, with a bounded length.
    //
    // It is not a credential: it only selects which cookie to read, and the cookie holds
    // the session id.
    for (const QChar character : value) {
        if (character.unicode() > 127 || !character.isLetterOrNumber()) {
            return QByteArray{};
        }
    }
    return value.toLatin1();
}

QByteArray WebEdge::cookieNameFor(const QByteArray &nonce) const
{
    if (nonce.isEmpty()) {
        return m_config.cookieName.toUtf8();
    }
    return m_config.cookieName.toUtf8() + '_' + nonce;
}

QByteArray WebEdge::sessionIdOf(const QHttpServerRequest &request) const
{
    return cookieValue(request.value("Cookie"), cookieNameFor(tabNonce(request)));
}

void WebEdge::stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response)
{
    QHttpHeaders headers{response.headers()};
    headers.append(QByteArrayLiteral("Content-Security-Policy"), m_csp);
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

    // Bundle cache headers. no-cache means revalidate: the browser keeps the bytes and
    // confirms them with a conditional GET, so a repeat visit gets a 304. It also keeps a
    // browser from pinning a stale service worker.
    const QString requested{bundlePathFor(bundleFor(request), request.url().path())};
    if (!requested.isEmpty()) {
        const QByteArray etag{etagFor(requested)};
        if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
            headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
        }
        headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                       QByteArrayLiteral("no-cache"));
    }

    // Issue a session on the page load, so the browser has a credential for the wss
    // upgrade. Only for a browser without a live session: reissuing would replace a
    // signed-in credential (the OAuth callback redirects here) and let one browser mint
    // sessions by reloading.
    if (request.url().path() == m_config.clientRoute) {
        const QByteArray cookie{sessionCookieFor(request)};
        if (!cookie.isEmpty()) {
            headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
        }
    }
    response.setHeaders(std::move(headers));
}

QString WebEdge::canonicalRootOf(const QString &root) const
{
    // Resolved at start-up, like the ETag table. A deploy is a restart.
    return m_canonicalRoots.value(root);
}

void WebEdge::cacheBundle()
{
    m_etags.clear();
    m_canonicalRoots.clear();
    // Every bundle this edge may serve. The table is keyed by canonical absolute path, so
    // equal file names in two roots do not collide.
    for (const QString &bundle : std::as_const(m_config.bundles)) {
        const QDir root{bundle};
        m_canonicalRoots.insert(bundle, root.canonicalPath());
        const QFileInfoList entries{root.entryInfoList(QDir::Files | QDir::NoSymLinks)};
        for (const QFileInfo &entry : entries) {
            // A precompressed variant shares the identity of the file it encodes and is
            // never requested directly.
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
    // The table is keyed by canonical path, and most callers already hold one (the asset
    // route checked containment with it, and stampResponse gets it from bundlePathFor). Try
    // the string as a key first to skip another canonicalFilePath() (a stat and a
    // realpath). The shell's composed `<root>/index.html` misses and is resolved.
    const auto direct{m_etags.constFind(path)};
    if (direct != m_etags.constEnd()) {
        return direct.value();
    }
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
    // Hierarchical scopes rank, so a scope without a bundle gets the nearest one below it.
    // Set-based scopes do not rank, so an unmapped scope gets the default scope's bundle.
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
    const QByteArray sessionId{sessionIdOf(request)};
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
    // The path must also be inside the bundle this caller was served. The root was resolved
    // when the table was built; the file is resolved here, which is what catches a symlink
    // or `..` leaving the bundle.
    const QString canonicalRoot{canonicalRootOf(root)};
    if (canonicalRoot.isEmpty()) {
        return {};
    }
    const QString resolved{QFileInfo{QDir{root}, name}.canonicalFilePath()};
    if (!resolved.startsWith(canonicalRoot + QLatin1Char('/'))) {
        return {};
    }
    return m_etags.contains(resolved) ? resolved : QString{};
}

QHttpServerResponse WebEdge::shellOrNotFound(const QString &root, const QString &path,
                                             const QHttpServerRequest &request)
{
    // Only a navigation gets the shell. A POST or DELETE to an unknown URL is a client bug
    // or a probe.
    if (request.method() != QHttpServerRequest::Method::Get
        && request.method() != QHttpServerRequest::Method::Head) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
    }
    // An asset request (the last segment has an extension) gets a 404, not HTML with a 200.
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
    // A deep link is often a first page load, so it gets what the client route's response
    // gets. stampResponse() sees only the request and cannot tell a deep link from a 404;
    // here the response is index.html by construction.
    //
    // Without the cookie the wss upgrade answers 401 and the app reconnects forever.
    // Without the cache headers an intermediary may keep a replaced loader.
    //
    // Never reached for m_config.clientRoute, which is registered first, so the Set-Cookie
    // is not doubled.
    const QString index{QDir{root}.filePath(QStringLiteral("index.html"))};
    const QByteArray etag{etagFor(index)};
    QHttpHeaders headers{response.headers()};
    if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
        headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
    }
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                   QByteArrayLiteral("no-cache"));
    // As for the client route (see stampResponse), only a browser without a live session
    // gets one.
    const QByteArray cookie{sessionCookieFor(request)};
    if (!cookie.isEmpty()) {
        headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
    }
    response.setHeaders(std::move(headers));
}

void WebEdge::computeScriptHashes()
{
    m_scriptHashes.clear();
    // The union across bundles: response stamping runs after the request, when the bundle
    // is no longer known. Every hash is of a loader this build generated, so the union
    // gives an attacker nothing.
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
    // Inline <script> blocks without a src need their sha256 in script-src.
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

    // 1. No connect point Source is built here. Each is instantiated per connection in
    //    hostConnection() with a Caller for its one user; shared state belongs to the
    //    entity singleton.
    //
    // 1.5. The framework Pages connect point (edge-delivered pages): one
    // PageStore/PagesService shared by every connection, built once here. Each connection
    // gets its own PagesEdgeSource in hostConnection(). Nothing is built when the project
    // has no pages.
    if (!m_config.pages.isEmpty()) {
        m_pageStore = new PageStore{m_config.pagesDir, this};
        for (const WebEdgePage &page : m_config.pages) {
            m_pageStore->addPage(page.path, page.file, page.scope, page.graphics);
        }
        // Development-only file watching, enabled only by devWatch, which only the `synqt
        // dev` launch sets (dev_command() in tools/synqt/synqt/run.py, through --dev).
        // Missing local TLS does not mean development: a production edge behind a
        // TLS-terminating proxy has none.
        if (m_config.devWatch) {
            m_pageStore->setWatching(true);
        }
        m_pagesService = new PagesService{m_pageStore, this};
        buildPageSeedHooks();
    }

    // 2. The HTTP server: serve the bundle, stamp headers, and verify upgrades.
    m_httpServer = new QHttpServer{this};

    // Qt's own request limits, applied before any SynQt code runs. Qt's defaults suit a
    // general server that accepts uploads (32 MiB bodies). The idle timeout closes a peer
    // that sends half a request (docs/security.md). Rate limiting stays off unless the
    // project asks, since Qt counts the peer address, which behind a balancer is the
    // balancer's.
    QHttpServerConfiguration httpConfiguration;
    httpConfiguration.setKeepAliveTimeout(
        std::chrono::seconds{m_config.keepAliveTimeoutSeconds});
    httpConfiguration.setMaximumBodySize(m_config.maxBodyBytes);
    if (m_config.maxRequestsPerSecond > 0) {
        httpConfiguration.setRateLimitPerSecond(m_config.maxRequestsPerSecond);
    }
    // The socket ceiling is counted by the edge in trackPendingUpgrade(), at accept: Qt's
    // setMaximumConnections and setMaximumConnectionsPerHost never count a WebSocket link
    // down. See m_socketsPerIp.
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
        // A CDN delivers the bundle, so this route only delivers the session. Without it a
        // browser that loaded the app elsewhere reaches the upgrade without a credential.
        m_httpServer->route(m_config.clientRoute, [this](const QHttpServerRequest &request) {
            return credentialResponse(request);
        });
    }

    // Login, callback and logout. The OAuth flow runs on the edge; the browser only
    // receives a session cookie. The client secret and the tokens never leave.
    if (m_config.identity.enabled) {
        CookiePolicy cookie;
        cookie.name = m_config.cookieName;
        cookie.sameSiteNone = (m_config.originModel == QLatin1String("split_origin"));
        cookie.secure = m_config.usesTls();
        m_identity = new IdentityProvider{m_config.identity, m_sessionManager, m_engine,
                                          httpOrigin(), cookie, this};
        // One definition of the visitor address for the whole edge: the device route
        // rate-limits on the same address the upgrade verifier caps.
        m_identity->setClientAddress(&m_clientAddress);
        // The declared scopes, so the mapping hook's answer resolves as an index into them.
        // The bundle gate, the connect point gate and the login all rank against
        // `m_config.scopeOrder`.
        m_identity->setScopeOrder(m_config.scopeOrder);
        // A session that ends releases its server-side tokens. Logout already does; this
        // covers the two ways a session ends without it.
        //
        // `sessionRemoved` covers revocation, including every session a reused device
        // credential opened. It also fires for a scope-change rotation, which ends nothing,
        // so that case goes to followRotation, as in dropSession.
        connect(m_sessionManager, &SessionManager::sessionExpired, m_identity,
                [this](const QString &token) { m_identity->forgetSession(token.toLatin1()); });
        connect(m_sessionManager, &SessionManager::sessionRemoved, m_identity,
                [this](const QString &token) {
            const QByteArray sessionId{token.toLatin1()};
            if (m_sessionManager->rotationOf(sessionId).isEmpty()) {
                m_identity->forgetSession(sessionId);
            }
        });
        connect(m_sessionManager, &SessionManager::sessionRotated, m_identity,
                [this](const QByteArray &from, const QByteArray &to) {
            m_identity->followRotation(from, to);
        });
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
        // The desktop sign-in. POST only, so the code and its verifier stay out of logs,
        // the address bar and caches. The handler refuses everything unless the project
        // builds a desktop client.
        m_httpServer->route(m_identity->claimRoute(), QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleClaim(request);
        });
        // Staying signed in: the same shape, spending a credential stored at the last
        // launch. Registered always and refused inside, so a project that persists nothing
        // answers as for any unknown path.
        m_httpServer->route(m_identity->deviceRoute(), QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return m_identity->handleDevice(request);
        });
    }
    // The monitor's operator gate. POST only, so a password never appears in a log, an
    // address bar or a cache.
    if (!m_config.signInPath.isEmpty() && m_config.signIn) {
        m_httpServer->route(m_config.signInPath, QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return handleSignIn(request);
        });
    }
#ifdef SYNQT_DEV_TOOLS
    // The development scope picker, which replaces every sign-in above. Registered only for
    // `synqt dev --identity-picker`, so it does not exist otherwise. A release SynQtEdge
    // does not contain the class.
    if (m_config.identityPicker) {
        m_picker = new IdentityPicker{m_sessionManager, m_config.scopeOrder, this};
        // The people from `.dev-identities`, checked by `synqt dev` and passed as
        // configuration.
        m_picker->setNamedIdentities(m_config.devIdentities, m_config.devIdentityProblems);
        if (m_identity) {
            // Named identities go through the project's mapping hook, through the identity
            // provider, so the picker answers as a real login does.
            m_picker->setScopeMapper([this](const QVariantMap &identity, QString *error) {
                return m_identity->mapScope(identity, error);
            });
        }
        m_httpServer->route(IdentityPicker::route(), QHttpServerRequest::Method::Get,
                            [this]() {
            return m_picker->page();
        });
        m_httpServer->route(IdentityPicker::route(), QHttpServerRequest::Method::Post,
                            [this](const QHttpServerRequest &request) {
            return handlePick(request);
        });
    }
#endif
    // Delivery of the bundle itself, only when this edge is the app's origin.
    if (m_config.serveClient) {
        registerBundleRoutes();
    }

    // Ending a session closes the connections it authorized: `sessionRemoved` is revocation
    // (and the rotation dropSession ignores), `sessionExpired` is the TTL sweep. Wired
    // here, since a project without sign-in still revokes and expires.
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
        // Read before anything is bound; a failure ends the start. Otherwise an unreadable
        // certificate or an unexpected key algorithm would fail every handshake on a
        // listening port.
        const QSslCertificate certificate{loadCertificate(m_config.certFile)};
        const QSslKey key{loadPrivateKey(m_config.keyFile)};
        if (certificate.isNull() || key.isNull()) {
            m_errorString = QStringLiteral("cannot terminate TLS with %1 and %2")
                                .arg(m_config.certFile, m_config.keyFile);
            return false;
        }
        // Read is not the same as usable. A backend with no key API of its own carries an
        // RSA or DSA key only (see unusableKeyReason). Said here rather than discovered as
        // a handshake that always fails.
        const QString unusable{unusableKeyReason(key)};
        if (!unusable.isEmpty()) {
            m_errorString = QStringLiteral("cannot terminate TLS with %1: %2")
                                .arg(m_config.keyFile, unusable);
            return false;
        }
        QSslServer *sslServer{new QSslServer{this}};
        QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
        configuration.setLocalCertificate(certificate);
        configuration.setPrivateKey(key);
        // The browser presents no client certificate. Only the server is authenticated.
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
    // Now that the port is known, both of them are answerable, and neither changes again.
    cachePolicy();
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
    // The socket ceilings, first and at accept. max_connections_per_ip and
    // max_connections_global are counted in hostConnection(), once an upgrade has been
    // accepted, so neither of them sees a peer that opens a socket and never finishes a
    // request. This is the bound on that peer, times SocketsPerLink so a browser fetching
    // its bundle over six parallel connections is not what it refuses. Keyed by the peer's
    // own address. There is no request yet to read a forwarding header from, which is also
    // true of the Qt ceiling this replaces. Counted back down when the socket is destroyed,
    // below, which an upgrade cannot disconnect the way it disconnects the socket's signals.
    const QString address{normalizedAddress(socket->peerAddress()).toString()};
    // Per address only when the address is the visitor's. Behind a balancer every socket
    // is the balancer's, and a per-address ceiling on it is a ceiling on the whole site:
    // `max_connections_per_ip * SocketsPerLink` sockets for everybody, which one visitor
    // holding that many reaches on their own, after which every other visitor is refused
    // at accept. The link ceiling still counts the visitor the forwarding header names
    // (verifyUpgrade), the global ceiling still holds, and this is the same rule the
    // inbound API surface applies to its own per-host ceiling (ApiServer::start).
    const bool perAddress{!m_clientAddress.trustsPeer(socket->peerAddress())};
    const int perIpCeiling{m_config.maxConnectionsPerIp * WebEdgeConfig::SocketsPerLink};
    const int globalCeiling{m_config.maxConnectionsGlobal * WebEdgeConfig::SocketsPerLink};
    if (m_socketsGlobal >= globalCeiling
        || (perAddress && m_socketsPerIp.value(address) >= perIpCeiling)) {
        emit upgradeRejected(QStringLiteral("socket cap reached"));
        socket->abort();
        return;
    }
    ++m_socketsGlobal;
    if (perAddress) {
        ++m_socketsPerIp[address];
    }

    const QString key{peerKey(socket->peerAddress().toString(), socket->peerPort())};
    QTimer *timer{new QTimer{socket}};
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, socket, key]() {
        m_pendingTimers.remove(key);
        emit upgradeRejected(QStringLiteral("handshake timeout"));
        socket->abort();
    });
    // Only a peer that connects and says nothing is on this clock. The browser fetches the
    // page, the loader and the bundle over the connection it will upgrade, so the deadline
    // ends at the first byte. A peer that speaks and then stalls is closed by QHttpServer's
    // keep-alive timeout; one that keeps dribbling is bounded by the header ceilings
    // (docs/security.md). The connection caps do not bound it, since they count hosted
    // connections. The observer removes itself after the first byte.
    connect(socket, &QIODevice::readyRead, timer, [socket, timer]() {
        timer->stop();
        disconnect(socket, &QIODevice::readyRead, timer, nullptr);
    });
    // The entry must go with the socket (a peer that hangs up mid-handshake never reaches
    // verifyUpgrade). The timer is a child of the socket and dies with it; watching the
    // socket's destroyed() instead makes QWebSocketPrivate::releaseConnections() print
    // "wildcard call disconnects from destroyed signal of QTcpSocket".
    //
    // Removed only while the entry still names this timer. The key is the peer address and
    // port, which the OS reuses, so a newer socket may already hold the key when an older
    // one is torn down.
    connect(timer, &QObject::destroyed, this, [this, key, timer]() {
        if (m_pendingTimers.value(key) == timer) {
            m_pendingTimers.remove(key);
        }
    });
    m_pendingTimers.insert(key, timer);
    // Recorded here, the last point where the raw socket is reachable: after the upgrade
    // the QWebSocket does not lead back to it. A threaded edge must move both, or the
    // connection is read on one thread and written on another.
    //
    // The tag is a plain QObject child that dies with the socket. The timeout timer cannot
    // serve, because it is deleted when a valid upgrade request arrives, while the raw
    // socket is still needed. Nothing wildcard-disconnects the tag (see the timer note
    // above).
    //
    // Recorded on every edge: a threaded edge moves the connection, and every edge owns it
    // once the upgrade is accepted (see carry()).
    //
    // Conditional, as for the timer: a late teardown must not evict a newer connection's
    // entry.
    QObject *tag{new QObject{socket}};
    connect(tag, &QObject::destroyed, this, [this, key, socket, address, perAddress]() {
        // Null as well as this socket: by the time a child's destroyed() runs, the parent
        // has cleared every QPointer to itself. A live entry under the same key belongs to
        // a newer connection and stays.
        const QPointer<QAbstractSocket> held{m_pendingRawSockets.value(key)};
        if (held.isNull() || held.data() == socket) {
            m_pendingRawSockets.remove(key);
        }
        // The socket is gone, closed by QHttpServer or destroyed with its upgraded
        // connection. It leaves the socket ceilings.
        --m_socketsGlobal;
        if (perAddress && --m_socketsPerIp[address] <= 0) {
            m_socketsPerIp.remove(address);
        }
    });
    m_pendingRawSockets.insert(key, socket);
    timer->start(m_config.handshakeTimeoutMs);
}

QHttpServerResponse WebEdge::credentialResponse(const QHttpServerRequest &request)
{
    // A CDN delivered the app, so this is the browser's only HTTP request to this origin,
    // for a session. stampResponse() sets the cookie on the client route, as when this edge
    // serves the page. The body is empty.
    QHttpServerResponse response{QHttpServerResponse::StatusCode::NoContent};

    // The request carries credentials, so only a listed origin is answered. `Allow-Origin`
    // echoes that origin (a credentialed request refuses `*`), and `Vary` stops a cache
    // from reusing the answer for another origin.
    const QString origin{QString::fromUtf8(request.value("Origin"))};
    QHttpHeaders headers{response.headers()};
    if (!origin.isEmpty() && m_allowedOrigins.contains(origin)) {
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
    // Serve the rest of the bundle (the loader, the .wasm module, assets). Only files whose
    // canonical path is inside the canonical bundle root are reachable; absolute paths, NUL
    // and backslash are refused first.
    //
    // Skipped when a CDN delivers the bundle: every non-route path is then a 404.
    m_httpServer->route(QStringLiteral("/<arg>"),
                        [this](const QString &asset,
                               const QHttpServerRequest &request) {
        // Resolved per request: which bundle a caller may read depends on their session.
        const QString root{bundleFor(request)};
        const QString bundleRoot{canonicalRootOf(root)};
        if (asset.isEmpty() || QDir::isAbsolutePath(asset)
            || asset.contains(QLatin1Char('\0')) || asset.contains(QLatin1Char('\\'))) {
            return QHttpServerResponse{QHttpServerResponse::StatusCode::Forbidden};
        }
        const QString resolved{QFileInfo{QDir{root}, asset}.canonicalFilePath()};
        if (resolved.isEmpty()) {
            // No such file in the bundle. This route and the shell fallback share the
            // "/<arg>" template and this one is registered first, so a one-segment client
            // route ("/about") lands here and is answered as the fallback would.
            return shellOrNotFound(root, asset, request);
        }
        if (bundleRoot.isEmpty() || !resolved.startsWith(bundleRoot + QLatin1Char('/'))) {
            // It exists outside the bundle: refused, never treated as a client route.
            return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
        }
        if (!QFileInfo{resolved}.isFile()) {
            // A directory inside the bundle serves nothing (the route is one segment deep
            // and the ETag cache indexes top-level files only). Treating it as a missing
            // asset keeps a client route named after a bundle directory ("/assets") working
            // on refresh. Checked after containment, so probing outside the bundle is still
            // refused first.
            return shellOrNotFound(root, asset, request);
        }
        // Serve a precompressed variant (Brotli or gzip) when the client accepts it; the
        // resource keeps its type through Content-Encoding. The .wasm and the Emscripten
        // glue .js are the largest files on a first visit.
        //
        // Chosen before the conditional check, because each encoding is its own
        // representation with its own ETag, and a validator for one earns a 304 for that one
        // only (RFC 9110 section 8.8.3).
        const QByteArray mime{bundleContentType(resolved)};
        QByteArray encoding;
        QByteArray encodedBody;
        bool hasVariants{false};
        if (!mime.isEmpty()) {
            const QByteArray accept{request.value("Accept-Encoding")};
            const QDateTime sourceTime{QFileInfo{resolved}.lastModified()};
            const std::pair<const char *, const char *> variants[]{{".br", "br"},
                                                                   {".gz", "gzip"}};
            for (const auto &[suffix, name] : variants) {
                const QFileInfo variant{resolved + QLatin1String(suffix)};
                // A variant older than its source is stale (`synqt dev` rebuilds in place
                // and leaves the first build's variants), so it is not a representation.
                if (!variant.isFile() || variant.lastModified() < sourceTime) {
                    continue;
                }
                hasVariants = true;
                if (!encoding.isEmpty() || !accept.contains(name)) {
                    continue;
                }
                QFile file{variant.filePath()};
                if (file.open(QIODevice::ReadOnly)) {
                    encoding = name;
                    encodedBody = file.readAll();
                }
            }
        }
        const QByteArray etag{representationTag(etagFor(resolved), encoding)};
        const auto described{[&](QHttpServerResponse &response) {
            QHttpHeaders headers{response.headers()};
            if (!encoding.isEmpty()) {
                headers.append(QHttpHeaders::WellKnownHeader::ContentEncoding, encoding);
            }
            // Every representation of a file that has variants says so, the plain one
            // included, or a shared cache could hand an encoded body to a client that never
            // asked for it.
            if (hasVariants) {
                headers.append(QHttpHeaders::WellKnownHeader::Vary,
                               QByteArrayLiteral("Accept-Encoding"));
            }
            if (!etag.isEmpty()) {
                headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::ETag, etag);
            }
            response.setHeaders(std::move(headers));
        }};
        if (auto notModified{notModifiedFor(request, etag)}) {
            described(*notModified);
            return std::move(*notModified);
        }
        QHttpServerResponse response{encoding.isEmpty()
                                         ? QHttpServerResponse::fromFile(resolved)
                                         : QHttpServerResponse{mime, encodedBody}};
        described(response);
        return response;
    });
    // The application shell for any unmatched path, so a deep link or a refresh on
    // "/c/summer-sale" loads the app instead of a 404.
    //
    // A route rather than setMissingHandler(): Qt answers a missing handler through a
    // QHttpServerResponder without running the after-request handlers, so the shell would
    // lack CSP, COOP and COEP. Registered last, so every real route wins. The QUrl
    // parameter lets "<arg>" capture several segments ("/a/b/c").
    m_httpServer->route(QStringLiteral("/<arg>"), QHttpServerRequest::Method::Get
                                                       | QHttpServerRequest::Method::Head,
                        [this](const QUrl &rest, const QHttpServerRequest &request) {
        return shellOrNotFound(bundleFor(request), rest.path(), request);
    });
}

QHttpServerWebSocketUpgradeResponse WebEdge::verifyUpgrade(const QHttpServerRequest &request)
{
    // The upgrade request arrived in time. Cancel the handshake-timeout timer.
    const QString key{peerKey(request.remoteAddress().toString(), request.remotePort())};
    if (QTimer *timer{m_pendingTimers.take(key)}) {
        timer->stop();
        timer->deleteLater();
    }

    // 1. Origin check. The primary defense against cross-site WebSocket hijacking.
    const QString origin{QString::fromUtf8(request.value("Origin"))};
    if (!m_allowedOrigins.contains(origin)) {
        emit upgradeRejected(QStringLiteral("origin not allowed: %1").arg(origin));
        return QHttpServerWebSocketUpgradeResponse::deny(
            403, QByteArrayLiteral("origin not allowed"));
    }

    // 2. Session credential. The cookie must map to a live session.
    const QByteArray sessionId{sessionIdOf(request)};
    if (!m_sessionManager->isLive(sessionId)) {
        emit upgradeRejected(QStringLiteral("no valid session"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            401, QByteArrayLiteral("no valid session"));
    }

    // 3. Scope precondition: an anonymous connection is rejected when identity is required.
    //
    // Anonymous means the session carries no identity. The session must be checked:
    // refusing on the flag alone would make `identity.required: true` refuse signed-in
    // visitors too.
    if (m_config.identityRequired) {
        const SessionRecord *record{m_sessionManager->lookup(sessionId)};
        if (!record || record->identity.isEmpty()) {
            emit upgradeRejected(QStringLiteral("authentication required"));
            return QHttpServerWebSocketUpgradeResponse::deny(
                403, QByteArrayLiteral("authentication required"));
        }
    }

    // 4. Rate and resource checks: the per-IP and global connection caps.
    //
    // The cap counts the visitor address, which is the peer until the deployment names a
    // balancer. Behind a balancer, counting the peer would put every visitor in one bucket.
    const QString ip{m_clientAddress.resolve(request.remoteAddress(),
                                             request.value("X-Forwarded-For"))};
    if (m_activeGlobal >= m_config.maxConnectionsGlobal
        || m_activePerIp.value(ip) >= m_config.maxConnectionsPerIp) {
        emit upgradeRejected(QStringLiteral("connection cap reached"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            503, QByteArrayLiteral("too many connections"));
    }

    // Accepted: stash the verified id by peer, so the accepted socket (whose headers cannot
    // be reread) is bound to its session when hosted. Done last, so a refused upgrade
    // leaves nothing behind.
    rememberVerifiedSession(key, sessionId, ip);
    emit upgradeAccepted(key);
    return QHttpServerWebSocketUpgradeResponse::accept();
}

void WebEdge::rememberVerifiedSession(const QString &peer, const QByteArray &sessionId,
                                      const QString &clientIp)
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    // An accepted upgrade is hosted in the same event-loop turn, so an entry still here
    // after the handshake window belongs to a socket that never arrived (the peer hung up
    // between the 101 and the first frame). The sweep keeps the map bounded by the accept
    // rate.
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
    // Each Source gets its own QML context with its own Caller (and Client alias) and the
    // edge's consumed mesh accessors (Database, ...).
    QQmlContext *context{new QQmlContext{m_engine->rootContext(), parent}};
    if (caller) {
        context->setContextProperty(QStringLiteral("Caller"), caller);
        context->setContextProperty(QStringLiteral("Client"), caller);  // browser-user alias
    }
    for (auto it{m_contextObjects.constBegin()}; it != m_contextObjects.constEnd(); ++it) {
        context->setContextProperty(it.key(), it.value());
    }
    QQmlComponent component{m_engine, QUrl::fromLocalFile(connectPoint.serverFile)};
    // Checked before create(), whose own "Component is not ready" says less than the error
    // below.
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
    // A shared Source's Caller starts as nobody and becomes the current caller for each
    // forwarded call (SynQt::Caller::adopt). Minted here so the context naming it is built
    // once, with the Source.
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
    // This Source holds the state every mirror publishes, for all sessions, so it applies
    // no `<scope>` gate. Each mirror gates for its own session.
    SourceFactory::holdsSharedState(source);
    entry.source = source;
    entry.caller = caller;
    return source;
}

QObject *WebEdge::sourceForConnection(const WebEdgeConnectPoint &connectPoint,
                                      const QByteArray &sessionId, QString *error)
{
    // Every hosted connection has a session: the verifier admits nobody without one, and
    // hostConnection() ends a socket without a verified session.
    Q_ASSERT(!sessionId.isEmpty());
    SessionSources &sources{m_sessionSources[sessionId]};
    if (QObject *existing{sources.byConnectPoint.value(connectPoint.name)}) {
        return existing;
    }
    // Parented to the edge, not the socket: it outlives the connection until
    // releaseSessionSources(). Its Caller holds the session, so `Caller.emitSignal` reaches
    // every tab of that session.
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
    // Every relay pointed at this entity follows the replacement Replica; the runtime
    // retires the old one a turn later. The browser never sees the mesh reconnect.
    for (auto it{m_relayTargets.cbegin()}; it != m_relayTargets.cend(); ++it) {
        if (it.value() == entity) {
            SourceFactory::relay(it.key(), replica);
        }
    }
}

void WebEdge::pointRelay(QObject *source, const QString &entity)
{
    if (entity.isEmpty()) {
        SourceFactory::relay(source, nullptr);
        m_relayTargets.remove(source);
        return;
    }
    SourceFactory::relay(source, m_entitiesBehind.value(entity).data());
    if (!m_relayTargets.contains(source)) {
        connect(source, &QObject::destroyed, this,
                [this, source]() { m_relayTargets.remove(source); });
    }
    m_relayTargets.insert(source, entity);
}

bool WebEdge::servesScope(const WebEdgeConnectPoint &connectPoint, const Caller *caller) const
{
    if (!connectPoint.scope.isEmpty() && !caller->hasScope(connectPoint.scope)) {
        return false;
    }
    if (connectPoint.behind.isEmpty()) {
        return true;
    }
    // A scope without a line is served by nobody, and so is a tier whose entity is not up
    // yet.
    const QString entity{entityFor(connectPoint, caller->scope())};
    return !entity.isEmpty() && !m_entitiesBehind.value(entity).isNull();
}

QString WebEdge::entityFor(const WebEdgeConnectPoint &connectPoint,
                           const QString &scope) const
{
    if (const QString named{connectPoint.behind.value(scope)}; !named.isEmpty()) {
        return named;
    }
    if (!m_config.scopesHierarchical) {
        // Set-based: one scope, no order to fall back along.
        return QString{};
    }
    const qsizetype held{m_config.scopeOrder.indexOf(scope)};
    if (held < 0) {
        return QString{};
    }
    // The highest tier at or below the caller's scope, never above.
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
    // Built from the C++ helper: a front implements nothing, so there is no server file.
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
    pointRelay(source, entity);
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
    // The Caller is bound before mirroring, so a call arriving in the same turn has its
    // caller.
    caller->setSource(mirror);
    SourceFactory::mirror(mirror, shared, caller);
    return mirror;
}

void WebEdge::dropSession(const QByteArray &sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }
    // A scope change does not end a session. `SessionManager::setScope` rotates the
    // credential and reports the old id as removed, and the manager remembers the hand-off,
    // so a rotation is ignored here. What remains are the three real ends: signed out,
    // revoked, expired.
    if (!m_sessionManager->rotationOf(sessionId).isEmpty()) {
        return;
    }
    // Taken first: closing a socket runs its disconnected handler, which re-enters
    // releaseSessionSources.
    const QList<WebSocketTransport *> open{m_sessionSockets.values(sessionId)};
    m_sessionSockets.remove(sessionId);
    for (WebSocketTransport *transport : open) {
        if (transport) {
            // Going Away, which the client's reconnect backoff expects. Sent through the
            // device, so it arrives even when the socket is on another thread.
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

    // The session's Sources move once, however many tabs it has: the first connection finds
    // the old key and moves it. Copied before the insert, which may rehash.
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
    // The session's last connection is gone, so its Sources go too. A returning user gets a
    // fresh Source. State that must survive belongs in the entity singleton or behind a
    // persistence connect point.
    for (QObject *source : std::as_const(entry->byConnectPoint)) {
        delete source;
    }
    m_sessionSources.erase(entry);
}

void WebEdge::onNewWebSocketConnection()
{
    while (std::unique_ptr<QWebSocket> pending{m_httpServer->nextPendingWebSocketConnection()}) {
        // Unparented: hostConnection() decides what carries it, which on a threaded edge is
        // a channel for another thread.
        hostConnection(pending.release());
    }
}

/// Give the accepted socket to its carrier and return the device QtRO writes to.
///
/// On a one-thread edge that is the socket itself. On a threaded edge the socket and the
/// raw socket under it become children of a channel for an IO thread, and the device stays
/// on this thread with the QtRO host and the Sources. The caller moves the channel last,
/// once the connection is hosted.
WebSocketTransport *WebEdge::carry(QWebSocket *socket, QObject *connection)
{
    QAbstractSocket *raw{m_pendingRawSockets.take(
        peerKey(socket->peerAddress().toString(), socket->peerPort()))};
    if (m_ioThreads && !raw) {
        // Should not happen: every accepted socket is recorded under the same key. If it
        // does, the connection stays on this thread; a QWebSocket read on one thread and
        // written on another is a data race.
        qWarning("SynQt: no raw socket found for an accepted upgrade; serving this "
                 "connection on the main thread instead of an IO thread");
    }
    if (!m_ioThreads || !raw) {
        socket->setParent(connection);
        // And the raw socket, which nothing owns: QHttpServer unparents it for the upgrade,
        // and the QWebSocket is not its parent. A socket already under the QWebSocket keeps
        // that owner, as in SocketChannel.
        if (raw && !isUnder(raw, socket)) {
            raw->setParent(connection);
        }
        return new WebSocketTransport{socket, connection};
    }
    SocketChannel *channel{new SocketChannel{socket, raw}};
    WebSocketTransport *transport{new WebSocketTransport{channel, connection}};
    // A batch may grow to what the browser end may receive: batching merges one pass of
    // writes into one WebSocket message.
    transport->setWriteBatchLimit(m_config.maxMessageBytes);
    return transport;
}

void WebEdge::hostConnectPoint(const WebEdgeConnectPoint &connectPoint,
                               const QByteArray &sessionId, QRemoteObjectHost *node,
                               QHash<QString, QObject *> *hosted)
{
    QString error;
    QObject *source{sourceForConnection(connectPoint, sessionId, &error)};
    if (!source) {
        // Logged as well as signalled. A Source that fails to load is a defect in the
        // entity, the same on every connection, and the browser only sees a connect point
        // that never arrives.
        qWarning("SynQt: connect point %s is not served on this connection: %s",
                 qUtf8Printable(connectPoint.name), qUtf8Printable(error));
        emit upgradeRejected(error);
        return;
    }
    // Remoted on this connection's own node. A per-caller Source is remoted on one node per
    // tab, which QtRO allows.
    if (!node->enableRemoting(source, connectPoint.name)) {
        qWarning("SynQt: connect point %s loaded but could not be remoted",
                 qUtf8Printable(connectPoint.name));
        emit upgradeRejected(
            QStringLiteral("enableRemoting failed for %1").arg(connectPoint.name));
        return;
    }
    hosted->insert(connectPoint.name, source);
}

void WebEdge::hostConnection(QWebSocket *socket)
{
    // Reject oversized frames before buffering (DoS guard).
    socket->setMaxAllowedIncomingMessageSize(static_cast<quint64>(m_config.maxMessageBytes));
    socket->setMaxAllowedIncomingFrameSize(static_cast<quint64>(m_config.maxMessageBytes));

    // The session and the visitor address, both stashed by the verifier for this peer: the
    // accepted socket's headers cannot be reread, and the forwarding header is gone, so
    // recomputing the address here would give the balancer's and release a bucket the
    // accept never counted.
    const QString key{peerKey(socket->peerAddress().toString(), socket->peerPort())};
    const VerifiedSession verified{m_pendingSessions.take(key)};
    const QByteArray sessionId{verified.id};
    if (sessionId.isEmpty()) {
        // No verifier record for this socket (swept as stale, or never verified). There is
        // no session to vouch for, so the connection is ended, not hosted as anonymous.
        qWarning("SynQt: an accepted upgrade from %s has no verified session; closing it",
                 qUtf8Printable(key));
        emit upgradeRejected(QStringLiteral("no verified session for the accepted socket"));
        QAbstractSocket *raw{m_pendingRawSockets.take(key)};
        if (raw && !isUnder(raw, socket)) {
            raw->setParent(socket);
        }
        socket->abort();
        socket->deleteLater();
        return;
    }
    const QString ip{verified.clientIp.isEmpty()
                         ? normalizedAddress(socket->peerAddress()).toString()
                         : verified.clientIp};

    // Everything this connection owns on this thread hangs off one object, so ending it is
    // one deletion, and the edge teardown can remove every connection before stopping the
    // threads. Children are deleted in insertion order, so the device is built after the
    // node: QtRO writes a last message to every listener as the node is destroyed.
    QObject *connection{new QObject{m_connections}};

    ++m_activeGlobal;
    ++m_activePerIp[ip];

    // Claimed before any Source is fetched and released when the socket closes. The count
    // keeps a session's shared Sources alive while any tab is open.
    ++m_sessionSources[sessionId].connections;

    // One QtRO host node per connection, and one Source per connect point on it, each with
    // a Caller bound to this session.
    QRemoteObjectHost *node{new QRemoteObjectHost{connection}};
    node->setHostUrl(QUrl{QStringLiteral("synqt-edge:///%1")
                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                     QRemoteObjectHost::AllowExternalRegistration);

    // A gate Caller reads the session's live scope for per-point decisions. It hosts no
    // Source and emits nothing (empty contract).
    Caller *gate{Caller::forUser(QString{}, m_sessionManager, sessionId, nullptr, node)};
    gate->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);

    // What this connection hosts, by point name, so a scope change can add or withdraw
    // points. Shared only with the rotation handler below.
    const auto hosted{std::make_shared<QHash<QString, QObject *>>()};
    for (const WebEdgeConnectPoint &connectPoint : m_config.connectPoints) {
        // Scope gating: a scoped point is never hosted for an under-scoped session, so its
        // Replica cannot be acquired. A front also hosts nothing for a scope no tier
        // serves.
        if (!servesScope(connectPoint, gate)) {
            continue;
        }
        hostConnectPoint(connectPoint, sessionId, node, hosted.get());
    }

    // The framework SessionState point: who this connection's visitor is. Hosted on every
    // connection, since `Session.scope` and `Session.identity` are always available to a
    // client.
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

    // The framework Pages point, hosted like the application points: a fresh Source per
    // connection with its own Caller, over the shared PageStore and PagesService.
    // PagesService gates each page per request.
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
    transport->setReadBufferLimit(m_config.maxMessageBytes * BufferFrames);
    // The same bound for writes: without it a tab that stops reading would make the edge
    // keep every published message for it.
    transport->setWriteBufferLimit(m_config.maxMessageBytes * BufferFrames);
    transport->open(QIODevice::ReadWrite);
    node->addHostSideConnection(transport);

    // The session this connection is bound to. An elevation rotates the credential
    // (SessionManager::setScope, through Caller.setScope), and every key the edge keeps for
    // this connection moves with it. Shared, so the rotation and disconnect handlers read
    // the same value.
    const auto liveSession{std::make_shared<QByteArray>(sessionId)};
    {
        m_sessionSockets.insert(sessionId, transport);
        // Received on `connection`, so it goes when the connection does.
        connect(m_sessionManager, &SessionManager::sessionRotated, connection,
                [this, liveSession, transport, node, gate, hosted](
                    const QByteArray &from, const QByteArray &to) {
            if (*liveSession != from) {
                return;
            }
            followRotation(from, to, transport);
            *liveSession = to;
            // A rotation is a scope change, so the hosted points are decided again for the
            // new scope. A point the visitor is now entitled to is hosted on this node, and
            // QtRO announces it, so the client's Replica comes up without a reconnect. A
            // point the session no longer qualifies for is withdrawn, and its replication
            // stops.
            //
            // The gate reads the session live and already has the new id: Caller subscribes
            // to this signal in forUser(), which connected earlier, and Qt delivers in
            // connection order.
            //
            // A front is decided again too, including which tier answers it (`behind:`
            // names one per tier), so a demoted caller no longer reaches the higher tier's
            // entity.
            for (const WebEdgeConnectPoint &connectPoint :
                 std::as_const(m_config.connectPoints)) {
                const bool fronted{!connectPoint.behind.isEmpty()};
                if (connectPoint.scope.isEmpty() && !fronted) {
                    continue;
                }
                const QString scope{gate->scope()};
                const bool entitled{servesScope(connectPoint, gate)};
                QObject *current{hosted->value(connectPoint.name)};
                if (entitled && !current) {
                    hostConnectPoint(connectPoint, to, node, hosted.get());
                    current = hosted->value(connectPoint.name);
                    // A continued session Source still points at the old tier; decide
                    // again.
                    if (fronted && current) {
                        pointRelay(current, entityFor(connectPoint, scope));
                    }
                } else if (!entitled && current) {
                    hosted->remove(connectPoint.name);
                    // The Source stays: a per-session Source belongs to the session and is
                    // reclaimed with it (releaseSessionSources). A raised scope continues
                    // from it.
                    withdrawSource(node, current);
                    if (fronted) {
                        // Point it at nothing, so it stops pulling a tier the caller no
                        // longer reaches.
                        pointRelay(current, QString{});
                    }
                } else if (entitled && current && fronted) {
                    const QString entity{entityFor(connectPoint, scope)};
                    if (m_relayTargets.value(current) != entity) {
                        pointRelay(current, entity);
                    }
                }
            }
        });
    }
    // Watched on the device, which is on this thread and relays the socket's disconnect.
    connect(transport, &WebSocketTransport::disconnected, this,
            [this, transport, connection, ip, liveSession]() {
        --m_activeGlobal;
        if (--m_activePerIp[ip] <= 0) {
            m_activePerIp.remove(ip);
        }
        m_sessionSockets.remove(*liveSession, transport);
        releaseSessionSources(*liveSession);
        // Deletes the node, the Sources, the Callers and the device; the device deletes the
        // socket on the socket's thread.
        connection->deleteLater();
    });

    // Last: the socket moves to its thread with the connection already hosted. Anything
    // QtRO has written waits in the device batch and crosses on the next pass.
    if (m_ioThreads) {
        transport->moveSocketToThread(m_ioThreads->nextThread());
    }
}

} // namespace SynQt
