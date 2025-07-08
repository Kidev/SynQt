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
///  - In process (default): this provider owns an OAuthBackend; the secret and tokens live
///    on the edge.
///  - provider_entity: the edge holds no secret. The begin/exchange steps are delegated to
///    a dedicated auth entity over the Identity mesh connect point via attachRemote(); the
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

    QString loginRoute() const;
    QString callbackRoute() const;
    QString logoutRoute() const;

    /// The edge's public origin (e.g. https://host:port), used to form the callback
    /// redirect_uri. Set once the edge has bound its port.
    void setEdgeOrigin(const QString &origin);

    /// Promote this edge to provider_entity mode: the begin/exchange/refresh steps run on
    /// the auth entity behind the given Identity Replica, and this edge holds no secret. Must
    /// be called before the first login (typically once the mesh link is up).
    void attachRemote(QObject *identityReplica);
    bool isRemote() const;

    /// Server-side tokens for a session (never sent to the browser); empty if none/expired.
    /// In provider_entity mode the tokens live on the auth entity, so this is always empty.
    QVariantMap tokensForSession(const QByteArray &sessionId) const;

    /// The in-process OAuth engine, or nullptr in provider_entity mode. Exposed for the
    /// refresh sweep and for tests.
    OAuthBackend *backend() const;

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
                          const QString &error);

private:
    struct PendingLogin
    {
        QString csrfToken;  ///< bound to the initiating browser via a cookie (login CSRF)
        qint64 createdMs{0};
    };

    /// Delegates to the local backend or the remote auth entity depending on the mode.
    struct BeginOutcome { QString state; QString authorizeUrl; QString error; };
    BeginOutcome beginLogin(const QString &providerName);
    struct ExchangeOutcome { QVariantMap identity; QString tokenKey; QString error; };
    ExchangeOutcome exchangeCode(const QString &state, const QString &code);
    void bindRemoteSession(const QString &state, const QByteArray &sessionId);
    void releaseRemoteTokens(const QByteArray &sessionId);

    QString mapScope(const QVariantMap &identity);
    QByteArray buildCookie(const QByteArray &token) const;
    QByteArray buildStateCookie(const QByteArray &value, bool expire) const;
    void expirePending();

    IdentityConfig m_config;
    SessionManager *m_sessions;
    QQmlEngine *m_engine;
    QString m_edgeOrigin;
    CookiePolicy m_cookie;
    OAuthBackend *m_backend{nullptr};      ///< in-process engine (null when remote)
    QPointer<QObject> m_remote;            ///< Identity Replica in provider_entity mode
    QQmlComponent *m_mappingComponent{nullptr};
    QObject *m_mapping{nullptr};

    QHash<QString, PendingLogin> m_pending; ///< state -> browser CSRF binding

    /// Delegated results, keyed by request id, filled by the onBeginResult/onExchangeResult
    /// slots and consumed by the waiting route handler (provider_entity mode only).
    QHash<QString, BeginOutcome> m_beginResults;
    QHash<QString, ExchangeOutcome> m_exchangeResults;
};

} // namespace SynQt

#endif // SYNQT_IDENTITYPROVIDER_H
