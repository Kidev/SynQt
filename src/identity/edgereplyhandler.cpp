// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "edgereplyhandler.h"

#include <QAbstractOAuth>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>

#include <utility>

namespace SynQt {

EdgeReplyHandler::EdgeReplyHandler(QString callbackUrl, QObject *parent)
    : QAbstractOAuthReplyHandler{parent}
    , m_callbackUrl{std::move(callbackUrl)}
{
}

QString EdgeReplyHandler::callback() const
{
    return m_callbackUrl;
}

void EdgeReplyHandler::receiveCallback(const QVariantMap &parameters)
{
    emit callbackReceived(parameters);
}

void EdgeReplyHandler::networkReplyFinished(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        emit tokenRequestErrorOccurred(QAbstractOAuth::Error::NetworkError,
                                       reply->errorString());
        return;
    }

    const QByteArray data{reply->readAll()};
    emit replyDataReceived(data);

    // The token endpoint returns JSON (per RFC 6749) or, for some older providers,
    // application/x-www-form-urlencoded. Parse either into the tokens map.
    const QByteArray contentType{
        reply->header(QNetworkRequest::ContentTypeHeader).toByteArray()};
    QVariantMap tokens;
    if (contentType.contains("application/json") || data.trimmed().startsWith('{')) {
        const QJsonDocument document{QJsonDocument::fromJson(data)};
        if (document.isObject()) {
            tokens = document.object().toVariantMap();
        }
    } else {
        const QUrlQuery query{QString::fromUtf8(data)};
        const QList<QPair<QString, QString>> items{query.queryItems()};
        for (const QPair<QString, QString> &item : items) {
            tokens.insert(item.first, item.second);
        }
    }

    if (tokens.isEmpty()) {
        emit tokenRequestErrorOccurred(QAbstractOAuth::Error::OAuthTokenNotFoundError,
                                       QStringLiteral("no tokens in the response"));
        return;
    }
    emit tokensReceived(tokens);
}

} // namespace SynQt
