// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Development-only, and this is the guard that says so at compile time rather than at link
// time. It sits above every #include so that a translation unit that reaches here in a
// release build fails naming the mistake rather than on whichever Qt header it could
// not find afterwards.
#ifndef SYNQT_DEV_TOOLS
#error "stubidentityserver.h is development-only. It is compiled into SynQtEdge only when CMake is configured with -DSYNQT_DEV_TOOLS=ON, which `synqt dev` does and `synqt build` never does. If you are reading this from a release build, something is including a development header: fix the include rather than turning the option on."
#endif

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

/// A dev-only OpenID Connect / OAuth2 provider for exercising the login flow without a real
/// OAuth app. It authenticates a preconfigured user with no password. For `synqt dev` and
/// tests only: it refuses to start without the explicit dev acknowledgement, and a shipped
/// edge refuses a devStub provider entry (see IdentityProvider).
///
/// It serves /authorize (asking which configured person you are when there are several),
/// /token (checks the client secret and the PKCE S256 verifier, issues tokens and a signed ID
/// token), /userinfo and /jwks. Everything else in the login is the shipped flow.
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

    /// Leave a claim out of the ID tokens this stub signs ("exp", "sub", "email_verified"), to
    /// drive the refusal of a validly signed token missing a claim.
    void omitIdTokenClaim(const QString &claim);
    /// Sign `claim` again after omitIdTokenClaim.
    void restoreIdTokenClaim(const QString &claim);

    /// Answer a refresh without `expires_in`, which RFC 6749 section 5.1 only recommends.
    void setRefreshOmitsExpiry(bool omits);

    /// Answer the token exchange this much later than it is ready.
    ///
    /// A real provider takes a round trip, and what an entity does while it waits is the
    /// thing to be able to see. A slot that blocks on the exchange holds its
    /// entity's event loop, and only a provider that is slow on purpose can show whether
    /// the entity kept serving in the meantime. The answer itself is unchanged.
    void setTokenDelayMs(int milliseconds);

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
    /// How long /token sits on a ready answer (setTokenDelayMs).
    int m_tokenDelayMs{0};
    QString m_jwkModulus;  ///< base64url
    QString m_jwkExponent; ///< base64url
};

} // namespace SynQt

#endif // SYNQT_STUBIDENTITYSERVER_H
