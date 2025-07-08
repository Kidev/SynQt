// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "jwksverifier.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <jwt-cpp/jwt.h>

#include <system_error>

namespace SynQt {

namespace {

QByteArray decodeBase64Url(const QString &segment)
{
    return QByteArray::fromBase64(segment.toUtf8(), QByteArray::Base64UrlEncoding);
}

QJsonObject jsonSegment(const QString &segment)
{
    return QJsonDocument::fromJson(decodeBase64Url(segment)).object();
}

// The JWK whose kid matches (or the sole key when the token carries no kid).
QJsonObject selectKey(const QByteArray &jwks, const QString &kid)
{
    // Copy-init, not brace-init: QJsonArray{anArray} would wrap the array as a single
    // element (its initializer_list is of QJsonValue), not copy it.
    const QJsonArray keys =
        QJsonDocument::fromJson(jwks).object().value(QStringLiteral("keys")).toArray();
    for (qsizetype i{0}; i < keys.size(); ++i) {
        const QJsonObject key{keys.at(i).toObject()};
        if (kid.isEmpty() || key.value(QStringLiteral("kid")).toString() == kid) {
            return key;
        }
    }
    return {};
}

bool audienceMatches(const QJsonValue &aud, const QString &expected)
{
    if (aud.isString()) {
        return aud.toString() == expected;
    }
    if (aud.isArray()) {
        const QJsonArray values = aud.toArray();  // copy-init (see selectKey)
        for (qsizetype i{0}; i < values.size(); ++i) {
            if (values.at(i).toString() == expected) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

JwksVerifier::JwksVerifier(QNetworkAccessManager *network, QObject *parent)
    : QObject{parent}
    , m_network{network}
{
}

bool JwksVerifier::ensureJwks(const QUrl &jwksUrl, QString *error)
{
    if (m_jwksCache.contains(jwksUrl.toString())) {
        return true;
    }
    QNetworkReply *reply{m_network->get(QNetworkRequest{jwksUrl})};
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        if (error) {
            *error = QStringLiteral("JWKS fetch failed: %1").arg(reply->errorString());
        }
        reply->deleteLater();
        return false;
    }
    m_jwksCache.insert(jwksUrl.toString(), reply->readAll());
    reply->deleteLater();
    return true;
}

QVariantMap JwksVerifier::verify(const QString &idToken, const IdentityProviderConfig &provider,
                                 const QString &expectedNonce, QString *error)
{
    const auto fail{[error](const QString &message) -> QVariantMap {
        if (error) {
            *error = message;
        }
        return {};
    }};

    // A JWT is three non-empty base64url segments: header.payload.signature.
    const QStringList parts{idToken.split(QLatin1Char('.'))};
    if (parts.size() != 3 || parts.at(0).isEmpty() || parts.at(1).isEmpty()
        || parts.at(2).isEmpty()) {
        return fail(QStringLiteral("malformed ID token"));
    }

    const QJsonObject header{jsonSegment(parts.at(0))};
    if (header.value(QStringLiteral("alg")).toString() != QLatin1String("RS256")) {
        return fail(QStringLiteral("unsupported ID-token algorithm"));
    }

    if (!ensureJwks(provider.jwksUrl, error)) {
        return {};
    }
    const QJsonObject jwk{selectKey(m_jwksCache.value(provider.jwksUrl.toString()),
                                    header.value(QStringLiteral("kid")).toString())};
    if (jwk.isEmpty() || jwk.value(QStringLiteral("kty")).toString() != QLatin1String("RSA")) {
        return fail(QStringLiteral("no matching RSA signing key in JWKS"));
    }

    // Build the RSA public key from the JWK modulus/exponent and verify the RS256
    // signature over the exact signing input (base64url header "." base64url payload).
    std::error_code ec;
    const std::string pem{jwt::helper::create_public_key_from_rsa_components(
        jwk.value(QStringLiteral("n")).toString().toStdString(),
        jwk.value(QStringLiteral("e")).toString().toStdString(), ec)};
    if (ec) {
        return fail(QStringLiteral("could not build signing key: %1")
                        .arg(QString::fromStdString(ec.message())));
    }

    const std::string signingInput{(parts.at(0) + QLatin1Char('.') + parts.at(1)).toStdString()};
    const QByteArray signature{decodeBase64Url(parts.at(2))};
    const jwt::algorithm::rs256 algorithm{pem, "", "", ""};
    algorithm.verify(signingInput,
                     std::string{signature.constData(),
                                 static_cast<size_t>(signature.size())},
                     ec);
    if (ec) {
        return fail(QStringLiteral("ID-token signature invalid"));
    }

    // Claim checks (parsed with Qt, so a missing claim never throws).
    const QJsonObject payload{jsonSegment(parts.at(1))};
    if (!provider.issuer.isEmpty()
        && payload.value(QStringLiteral("iss")).toString() != provider.issuer) {
        return fail(QStringLiteral("ID-token issuer mismatch"));
    }
    const QString audience{provider.audience.isEmpty() ? provider.clientId : provider.audience};
    if (!audienceMatches(payload.value(QStringLiteral("aud")), audience)) {
        return fail(QStringLiteral("ID-token audience mismatch"));
    }
    const qint64 now{QDateTime::currentSecsSinceEpoch()};
    if (payload.contains(QStringLiteral("exp"))
        && static_cast<qint64>(payload.value(QStringLiteral("exp")).toDouble()) + 60 < now) {
        return fail(QStringLiteral("ID token expired"));
    }
    if (!expectedNonce.isEmpty()
        && payload.value(QStringLiteral("nonce")).toString() != expectedNonce) {
        return fail(QStringLiteral("ID-token nonce mismatch"));
    }

    return payload.toVariantMap();
}

} // namespace SynQt
