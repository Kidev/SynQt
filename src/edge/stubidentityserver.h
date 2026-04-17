// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_STUBIDENTITYSERVER_H
#define SYNQT_STUBIDENTITYSERVER_H

#include <QHash>
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
/// It serves /authorize (immediately redirects back with a code), /token (verifies the
/// client secret and the PKCE S256 verifier, issues tokens and a signed ID token),
/// /userinfo (Bearer-guarded profile), and /jwks (the ID-token signing key).
class StubIdentityServer : public QObject
{
    Q_OBJECT

public:
    /// A guard type that can only be constructed here, so a caller must write the intent.
    struct DevOnly { explicit DevOnly() = default; };

    explicit StubIdentityServer(DevOnly acknowledgement, QObject *parent = nullptr);
    ~StubIdentityServer() override;

    void setClientCredentials(const QString &clientId, const QString &clientSecret);
    void setUser(const QVariantMap &user);  ///< the profile /userinfo returns
    void setIssuer(const QString &issuer);   ///< iss for the ID token

    /// Leave a claim out of the ID tokens this stub signs ("exp", "sub").
    ///
    /// A provider that omits a required claim is precisely what the ID-token verifier is
    /// there to refuse, and the only way to produce a validly signed token that is missing
    /// one is for the signer to leave it out: mutating the payload of a good token breaks
    /// the signature, so the verifier would refuse it a step earlier and prove nothing.
    /// This widens no production surface (the stub is a fake provider that a shipped edge
    /// already refuses to run); it only lets the fake misbehave the way a real one can.
    void omitIdTokenClaim(const QString &claim);

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
    };

    QHttpServer *m_server{nullptr};
    QTcpServer *m_tcp{nullptr};
    quint16 m_port{0};
    QString m_clientId{QStringLiteral("stub-client")};
    QString m_clientSecret{QStringLiteral("stub-secret")};
    QVariantMap m_user;
    QString m_issuer;
    QHash<QString, PendingCode> m_codes;      ///< code -> PKCE challenge + nonce
    QHash<QString, QString> m_accessTokens;   ///< access token -> subject
    QHash<QString, QString> m_refreshTokens;  ///< refresh token -> subject (for the refresh grant)

    /// RSA signing material for the ID token, generated at start(). n/e feed the JWKS.
    void ensureKeys();
    std::string signIdToken(const QString &nonce) const;
    std::string m_publicKeyPem;
    std::string m_privateKeyPem;
    QString m_kid;
    /// Claims left out of a signed ID token, so the fake can misbehave (omitIdTokenClaim).
    QSet<QString> m_omittedClaims;
    QString m_jwkModulus;  ///< base64url
    QString m_jwkExponent; ///< base64url
};

} // namespace SynQt

#endif // SYNQT_STUBIDENTITYSERVER_H
