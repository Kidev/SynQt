// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYCONFIG_H
#define SYNQT_IDENTITYCONFIG_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace SynQt {

/// Whether an identity endpoint may be spoken to at all.
///
/// Every one of these URLs carries something that must not be readable in transit or
/// forgeable on the way back: the token endpoint carries the client secret and returns
/// the tokens, the JWKS endpoint returns the keys every ID token is then trusted against
/// (fetch those over http and anyone on the path chooses who your users are), and the
/// authorize endpoint is where the browser is sent. So https is required, with one
/// exception: a loopback host, which is the dev stub provider and cannot be reached from
/// another machine. `synqt check` reports the same rule before anything runs.
inline bool isSecureIdentityEndpoint(const QUrl &url)
{
    if (url.scheme() == QLatin1String("https")) {
        return true;
    }
    const QString host{url.host()};
    return url.scheme() == QLatin1String("http")
        && (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1")
            || host == QLatin1String("::1") || host == QLatin1String("[::1]"));
}

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
    /// Whether an unauthenticated browser is refused at the upgrade is
    /// `WebEdgeConfig::identityRequired`, which is where the check that reads it lives.
    /// It is deliberately not repeated here: two fields for one decision is a way for the
    /// generated edge to set the one nothing reads.
    QString providerEntity;        ///< empty: in-process at the edge

    QString loginRoute{QStringLiteral("/auth/login")};
    QString callbackRoute{QStringLiteral("/auth/callback")};
    QString logoutRoute{QStringLiteral("/auth/logout")};

    QString mappingHook;           ///< web/identity/map.qml (optional)
    QString appRoute{QStringLiteral("/")}; ///< where to send the browser after login

    QList<IdentityProviderConfig> providers;

    /// Server-side access-token refresh (see "Session lifecycle" in
    /// [Authentication](https://synqt.org/authentication/)).
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
