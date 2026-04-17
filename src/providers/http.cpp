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

// Headers the transport owns. Qt derives each of these from the request it is about to
// send, and a value written over the top of one is either ignored or corrupts the message
// (a Content-Length that disagrees with the body is a request smuggling primitive, and a
// Host that disagrees with the URL is how an allowlisted prefix reaches somewhere else).
// Declaring one is a mistake worth naming rather than a capability worth having.
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

// How large an answer this helper will hold before it gives up on the call.
//
// QNetworkReply buffers the whole body in memory and readAll() hands it over, so without a
// ceiling the size of an outbound answer is decided by whoever is answering. An allowlisted
// third party is not the same thing as a trusted one: a compromised or broken
// endpoint that streams gigabytes takes the entity down with it, and a `Content-Length` is
// not a promise anybody has to keep. Sixteen mebibytes is far above any JSON API answer and
// far below what a service can spend on one call.
constexpr qint64 kMaxResponseBytes{16 * 1024 * 1024};

// A body as bytes. A string is sent as written. Anything structured (the ordinary case from
// QML, where a body is an object) is serialized as JSON, which is what the Content-Type
// already says it is.
QByteArray bodyBytes(const QVariant &value)
{
    // An object built inside a closure reaches a QVariant parameter as a QJSValue rather
    // than as a QVariantMap, so unwrap before deciding what this is (the same note is in
    // src/gateway/apirequest.cpp, where the same thing bit a reply).
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
    // The first answer wins, and it has to. A refused redirect rejects here and then
    // aborts the reply, whose `finished` arrives a moment later carrying Qt's own
    // "Operation canceled". Without this guard that second answer would overwrite the
    // reason with a generic one, and on a promise nobody attached a handler to it would
    // also queue a second deleteLater.
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
    // Settled either way, so this promise has nothing left to do, and it is a child of
    // the Http helper, which lives as long as the entity does. Retired after the current
    // turn, which is after `Http.get(url).then(...)` has attached its handler, and
    // whether or not one was ever attached. A call whose result nobody reads (a fire and
    // forget POST is the ordinary case) must not be the one that accumulates.
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

// Whether `url` is inside `prefix`, as an allowlist has to mean it. The same scheme, the
// same host, the same port, and a path at or under the prefix's path.
//
// Not a string prefix. `startsWith` is the obvious way to write this and it is wrong in
// three ways at once, each of which sends the endpoint's own credential headers to a host
// the deployment never named:
//
//   https://api.example.com@evil.test/    (userinfo: the host is evil.test, and the
//                                            declared prefix is a prefix of the string)
//   https://api.example.com.evil.test/    (a suffix on the host)
//   https://api.example.com/v1evil        (a suffix on the last path segment)
//
// So it is compared as a URL. Userinfo is refused outright rather than compared: nothing
// this framework composes needs it, and it exists here only as the trick above.
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
    // Defaulted the same way on both sides, so `https://x` and `https://x:443` are one
    // place and neither is a way past the other.
    const int defaultPort{url.scheme() == QLatin1String("https") ? 443 : 80};
    if (url.port(defaultPort) != prefix.port(defaultPort)) {
        return false;
    }
    // Normalized, so `/v1/../../admin` and its percent-encoded twin collapse before they
    // are compared. A prefix with no path allows the whole host.
    const QString base{prefix.adjusted(QUrl::NormalizePathSegments).path()};
    const QString path{url.adjusted(QUrl::NormalizePathSegments).path()};
    if (base.isEmpty() || base == QLatin1String("/")) {
        return true;
    }
    if (!path.startsWith(base)) {
        return false;
    }
    // At a segment boundary. `/v1` covers `/v1` and `/v1/things`, and does not cover
    // `/v1evil`. A prefix written with a trailing slash has already said where it ends.
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

    // The allowlist first, because it is the narrower question and the one the topology
    // answered. This entity may call these places and nowhere else. Rejected with the list
    // in the message, since the mistake is nearly always a prefix that does not cover the
    // path being composed.
    const HttpEndpointConfig *endpoint{match(target)};
    if (!endpoint) {
        const QStringList prefixes{allowed()};
        promise->reject(
            QStringLiteral("%1 is not in this entity's network.outbound allowlist (%2)")
                .arg(url, prefixes.isEmpty() ? QStringLiteral("empty")
                                             : prefixes.join(QStringLiteral(", "))));
        return promise;
    }

    // Refuse plaintext in release. An outbound call must be TLS-verified. https requests
    // are certificate-verified by QNetworkAccessManager by default.
    if (m_release && target.scheme() != QLatin1String("https")) {
        promise->reject(QStringLiteral("refusing a plaintext outbound request in release: %1")
                            .arg(url));
        return promise;
    }

    QNetworkRequest request{target};
    // Redirects are decided here rather than by the transport. Qt's default policy
    // (NoLessSafeRedirectPolicy) follows a 302 to any host as long as it does not step
    // down from https to http, and it carries the original request's headers with it,
    // which on this path are the endpoint's own credential headers, the ones a call site
    // never sees and cannot choose. So an allowlisted third party that answers with a
    // redirect (or a path under the prefix that an application composes from user input,
    // where the redirect target is somebody else's to choose) would send the deployment's
    // API key to a host `network.outbound` never named, and would reach it besides. The
    // allowlist checks where the call ends up rather than where it starts, so every hop is
    // put through `match()` below.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    // Every call has an end. Without this a third party that accepts the connection and
    // then answers nothing (a wedged gateway, a machine that went away mid-request) leaves
    // a promise that never settles and a reply that is never freed, one per call, for the
    // life of the entity. A `.catchError()` written for exactly this case never runs. Qt's
    // own default rather than a number invented here, and it is a deadline on transfer
    // rather than on the whole exchange, so a slow stream that keeps arriving is not cut
    // off at an arbitrary total.
    request.setTransferTimeout();
    // The endpoint's own headers first, so a call site can add to them but the credential
    // the deployment declared is not something a call site has to remember to send.
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

    // Each hop, before it is taken. `redirectAllowed()` is the only thing that lets the
    // transport continue under UserVerifiedRedirectPolicy, so a target the allowlist does
    // not cover is simply never allowed: the promise is rejected with the place it tried
    // to go, and the reply is abandoned before a single header reaches it.
    QObject::connect(reply, &QNetworkReply::redirected, promise,
                     [this, promise, reply](const QUrl &redirect) {
        if (match(redirect) == nullptr) {
            promise->reject(
                QStringLiteral("refusing a redirect to %1, which is not in this entity's "
                               "network.outbound allowlist (%2)")
                    .arg(redirect.toString(QUrl::RemoveUserInfo),
                         allowed().isEmpty() ? QStringLiteral("empty")
                                             : allowed().join(QStringLiteral(", "))));
            reply->abort();
            return;
        }
        emit reply->redirectAllowed();
    });

    // The ceiling, applied while the body is arriving rather than after. Both halves are
    // needed. The announced length catches a well-formed large answer before a byte of it is
    // buffered, and the running count catches a chunked one that announces nothing, which
    // is the shape anybody sending a body deliberately too large would use.
    QObject::connect(reply, &QNetworkReply::downloadProgress, promise,
                     [promise, reply](qint64 received, qint64 total) {
        if (received <= kMaxResponseBytes && total <= kMaxResponseBytes) {
            return;
        }
        promise->reject(QStringLiteral("the answer from %1 is larger than the %2 byte "
                                       "limit an outbound call will hold")
                            .arg(reply->url().toString(QUrl::RemoveUserInfo))
                            .arg(kMaxResponseBytes));
        reply->abort();
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
            // A JSON reply arrives parsed as well as raw. Nearly every call is to a JSON
            // API, and `JSON.parse(r.body)` at every call site is a line that can only be
            // written one way and can only fail one way.
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
