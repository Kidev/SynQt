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

/// The secret-bearing OAuth2 / OpenID Connect engine. It holds the client secret, builds
/// the authorization URL (PKCE + state), performs the server-side token exchange, verifies
/// and normalizes the identity (userinfo or a JWKS-verified ID token), and owns the stored
/// access/refresh/ID tokens with their expiry. It also refreshes an access token before it
/// expires, server-side, using the refresh token (see "Session lifecycle" in
/// [Authentication](https://synqt.org/authentication/)).
///
/// This engine is free of any browser I/O: it exposes no cookies and no HTTP
/// routes. The web edge's IdentityProvider drives it for the in-process case. A dedicated
/// auth entity's IdentityService drives the same engine for the provider_entity case, so
/// the secret and the tokens live in exactly one place either way.
class OAuthBackend : public QObject
{
    Q_OBJECT

public:
    explicit OAuthBackend(IdentityConfig config, QObject *parent = nullptr);
    ~OAuthBackend() override;

    /// The authorization step. Build the provider's authorize URL and hold a pending login
    /// keyed by the returned state (the PKCE verifier and the OIDC nonce stay here). The
    /// redirectUri is the caller's public callback URL. On failure `error` is set.
    struct BeginResult
    {
        QString state;
        QUrl authorizeUrl;
        QString error;
    };
    /// Two things travel with the state, and they have different jobs:
    ///
    ///  - `binding` is what the caller must present again on the callback, and exchange()
    ///    refuses one that does not. The edge puts the browser's CSRF cookie value here, so
    ///    a state alone is not enough to complete a login.
    ///  - `context` is opaque and is handed back on exchange. The edge puts the
    ///    desktop loopback return in it, which it needs to answer the waiting client.
    ///
    /// Both are held here with the state rather than by the caller, which is what lets any
    /// edge process finish a login any other one started (a replicated edge). With a single
    /// edge it changes nothing except where the record lives.
    BeginResult begin(const QString &providerName, const QString &redirectUri,
                      const QString &binding = QString{},
                      const QString &context = QString{});

    /// The token step. Exchange the returned authorization code for tokens (client secret +
    /// PKCE verifier), verify and normalize the identity, and store the tokens under a key
    /// (the state, until rekeyed to a stable session id). Consumes the pending state.
    struct ExchangeResult
    {
        QVariantMap identity;
        QString tokenKey;
        QString error;
        /// The `context` begin() was given, back again. It comes back on a failed exchange
        /// too: a desktop login that is refused has to tell the waiting client
        /// so, over the loopback address that is in here, or the app sits on its listener
        /// until the timeout and the visitor reads a refusal as a hang. Empty only when
        /// there was no record to match (unknown or expired state) or the presented binding
        /// did not match it, since neither of those is a login this caller started.
        QString context;
    };
    /// `presentedBinding` is checked against what begin() stored, in constant time, BEFORE
    /// the code is spent: a callback whose binding does not match is somebody else's, and
    /// exchanging first would burn a real authorization code on it. The pending record is
    /// consumed either way, so a callback cannot be replayed against a second process.
    ExchangeResult exchange(const QString &state, const QString &code,
                            const QString &redirectUri,
                            const QString &presentedBinding = QString{});

    /// Move a stored token entry to a stable key (the session id) once the session exists.
    void rekeyTokens(const QString &fromKey, const QString &toKey);

    /// The stored tokens for a key (never sent to a browser); empty if none.
    QVariantMap tokens(const QString &key) const;

    void releaseTokens(const QString &key);

    /// How many token entries this engine is holding, under state keys and session ids
    /// alike. A count and nothing else. It exists so a test can ask whether a login that
    /// minted no session left its tokens behind, which is not a question `tokens()` can
    /// answer without the key.
    int heldTokenCount() const;

    /// Refresh every stored access token that is within `marginSeconds` of expiry, using its
    /// refresh token, without involving the browser. Returns how many were refreshed. Called
    /// both directly and by the periodic sweep timer.
    int refreshExpiring(int marginSeconds);

    /// Enable the periodic refresh sweep. Every `intervalSeconds` refresh tokens due within
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
        QString binding;   ///< must be re-presented on the callback. See begin()
        QString context;   ///< opaque, handed back on exchange. See begin()
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
    QNetworkAccessManager *network();
    bool refreshOne(const QString &key);
    void expirePending();

    IdentityConfig m_config;
    QNetworkAccessManager *m_network{nullptr};
    JwksVerifier *m_jwks{nullptr};
    QTimer *m_refreshTimer{nullptr};
    int m_refreshMargin{0};
    bool m_sweeping{false};   ///< a refresh sweep is running; see refreshExpiring()

    QHash<QString, Pending> m_pending;      ///< state -> pending login (verifier + nonce)
    QHash<QString, TokenEntry> m_tokens;    ///< key -> stored tokens
};

} // namespace SynQt

#endif // SYNQT_OAUTHBACKEND_H
