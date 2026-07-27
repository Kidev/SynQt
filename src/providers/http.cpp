// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <QJSEngine>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace SynQt {

namespace {

// Headers the transport owns. Qt derives each from the request, and overriding one is
// ignored or corrupts the message: a mismatched Content-Length enables request smuggling,
// and a mismatched Host sends an allowlisted prefix elsewhere. Declaring one is an error.
bool isReservedHeader(const QString &name)
{
    static const QStringList reserved{QStringLiteral("host"),
                                      QStringLiteral("content-length"),
                                      QStringLiteral("connection"),
                                      QStringLiteral("keep-alive"),
                                      QStringLiteral("transfer-encoding"),
                                      QStringLiteral("te"),
                                      QStringLiteral("trailer"),
                                      QStringLiteral("upgrade")};
    return reserved.contains(name.toLower());
}

void applyHeaders(QNetworkRequest &request, const QMap<QString, QString> &headers)
{
    for (auto it{headers.constBegin()}; it != headers.constEnd(); ++it) {
        if (isReservedHeader(it.key())) {
            qWarning("SynQt::Http: refusing to set the '%s' header; the transport owns it",
                     qPrintable(it.key()));
            continue;
        }
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
}

QMap<QString, QString> asHeaderMap(const QVariantMap &headers)
{
    QMap<QString, QString> result;
    for (auto it{headers.constBegin()}; it != headers.constEnd(); ++it) {
        result.insert(it.key(), it.value().toString());
    }
    return result;
}

// The largest response this helper holds before failing the call.
//
// QNetworkReply buffers the whole body, so without a ceiling the responder decides the
// memory cost. An allowlisted third party is not necessarily trustworthy, and
// `Content-Length` is not binding. 16 MiB is far above any JSON API response and far below
// what one call may cost.
constexpr qint64 kMaxResponseBytes{16 * 1024 * 1024};

// A body as bytes. A string is sent as written; anything structured (usually an object from
// QML) is serialized as JSON, matching the Content-Type.
QByteArray bodyBytes(const QVariant &value)
{
    // An object built inside a closure arrives as a QJSValue, not a QVariantMap, so unwrap
    // it first (the same applies in src/gateway/apirequest.cpp).
    const QVariant body{value.metaType().id() == qMetaTypeId<QJSValue>()
                            ? value.value<QJSValue>().toVariant()
                            : value};
    if (!body.isValid() || body.isNull()) {
        return QByteArray{};
    }
    if (body.typeId() == QMetaType::QString || body.typeId() == QMetaType::QByteArray) {
        return body.toByteArray();
    }
    return QJsonDocument::fromVariant(body).toJson(QJsonDocument::Compact);
}

} // namespace

HttpPromise::HttpPromise(QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
{
}

void HttpPromise::then(const QJSValue &onFulfilled, const QJSValue &onRejected)
{
    m_onFulfilled = onFulfilled;
    m_onRejected = onRejected;
    if (m_settled) {
        deliver();
    }
}

void HttpPromise::resolve(const QVariantMap &response)
{
    if (m_settled) {
        return;  // the first answer is the answer. See reject()
    }
    m_response = response;
    m_ok = true;
    m_settled = true;
    deliver();
}

void HttpPromise::reject(const QString &message)
{
    // The first answer wins. A refused redirect rejects here and aborts the reply, whose
    // `finished` then arrives with Qt's "Operation canceled"; this guard keeps the real
    // reason and avoids a second deleteLater.
    if (m_settled) {
        return;
    }
    m_error = message;
    m_ok = false;
    m_settled = true;
    deliver();
}

void HttpPromise::deliver()
{
    if (m_handled || !m_settled) {
        return;
    }
    if (m_ok && m_onFulfilled.isCallable()) {
        m_handled = true;
        m_onFulfilled.call(QJSValueList{m_engine->toScriptValue(m_response)});
    } else if (!m_ok && m_onRejected.isCallable()) {
        m_handled = true;
        m_onRejected.call(QJSValueList{m_engine->toScriptValue(m_error)});
    }
    // Settled, so the promise is done. It is a child of the Http helper, which lives as
    // long as the entity, so it is retired after this turn, after `Http.get(url).then(...)`
    // has attached a handler, whether or not one was attached. A call nobody reads (a
    // fire-and-forget POST) must not accumulate.
    deleteLater();
}

HttpEndpoint::HttpEndpoint(Http *http, HttpEndpointConfig config, QObject *parent)
    : QObject{parent}
    , m_http{http}
    , m_config{std::move(config)}
{
}

QString HttpEndpoint::url() const
{
    return m_config.url;
}

QString HttpEndpoint::resolve(const QString &path) const
{
    if (path.isEmpty()) {
        return m_config.url;
    }
    if (path.contains(QStringLiteral("://"))) {
        return path;
    }
    QString base{m_config.url};
    const bool baseEnds{base.endsWith(QLatin1Char('/'))};
    const bool pathStarts{path.startsWith(QLatin1Char('/'))};
    if (baseEnds && pathStarts) {
        return base + path.mid(1);
    }
    if (!baseEnds && !pathStarts) {
        return base + QLatin1Char('/') + path;
    }
    return base + path;
}

HttpPromise *HttpEndpoint::get(const QString &path, const QVariantMap &headers)
{
    return m_http->send(QStringLiteral("GET"), resolve(path), QVariant{}, headers);
}

HttpPromise *HttpEndpoint::post(const QString &path, const QVariant &body,
                                const QVariantMap &headers)
{
    return m_http->send(QStringLiteral("POST"), resolve(path), body, headers);
}

HttpPromise *HttpEndpoint::put(const QString &path, const QVariant &body,
                               const QVariantMap &headers)
{
    return m_http->send(QStringLiteral("PUT"), resolve(path), body, headers);
}

HttpPromise *HttpEndpoint::del(const QString &path, const QVariantMap &headers)
{
    return m_http->send(QStringLiteral("DELETE"), resolve(path), QVariant{}, headers);
}

Http::Http(QNetworkAccessManager *network, QJSEngine *engine, bool release,
           QList<HttpEndpointConfig> endpoints, QObject *parent)
    : QObject{parent}
    , m_network{network}
    , m_engine{engine}
    , m_release{release}
    , m_endpoints{std::move(endpoints)}
{
    for (const HttpEndpointConfig &endpoint : std::as_const(m_endpoints)) {
        if (!endpoint.name.isEmpty()) {
            m_named.insert(endpoint.name, new HttpEndpoint{this, endpoint, this});
        }
    }
}

HttpEndpoint *Http::api(const QString &name) const
{
    HttpEndpoint *endpoint{m_named.value(name)};
    if (!endpoint) {
        const QStringList names{m_named.keys()};
        qWarning("SynQt::Http: no outbound endpoint named '%s' (declared: %s)",
                 qPrintable(name),
                 qPrintable(names.isEmpty() ? QStringLiteral("none")
                                            : names.join(QStringLiteral(", "))));
    }
    return endpoint;
}

QStringList Http::allowed() const
{
    QStringList prefixes;
    prefixes.reserve(m_endpoints.size());
    for (const HttpEndpointConfig &endpoint : m_endpoints) {
        prefixes.append(endpoint.url);
    }
    return prefixes;
}

namespace {

// Whether `url` is inside `prefix`: the same scheme, host and port, and a path at or under
// the prefix path.
//
// Not a string prefix. `startsWith` fails three ways, each sending the endpoint's
// credential headers to a host the deployment never named:
//
//   https://api.example.com@evil.test/    (userinfo: the host is evil.test)
//   https://api.example.com.evil.test/    (a suffix on the host)
//   https://api.example.com/v1evil        (a suffix on the last path segment)
//
// So URLs are compared. Userinfo is refused outright: nothing here needs it.
bool isUnder(const QUrl &url, const QUrl &prefix)
{
    if (!url.isValid() || !prefix.isValid() || url.host().isEmpty()) {
        return false;
    }
    if (!url.userInfo().isEmpty()) {
        return false;
    }
    if (url.scheme().compare(prefix.scheme(), Qt::CaseInsensitive) != 0
        || url.host().compare(prefix.host(), Qt::CaseInsensitive) != 0) {
        return false;
    }
    // Default ports on both sides, so `https://x` and `https://x:443` are the same place.
    const int defaultPort{url.scheme() == QLatin1String("https") ? 443 : 80};
    if (url.port(defaultPort) != prefix.port(defaultPort)) {
        return false;
    }
    // Normalized, so `/v1/../../admin` and its percent-encoded form collapse before
    // comparison. A prefix without a path allows the whole host.
    const QString base{prefix.adjusted(QUrl::NormalizePathSegments).path()};
    const QString path{url.adjusted(QUrl::NormalizePathSegments).path()};
    if (base.isEmpty() || base == QLatin1String("/")) {
        return true;
    }
    if (!path.startsWith(base)) {
        return false;
    }
    // At a segment boundary: `/v1` covers `/v1` and `/v1/things`, not `/v1evil`. A trailing
    // slash on the prefix already marks the end.
    return path.size() == base.size()
           || base.endsWith(QLatin1Char('/'))
           || path.at(base.size()) == QLatin1Char('/');
}

} // namespace

const HttpEndpointConfig *Http::match(const QUrl &url) const
{
    for (const HttpEndpointConfig &endpoint : m_endpoints) {
        if (isUnder(url, QUrl{endpoint.url})) {
            return &endpoint;
        }
    }
    return nullptr;
}

HttpPromise *Http::get(const QString &url, const QVariantMap &headers)
{
    return send(QStringLiteral("GET"), url, QVariant{}, headers);
}

HttpPromise *Http::post(const QString &url, const QVariant &body, const QVariantMap &headers)
{
    return send(QStringLiteral("POST"), url, body, headers);
}

HttpPromise *Http::put(const QString &url, const QVariant &body, const QVariantMap &headers)
{
    return send(QStringLiteral("PUT"), url, body, headers);
}

HttpPromise *Http::del(const QString &url, const QVariantMap &headers)
{
    return send(QStringLiteral("DELETE"), url, QVariant{}, headers);
}

HttpPromise *Http::send(const QString &method, const QString &url, const QVariant &body,
                        const QVariantMap &headers)
{
    HttpPromise *promise{new HttpPromise{m_engine, this}};
    const QUrl target{url};

    // The allowlist first: this entity may call these places and no others. The error lists
    // the prefixes, since the usual mistake is a prefix that does not cover the path.
    const HttpEndpointConfig *endpoint{match(target)};
    if (!endpoint) {
        const QStringList prefixes{allowed()};
        promise->reject(
            QStringLiteral("%1 is not in this entity's network.outbound allowlist (%2)")
                .arg(url, prefixes.isEmpty() ? QStringLiteral("empty")
                                             : prefixes.join(QStringLiteral(", "))));
        return promise;
    }

    // Refuse plaintext in release: outbound calls must be TLS-verified.
    // QNetworkAccessManager verifies https certificates by default.
    if (m_release && target.scheme() != QLatin1String("https")) {
        promise->reject(QStringLiteral("refusing a plaintext outbound request in release: %1")
                            .arg(url));
        return promise;
    }

    QNetworkRequest request{target};
    // Redirects are decided here, not by the transport. Qt's default
    // (NoLessSafeRedirectPolicy) follows a redirect to any host that does not downgrade to
    // http, and carries the original headers, which here are the endpoint's credential
    // headers. An allowlisted third party that redirects (or a path built from user input)
    // would then send the deployment's API key to a host `network.outbound` never named.
    // Every hop goes through `match()` below.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    // Every call has a deadline. Otherwise a peer that accepts the connection and never
    // answers leaves an unsettled promise and an unfreed reply per call, and
    // `.catchError()` never runs. This is Qt's own transfer timeout, which applies to
    // inactivity, so a slow but steady stream is not cut off.
    request.setTransferTimeout();
    // The endpoint's own headers first. A call site can add headers, but does not have to
    // send the configured credential itself.
    applyHeaders(request, endpoint->headers);
    applyHeaders(request, asHeaderMap(headers));

    QNetworkReply *reply{nullptr};
    if (method == QLatin1String("GET")) {
        reply = m_network->get(request);
    } else if (method == QLatin1String("DELETE")) {
        reply = m_network->deleteResource(request);
    } else if (method == QLatin1String("PUT")) {
        if (!request.hasRawHeader(QByteArrayLiteral("Content-Type"))) {
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QByteArrayLiteral("application/json"));
        }
        reply = m_network->put(request, bodyBytes(body));
    } else {
        if (!request.hasRawHeader(QByteArrayLiteral("Content-Type"))) {
            request.setHeader(QNetworkRequest::ContentTypeHeader,
                              QByteArrayLiteral("application/json"));
        }
        reply = m_network->post(request, bodyBytes(body));
    }

    // Each hop, before it is taken. Under UserVerifiedRedirectPolicy the transport
    // continues only through `redirectAllowed()`, so a target outside the starting endpoint
    // is refused: the promise rejects with the target, and the reply is abandoned before
    // any header is sent.
    //
    // The target must be under this endpoint, not just somewhere in the allowlist: the
    // redirected request copies the original headers, including this endpoint's credentials
    // (Qt drops only Content-Length and Content-Type on a method downgrade). Another
    // allowlist entry is another party with its own key.
    const QString endpointUrl{endpoint->url};
    QObject::connect(reply, &QNetworkReply::redirected, promise,
                     [promise, reply, endpointUrl](const QUrl &redirect) {
        if (!isUnder(redirect, QUrl{endpointUrl})) {
            promise->reject(
                QStringLiteral("refusing a redirect to %1, which is not under the "
                               "network.outbound entry this call was made through (%2)")
                    .arg(redirect.toString(QUrl::RemoveUserInfo), endpointUrl));
            reply->abort();
            return;
        }
        emit reply->redirectAllowed();
    });

    // The ceiling is enforced while the body arrives, on Qt's signals. The announced length
    // is checked when the headers arrive, refusing a large response before its body is
    // held; the received size is counted on every readyRead, refusing a chunked response
    // with no length. downloadProgress is not used: Qt throttles it.
    const auto refuseOversized{[promise, reply]() {
        promise->reject(QStringLiteral("the answer from %1 is larger than the %2 byte "
                                       "limit an outbound call will hold")
                            .arg(reply->url().toString(QUrl::RemoveUserInfo))
                            .arg(kMaxResponseBytes));
        reply->abort();
    }};
    QObject::connect(reply, &QNetworkReply::metaDataChanged, promise,
                     [reply, refuseOversized]() {
        const QVariant announced{reply->header(QNetworkRequest::ContentLengthHeader)};
        if (announced.isValid() && announced.toLongLong() > kMaxResponseBytes) {
            refuseOversized();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, promise, [reply, refuseOversized]() {
        if (reply->bytesAvailable() > kMaxResponseBytes) {
            refuseOversized();
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, promise, [promise, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            promise->reject(reply->errorString());
        } else {
            const QByteArray payload{reply->readAll()};
            QVariantMap response{
                {QStringLiteral("status"),
                 reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)},
                {QStringLiteral("body"), QString::fromUtf8(payload)}};
            // A JSON response is also delivered parsed, since nearly every call is to a
            // JSON API.
            const QString contentType{
                reply->header(QNetworkRequest::ContentTypeHeader).toString()};
            if (contentType.contains(QLatin1String("json"), Qt::CaseInsensitive)) {
                const QJsonDocument document{QJsonDocument::fromJson(payload)};
                if (!document.isNull()) {
                    response.insert(QStringLiteral("json"), document.toVariant());
                }
            }
            promise->resolve(response);
        }
        reply->deleteLater();
    });
    return promise;
}

} // namespace SynQt
