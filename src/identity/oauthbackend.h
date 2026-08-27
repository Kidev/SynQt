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

#include <functional>

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
    /// How many logins may wait for the browser to return from the provider at once. Each
    /// holds a QOAuth2AuthorizationCodeFlow for up to five minutes and anyone can open one,
    /// so the count is bounded; far above real concurrency.
    static constexpr int MaxPendingLogins{1024};

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
    /// Two things travel with the state:
    ///
    ///  - `binding`, which the caller must present again on the callback; exchange() refuses a
    ///    mismatch. The edge puts the browser's CSRF cookie value here.
    ///  - `context`, opaque and handed back on exchange. The edge puts the desktop loopback
    ///    return here.
    ///
    /// Both are held with the state, so any edge process can finish a login another began.
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
        /// The `context` begin() was given, back again, on a failed exchange too, so a
        /// refused desktop login can tell its waiting client. Empty when no record matched
        /// or the binding did not.
        QString context;
    };
    /// `presentedBinding` is checked against what begin() stored, in constant time, before the
    /// code is spent. The pending record is consumed either way, so a callback cannot be replayed.
    ///
    /// This form waits for the provider, which suits an HTTP route handler. A connect point slot
    /// must use exchangeAsync instead: a waiting slot holds its entity's event loop, and a mesh
    /// link dropping meanwhile would tear down the Source the slot runs on.
    ExchangeResult exchange(const QString &state, const QString &code,
                            const QString &redirectUri,
                            const QString &presentedBinding = QString{});

    /// The token step as a slot runs it: nothing waits, and `done` is called with the result
    /// when the provider has answered (at once for a refusal that needs no provider).
    using ExchangeCallback = std::function<void(const ExchangeResult &result)>;
    void exchangeAsync(const QString &state, const QString &code, const QString &redirectUri,
                       const QString &presentedBinding, ExchangeCallback done);

    /// Move a stored token entry to a stable key (the session id) once the session exists. This
    /// also marks the entry claimed (see setUnclaimedWindow).
    void rekeyTokens(const QString &fromKey, const QString &toKey);

    /// How long tokens may sit unclaimed under a state key before they are released, in case the
    /// edge that asked went away before binding a session. Five minutes by default, the window a
    /// login already has. Zero releases anything unclaimed at the next sweep.
    void setUnclaimedWindow(int seconds);

    /// The stored tokens for a key (never sent to a browser). Empty if none.
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
        /// When this entry was stored, and whether a session has been bound to it. An
        /// unbound entry is a login in flight. See setUnclaimedWindow.
        qint64 storedMs{0};
        bool bound{false};
    };

    /// One exchange in flight. See exchangeAsync. A child of the backend, so a backend
    /// going away takes the exchanges it was running with it, and every callback it holds.
    class ExchangeJob;

    QOAuth2AuthorizationCodeFlow *makeFlow(const IdentityProviderConfig &provider,
                                           const QString &redirectUri);
    /// One bounded GET with a bearer token, answered through `done` with the body, or an
    /// empty body and the error. The provider's profile endpoints are read through this.
    using BodyCallback = std::function<void(const QByteArray &body, const QString &error)>;
    void httpGet(const QUrl &url, const QString &bearer, QObject *context, BodyCallback done);
    QNetworkAccessManager *network();
    bool refreshOne(const QString &key);
    void expirePending();
    /// Drop every entry no session was ever bound to that is past the window. Runs on its
    /// own timer, not the refresh one: refreshing is optional and letting go of a secret
    /// nobody claimed is not.
    void releaseUnclaimed();

    IdentityConfig m_config;
    QNetworkAccessManager *m_network{nullptr};
    JwksVerifier *m_jwks{nullptr};
    QTimer *m_refreshTimer{nullptr};
    QTimer *m_unclaimedTimer{nullptr};
    int m_unclaimedWindowSeconds{300};
    int m_refreshMargin{0};
    bool m_sweeping{false};   ///< a refresh sweep is running. See refreshExpiring()

    QHash<QString, Pending> m_pending;      ///< state -> pending login (verifier + nonce)
    QHash<QString, TokenEntry> m_tokens;    ///< key -> stored tokens
};

} // namespace SynQt

#endif // SYNQT_OAUTHBACKEND_H
