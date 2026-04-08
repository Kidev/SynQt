// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "apirequest.h"

#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

namespace SynQt {

namespace {

// A value as QML passed it. An object literal arrives as a QVariantMap when the engine sees
// the parameter type at the call site, and as a QJSValue inside a closure that runs later.
// Both become the same QVariant here, so `request.reply({ok: true})` inside `.then(...)`
// sends its body.
QVariant fromQml(const QVariant &value)
{
    if (value.metaType().id() == qMetaTypeId<QJSValue>()) {
        return value.value<QJSValue>().toVariant();
    }
    return value;
}

} // namespace

ApiRequest::ApiRequest(QString method, QString path, QVariantMap params, QVariantMap query,
                       QVariantMap headers, QVariant body, QString client, QObject *parent)
    : QObject{parent}
    , m_method{std::move(method)}
    , m_path{std::move(path)}
    , m_params{std::move(params)}
    , m_query{std::move(query)}
    , m_headers{std::move(headers)}
    , m_body{std::move(body)}
    , m_client{std::move(client)}
{
}

QString ApiRequest::client() const
{
    return m_client;
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

void ApiRequest::reply(const QVariant &value, int status)
{
    const QVariant body{fromQml(value)};
    // A map or a list is serialized as JSON; anything else is sent as text. Deciding here
    // means a handler returning an object never sends a QVariant spelling.
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
    // Answered exactly once: a second response would be written to a socket the first
    // already closed, and the caller would see a truncated body.
    if (m_answered) {
        return;
    }
    m_answered = true;
    emit answered(status, contentType, body);
}

} // namespace SynQt
