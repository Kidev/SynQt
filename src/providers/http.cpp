// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "http.h"

#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

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
        deleteLater();
    } else if (!m_ok && m_onRejected.isCallable()) {
        m_handled = true;
        m_onRejected.call(QJSValueList{m_engine->toScriptValue(m_error)});
        deleteLater();
    }
}

Http::Http(QNetworkAccessManager *network, QJSEngine *engine, bool release, QObject *parent)
    : QObject{parent}
    , m_network{network}
    , m_engine{engine}
    , m_release{release}
{
}

HttpPromise *Http::get(const QString &url)
{
    return send(QStringLiteral("GET"), url, QVariant{});
}

HttpPromise *Http::post(const QString &url, const QVariant &body)
{
    return send(QStringLiteral("POST"), url, body);
}

HttpPromise *Http::del(const QString &url)
{
    return send(QStringLiteral("DELETE"), url, QVariant{});
}

HttpPromise *Http::send(const QString &method, const QString &url, const QVariant &body)
{
    HttpPromise *promise{new HttpPromise{m_engine, this}};
    const QUrl target{url};

    // Refuse plaintext in release: an outbound call must be TLS-verified. https requests
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
