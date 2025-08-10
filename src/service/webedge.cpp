// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "webedge.h"

#include "caller.h"
#include "identityprovider.h"
#include "pageseed.h"
#include "pagesedgesource.h"
#include "pagesservice.h"
#include "pagestore.h"
#include "sessionmanager.h"
#include "topology.h"           // loadCertificate / loadPrivateKey
#include "websockettransport.h" // reused host-side (from src/transport)

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QHttpHeaders>
#include <QHttpServer>
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

#include <functional>
#include <optional>
#include <utility>

namespace SynQt {

namespace {

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
    , m_sessionManager{new SessionManager{m_config.defaultScope,
                                          m_config.sessionTtlMinutes, this}}
{
}

WebEdge::~WebEdge() = default;

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
// frame of data, not a feed, so both bounds are generous for every honest use. They exist
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
/// untyped seedFor matches; annotating a parameter type (`route: string`) changes the method
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
    // caller, say); it converts to nothing, which the object check downstream reports.
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
    // QJsonDocument::fromVariant() knows nothing about; unwrap it to plain containers
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
    // Once per route, never once per request: a browser chooses how often it asks for a
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
    // (identityprovider.cpp): the app writes a PageSeed QML object carrying
    // `function seedFor(route, parameters, caller)`, and the edge calls it after the
    // route's scope check. Each hook is built once here, never per request and never per
    // connection; a project whose routes declare no seed builds nothing at all.
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
        QObject *hook{component->create()};
        if (!hook) {
            // The hook's own file and QML diagnostic, which the developer wrote; never
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
            // The single most likely mistake: the edge calls seedFor with untyped
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

QString WebEdge::httpOrigin() const
{
    const QString scheme{m_config.usesTls() ? QStringLiteral("https") : QStringLiteral("http")};
    return QStringLiteral("%1://%2:%3").arg(scheme, m_config.host).arg(m_port);
}

QString WebEdge::wssOrigin() const
{
    const QString scheme{m_config.usesTls() ? QStringLiteral("wss") : QStringLiteral("ws")};
    return QStringLiteral("%1://%2:%3").arg(scheme, m_config.host).arg(m_port);
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
        // pthread workers are actually spawned from: measured on the real threaded bundle
        // (Qt 6.11.1, Emscripten 4.0.7), a strict worker-src 'self' with no blob: kept the
        // page isolated, spawned every pthread worker, and logged no CSP violation in
        // Chromium or Firefox. blob: is kept as a deliberate margin, not a present need:
        // WebKit is a version 1 target that could not be measured here, a future emsdk may
        // go back to blob: workers, and the exposure is near zero (constructing a blob:
        // worker already needs script execution, which script-src governs). See
        // <https://synqt.org/csp/>; do not restate the blob: need as fact without a
        // Safari measurement.
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

QByteArray WebEdge::issueSessionCookie()
{
    const QByteArray token{m_sessionManager->createSession()};

    QByteArray cookie{m_config.cookieName.toUtf8() + "=" + token + "; HttpOnly; Path=/"};
    if (m_config.originModel == QLatin1String("split_origin")) {
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
    const QString requested{bundlePathFor(request.url().path())};
    if (!requested.isEmpty()) {
        const QByteArray etag{etagFor(requested)};
        if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
            headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
        }
        headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                       QByteArrayLiteral("no-cache"));
    }

    // Issue an (anonymous) session on the page load, so the browser has a credential to
    // present at the wss upgrade.
    if (request.url().path() == m_config.clientRoute) {
        headers.append(QHttpHeaders::WellKnownHeader::SetCookie, issueSessionCookie());
    }
    response.setHeaders(std::move(headers));
}

void WebEdge::cacheBundle()
{
    m_etags.clear();
    const QDir root{m_config.bundleDir};
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

QByteArray WebEdge::etagFor(const QString &path) const
{
    return m_etags.value(QFileInfo{path}.canonicalFilePath());
}

QString WebEdge::bundlePathFor(const QString &urlPath) const
{
    if (urlPath == m_config.clientRoute) {
        return QDir{m_config.bundleDir}.filePath(QStringLiteral("index.html"));
    }
    const QString name{urlPath.mid(1)};
    if (name.isEmpty() || name.contains(QLatin1Char('/'))) {
        return {};
    }
    const QString resolved{QFileInfo{QDir{m_config.bundleDir}, name}.canonicalFilePath()};
    return m_etags.contains(resolved) ? resolved : QString{};
}

QHttpServerResponse WebEdge::shellOrNotFound(const QString &path,
                                             const QHttpServerRequest &request)
{
    // Only a navigation gets the shell. A POST or a DELETE to an unknown URL is a
    // client bug or a probe, and answering it with HTML would hide that.
    if (request.method() != QHttpServerRequest::Method::Get
        && request.method() != QHttpServerRequest::Method::Head) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
    }
    // An asset request (its last segment has an extension) must fail honestly rather
    // than receive HTML with a 200, which would surface as a confusing module-load
    // error instead of a missing file.
    const qsizetype lastSlash{path.lastIndexOf(QLatin1Char('/'))};
    if (path.mid(lastSlash + 1).contains(QLatin1Char('.'))) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
    }
    const QString index{QDir{m_config.bundleDir}.filePath(QStringLiteral("index.html"))};
    if (auto notModified{notModifiedFor(request, etagFor(index))}) {
        stampShell(*notModified);
        return std::move(*notModified);
    }
    QHttpServerResponse response{QHttpServerResponse::fromFile(index)};
    stampShell(response);
    return response;
}

void WebEdge::stampShell(QHttpServerResponse &response)
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
    const QString index{QDir{m_config.bundleDir}.filePath(QStringLiteral("index.html"))};
    const QByteArray etag{etagFor(index)};
    QHttpHeaders headers{response.headers()};
    if (!etag.isEmpty() && !headers.contains(QHttpHeaders::WellKnownHeader::ETag)) {
        headers.append(QHttpHeaders::WellKnownHeader::ETag, etag);
    }
    headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                   QByteArrayLiteral("no-cache"));
    headers.append(QHttpHeaders::WellKnownHeader::SetCookie, issueSessionCookie());
    response.setHeaders(std::move(headers));
}

