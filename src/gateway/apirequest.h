// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_APIREQUEST_H
#define SYNQT_APIREQUEST_H

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace SynQt {

/// One inbound HTTP request, as the entity's QML sees it.
///
/// A handler gets this and nothing else: no socket, no QHttpServerRequest, no way to reach
/// the transport. It answers by calling `reply()` or `fail()`, or by returning a value, and it
/// may answer later, after waiting on a connect point.
///
/// Every request is answered exactly once. A second `reply()` is ignored, and a request nobody
/// answers is closed with 504 by the server's timeout.
class ApiRequest : public QObject
{
    Q_OBJECT

    /// GET, POST, PUT, DELETE.
    Q_PROPERTY(QString method READ method CONSTANT)
    /// The path as routed, without the query string.
    Q_PROPERTY(QString path READ path CONSTANT)
    /// The `<name>` placeholders the route captured, percent-decoded.
    Q_PROPERTY(QVariantMap params READ params CONSTANT)
    /// The decoded query string pairs.
    Q_PROPERTY(QVariantMap query READ query CONSTANT)
    /// Request headers, lower-cased names. The API key header is removed before a handler
    /// sees this. A handler has no reason to read the credential that admitted the call,
    /// and one that logs its headers must not log it.
    Q_PROPERTY(QVariantMap headers READ headers CONSTANT)
    /// The parsed JSON body for an `application/json` request, the raw string otherwise,
    /// and undefined when there is no body.
    Q_PROPERTY(QVariant body READ body CONSTANT)
    /// Who is calling, as an address. The peer that connected, or what a proxy on
    /// `network.inbound.trusted_proxies` said is behind it. This is the resolved answer and
    /// the forwarding header in `headers` is not. That one is whatever the last hop sent,
    /// and on a surface that trusts nobody it is whatever the client typed.
    Q_PROPERTY(QString client READ client CONSTANT)

public:
    ApiRequest(QString method, QString path, QVariantMap params, QVariantMap query,
               QVariantMap headers, QVariant body, QString client,
               QObject *parent = nullptr);

    QString method() const;
    QString path() const;
    QVariantMap params() const;
    QVariantMap query() const;
    QVariantMap headers() const;
    QVariant body() const;
    QString client() const;

    /// Answer with a body and a status (200 by default). A map or a list is sent as JSON;
    /// anything else as text.
    Q_INVOKABLE void reply(const QVariant &value, int status = 200);
    /// Answer with an error status and a message, as `{"error": message}`.
    Q_INVOKABLE void fail(int status, const QString &message);

    bool isAnswered() const;

    /// The route's captures, set by Api::dispatch once a pattern has matched. Not
    /// invokable and not writable from QML: a handler reads what it was routed with.
    void setParams(QVariantMap params);

signals:
    /// The server writes the response when this fires. Emitted once per request.
    void answered(int status, const QByteArray &contentType, const QByteArray &body);

private:
    void send(int status, const QByteArray &contentType, const QByteArray &body);

    QString m_method;
    QString m_path;
    QVariantMap m_params;
    QVariantMap m_query;
    QVariantMap m_headers;
    QVariant m_body;
    QString m_client;
    bool m_answered{false};
};

} // namespace SynQt

#endif // SYNQT_APIREQUEST_H
