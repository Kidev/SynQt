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
    /// Fetch the key set unless it is already held. `force` fetches anyway, which is what
    /// a token signed by a key the cached set does not contain asks for.
    bool ensureJwks(const QUrl &jwksUrl, QString *error, bool force = false);

    /// One provider's key set and when it was fetched.
    struct CachedJwks
    {
        QByteArray json;
        qint64 fetchedMs{0};
    };

    QNetworkAccessManager *m_network;
    /// jwksUrl -> the key set. Providers rotate their signing keys (some weekly), and the
    /// first token signed by a new one names a kid this set does not have. Cached forever
    /// with no way to refetch, that is every login failing until the process restarts, so
    /// an unknown kid refetches once, no more often than kMinRefetchMs.
    QHash<QString, CachedJwks> m_jwksCache;
};

} // namespace SynQt

#endif // SYNQT_JWKSVERIFIER_H