void WebEdge::computeScriptHashes()
{
    m_scriptHashes.clear();
    QFile index{QDir{m_config.bundleDir}.filePath(QStringLiteral("index.html"))};
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

    // 1. Instantiate the shared connect points once (created here, hosted on every
    //    connection's node so their state stays in sync across browsers). A per_session
    //    connect point is instead instantiated per connection in hostConnection(), so
    //    each instance carries a Caller for its one user.
    for (const WebEdgeConnectPoint &connectPoint : m_config.connectPoints) {
        if (connectPoint.instance != InstanceMode::Shared) {
            continue;
        }
        QString error;
        QObject *source{createSource(connectPoint, nullptr, this, &error)};
        if (!source) {
            m_errorString = error;
            return false;
        }
        m_sharedSources.insert(connectPoint.name, source);
    }

    // 1.5. The framework's own Pages connect point (edge-delivered pages): one
    //      PageStore/PagesService shared by every connection, since the page table
    //      is the same for everyone. Built once, here, and never rebuilt per
    //      connection; a per-connection PagesEdgeSource is created in
    //      hostConnection() so each carries its own Caller. Nothing is created when
    //      the project configures no pages, so that app pays nothing.
    if (!m_config.pages.isEmpty()) {
        m_pageStore = new PageStore{m_config.pagesDir, this};
        for (const WebEdgePage &page : m_config.pages) {
            m_pageStore->addPage(page.path, page.file, page.scope);
        }
        // Development-only watching, keyed to an explicit dev flag. Only the
        // "synqt dev" launch path (dev_command() in tools/synqt/synqt/run.py, via the
        // generated edge's --dev option) sets devWatch; a built or served edge leaves
        // it false and never watches. Deriving "development" from the absence of local
        // TLS would be wrong: a production edge that terminates TLS at a reverse proxy
        // and speaks plaintext on the loopback hop has no local cert yet is not dev.
        if (m_config.devWatch) {
            m_pageStore->setWatching(true);
        }
        m_pagesService = new PagesService{m_pageStore, this};
        buildPageSeedHooks();
    }

    // 2. The HTTP server: serve the bundle, stamp headers, and verify upgrades.
    m_httpServer = new QHttpServer{this};
    m_httpServer->route(m_config.clientRoute, [this](const QHttpServerRequest &request) {
        const QString index{QDir{m_config.bundleDir}.filePath(QStringLiteral("index.html"))};
        if (auto notModified{notModifiedFor(request, etagFor(index))}) {
            return std::move(*notModified);
        }
        return QHttpServerResponse::fromFile(index);
    });

    // Login/callback/logout: the whole OAuth flow runs here, on the edge. The browser
    // ends with only a session cookie; the client secret and tokens never leave.
    if (m_config.identity.enabled) {
        CookiePolicy cookie;
        cookie.name = m_config.cookieName;
        cookie.sameSiteNone = (m_config.originModel == QLatin1String("split_origin"));
        cookie.secure = m_config.usesTls();
        m_identity = new IdentityProvider{m_config.identity, m_sessionManager, m_engine,
                                          httpOrigin(), cookie, this};
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
    }
    // Serve the rest of the bundle (the loader, the .wasm module, assets). Only files
    // that resolve to a real path INSIDE the bundle directory are reachable: reject
    // absolute paths and NUL/backslash, then verify the canonical (symlink- and
    // ..-resolved) path stays under the canonical bundle root.
    const QString bundleRoot{QDir{m_config.bundleDir}.canonicalPath()};
    m_httpServer->route(QStringLiteral("/<arg>"),
                        [this, bundleRoot](const QString &asset,
                                           const QHttpServerRequest &request) {
        if (asset.isEmpty() || QDir::isAbsolutePath(asset)
            || asset.contains(QLatin1Char('\0')) || asset.contains(QLatin1Char('\\'))) {
            return QHttpServerResponse{QHttpServerResponse::StatusCode::Forbidden};
        }
        const QString resolved{QFileInfo{QDir{m_config.bundleDir}, asset}.canonicalFilePath()};
        if (resolved.isEmpty()) {
            // The bundle holds no such file. This route and the shell fallback below
            // share the "/<arg>" template and this one is registered first, so a
            // single-segment client route ("/about") is matched here and would never
            // reach the fallback. Answer it on the fallback's own terms.
            return shellOrNotFound(asset, request);
        }
        if (bundleRoot.isEmpty() || !resolved.startsWith(bundleRoot + QLatin1Char('/'))) {
            // It exists, but outside the bundle. Refuse it, and never dress the attempt
            // up as a client route.
            return QHttpServerResponse{QHttpServerResponse::StatusCode::NotFound};
        }
        if (!QFileInfo{resolved}.isFile()) {
            // A directory inside the bundle serves nothing: this route is one segment
            // deep and the ETag cache indexes top-level files only, so nothing under it
            // is reachable anyway. Treating it as "no such asset" is what keeps a client
            // route named after a bundle directory ("/assets") working on refresh, when
            // its neighbors already do. After the containment test, so an attempt to
            // probe outside the bundle is still refused before anything else looks at
            // the path.
            return shellOrNotFound(asset, request);
        }
        if (auto notModified{notModifiedFor(request, etagFor(resolved))}) {
            return std::move(*notModified);
        }
        // Serve a precompressed variant when the client accepts it (the build
        // precompresses with Brotli and gzip): the bytes are smaller and the resource
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
    // remainder ("/a/b/c"), not just one path component.
    m_httpServer->route(QStringLiteral("/<arg>"), QHttpServerRequest::Method::Get
                                                       | QHttpServerRequest::Method::Head,
                        [this](const QUrl &rest, const QHttpServerRequest &request) {
        return shellOrNotFound(rest.path(), request);
    });
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
    connect(socket, &QObject::destroyed, this, [this, key]() { m_pendingTimers.remove(key); });
    m_pendingTimers.insert(key, timer);
    timer->start(m_config.handshakeTimeoutMs);
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

    // 2. Session credential: the cookie must map to a live session. Stash the verified id
    //    by peer so the accepted socket (whose headers are not re-readable) can be bound
    //    to its session when it is hosted.
    const QByteArray sessionId{sessionIdFromCookie(request.value("Cookie"))};
    if (!m_sessionManager->isLive(sessionId)) {
        emit upgradeRejected(QStringLiteral("no valid session"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            401, QByteArrayLiteral("no valid session"));
    }
    m_pendingSessions.insert(key, sessionId);

    // 3. Scope precondition: an anonymous connection is rejected when identity is
    //    required (per-connect-point scope gating lands with sessions in M7).
    if (m_config.identityRequired) {
        emit upgradeRejected(QStringLiteral("authentication required"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            403, QByteArrayLiteral("authentication required"));
    }

    // 4. Rate and resource checks: per-IP and global connection caps.
    const QString ip{request.remoteAddress().toString()};
    if (m_activeGlobal >= m_config.maxConnectionsGlobal
        || m_activePerIp.value(ip) >= m_config.maxConnectionsPerIp) {
        emit upgradeRejected(QStringLiteral("connection cap reached"));
        return QHttpServerWebSocketUpgradeResponse::deny(
            503, QByteArrayLiteral("too many connections"));
    }

    emit upgradeAccepted(key);
    return QHttpServerWebSocketUpgradeResponse::accept();
}

QObject *WebEdge::createSource(const WebEdgeConnectPoint &connectPoint, QObject *caller,
                              QObject *parent, QString *error)
{
    // Each Source gets its own QML context so a per_session instance can see its Caller
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
    QObject *source{component.create(context)};
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

void WebEdge::onNewWebSocketConnection()
{
    while (std::unique_ptr<QWebSocket> pending{m_httpServer->nextPendingWebSocketConnection()}) {
        QWebSocket *socket{pending.release()};
        socket->setParent(this);
        hostConnection(socket);
    }
}

void WebEdge::hostConnection(QWebSocket *socket)
{
    // Reject oversized frames before buffering (DoS guard).
    socket->setMaxAllowedIncomingMessageSize(static_cast<quint64>(m_config.maxMessageBytes));
    socket->setMaxAllowedIncomingFrameSize(static_cast<quint64>(m_config.maxMessageBytes));

    const QString ip{socket->peerAddress().toString()};
    ++m_activeGlobal;
    ++m_activePerIp[ip];
    connect(socket, &QWebSocket::disconnected, this, [this, socket, ip]() {
        --m_activeGlobal;
        if (--m_activePerIp[ip] <= 0) {
            m_activePerIp.remove(ip);
        }
        socket->deleteLater();  // deletes the per-connection node/sources/caller parented to it
    });

    // Identify the session behind this socket: the id the verifier stashed for this peer
    // (the accepted socket's handshake headers are not re-readable server-side).
    const QString key{peerKey(socket->peerAddress().toString(), socket->peerPort())};
    const QByteArray sessionId{m_pendingSessions.take(key)};

    // One QtRO host node per connection. per_session Sources are minted fresh with a
    // Caller bound to this session; shared Sources are the single instances, hosted here
    // too so their state stays in sync across every browser.
    QRemoteObjectHost *node{new QRemoteObjectHost{socket}};
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
        QObject *source{nullptr};
        if (connectPoint.instance == InstanceMode::Shared) {
            source = m_sharedSources.value(connectPoint.name);
        } else {
            Caller *caller{Caller::forUser(connectPoint.contract, m_sessionManager,
                                           sessionId, nullptr, socket)};
            caller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
            QString error;
            source = createSource(connectPoint, caller, socket, &error);
            if (!source) {
                emit upgradeRejected(error);
                continue;
            }
            caller->setParent(source);
            caller->setSource(source);  // Caller.emitSignal reaches this one caller
        }
        if (source && !node->enableRemoting(source, connectPoint.name)) {
            emit upgradeRejected(
                QStringLiteral("enableRemoting failed for %1").arg(connectPoint.name));
        }
    }

    // The framework's own Pages connect point, hosted the same way as every
    // per_session connect point above: a fresh Source per connection, carrying this
    // connection's own Caller, over the PageStore/PagesService shared by every
    // connection. Page-level scope gating happens inside PagesService, per request,
    // so there is no single connect-point-level scope to check here.
    if (m_pagesService) {
        Caller *pagesCaller{Caller::forUser(QStringLiteral("Pages"), m_sessionManager,
                                            sessionId, nullptr, socket)};
        pagesCaller->setScopeOrder(m_config.scopeOrder, m_config.scopesHierarchical);
        PagesEdgeSource *pagesSource{
            new PagesEdgeSource{m_pageStore, m_pagesService, pagesCaller, socket}};
        pagesCaller->setParent(pagesSource);
        pagesCaller->setSource(pagesSource);
        if (!node->enableRemoting(pagesSource, QStringLiteral("Pages"))) {
            emit upgradeRejected(QStringLiteral("enableRemoting failed for Pages"));
        }
    }

    WebSocketTransport *transport{new WebSocketTransport{socket, socket}};
    transport->open(QIODevice::ReadWrite);
    node->addHostSideConnection(transport);
}

} // namespace SynQt
