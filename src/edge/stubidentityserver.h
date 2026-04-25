// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_STUBIDENTITYSERVER_H
#define SYNQT_STUBIDENTITYSERVER_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantMap>

#include <string>

QT_BEGIN_NAMESPACE
class QHttpServer;
class QHttpServerRequest;
class QHttpServerResponse;
class QTcpServer;
QT_END_NAMESPACE

namespace SynQt {

/// A dev-only OpenID Connect / OAuth2 provider, so a developer can exercise the whole
/// login flow without registering a real OAuth app. It authenticates a preconfigured user
/// with no password prompt. It is for `synqt dev` and tests ONLY and must never ship: it
/// refuses to start unless the caller passes the explicit dev acknowledgement, and the
/// runtime also refuses a devStub provider entry in a shipped edge (see IdentityProvider).
///
/// It serves /authorize (redirects back with a code, after asking which of the configured
/// people you are when there is more than one), /token (verifies the client secret and the
/// PKCE S256 verifier, issues tokens and a signed ID token), /userinfo (Bearer-guarded
/// profile), and /jwks (the ID-token signing key).
///
/// Nothing else about the login is faked. The state, the PKCE challenge, the code
/// exchange, the ID-token signature check against the JWKS, the mapping hook, the session
/// and its cookie are the ones a real provider's login goes through, which is the point:
/// what runs under `synqt dev` is the shipped flow with a stand-in at one end of it.
class StubIdentityServer : public QObject
{
    Q_OBJECT

public:
    /// A guard type that can only be constructed here, so a caller must write the intent.
    struct DevOnly { explicit DevOnly() = default; };

    explicit StubIdentityServer(DevOnly acknowledgement, QObject *parent = nullptr);
    ~StubIdentityServer() override;

    void setClientCredentials(const QString &clientId, const QString &clientSecret);
    void setUser(const QVariantMap &user);  ///< the profile /userinfo returns. Forgets the rest
    /// Offer one more person to sign in as. With two or more, /authorize asks which.
    ///
    /// A dev sign-in is worth having because an app's scopes are worth exercising, and a
    /// scope is what the mapping hook returns for an identity. So the way to reach a scope
    /// here is to configure somebody the project's own hook maps there, which keeps the
    /// hook on the path rather than handing out a scope beside it.
    void addUser(const QVariantMap &user);
    int userCount() const;
    void setIssuer(const QString &issuer);   ///< iss for the ID token

    /// Leave a claim out of the ID tokens this stub signs ("exp", "sub").
    ///
    /// A provider that omits a required claim is precisely what the ID-token verifier is
    /// there to refuse, and the only way to produce a validly signed token that is missing
    /// one is for the signer to leave it out. Mutating the payload of a good token breaks
    /// the signature, so the verifier would refuse it a step earlier and prove nothing.
    /// This widens no production surface (the stub is a fake provider that a shipped edge
    /// already refuses to run). It only lets the fake misbehave the way a real one can.
    void omitIdTokenClaim(const QString &claim);

    /// Answer a refresh without naming how long the new token lasts.
    ///
    /// The same idea as omitIdTokenClaim, for the other half of what a provider answers.
    /// `expires_in` is RECOMMENDED and not REQUIRED by RFC 6749 section 5.1, so a provider
    /// that leaves it out is conforming, and what the edge does with an entry whose expiry
    /// nobody named is worth being able to drive. Only the refresh answer, because the
    /// exchange answer is what a test needs in order to get a first sweep at all.
    void setRefreshOmitsExpiry(bool omits);

    bool start(quint16 port = 0);
    quint16 port() const;
    QString baseUrl() const;                  // http://127.0.0.1:<port>

private:
    QHttpServerResponse handleAuthorize(const QHttpServerRequest &request);
    QHttpServerResponse handleToken(const QHttpServerRequest &request);
    QHttpServerResponse handleUserinfo(const QHttpServerRequest &request);
    QHttpServerResponse handleJwks(const QHttpServerRequest &request);

    struct PendingCode
    {
        QString codeChallenge;
        QString nonce;
        int user{0};  ///< which of m_users the browser picked
    };

    /// The page /authorize serves when more than one person is configured: one link per
    /// user, back to this same request with the choice on it.
    QHttpServerResponse chooser(const QHttpServerRequest &request) const;
    /// Redirect back to the caller with a fresh code for `user`.
    QHttpServerResponse grant(const QHttpServerRequest &request, int user);

    QHttpServer *m_server{nullptr};
    QTcpServer *m_tcp{nullptr};
    quint16 m_port{0};
    QString m_clientId{QStringLiteral("stub-client")};
    QString m_clientSecret{QStringLiteral("stub-secret")};
    QList<QVariantMap> m_users;               ///< at least one; /authorize asks past the first
    QString m_issuer;
    QHash<QString, PendingCode> m_codes;      ///< code -> PKCE challenge + nonce + user
    QHash<QString, int> m_accessTokens;       ///< access token -> user
    QHash<QString, int> m_refreshTokens;      ///< refresh token -> user (for the refresh grant)

    /// RSA signing material for the ID token, generated at start(). n/e feed the JWKS.
    void ensureKeys();
    std::string signIdToken(const QString &nonce, const QVariantMap &user) const;
    std::string m_publicKeyPem;
    std::string m_privateKeyPem;
    QString m_kid;
    /// Claims left out of a signed ID token, so the fake can misbehave (omitIdTokenClaim).
    QSet<QString> m_omittedClaims;
    /// Whether a refresh answer names a lifetime (setRefreshOmitsExpiry).
    bool m_refreshOmitsExpiry{false};
    QString m_jwkModulus;  ///< base64url
    QString m_jwkExponent; ///< base64url
};

} // namespace SynQt

#endif // SYNQT_STUBIDENTITYSERVER_H
