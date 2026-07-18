// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYSERVICE_H
#define SYNQT_IDENTITYSERVICE_H

#include "identityconfig.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace SynQt {

class OAuthBackend;

/// The auth-entity half of easy auth (docs/authentication.md "Where identity runs"). It owns
/// the OAuthBackend (the client secret, the token exchange, ID-token verification, the
/// stored tokens and their server-side refresh) and exposes it to the Identity connect
/// point Source over the mesh. The edges consuming that connect point hold no secret and no
/// token; they only drive login/callback and issue the session cookie.
///
/// The begin/exchange methods are synchronous (the backend runs a bounded nested loop for the
/// token exchange), so the per_peer Source can emit each result on itself and answer only the
/// edge that asked; a user's identity never crosses to another edge.
class IdentityService : public QObject
{
    Q_OBJECT

public:
    explicit IdentityService(const IdentityConfig &config, QObject *parent = nullptr);
    ~IdentityService() override;

    /// Build the authorization URL for a provider (PKCE + state held here). Returns
    /// { state, authorizeUrl, error }.
    Q_INVOKABLE QVariantMap beginLogin(const QString &provider, const QString &redirectUri);

    /// Exchange the returned code for tokens (secret + verifier), verify and normalize the
    /// identity, and store the tokens under the state key. Returns { identityJson, error };
    /// the tokens never leave this entity.
    Q_INVOKABLE QVariantMap exchangeCode(const QString &state, const QString &code,
                                         const QString &redirectUri);

    /// Move the tokens from the temporary state key to the stable session id.
    Q_INVOKABLE void bindSession(const QString &state, const QString &sessionId);

    /// Drop the tokens for a session (logout / expiry).
    Q_INVOKABLE void releaseSession(const QString &sessionId);

    OAuthBackend *backend() const;

private:
    OAuthBackend *m_backend;
};

} // namespace SynQt

#endif // SYNQT_IDENTITYSERVICE_H
