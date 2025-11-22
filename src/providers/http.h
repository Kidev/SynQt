// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_HTTP_H
#define SYNQT_HTTP_H

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
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

/// The outbound HTTP helper, exposed as `Http` to the QML of an entity whose
/// `network.outbound` names somewhere to call. A promise-returning wrapper over
/// QNetworkAccessManager that enforces TLS verification and refuses plaintext in release,
/// so entity code never touches sockets. Outbound only.
///
/// It is installed on the allowlist and nothing else. An entity that declares no
/// `network.outbound` never has `Http` in scope, and one that does can reach only the URL
/// prefixes it named. The check is here rather than in a comment because the entity's QML
/// is where a URL is composed at run time, and a prefix list nothing enforced would be
/// documentation of an intention.
class Http : public QObject
{
    Q_OBJECT

public:
    Http(QNetworkAccessManager *network, QJSEngine *engine, bool release,
         QStringList allowed, QObject *parent = nullptr);

    Q_INVOKABLE HttpPromise *get(const QString &url);
    Q_INVOKABLE HttpPromise *post(const QString &url, const QVariant &body = QVariant());
    Q_INVOKABLE HttpPromise *put(const QString &url, const QVariant &body = QVariant());
    Q_INVOKABLE HttpPromise *del(const QString &url);

    /// The allowlist this helper was built with, as declared. Read by tests and by the
    /// rejection message, so the two cannot describe different lists.
    QStringList allowed() const;

private:
    HttpPromise *send(const QString &method, const QString &url, const QVariant &body);
    /// Whether `url` starts with one of the allowed prefixes, compared on the normalized
    /// URL rather than the string handed in: `https://api.example.com/../admin` and a
    /// percent-encoded traversal both normalize before they are matched, so a prefix
    /// cannot be escaped by spelling.
    bool isAllowed(const QUrl &url) const;

    QNetworkAccessManager *m_network;
    QJSEngine *m_engine;
    bool m_release;
    QStringList m_allowed;
};

} // namespace SynQt

#endif // SYNQT_HTTP_H
