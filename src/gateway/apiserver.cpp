// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "apiserver.h"

#include "api.h"
#include "apirequest.h"
#include "constanttime.h"
#include "topology.h"  // loadCertificate / loadPrivateKey

#include <QDateTime>
#include <QFuture>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerConfiguration>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerResponder>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>

#include <chrono>
#include <memory>
#include <utility>

namespace SynQt {

namespace {

/// A response with the JSON body an API caller expects for a refusal, so a machine caller
/// parses one shape whatever went wrong.
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

/// The body as QML should see it: a parsed object for JSON, the text otherwise, and an
/// invalid QVariant when there is none.
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

/// How long a connection may sit idle between requests before the transport closes it.
/// A machine caller either has something to say or has gone away. This is what ends the
/// half-open connections that neither a size limit nor a rate limit can see.
constexpr int kKeepAliveTimeoutSeconds{30};

/// The deadline used when the topology names none. See handle().
constexpr int kFallbackReplyTimeoutMs{15000};

/// How many addresses the rate window may name before it is dropped and started again.
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

    // Qt's own ceilings on the request, set before a route can see one. The body check in
    // refuse() below is a check on a body Qt has already read into memory, so on its own it
    // bounds what a handler is handed and not what the process allocates: Qt's default is
    // 32 MiB, which an unauthenticated caller can spend per connection whatever
    // `network.inbound.max_body_bytes` says. Declaring it here is what makes the number in
    // the topology the number the transport enforces. The idle timeout is the other half:
    // it is what closes a peer that opens a connection, sends half a request and stops,
    // which no size limit covers. The same reasoning, and the same two calls, as the web
    // edge (webedge.cpp).
    QHttpServerConfiguration httpConfiguration;
    httpConfiguration.setMaximumBodySize(m_config.maxBodyBytes);
    httpConfiguration.setKeepAliveTimeout(std::chrono::seconds{kKeepAliveTimeoutSeconds});
    m_server->setConfiguration(httpConfiguration);

    // One catch-all route rather than one route per declared path: the routing table lives
    // in `Api`, where QML declared it, and having QHttpServer hold a second copy of it
    // would mean two tables to keep in step and a 404 that could disagree with itself.
    m_server->route(QStringLiteral("/<arg>"), [this](const QUrl &, const QHttpServerRequest &request) {
        return handle(request);
    });
    m_server->route(QStringLiteral("/"), [this](const QHttpServerRequest &request) {
        return handle(request);
    });

