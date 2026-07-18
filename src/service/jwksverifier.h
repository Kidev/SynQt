// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_JWKSVERIFIER_H
#define SYNQT_JWKSVERIFIER_H

#include "identityconfig.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
QT_END_NAMESPACE

namespace SynQt {

/// Verifies an OpenID Connect ID token: the RS256 signature against the provider JWKS
/// (fetched and cached with QNetworkAccessManager), plus the iss, aud, exp and nonce
/// claims. The crypto is jwt-cpp's; SynQt does no hand-rolled cryptography and reports
/// failure through the return value, never across an exception.
class JwksVerifier : public QObject
{
    Q_OBJECT

public:
    explicit JwksVerifier(QNetworkAccessManager *network, QObject *parent = nullptr);

    /// The verified claims (sub, email, name, ...) on success, or an empty map with *error
    /// set on any failure (bad signature, wrong issuer/audience, expired, nonce mismatch).
    QVariantMap verify(const QString &idToken, const IdentityProviderConfig &provider,
                       const QString &expectedNonce, QString *error);

private:
    bool ensureJwks(const QUrl &jwksUrl, QString *error);

    QNetworkAccessManager *m_network;
    QHash<QString, QByteArray> m_jwksCache;  // jwksUrl -> raw JWKS JSON
};

} // namespace SynQt

#endif // SYNQT_JWKSVERIFIER_H
