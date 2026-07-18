// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_EDGEREPLYHANDLER_H
#define SYNQT_EDGEREPLYHANDLER_H

#include <QAbstractOAuthReplyHandler>
#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QNetworkReply;
QT_END_NAMESPACE

namespace SynQt {

/// A reply handler for the server-side (web-edge) OAuth flow. Unlike Qt's loopback
/// handler, its callback() is the edge's PUBLIC callback URL (the provider redirect_uri),
/// and the edge feeds the received authorization parameters in through receiveCallback()
/// rather than listening on a local port. It parses the token-endpoint response (JSON or
/// form-encoded) into the tokens map the flow consumes.
class EdgeReplyHandler : public QAbstractOAuthReplyHandler
{
    Q_OBJECT

public:
    explicit EdgeReplyHandler(QString callbackUrl, QObject *parent = nullptr);

    QString callback() const override;
    void networkReplyFinished(QNetworkReply *reply) override;

    /// Called by the edge callback route with the query parameters the provider returned
    /// (code, state, ...). Drives the flow's state check and token request.
    void receiveCallback(const QVariantMap &parameters);

private:
    QString m_callbackUrl;
};

} // namespace SynQt

#endif // SYNQT_EDGEREPLYHANDLER_H