    if (!m_config.certFile.isEmpty() && !m_config.keyFile.isEmpty()) {
        // As on the web edge. A surface told to terminate TLS and unable to refuses to
        // start, rather than listening on a port whose handshake can never complete.
        const QSslCertificate certificate{loadCertificate(m_config.certFile)};
        const QSslKey key{loadPrivateKey(m_config.keyFile)};
        if (certificate.isNull() || key.isNull()) {
            m_errorString = QStringLiteral("cannot terminate TLS with %1 and %2")
                                .arg(m_config.certFile, m_config.keyFile);
            return false;
        }
        QSslServer *sslServer{new QSslServer{this}};
        QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
        configuration.setLocalCertificate(certificate);
        configuration.setPrivateKey(key);
        // A machine caller presents no client certificate. Only the server is
        // authenticated here, and the caller authenticates with its API key.
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
    // `value()` joins every `X-Forwarded-For` line the request carries, in the order they
    // arrived, which is what RFC 9110 says they mean. Reading only the first would hand a
    // caller the answer on a proxy that appends a line of its own instead of extending the
    // one it was given. The client's own line would be the one read, and the resolver would
    // walk a chain the client wrote from end to end.
    return m_clientAddress.resolve(request.remoteAddress(),
                                   request.value(QByteArrayLiteral("X-Forwarded-For")));
}

QString ApiServer::originOf(const QHttpServerRequest &request) const
{
    return QString::fromUtf8(
        request.headers().value(QHttpHeaders::WellKnownHeader::Origin).toByteArray());
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
    const bool within{++m_rateWindow[caller] <= m_config.ratePerMinutePerIp};
    if (m_rateWindow.size() > kMaxRateEntries) {
        // A table keyed by whatever address dialled in is a table a caller can grow, one
        // entry per address, for as long as the window lasts. What it must not become is a
        // way to clear the count. This window is shared by every caller, so emptying it on
        // overflow would let one that can present many addresses reset its own budget on
        // demand, and the rate limit would be gone rather than generous. Everything in this
        // table belongs to the current minute by construction (the whole of it is dropped
        // when the minute turns, above), so there is nothing stale to prune and the only
        // right answer to an overflowing table is to refuse while it lasts.
        return false;
    }
    return within;
}

QString ApiServer::refuse(const QHttpServerRequest &request, int *status) const
{
    // The API key. A surface written `public: true` skips it and says so in the topology;
    // everything else needs the header, compared in constant time so the comparison itself
    // does not leak how much of a guess was right.
    if (!m_config.anonymous) {
        const QByteArray presented{
            request.headers().value(m_config.keyHeader).toByteArray()};
        bool accepted{false};
        for (const QByteArray &key : m_config.apiKeys) {
            // Every candidate is compared, and each comparison reads every byte: no early
            // return on the first mismatch and no `break` on the first match, so how long
            // this takes does not say how much of a guess was right.
            accepted = constantTimeEquals(presented, key) || accepted;
        }
        if (!accepted) {
            *status = 401;
            return QStringLiteral("missing or unknown API key");
        }
    }

    // The origin, for browser callers. A request with no Origin is not a browser and is
    // governed by the key above. One that carries an Origin this surface never named is
    // refused, so a key that leaked into a page still buys nothing.
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

namespace {

// A future already holding `response`, for every answer this server has before it returns.
QFuture<QHttpServerResponse> settled(QHttpServerResponse &&response)
{
    QPromise<QHttpServerResponse> promise;
    QFuture<QHttpServerResponse> future{promise.future()};
    promise.start();
    promise.addResult(std::move(response));
    promise.finish();
    return future;
}

} // namespace

QFuture<QHttpServerResponse> ApiServer::handle(const QHttpServerRequest &request)
{
    // The address this request is counted against, which is the peer's until the topology
    // names a proxy in front of this surface. Resolved once, and the only client address
    // that goes any further: the rate limit keys on it and the handler is handed it.
    const QString caller{callerAddress(request)};
    if (!withinRate(caller)) {
        emit requestRefused(QStringLiteral("rate limit for %1").arg(caller));
        return settled(errorResponse(429, QStringLiteral("too many requests")));
    }

    int status{400};
    const QString refusal{refuse(request, &status)};
    if (!refusal.isEmpty()) {
        emit requestRefused(refusal);
        return settled(errorResponse(status, refusal));
    }

    // QHttpServerRequest hands the path and the query already separated, so the query is
    // read from the parsed one rather than re-split out of the URL: two parsers of one
    // string is how a route and its parameters end up disagreeing.
    QVariantMap query;
    const QUrlQuery parsed{request.query()};
    for (const auto &pair : parsed.queryItems(QUrl::FullyDecoded)) {
        query.insert(pair.first, pair.second);
    }
    const QString path{request.url().path()};

    // Parented to this server, and retired once the response is written. It outlives this
    // function because a handler that reaches a connect point answers on a later turn,
    // and the QHttpServerResponder it answers through is held by the lambda below.
    ApiRequest *apiRequest{new ApiRequest{methodOf(request), path, QVariantMap{}, query,
                                          headersOf(request, m_config.keyHeader),
                                          bodyOf(request), caller, this}};

    // Shared, because three things may settle it and only the first one counts: the
    // handler answering, the deadline below, and a route that never matched. The promise
    // outlives this function whenever the handler does.
    auto promise{std::make_shared<QPromise<QHttpServerResponse>>()};
    QFuture<QHttpServerResponse> future{promise->future()};
    promise->start();
    auto answer = [promise](QHttpServerResponse &&response) {
        if (promise->future().isFinished()) {
            return;
        }
        promise->addResult(std::move(response));
        promise->finish();
    };

    connect(apiRequest, &ApiRequest::answered, this,
            [answer, apiRequest](int code, const QByteArray &type, const QByteArray &payload) {
        answer(QHttpServerResponse{type, payload,
                                   static_cast<QHttpServerResponse::StatusCode>(code)});
        apiRequest->deleteLater();
    });

    if (!m_api->dispatch(apiRequest)) {
        apiRequest->deleteLater();
        return settled(errorResponse(404, QStringLiteral("no route for %1 %2")
                                              .arg(methodOf(request), path)));
    }
    if (apiRequest->isAnswered()) {
        return future;  // answered synchronously, which is the ordinary case
    }

    // The handler is answering later. Hold the connection open for it, with a deadline, so
    // a handler that never answers costs one 504 rather than a socket held forever. The
    // timer is a child of the request, so answering first destroys it.
    //
    // A deadline is not optional here, whatever the topology says. `reply_timeout_ms: 0`
    // reads as "let a handler take as long as it likes", but what it bought was a
    // request object and an unfinished promise per call that nothing ever retired, and a
    // connection held for the life of the process. A handler that forgets to answer once is
    // a leak, and one that forgets on every call is a caller's way to exhaust the entity.
    // So zero means the default rather than none, and it is said out loud the first time.
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
    return future;
}

} // namespace SynQt
