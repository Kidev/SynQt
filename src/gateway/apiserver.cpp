// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "apiserver.h"

#include "api.h"
#include "apirequest.h"
#include "constanttime.h"
#include "topology.h"  // loadCertificate / loadPrivateKey

#include <QDateTime>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerConfiguration>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerResponder>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>

namespace SynQt {

namespace {

/// A refusal with the JSON body an API caller expects, so a machine caller parses one
/// shape.
QHttpServerResponse errorResponse(int status, const QString &message)
{
    const QJsonObject payload{{QStringLiteral("error"), message}};
    return QHttpServerResponse{QByteArrayLiteral("application/json"),
                               QJsonDocument{payload}.toJson(QJsonDocument::Compact),
                               static_cast<QHttpServerResponse::StatusCode>(status)};
}

QVariantMap headersOf(const QHttpServerRequest &request, const QByteArray &drop)
{
    QVariantMap headers;
    const QHttpHeaders received{request.headers()};
    for (qsizetype index{0}; index < received.size(); ++index) {
        const QByteArray name{received.nameAt(index).toString().toUtf8().toLower()};
        if (name == drop.toLower()) {
            continue;  // the credential that admitted the call is not the handler's business
        }
        headers.insert(QString::fromUtf8(name),
                       QString::fromUtf8(received.valueAt(index).toByteArray()));
    }
    return headers;
}

/// The body as QML sees it: a parsed object for JSON, text otherwise, an invalid QVariant
/// when there is none.
QVariant bodyOf(const QHttpServerRequest &request)
{
    const QByteArray raw{request.body()};
    if (raw.isEmpty()) {
        return QVariant{};
    }
    const QByteArray contentType{
        request.headers().value(QHttpHeaders::WellKnownHeader::ContentType).toByteArray()};
    if (contentType.contains("application/json")) {
        QJsonParseError error;
        const QJsonDocument document{QJsonDocument::fromJson(raw, &error)};
        if (error.error == QJsonParseError::NoError) {
            return document.toVariant();
        }
    }
    return QString::fromUtf8(raw);
}

/// How long a connection may sit idle between requests before the transport closes it. This
/// ends half-open connections that size and rate limits cannot see.
constexpr int kKeepAliveTimeoutSeconds{30};

/// The deadline used when the topology names none. See handle().
constexpr int kFallbackReplyTimeoutMs{15000};

/// How many addresses the rate window may name. Past it a new address is refused and not
/// recorded, until the window turns.
constexpr int kMaxRateEntries{4096};

QString methodOf(const QHttpServerRequest &request)
{
    switch (request.method()) {
    case QHttpServerRequest::Method::Get:
        return QStringLiteral("GET");
    case QHttpServerRequest::Method::Post:
        return QStringLiteral("POST");
    case QHttpServerRequest::Method::Put:
        return QStringLiteral("PUT");
    case QHttpServerRequest::Method::Delete:
        return QStringLiteral("DELETE");
    case QHttpServerRequest::Method::Patch:
        return QStringLiteral("PATCH");
    case QHttpServerRequest::Method::Head:
        return QStringLiteral("HEAD");
    case QHttpServerRequest::Method::Options:
        return QStringLiteral("OPTIONS");
    default:
        return QStringLiteral("UNKNOWN");
    }
}

} // namespace

ApiServer::ApiServer(ApiConfig config, QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_clientAddress{m_config.trustedProxies}
    , m_engine{engine}
    , m_api{new Api{engine, this}}
    , m_requests{new QObject{this}}
{
}

ApiServer::~ApiServer() = default;

Api *ApiServer::api() const
{
    return m_api;
}

QString ApiServer::errorString() const
{
    return m_errorString;
}

quint16 ApiServer::serverPort() const
{
    return m_port;
}

bool ApiServer::start()
{
    if (m_config.port == 0 && m_config.host.isEmpty()) {
        m_errorString = QStringLiteral("network.inbound names no port to listen on");
        return false;
    }

    m_server = new QHttpServer{this};

    // Qt's own request ceilings, set before any route runs. The body check in refuse()
    // below applies after Qt has read the body, so on its own it bounds what a handler
    // gets, not what the process allocates (Qt's default is 32 MiB per connection). Setting
    // it here makes `network.inbound.max_body_bytes` the limit the transport enforces. The
    // idle timeout closes a peer that sends half a request and stops. The web edge does the
    // same (webedge.cpp).
    QHttpServerConfiguration httpConfiguration;
    httpConfiguration.setMaximumBodySize(m_config.maxBodyBytes);
    httpConfiguration.setKeepAliveTimeout(std::chrono::seconds{kKeepAliveTimeoutSeconds});
    // The socket ceilings, at accept, bound a caller that opens connections and never sends
    // a request. Per address only when the address is the caller's: behind a proxy every
    // socket is the proxy's.
    httpConfiguration.setMaximumConnections(
        static_cast<quint32>(qMax(0, m_config.maxConnectionsGlobal)));
    httpConfiguration.setMaximumConnectionsPerHost(
        m_config.trustedProxies.isEmpty()
            ? static_cast<quint32>(qMax(0, m_config.maxConnectionsPerIp))
            : quint32{0});
    m_server->setConfiguration(httpConfiguration);

    // One catch-all route: the routing table lives in `Api`, where QML declared it, so
    // there is one table and one 404.
    //
    // Answered through the responder, never by returning a future. Qt 6.12 keeps about 150
    // bytes for every already-settled QFuture a route returns, for as long as the keep-alive
    // connection it arrived on stays open, and nearly every answer here is settled at once.
    m_server->route(QStringLiteral("/<arg>"),
                    [this](const QUrl &, const QHttpServerRequest &request,
                           QHttpServerResponder &responder) { handle(request, responder); });
    m_server->route(QStringLiteral("/"),
                    [this](const QHttpServerRequest &request, QHttpServerResponder &responder) {
        handle(request, responder);
    });

    if (!m_config.certFile.isEmpty() && !m_config.keyFile.isEmpty()) {
        // As on the web edge: a surface that must terminate TLS and cannot refuses to
        // start.
        const QSslCertificate certificate{loadCertificate(m_config.certFile)};
        const QSslKey key{loadPrivateKey(m_config.keyFile)};
        if (certificate.isNull() || key.isNull()) {
            m_errorString = QStringLiteral("cannot terminate TLS with %1 and %2")
                                .arg(m_config.certFile, m_config.keyFile);
            return false;
        }
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
        // A machine caller presents no client certificate; it authenticates with its API
        // key.
        configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
        sslServer->setSslConfiguration(configuration);
        m_tcpServer = sslServer;
    } else {
        m_tcpServer = new QTcpServer{this};
    }

    if (!m_tcpServer->listen(QHostAddress{m_config.host}, m_config.port)) {
        m_errorString = m_tcpServer->errorString();
        return false;
    }
    m_port = m_tcpServer->serverPort();
    if (!m_server->bind(m_tcpServer)) {
        m_errorString = QStringLiteral("failed to bind the API server to the transport");
        return false;
    }
    m_api->setListening(true);
    return true;
}

QString ApiServer::callerAddress(const QHttpServerRequest &request) const
{
    // `value()` joins every `X-Forwarded-For` line in arrival order, as RFC 9110 defines.
    // Reading only the first line would read the client's own line when a proxy appends a
    // separate line.
    return m_clientAddress.resolve(request.remoteAddress(),
                                   request.value(QByteArrayLiteral("X-Forwarded-For")));
}

QString ApiServer::originOf(const QHttpServerRequest &request) const
{
    return QString::fromUtf8(
        request.headers().value(QHttpHeaders::WellKnownHeader::Origin).toByteArray());
}

std::optional<QHttpServerResponse> ApiServer::preflightAnswer(const QHttpServerRequest &request,
                                                              const QString &origin) const
{
    // An OPTIONS is a preflight only with an Origin and the method being asked about.
    // Otherwise it is an ordinary request for the routes.
    const QByteArray askedMethod{
        request.headers().value(QByteArrayLiteral("Access-Control-Request-Method")).toByteArray()};
    if (request.method() != QHttpServerRequest::Method::Options || origin.isEmpty()
        || askedMethod.isEmpty()) {
        return std::nullopt;
    }
    if (!m_config.allowedOrigins.contains(origin)) {
        // The same refusal a real request from this origin gets, with no CORS header, so
        // the browser does not send the real one.
        return errorResponse(403, QStringLiteral("origin %1 is not allowed to call this API")
                                      .arg(origin));
    }
    QHttpServerResponse response{QHttpServerResponse::StatusCode::NoContent};
    QHttpHeaders headers{response.headers()};
    headers.append(QByteArrayLiteral("Access-Control-Allow-Origin"), origin.toUtf8());
    headers.append(QByteArrayLiteral("Access-Control-Allow-Methods"),
                   QByteArrayLiteral("GET, POST, PUT, DELETE, PATCH, HEAD"));
    // Echo the headers the browser asked about (it always asks for the key header), instead
    // of allowing everything.
    const QByteArray askedHeaders{
        request.headers().value(QByteArrayLiteral("Access-Control-Request-Headers")).toByteArray()};
    headers.append(QByteArrayLiteral("Access-Control-Allow-Headers"),
                   askedHeaders.isEmpty() ? m_config.keyHeader : askedHeaders);
    headers.append(QByteArrayLiteral("Access-Control-Max-Age"), QByteArrayLiteral("600"));
    headers.append(QHttpHeaders::WellKnownHeader::Vary, QByteArrayLiteral("Origin"));
    response.setHeaders(std::move(headers));
    return response;
}

void ApiServer::allowOrigin(QHttpServerResponse &response, const QString &origin)
{
    // The requesting origin, never `*`, with `Vary` so a cache does not reuse it for
    // another origin. No `Allow-Credentials`: callers authenticate with the key header, not
    // cookies.
    QHttpHeaders headers{response.headers()};
    headers.append(QByteArrayLiteral("Access-Control-Allow-Origin"), origin.toUtf8());
    headers.append(QHttpHeaders::WellKnownHeader::Vary, QByteArrayLiteral("Origin"));
    response.setHeaders(std::move(headers));
}

bool ApiServer::withinRate(const QString &caller)
{
    if (m_config.ratePerMinutePerIp <= 0) {
        return true;
    }
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    if (now - m_rateWindowStartMs >= 60000) {
        m_rateWindow.clear();
        m_rateWindowStartMs = now;
    }
    // Keyed by caller address, so a caller with many addresses could grow the table one
    // address at a time. Emptying it at the ceiling would reset every budget in it, so a
    // full table refuses a new address instead, before recording it. Everything in it
    // belongs to the current minute (the table is dropped when the minute turns), so there
    // is nothing stale to prune.
    auto counted{m_rateWindow.find(caller)};
    if (counted == m_rateWindow.end()) {
        if (m_rateWindow.size() >= kMaxRateEntries) {
            return false;
        }
        counted = m_rateWindow.insert(caller, 0);
    }
    return ++counted.value() <= m_config.ratePerMinutePerIp;
}

QString ApiServer::refuse(const QHttpServerRequest &request, int *status) const
{
    // The API key. A `public: true` surface skips it; everything else requires the header,
    // compared in constant time.
    if (!m_config.anonymous) {
        const QByteArray presented{
            request.headers().value(m_config.keyHeader).toByteArray()};
        bool accepted{false};
        for (const QByteArray &key : m_config.apiKeys) {
            // Every candidate is compared in full, with no early return or break, so the
            // timing reveals nothing about a guess.
            accepted = constantTimeEquals(presented, key) || accepted;
        }
        if (!accepted) {
            *status = 401;
            return QStringLiteral("missing or unknown API key");
        }
    }

    // The origin, for browser callers. A request without Origin is not a browser and is
    // governed by the key. An Origin this surface does not name is refused, so a key leaked
    // into a page is useless.
    const QString origin{originOf(request)};
    if (!origin.isEmpty() && !m_config.allowedOrigins.contains(origin)) {
        *status = 403;
        return QStringLiteral("origin %1 is not allowed to call this API").arg(origin);
    }

    if (request.body().size() > m_config.maxBodyBytes) {
        *status = 413;
        return QStringLiteral("body larger than the %1 byte limit")
            .arg(m_config.maxBodyBytes);
    }
    return QString{};
}

void ApiServer::handle(const QHttpServerRequest &request, QHttpServerResponder &responder)
{
    // The address this request is counted against: the peer, until the topology names a
    // proxy. Resolved once; the rate limit and the handler both use it.
    const QString caller{callerAddress(request)};
    if (!withinRate(caller)) {
        emit requestRefused(QStringLiteral("rate limit for %1").arg(caller));
        responder.sendResponse(errorResponse(429, QStringLiteral("too many requests")));
        return;
    }

    // A preflight, before the key check: it never carries a key, and refusing it would make
    // the surface unreachable from any page. It is rate-limited like other requests.
    const QString origin{originOf(request)};
    if (const std::optional<QHttpServerResponse> preflight{preflightAnswer(request, origin)}) {
        responder.sendResponse(*preflight);
        return;
    }
    // Whether a page at this origin may read the answer, decided once and only for a named
    // origin.
    const bool corsAllowed{!origin.isEmpty() && m_config.allowedOrigins.contains(origin)};

    int status{400};
    const QString refusal{refuse(request, &status)};
    if (!refusal.isEmpty()) {
        emit requestRefused(refusal);
        QHttpServerResponse refused{errorResponse(status, refusal)};
        if (corsAllowed) {
            allowOrigin(refused, origin);
        }
        responder.sendResponse(refused);
        return;
    }

    // QHttpServerRequest already separates path and query, so the parsed query is used
    // rather than splitting the URL again.
    QVariantMap query;
    const QUrlQuery parsed{request.query()};
    for (const auto &pair : parsed.queryItems(QUrl::FullyDecoded)) {
        query.insert(pair.first, pair.second);
    }
    const QString path{request.url().path()};

    // Retired once the response is written. It outlives this function because a handler
    // that reaches a connect point answers later, through the responder held below.
    ApiRequest *apiRequest{new ApiRequest{methodOf(request), path, QVariantMap{}, query,
                                          headersOf(request, m_config.keyHeader),
                                          bodyOf(request), caller, m_requests}};

    // Shared, because the handler, the deadline below and an unmatched route may each
    // answer, and only the first counts.
    //
    // The responder stays where Qt put it while the handler runs, and is moved out only when
    // the handler has not answered by the time it returns. A responder destroyed inside the
    // route handler leaves Qt a dead signal connection on the socket, one per request, for
    // as long as the keep-alive connection stays open.
    struct Pending
    {
        QHttpServerResponder *inPlace{nullptr};
        std::optional<QHttpServerResponder> held;
        bool answered{false};
    };
    auto pending{std::make_shared<Pending>()};
    pending->inPlace = &responder;
    const auto answer{[pending, corsAllowed, origin](QHttpServerResponse &&response) {
        if (pending->answered) {
            return;
        }
        pending->answered = true;
        if (corsAllowed) {
            allowOrigin(response, origin);
        }
        if (pending->inPlace != nullptr) {
            pending->inPlace->sendResponse(response);
            return;
        }
        pending->held->sendResponse(response);
        // Qt reads the connection's next request once the responder is gone.
        pending->held.reset();
    }};

    connect(apiRequest, &ApiRequest::answered, this,
            [answer, apiRequest](int code, const QByteArray &type, const QByteArray &payload) {
        answer(QHttpServerResponse{type, payload,
                                   static_cast<QHttpServerResponse::StatusCode>(code)});
        apiRequest->deleteLater();
    });

    const bool routed{m_api->dispatch(apiRequest)};
    if (!routed) {
        apiRequest->deleteLater();
        answer(errorResponse(404, QStringLiteral("no route for %1 %2")
                                      .arg(methodOf(request), path)));
    }
    if (pending->answered) {
        pending->inPlace = nullptr;
        return;  // answered synchronously, which is the ordinary case
    }
    pending->held.emplace(std::move(responder));
    pending->inPlace = nullptr;

    // The handler answers later. Keep the connection open with a deadline, so a handler
    // that never answers costs one 504, not a held socket. The timer is a child of the
    // request, so answering first destroys it.
    //
    // The deadline is mandatory: `reply_timeout_ms: 0` would leave a request object, a
    // responder and a connection per call for the life of the process. Zero means the
    // default, and that is logged once.
    int deadlineMs{m_config.replyTimeoutMs};
    if (deadlineMs <= 0) {
        if (!m_warnedAboutDeadline) {
            m_warnedAboutDeadline = true;
            qWarning("SynQt: network.inbound.reply_timeout_ms is not set, so a handler that "
                     "never answers would hold its request forever; using %d ms",
                     kFallbackReplyTimeoutMs);
        }
        deadlineMs = kFallbackReplyTimeoutMs;
    }
    const QString method{methodOf(request)};
    QTimer *deadline{new QTimer{apiRequest}};
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, this,
            [this, answer, apiRequest, method, path, deadlineMs]() {
        const QString reason{QStringLiteral("the handler for %1 %2 did not answer within "
                                            "%3 ms")
                                 .arg(method, path)
                                 .arg(deadlineMs)};
        emit requestRefused(reason);
        answer(errorResponse(504, reason));
        apiRequest->deleteLater();
    });
    deadline->start(deadlineMs);
}

} // namespace SynQt
