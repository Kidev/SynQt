// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYPROVIDER_H
#define SYNQT_IDENTITYPROVIDER_H

#include "identityconfig.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QHttpServerRequest;
class QHttpServerResponse;
class QQmlComponent;
class QQmlEngine;
QT_END_NAMESPACE

namespace SynQt {

class ClientAddress;
class DeviceRegistry;
class SessionManager;
class OAuthBackend;

/// How the session cookie must be flagged, mirrored from the edge so the login-issued
/// cookie is byte-identical to the one the upgrade verifier accepts.
struct CookiePolicy
{
    QString name{QStringLiteral("synqt_session")};
    bool sameSiteNone{false}; ///< split_origin -> SameSite=None; Secure
    bool secure{true};        ///< set on TLS (and always under SameSite=None)
};

/// The browser-facing half of easy auth on the web edge: the login/callback/logout routes,
/// the login-CSRF state cookie, the scope-mapping hook, session creation and the httpOnly
/// session cookie. The secret-bearing OAuth mechanics (token exchange, ID-token
/// verification, token storage, refresh) live in an OAuthBackend, so the browser only ever
/// ends with the session cookie.
///
/// Two modes (see "Where identity runs" in
/// [Authentication](https://synqt.org/authentication/)):
///  - In process (default), this provider owns an OAuthBackend. The secret and tokens live
///    on the edge.
///  - provider_entity. The edge holds no secret. The begin/exchange steps are delegated to
///    a dedicated auth entity over the Identity mesh connect point via attachRemote(). The
///    secret and tokens live only on the auth entity.
class IdentityProvider : public QObject
{
    Q_OBJECT

public:
    IdentityProvider(IdentityConfig config, SessionManager *sessions, QQmlEngine *engine,
                     QString edgeOrigin, CookiePolicy cookie, QObject *parent = nullptr);
    ~IdentityProvider() override;

    /// The route handlers, invoked by the web edge.
    QHttpServerResponse handleLogin(const QHttpServerRequest &request);
    QHttpServerResponse handleCallback(const QHttpServerRequest &request);
    QHttpServerResponse handleLogout(const QHttpServerRequest &request);

    /// The desktop half. A native client exchanges the claim code the loopback redirect
    /// carried for the session it stands for. POST only, single use, short lived.
    QHttpServerResponse handleClaim(const QHttpServerRequest &request);

    /// Staying signed in. A native client spends the device credential it stored at its last
    /// launch for a fresh session and the next credential. POST only, single use, and the one
    /// route that turns something on disk back into a session.
    QHttpServerResponse handleDevice(const QHttpServerRequest &request);

    /// The edge's client-address resolver, so the device route's rate window counts the
    /// visitor rather than the balancer. Owned by the WebEdge that sets it, which outlives
    /// this. Null means the peer address is the visitor, which is true until a deployment
    /// says otherwise.
    void setClientAddress(const ClientAddress *resolver);

    QString loginRoute() const;
    QString callbackRoute() const;
    QString logoutRoute() const;
    /// Where handleClaim() is served, under the login route.
    QString claimRoute() const;
    /// Where handleDevice() is served, under the login route.
    QString deviceRoute() const;

    /// The device registry, or nullptr when the project does not persist desktop sessions
    /// (or the configured store would not open). Exposed for the tests and for the edge,
    /// which ends a session's family when that session is signed out.
    DeviceRegistry *devices() const;

    /// The edge's public origin (e.g. https://host:port), used to form the callback
    /// redirect_uri. Set once the edge has bound its port.
    void setEdgeOrigin(const QString &origin);

    /// Promote this edge to provider_entity mode. The begin/exchange/refresh steps run on
    /// the auth entity behind the given Identity Replica, and this edge holds no secret. Must
    /// be called before the first login (typically once the mesh link is up).
    void attachRemote(QObject *identityReplica);
    bool isRemote() const;

    /// Server-side tokens for a session (never sent to the browser). Empty if none/expired.
    /// In provider_entity mode the tokens live on the auth entity, so this is always empty.
    QVariantMap tokensForSession(const QByteArray &sessionId) const;

    /// The in-process OAuth engine, or nullptr in provider_entity mode. Exposed for the
    /// refresh sweep and for tests.
    OAuthBackend *backend() const;

