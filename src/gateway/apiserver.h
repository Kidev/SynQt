// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_APISERVER_H
#define SYNQT_APISERVER_H

#include "apiconfig.h"
#include "clientaddress.h"

#include <QFuture>
#include <QHash>
#include <QHttpServerResponse>
#include <QList>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QHttpServer;
class QHttpServerRequest;
class QJSEngine;
class QTcpServer;
QT_END_NAMESPACE

namespace SynQt {

class Api;

/// The inbound HTTP surface of one entity. A QHttpServer in front of the `Api` helper the
/// entity's QML declares its routes on.
///
/// Everything an untrusted caller can influence is checked before a handler exists. In
/// order: the rate limit, the API key, the request origin, and the body size. A request
/// that fails any of them is answered from here and never reaches QML, which is the same
/// shape as the web edge's upgrade pipeline and for the same reason. A check that runs
/// after the application code is a check the application code can forget to wait for.
///
/// This is the only part of SynQt outside the web edge that listens for the public, and
/// the only reason `SynQtGateway` exists as its own library: Qt HTTP Server is GPLv3-only,
/// so an entity that serves no inbound surface must not link it.
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
    /// The answer to one request, which may not exist yet.
    ///
    /// A future rather than a response, because a handler that reaches a connect point or
    /// calls an upstream answers on a later turn. `Api`'s own documentation is written in
    /// that shape (`.then(lot => request.reply(lot))`) and a synchronous return could only
    /// have refused it. A handler that answers immediately settles the future before this
    /// returns, so the ordinary case costs one already-finished future.
    QFuture<QHttpServerResponse> handle(const QHttpServerRequest &request);
    /// The refusal this request earns before routing, or an empty string when it earns
    /// none. Ordered cheapest-first so a flood costs the least work possible.
    QString refuse(const QHttpServerRequest &request, int *status) const;
    bool withinRate(const QString &caller);
    /// The address this request is attributed to. The peer, or what a trusted proxy said
    /// is behind it. It is the rate limit's key and the only client address a handler is
    /// given, so nothing else in here reads the forwarding header.
    QString callerAddress(const QHttpServerRequest &request) const;
    QString originOf(const QHttpServerRequest &request) const;

    ApiConfig m_config;
    /// Which address a request counts against, built once from the configured list.
    ClientAddress m_clientAddress;
    QJSEngine *m_engine;
    Api *m_api;
    QHttpServer *m_server{nullptr};
    QTcpServer *m_tcpServer{nullptr};
    QString m_errorString;
    quint16 m_port{0};

    /// Per-IP request counters for the current minute window. Cleared wholesale when the
    /// window rolls over, so the map cannot grow past the number of peers seen in one
    /// minute and a long-lived process does not accumulate an entry per address ever seen.
    QHash<QString, int> m_rateWindow;
    qint64 m_rateWindowStartMs{0};
    /// Said once, not once per request. A missing reply deadline is a configuration
    /// mistake, and a caller decides how often it is reached.
    bool m_warnedAboutDeadline{false};
};

} // namespace SynQt

#endif // SYNQT_APISERVER_H
