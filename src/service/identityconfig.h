// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYCONFIG_H
#define SYNQT_IDENTITYCONFIG_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace SynQt {

/// One configured OAuth2 / OpenID Connect provider. The client_secret is resolved from
/// the edge environment only (never a literal in synqt.yaml, never in a client target).
/// A template owns how raw provider fields normalize into the identity object; here that
/// is expressed as the field names to read (OAuth2 userinfo) or the OIDC ID-token path.
struct IdentityProviderConfig
{
    QString name;
    QUrl authorizeUrl;
    QUrl tokenUrl;
    QUrl userinfoUrl;              ///< OAuth2 profile endpoint (empty for pure OIDC)
    QStringList scopes;

    QString clientId;
    QString clientSecret;          ///< resolved from env: only

    /// OpenID Connect: when true, identity comes from the ID token, whose signature is
    /// verified against the provider JWKS. Otherwise identity comes from the userinfo JSON.
    bool useIdToken{false};
    QUrl jwksUrl;                  ///< provider signing keys (OIDC)
    QString issuer;                ///< expected iss claim (OIDC)
    QString audience;              ///< expected aud claim (OIDC); defaults to clientId

    /// Normalization: which raw fields feed each identity field (userinfo path). Defaults
    /// suit the generic OAuth2 template; the GitHub template maps the numeric id to sub and
    /// falls back to the primary verified address from the emails endpoint.
    QString subField{QStringLiteral("id")};
    QString loginField{QStringLiteral("login")};
    QString nameField{QStringLiteral("name")};
    QString emailField{QStringLiteral("email")};
    QUrl emailsUrl;                ///< GitHub-style fallback for a private email

    /// A dev-only stub provider (issued by `synqt dev`); it must never run in a shipped
    /// edge. The runtime refuses it unless the dev gate is explicitly enabled.
    bool devStub{false};
};

/// The edge's identity configuration. By default identity runs in process on the edge;
/// provider_entity promotes it to a dedicated auth entity the edges consume over the mesh.
struct IdentityConfig
{
    bool enabled{false};
    bool required{false};          ///< an unauthenticated browser cannot acquire scoped CPs
    QString providerEntity;        ///< empty: in-process at the edge

    QString loginRoute{QStringLiteral("/auth/login")};
    QString callbackRoute{QStringLiteral("/auth/callback")};
    QString logoutRoute{QStringLiteral("/auth/logout")};

    QString mappingHook;           ///< web/identity/map.qml (optional)
    QString appRoute{QStringLiteral("/")}; ///< where to send the browser after login

    QList<IdentityProviderConfig> providers;

    /// Server-side access-token refresh (docs/authentication.md "Session lifecycle": Refresh).
    /// Every `refreshIntervalSeconds` the engine refreshes any token within
    /// `refreshMarginSeconds` of expiry, using its refresh token, without the browser. A
    /// non-positive interval disables the periodic sweep.
    int refreshIntervalSeconds{60};
    int refreshMarginSeconds{120};

    /// Only true under `synqt dev`; gates the dev stub provider so it can never ship.
    bool allowDevStub{false};

    const IdentityProviderConfig *provider(const QString &name) const
    {
        for (const IdentityProviderConfig &candidate : providers) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    }
};

} // namespace SynQt

#endif // SYNQT_IDENTITYCONFIG_H
