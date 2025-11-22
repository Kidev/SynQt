// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace SynQt {

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
    m_response = response;
    m_ok = true;
    m_settled = true;
    deliver();
}

void HttpPromise::reject(const QString &message)
{
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

Http::Http(QNetworkAccessManager *network, QJSEngine *engine, bool release,
           QStringList allowed, QObject *parent)
    : QObject{parent}
    , m_network{network}
    , m_engine{engine}
    , m_release{release}
    , m_allowed{std::move(allowed)}
{
}

QStringList Http::allowed() const
{
    return m_allowed;
}

bool Http::isAllowed(const QUrl &url) const
{
    // Compared on the normalized, fully-encoded URL, so a prefix cannot be escaped by
    // spelling: `https://api.example.com/v1/../../admin` and its percent-encoded twin both
    // collapse before they reach the comparison.
    const QString normalized{
        url.adjusted(QUrl::NormalizePathSegments).toString(QUrl::FullyEncoded)};
    for (const QString &prefix : m_allowed) {
        if (normalized.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

HttpPromise *Http::get(const QString &url)
{
    return send(QStringLiteral("GET"), url, QVariant{});
}

HttpPromise *Http::post(const QString &url, const QVariant &body)
{
    return send(QStringLiteral("POST"), url, body);
}

HttpPromise *Http::put(const QString &url, const QVariant &body)
{
    return send(QStringLiteral("PUT"), url, body);
}

HttpPromise *Http::del(const QString &url)
{
    return send(QStringLiteral("DELETE"), url, QVariant{});
}

HttpPromise *Http::send(const QString &method, const QString &url, const QVariant &body)
{
    HttpPromise *promise{new HttpPromise{m_engine, this}};
    const QUrl target{url};

    // The allowlist first, because it is the narrower question and the one the topology
    // answered: this entity may call these places and nowhere else. Rejected with the list
    // in the message, since the mistake is nearly always a prefix that does not cover the
    // path being composed.
    if (!isAllowed(target)) {
        promise->reject(
            QStringLiteral("%1 is not in this entity's network.outbound allowlist (%2)")
                .arg(url, m_allowed.isEmpty() ? QStringLiteral("empty")
                                              : m_allowed.join(QStringLiteral(", "))));
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
    QNetworkReply *reply{nullptr};
    if (method == QLatin1String("GET")) {
        reply = m_network->get(request);
    } else if (method == QLatin1String("DELETE")) {
        reply = m_network->deleteResource(request);
    } else if (method == QLatin1String("PUT")) {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArrayLiteral("application/json"));
        reply = m_network->put(request, body.toByteArray());
    } else {
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QByteArrayLiteral("application/json"));
        reply = m_network->post(request, body.toByteArray());
    }

    QObject::connect(reply, &QNetworkReply::finished, promise, [promise, reply]() {
        if (reply->error() != QNetworkReply::NoError) {
            promise->reject(reply->errorString());
        } else {
            promise->resolve(QVariantMap{
                {QStringLiteral("status"),
                 reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)},
                {QStringLiteral("body"), QString::fromUtf8(reply->readAll())}});
        }
        reply->deleteLater();
    });
    return promise;
}

} // namespace SynQt
