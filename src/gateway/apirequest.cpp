// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "apirequest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

namespace SynQt {

ApiRequest::ApiRequest(QString method, QString path, QVariantMap params, QVariantMap query,
                       QVariantMap headers, QVariant body, QObject *parent)
    : QObject{parent}
    , m_method{std::move(method)}
    , m_path{std::move(path)}
    , m_params{std::move(params)}
    , m_query{std::move(query)}
    , m_headers{std::move(headers)}
    , m_body{std::move(body)}
{
}

QString ApiRequest::method() const
{
    return m_method;
}

QString ApiRequest::path() const
{
    return m_path;
}

QVariantMap ApiRequest::params() const
{
    return m_params;
}

QVariantMap ApiRequest::query() const
{
    return m_query;
}

QVariantMap ApiRequest::headers() const
{
    return m_headers;
}

QVariant ApiRequest::body() const
{
    return m_body;
}

bool ApiRequest::isAnswered() const
{
    return m_answered;
}

void ApiRequest::setParams(QVariantMap params)
{
    m_params = std::move(params);
}

void ApiRequest::reply(const QVariant &body, int status)
{
    // A map or a list is what a JSON API returns, so those are serialized; anything else
    // is sent as the text it is. Deciding here rather than making the handler say means a
    // handler returning an object cannot accidentally send its QVariant spelling.
    if (body.metaType().id() == QMetaType::QVariantMap
        || body.metaType().id() == QMetaType::QVariantList) {
        send(status, QByteArrayLiteral("application/json"),
             QJsonDocument::fromVariant(body).toJson(QJsonDocument::Compact));
        return;
    }
    send(status, QByteArrayLiteral("text/plain; charset=utf-8"), body.toString().toUtf8());
}

void ApiRequest::fail(int status, const QString &message)
{
    const QJsonObject payload{{QStringLiteral("error"), message}};
    send(status, QByteArrayLiteral("application/json"),
         QJsonDocument{payload}.toJson(QJsonDocument::Compact));
}

void ApiRequest::send(int status, const QByteArray &contentType, const QByteArray &body)
{
    // Answered exactly once. A handler that replies on one branch and falls through to
    // another would otherwise write a second response onto a socket the first already
    // closed, which is a protocol error the caller sees as a truncated body.
    if (m_answered) {
        return;
    }
    m_answered = true;
    emit answered(status, contentType, body);
}

} // namespace SynQt
