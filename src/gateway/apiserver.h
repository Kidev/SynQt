// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_APISERVER_H
#define SYNQT_APISERVER_H

#include "apiconfig.h"
#include "clientaddress.h"

#include <QHash>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

QT_BEGIN_NAMESPACE
class QHttpServer;
class QHttpServerRequest;
class QJSEngine;
class QTcpServer;
QT_END_NAMESPACE

namespace SynQt {

class Api;

/// The inbound HTTP surface of one entity: a QHttpServer in front of the `Api` helper the
/// entity's QML declares its routes on.
///
/// Everything an untrusted caller controls is checked before a handler runs: the rate limit,
/// the API key, the origin, then the body size. A failing request is answered here and never
/// reaches QML. Its own library, `SynQtGateway`, because Qt HTTP Server is GPLv3 only.
class ApiServer : public QObject
{
    Q_OBJECT

public:
    ApiServer(ApiConfig config, QJSEngine *engine, QObject *parent = nullptr);
    ~ApiServer() override;

    /// The helper the entity's QML declares routes on. Alive before `start()`, because the
    /// entity singleton declares its routes as it is created and that happens first.
    Api *api() const;

    bool start();
    QString errorString() const;
    quint16 serverPort() const;

signals:
    void requestRefused(const QString &reason);

private:
    /// Answer one request through `responder`, now or later: a handler that calls a connect
    /// point or an upstream answers on a later turn, and the responder is moved out and held
    /// until it does.
    void handle(const QHttpServerRequest &request, QHttpServerResponder &responder);
    /// The refusal this request earns before routing, or an empty string when it earns
    /// none. Ordered cheapest-first so a flood costs the least work possible.
    QString refuse(const QHttpServerRequest &request, int *status) const;
    bool withinRate(const QString &caller);
    /// The address this request is attributed to. The peer, or what a trusted proxy said
    /// is behind it. It is the rate limit's key and the only client address a handler is
    /// given, so nothing else in here reads the forwarding header.
    QString callerAddress(const QHttpServerRequest &request) const;
    QString originOf(const QHttpServerRequest &request) const;
    /// The answer to a browser's preflight, when the request is one. A browser sends an
    /// OPTIONS carrying the origin and the method it means to use, and no key, before any
    /// cross-origin request with a custom header. This is answered for an origin the
    /// surface names and refused for any other, before the key is looked for, because a
    /// preflight never carries one. Empty when the request is not a preflight.
    std::optional<QHttpServerResponse> preflightAnswer(const QHttpServerRequest &request,
                                                       const QString &origin) const;
    /// Add the one header a browser needs to hand an answer to the page, when the request
    /// came from an origin the surface names. Nothing for any other caller.
    static void allowOrigin(QHttpServerResponse &response, const QString &origin);

    ApiConfig m_config;
    /// Which address a request counts against, built once from the configured list.
    ClientAddress m_clientAddress;
    QJSEngine *m_engine;
    Api *m_api;
    /// The parent of every request still waiting for its handler. Created before the HTTP
    /// server, so it is destroyed first: a waiting request holds a responder, and a responder
    /// destroyed after the connection it answers on writes to freed memory.
    QObject *m_requests;
    QHttpServer *m_server{nullptr};
    QTcpServer *m_tcpServer{nullptr};
    QString m_errorString;
    quint16 m_port{0};

    /// Per-IP request counters for the current minute window. Cleared wholesale when the
    /// window rolls over, so the map cannot grow past the number of peers seen in one
    /// minute and a long-lived process does not accumulate an entry per address ever seen.
    QHash<QString, int> m_rateWindow;
    qint64 m_rateWindowStartMs{0};
    /// Warned about once per process. A missing reply deadline is a configuration
    /// mistake, and a caller decides how often it is reached.
    bool m_warnedAboutDeadline{false};
};

} // namespace SynQt

#endif // SYNQT_APISERVER_H
