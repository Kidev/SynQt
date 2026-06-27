// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_JWKSVERIFIER_H
#define SYNQT_JWKSVERIFIER_H

#include "identityconfig.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QString>
#include <QVariantMap>

#include <functional>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QUrl;
QT_END_NAMESPACE

namespace SynQt {

/// Verifies an OpenID Connect ID token. The RS256 signature against the provider JWKS
/// (fetched and cached with QNetworkAccessManager), plus the iss, aud, exp and nonce
/// claims. The crypto is jwt-cpp's; SynQt does no hand-rolled cryptography and reports
/// failure through the return value, never across an exception.
class JwksVerifier : public QObject
{
    Q_OBJECT

public:
    explicit JwksVerifier(QNetworkAccessManager *network, QObject *parent = nullptr);

    /// The verified claims (sub, email, name, ...), or an empty map with the error, once
    /// the key set is on hand. At once when it is cached, after the fetch when it is not.
    ///
    /// Answered through `done`, with no form that waits: the caller is a connect point slot, and a
    /// slot that waits holds its entity's event loop, stacking every caller that arrives
    /// meanwhile beneath it. A test that wants to wait composes the wait itself.
    using VerifyCallback = std::function<void(const QVariantMap &claims, const QString &error)>;
    void verifyAsync(const QString &idToken, const IdentityProviderConfig &provider,
                     const QString &expectedNonce, VerifyCallback done);

private:
    /// Have the key set at `jwksUrl` on hand. Answer at once from the cache, or fetch it
    /// and answer when it arrives. `force` fetches although one is held, which is what a
    /// token signed by a key the cached set does not contain asks for. `ok` is then false
    /// with an empty error when the refetch floor refused it, which is not a failure to
    /// report.
    using FetchCallback = std::function<void(bool ok, const QString &error)>;
    void fetchJwks(const QUrl &jwksUrl, bool force, FetchCallback done);
    /// The checks that need no network: the key's shape, the signature, the claims.
    QVariantMap checkToken(const QStringList &parts, const QJsonObject &jwk,
                           const IdentityProviderConfig &provider,
                           const QString &expectedNonce, QString *error) const;

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
    /// How many fetches are waiting on the network right now, held under
    /// kMaxConcurrentFetches.
    int m_fetching{0};
};

} // namespace SynQt

#endif // SYNQT_JWKSVERIFIER_H
