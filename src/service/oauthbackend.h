// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_OAUTHBACKEND_H
#define SYNQT_OAUTHBACKEND_H

#include "identityconfig.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QOAuth2AuthorizationCodeFlow;
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

class JwksVerifier;

/// The secret-bearing OAuth2 / OpenID Connect engine: it holds the client secret, builds
/// the authorization URL (PKCE + state), performs the server-side token exchange, verifies
/// and normalizes the identity (userinfo or a JWKS-verified ID token), and owns the stored
/// access/refresh/ID tokens with their expiry. It also refreshes an access token before it
/// expires, server-side, using the refresh token (see "Session lifecycle" in
/// [Authentication](https://synqt.org/authentication/)).
///
/// This engine is deliberately free of any browser I/O: it exposes no cookies and no HTTP
/// routes. The web edge's IdentityProvider drives it for the in-process case; a dedicated
/// auth entity's IdentityService drives the same engine for the provider_entity case, so
/// the secret and the tokens live in exactly one place either way.
class OAuthBackend : public QObject
{
    Q_OBJECT

public:
    explicit OAuthBackend(IdentityConfig config, QObject *parent = nullptr);
    ~OAuthBackend() override;

    /// The authorization step: build the provider's authorize URL and hold a pending login
    /// keyed by the returned state (the PKCE verifier and the OIDC nonce stay here). The
    /// redirectUri is the caller's public callback URL. On failure `error` is set.
    struct BeginResult
    {
        QString state;
        QUrl authorizeUrl;
        QString error;
    };
    BeginResult begin(const QString &providerName, const QString &redirectUri);

    /// The token step: exchange the returned authorization code for tokens (client secret +
    /// PKCE verifier), verify and normalize the identity, and store the tokens under a key
    /// (the state, until rekeyed to a stable session id). Consumes the pending state.
    struct ExchangeResult
    {
        QVariantMap identity;
        QString tokenKey;
        QString error;
    };
    ExchangeResult exchange(const QString &state, const QString &code,
                            const QString &redirectUri);

    /// Move a stored token entry to a stable key (the session id) once the session exists.
    void rekeyTokens(const QString &fromKey, const QString &toKey);

    /// The stored tokens for a key (never sent to a browser); empty if none.
    QVariantMap tokens(const QString &key) const;

    void releaseTokens(const QString &key);

    /// Refresh every stored access token that is within `marginSeconds` of expiry, using its
    /// refresh token, without involving the browser. Returns how many were refreshed. Called
    /// both directly and by the periodic sweep timer.
    int refreshExpiring(int marginSeconds);

    /// Enable the periodic refresh sweep: every `intervalSeconds` refresh tokens due within
    /// `marginSeconds`. A non-positive interval disables it.
    void setAutoRefresh(int intervalSeconds, int marginSeconds);

    bool providerExists(const QString &name) const;
    bool isDevStub(const QString &name) const;

signals:
    /// Emitted after a stored token entry is refreshed server-side.
    void tokensRefreshed(const QString &key);

private:
    struct Pending
    {
        QOAuth2AuthorizationCodeFlow *flow{nullptr};
        QString providerName;
        QString nonce;
        qint64 createdMs{0};
    };

    struct TokenEntry
    {
        QString providerName;
        QString accessToken;
        QString refreshToken;
        QString idToken;
        qint64 expiresAtMs{0}; ///< 0 == unknown/never
    };

    QOAuth2AuthorizationCodeFlow *makeFlow(const IdentityProviderConfig &provider,
                                           const QString &redirectUri);
    QVariantMap normalizeIdentity(const IdentityProviderConfig &provider,
                                  QOAuth2AuthorizationCodeFlow *flow,
                                  const QString &expectedNonce, QString *error);
    QByteArray httpGet(const QUrl &url, const QString &bearer, QString *error);
    bool refreshOne(const QString &key);
    void expirePending();

    IdentityConfig m_config;
    QNetworkAccessManager *m_network{nullptr};
    JwksVerifier *m_jwks{nullptr};
    QTimer *m_refreshTimer{nullptr};
    int m_refreshMargin{0};

    QHash<QString, Pending> m_pending;      ///< state -> pending login (verifier + nonce)
    QHash<QString, TokenEntry> m_tokens;    ///< key -> stored tokens
};

} // namespace SynQt

#endif // SYNQT_OAUTHBACKEND_H
