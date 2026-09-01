// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_HTTP_H
#define SYNQT_HTTP_H

#include "promise.h"

#include <QJSValue>
#include <QMap>
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

/// One place an entity may call out to, as `network.outbound` declared it: a URL prefix,
/// optionally named, optionally with headers. Headers such as an API key are read from the
/// entity environment and attached by the runtime, so the entity's QML never holds them.
struct HttpEndpointConfig
{
    QString name;                    ///< the handle for `Http.api(name)`. Empty for a bare prefix
    QString url;                     ///< the prefix, and the base a named endpoint resolves against
    QMap<QString, QString> headers;  ///< attached to every call under `url`, already resolved
};

class Http;

/// One named `network.outbound` entry, as the entity's QML sees it: `Http.api("ltd2")`.
///
/// A call site names a path and nothing else. The base URL and the key
/// are the deployment's business, they are declared once in `synqt.yaml`, and changing
/// either is a configuration change rather than an edit to every place that calls.
class HttpEndpoint : public QObject
{
    Q_OBJECT
    /// The base this endpoint resolves paths against, for a message or a log line.
    Q_PROPERTY(QString url READ url CONSTANT)

public:
    HttpEndpoint(Http *http, HttpEndpointConfig config, QObject *parent = nullptr);

    QString url() const;

    Q_INVOKABLE SynQt::Promise *get(const QString &path = QString(),
                                 const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *post(const QString &path, const QVariant &body = QVariant(),
                                  const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *put(const QString &path, const QVariant &body = QVariant(),
                                 const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *del(const QString &path = QString(),
                                 const QVariantMap &headers = QVariantMap());

private:
    /// `path` joined onto the base, with exactly one slash between them. An absolute URL is
    /// passed through untouched and still meets the allowlist, so naming an endpoint never
    /// becomes a way around it.
    QString resolve(const QString &path) const;

    Http *m_http;
    HttpEndpointConfig m_config;
};

/// The outbound HTTP helper, exposed as `Http` to the QML of an entity whose
/// `network.outbound` names somewhere to call: a promise-returning wrapper over
/// QNetworkAccessManager that verifies TLS, refuses plaintext in release, and reaches only the
/// declared prefixes. Outbound only.
class Http : public QObject
{
    Q_OBJECT

public:
    Http(QNetworkAccessManager *network, QJSEngine *engine, bool release,
         QList<HttpEndpointConfig> endpoints, QObject *parent = nullptr);

    /// The named endpoint, or null (with the known names reported) when nothing declared
    /// that name. Named entries only. A bare prefix has nothing to be called.
    Q_INVOKABLE SynQt::HttpEndpoint *api(const QString &name) const;

    Q_INVOKABLE SynQt::Promise *get(const QString &url,
                                 const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *post(const QString &url, const QVariant &body = QVariant(),
                                  const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *put(const QString &url, const QVariant &body = QVariant(),
                                 const QVariantMap &headers = QVariantMap());
    Q_INVOKABLE SynQt::Promise *del(const QString &url,
                                 const QVariantMap &headers = QVariantMap());

    /// The prefixes this helper was built with, as declared. Read by tests and by the
    /// rejection message, so the two cannot describe different lists.
    QStringList allowed() const;

    SynQt::Promise *send(const QString &method, const QString &url, const QVariant &body,
                      const QVariantMap &headers);

private:
    /// The declared endpoint whose prefix covers `url`, or nullptr. What matched is what
    /// decides which headers are attached, so a key declared for one API is never sent to
    /// another one the same entity is also allowed to reach.
    const HttpEndpointConfig *match(const QUrl &url) const;

    QNetworkAccessManager *m_network;
    QJSEngine *m_engine;
    bool m_release;
    QList<HttpEndpointConfig> m_endpoints;
    QMap<QString, HttpEndpoint *> m_named;
};

} // namespace SynQt

#endif // SYNQT_HTTP_H
