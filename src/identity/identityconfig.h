// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IDENTITYCONFIG_H
#define SYNQT_IDENTITYCONFIG_H

#include "providerconfig.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace SynQt {

/// Whether an identity endpoint may be spoken to at all: https only, since these endpoints
/// carry the client secret, the tokens and the keys ID tokens are trusted against. The one
/// exception is a loopback host (the dev stub provider). `synqt check` applies the same rule.
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
/// A template owns how raw provider fields normalize into the identity object. Here that
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
    QString audience;              ///< expected aud claim (OIDC). Defaults to clientId

    /// Normalization: which raw fields feed each identity field (userinfo path). Defaults
    /// suit the generic OAuth2 template. The GitHub template maps the numeric id to sub and
    /// falls back to the primary verified address from the emails endpoint.
    QString subField{QStringLiteral("id")};
    QString loginField{QStringLiteral("login")};
    QString nameField{QStringLiteral("name")};
    QString emailField{QStringLiteral("email")};
    QUrl emailsUrl;                ///< GitHub-style fallback for a private email

    /// A dev-only stub provider (issued by `synqt dev`). It must never run in a shipped
    /// edge. The runtime refuses it unless the dev gate is explicitly enabled.
    bool devStub{false};
};

/// How strongly the OS store binds a desktop client's device credential to this machine, OS
/// user and application. Ordered, so a configured minimum is a floor.
///
/// The client reports its own level, so `min_binding` sets fleet policy and stops no attacker:
/// a patched client can claim Hardware. SynQt does not verify key attestation.
enum class DeviceBinding {
    None = 0,        ///< nothing can be persisted on this machine
    User = 1,        ///< at rest under the OS user. Any process running as them can read it
    Application = 2, ///< also bound to this application's code signature (signed macOS build)
    Hardware = 3     ///< a non-exportable key in a secure element (Secure Enclave, TPM 2.0)
};

inline QString deviceBindingName(DeviceBinding binding)
{
    switch (binding) {
    case DeviceBinding::None:
        return QStringLiteral("none");
    case DeviceBinding::User:
        return QStringLiteral("user");
    case DeviceBinding::Application:
        return QStringLiteral("application");
    case DeviceBinding::Hardware:
        return QStringLiteral("hardware");
    }
    return QStringLiteral("none");
}

/// The level named, or None when the name is not one of the four. An unrecognized name is
/// the weakest level rather than the strongest. This parses a value a client sent, and a
/// typo must never read as a stronger claim than the client made.
inline DeviceBinding deviceBindingFromName(const QString &name)
{
    if (name == QLatin1String("user")) {
        return DeviceBinding::User;
    }
    if (name == QLatin1String("application")) {
        return DeviceBinding::Application;
    }
    if (name == QLatin1String("hardware")) {
        return DeviceBinding::Hardware;
    }
    return DeviceBinding::None;
}

/// Staying signed in on the desktop across relaunches (`identity.desktop_session: device`).
/// The client persists a credential redeemable once, at one route, for a fresh session of the
/// ordinary length, never the session itself.
struct DeviceConfig
{
    /// Off unless the project opted in. Nothing is persisted, and a desktop visitor signs in
    /// once per launch, which is what a project that says nothing gets.
    bool enabled{false};

    int lifetimeDays{30};   ///< absolute: a family is refused this long after it was enrolled
    int inactivityDays{14}; ///< a family unused this long is refused

    /// How long the generation a redemption retired may still be presented, for a client that
    /// lost its connection before storing the answer. Past this window, the same presentation
    /// means two copies exist, and the family is revoked.
    int overlapSeconds{120};

    /// The weakest store a client may enrol from. A client below it keeps the session it
    /// signed in for and persists nothing, which is exactly what `desktop_session:
    /// memory` does. It is never a failure to sign in.
    DeviceBinding minBinding{DeviceBinding::User};

    /// Where the family table lives. Any persistence provider, because a multi-edge
    /// deployment needs a credential enrolled through one edge to redeem at another. A
    /// single edge can point it at a file of its own.
    ProviderConfig store;
};

/// The edge's identity configuration. By default identity runs in process on the edge;
/// provider_entity promotes it to a dedicated auth entity the edges consume over the mesh.
struct IdentityConfig
{
    bool enabled{false};
    /// Whether an unauthenticated browser is refused at the upgrade is
    /// `WebEdgeConfig::identityRequired`, which is where the check that reads it lives.
    /// It is not repeated here. Two fields for one decision is a way for the
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

    /// Only true under `synqt dev`. Gates the dev stub provider so it can never ship.
    bool allowDevStub{false};

    /// Whether a login may hand its answer back over a loopback redirect, as a native desktop
    /// client signs in (see [Desktop](https://synqt.org/desktop/)). Set by the generated edge only
    /// when a client entity lists the `desktop` target, since `?return=http://127.0.0.1:<port>/`
    /// is a link anyone can be sent.
    bool allowDesktopLogin{false};

    /// How long a desktop claim code stands for its session, in seconds. Clamped to [1, 300]
    /// where it is read. Not a `synqt.yaml` key.
    int claimTtlSeconds{60};

    /// Staying signed in across relaunches on the desktop. Disabled unless the project asked
    /// for it, and inert on a project with no desktop client. Enrolment happens at the claim
    /// exchange, which only a native client ever reaches.
    DeviceConfig device;

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
