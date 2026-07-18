// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_HTTP_H
#define SYNQT_HTTP_H

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QJSEngine;
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace SynQt {

/// The result of one Http call: a minimal promise. `Http.get(url).then(onOk, onErr)` runs
/// onOk({ status, body }) on success or onErr(message) on failure. Settles once; then()
/// attached after settling fires immediately.
class HttpPromise : public QObject
{
    Q_OBJECT

public:
    HttpPromise(QJSEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE void then(const QJSValue &onFulfilled,
                          const QJSValue &onRejected = QJSValue());

    void resolve(const QVariantMap &response);
    void reject(const QString &message);

private:
    void deliver();

    QJSEngine *m_engine;
    QJSValue m_onFulfilled;
    QJSValue m_onRejected;
    QVariantMap m_response;
    QString m_error;
    bool m_settled{false};
    bool m_ok{false};
    bool m_handled{false};
};

/// The gateway's outbound HTTP helper, exposed to the entity's QML as `Http`. A promise-
/// returning wrapper over QNetworkAccessManager that enforces TLS verification and refuses
/// plaintext in release, so gateway code never touches sockets. Outbound only.
class Http : public QObject
{
    Q_OBJECT

public:
    Http(QNetworkAccessManager *network, QJSEngine *engine, bool release,
         QObject *parent = nullptr);

    Q_INVOKABLE HttpPromise *get(const QString &url);
    Q_INVOKABLE HttpPromise *post(const QString &url, const QVariant &body = QVariant());
    Q_INVOKABLE HttpPromise *del(const QString &url);

private:
    HttpPromise *send(const QString &method, const QString &url, const QVariant &body);

    QNetworkAccessManager *m_network;
    QJSEngine *m_engine;
    bool m_release;
};

} // namespace SynQt

#endif // SYNQT_HTTP_H