    /// Drop everything held for a session that no longer exists, wherever it is held (on
    /// this edge, or on the auth entity). Logging out already does this. This is the same
    /// release for the session nobody logs out of, which is most of them: without it an
    /// edge keeps a visitor's access and refresh tokens for as long as the process lives,
    /// long after the session they belong to expired.
    ///
    /// Deliberately not the device credential: a session running out of time is what the
    /// credential exists to survive. Only signing out ends the family (handleLogout).
    void forgetSession(const QByteArray &sessionId);

signals:
    /// Internal: a delegated begin/exchange result for the given request has arrived from the
    /// auth entity, so the waiting route handler can resume.
    void beginArrived(const QString &requestId);
    void exchangeArrived(const QString &requestId);

private slots:
    /// The auth entity's answers to a delegated begin/exchange (provider_entity mode). String
    /// slots so the dynamic Identity Replica's signals can connect by name.
    void onBeginResult(const QString &requestId, const QString &state,
                       const QString &authorizeUrl, const QString &error);
    void onExchangeResult(const QString &requestId, const QString &identityJson,
                          const QString &context, const QString &error);

private:
    /// What the callback needs handed back: the desktop flow's answer, all three empty for
    /// a browser login. The loopback URL the system browser is sent back to, the nonce the
    /// native client will match that arrival against, and the S256 challenge whose verifier
    /// only that client holds.
    ///
    /// This is not held on this edge. It is serialized as the `context` the identity engine
    /// keeps with the state (in process, or on the auth entity when identity is promoted)
    /// and comes back on the exchange, alongside the CSRF `binding` the engine checks. That
    /// is the whole of what makes a login answerable by an edge process other than the one
    /// that began it, which is what a replicated edge needs and what a single edge is
    /// unaffected by.
    struct LoginContext
    {
        QString returnUrl;
        QString returnState;
        QString returnChallenge;

        bool isDesktop() const { return !returnUrl.isEmpty(); }
        QString toJson() const;
        static LoginContext fromJson(const QString &json);
    };

    /// A finished desktop login, waiting to be collected. The session already exists; this
    /// is the one-time code that stands for it until the client that started the login
    /// exchanges it over its own connection.
    struct PendingClaim
    {
        QByteArray sessionId;
        QString challenge;  ///< S256, matched against the verifier presented at the claim
        qint64 createdMs{0};
    };

    /// Delegates to the local backend or the remote auth entity depending on the mode.
    struct BeginOutcome { QString state; QString authorizeUrl; QString error; };
    BeginOutcome beginLogin(const QString &providerName, const QString &binding,
                            const QString &context);
    struct ExchangeOutcome
    {
        QVariantMap identity;
        QString tokenKey;
        QString error;
        QString context;  ///< what beginLogin was given, back again. See LoginContext
    };
    ExchangeOutcome exchangeCode(const QString &state, const QString &code,
                                 const QString &presentedBinding);
    void bindRemoteSession(const QString &state, const QByteArray &sessionId);
    void releaseRemoteTokens(const QByteArray &sessionId);

    QString mapScope(const QVariantMap &identity);
    /// The one answer both desktop routes give: the session, the cookie name to present it
    /// under, and the device credential to store in place of whatever was just spent (absent
    /// when the project persists nothing or this client's store was below the floor).
    QHttpServerResponse sessionAnswer(const QByteArray &sessionId, const QString &family,
                                      const QByteArray &secret, qint64 expiresMs);
    /// Remember which family a session came from, so signing that session out ends the
    /// credential too, and so reuse detection can end every session a stolen family opened.
    void bindFamily(const QByteArray &sessionId, const QString &family);
    void onReuseDetected(const QString &family);
    QByteArray buildCookie(const QByteArray &token) const;
    QByteArray buildStateCookie(const QByteArray &value, bool expire) const;
    void expireClaims();
    /// The loopback redirect the system browser is sent to once the session exists, or an
    /// empty response when this login was not a desktop one.
    QHttpServerResponse loopbackRedirect(const LoginContext &context, const QString &code,
                                         const QString &error) const;

    IdentityConfig m_config;
    SessionManager *m_sessions;
    QQmlEngine *m_engine;
    QString m_edgeOrigin;
    CookiePolicy m_cookie;
    OAuthBackend *m_backend{nullptr};      ///< in-process engine (null when remote)
    QPointer<QObject> m_remote;            ///< Identity Replica in provider_entity mode
    QQmlComponent *m_mappingComponent{nullptr};
    QObject *m_mapping{nullptr};

    DeviceRegistry *m_devices{nullptr};     ///< null unless the project persists sessions
    const ClientAddress *m_clientAddress{nullptr};  ///< the edge's; null means the peer

    QHash<QString, PendingClaim> m_claims;  ///< claim code -> the session it stands for

    /// Fixed-window request counts per client address for the device route, so a machine
    /// cannot sit there spending guesses. The secret is 256 bits, so this is not what makes
    /// guessing hopeless; it is what keeps a guesser from costing the edge a database read
    /// per attempt.
    struct RateWindow
    {
        qint64 startedMs{0};
        int count{0};
    };
    QHash<QString, RateWindow> m_deviceRate;

    /// Delegated results, keyed by request id, filled by the onBeginResult/onExchangeResult
    /// slots and consumed by the waiting route handler (provider_entity mode only).
    QHash<QString, BeginOutcome> m_beginResults;
    QHash<QString, ExchangeOutcome> m_exchangeResults;
};

} // namespace SynQt

#endif // SYNQT_IDENTITYPROVIDER_H
