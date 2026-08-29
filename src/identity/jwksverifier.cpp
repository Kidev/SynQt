// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "jwksverifier.h"

#include "boundedreply.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <jwt-cpp/jwt.h>

#include <system_error>

namespace SynQt {

namespace {

/// The minimum interval between refetches of one provider's key set. Key rotation is rare;
/// tokens naming unknown keys are not, so they must not trigger requests at will.
constexpr qint64 kMinRefetchMs{5 * 60 * 1000};

/// The largest key set this holds. A JWKS is a few public keys; a megabyte is far above any
/// real one and well below what parsing may cost.
constexpr qint64 kMaxJwksBytes{1024 * 1024};

/// How many key set fetches may be in flight at once, so a slow provider cannot turn a
/// queue of callbacks into unbounded open replies.
constexpr int kMaxConcurrentFetches{16};

/// How long one fetch may take.
constexpr int kFetchTimeoutMs{15000};

QByteArray decodeBase64Url(const QString &segment)
{
    return QByteArray::fromBase64(segment.toUtf8(), QByteArray::Base64UrlEncoding);
}

QJsonObject jsonSegment(const QString &segment)
{
    return QJsonDocument::fromJson(decodeBase64Url(segment)).object();
}

// The JWK whose kid matches, or the only key when the token has no kid.
//
// With several keys and no kid there is no answer: during a rotation a provider publishes
// two keys, and taking the first would make verification depend on the list order. The
// failure names the ambiguity instead of reporting an invalid signature.
QJsonObject selectKey(const QByteArray &jwks, const QString &kid)
{
    // Copy-init, not brace-init: QJsonArray{anArray} would wrap the array as one element.
    const QJsonArray keys =
        QJsonDocument::fromJson(jwks).object().value(QStringLiteral("keys")).toArray();
    if (kid.isEmpty()) {
        return keys.size() == 1 ? keys.first().toObject() : QJsonObject{};
    }
    for (qsizetype i{0}; i < keys.size(); ++i) {
        const QJsonObject key{keys.at(i).toObject()};
        if (key.value(QStringLiteral("kid")).toString() == kid) {
            return key;
        }
    }
    return {};
}

bool audienceMatches(const QJsonValue &aud, const QString &expected)
{
    if (aud.isString()) {
        return aud.toString() == expected;
    }
    if (aud.isArray()) {
        const QJsonArray values = aud.toArray();  // copy-init (see selectKey)
        for (qsizetype i{0}; i < values.size(); ++i) {
            if (values.at(i).toString() == expected) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

JwksVerifier::JwksVerifier(QNetworkAccessManager *network, QObject *parent)
    : QObject{parent}
    , m_network{network}
{
}

void JwksVerifier::fetchJwks(const QUrl &jwksUrl, bool force, FetchCallback done)
{
    const auto cached{m_jwksCache.constFind(jwksUrl.toString())};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    if (cached != m_jwksCache.constEnd()
        && (!force || now - cached->fetchedMs < kMinRefetchMs)) {
        // Cached, and either fresh or refetched too recently. The rate limit stops tokens
        // naming unknown keys from triggering provider requests.
        done(!force, QString{});
        return;
    }
    if (!isSecureIdentityEndpoint(jwksUrl)) {
        // These keys authenticate every ID token; over http, anyone on the path could
        // choose your users.
        done(false, QStringLiteral("refusing to fetch JWKS over a plaintext connection"));
        return;
    }
    if (m_fetching >= kMaxConcurrentFetches) {
        done(false, QStringLiteral("too many JWKS fetches are already waiting"));
        return;
    }
    ++m_fetching;

    QNetworkReply *reply{m_network->get(QNetworkRequest{jwksUrl})};
    // The size ceiling, checked as the body arrives. This endpoint is worth attacking, and
    // a document this large is not a key set, so it is not read to the end.
    refuseAnswersLargerThan(reply, kMaxJwksBytes);
    // The deadline. An abandoned reply carries no error, so the timer marks it before
    // aborting and the handler reads the mark. Otherwise a partial body could be cached as
    // the key set with `fetchedMs` set to now, and the refetch floor would refuse every
    // login for five minutes.
    QTimer *deadline{new QTimer{reply}};
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("synqtTimedOut", true);
        reply->abort();
    });
    deadline->start(kFetchTimeoutMs);

    // Freed whatever happens to this verifier: the handler is bound to `this` and goes with
    // it, and an undeleted reply would stay on the network manager.
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this, [this, reply, jwksUrl, done]() {
        --m_fetching;
        if (reply->property("synqtTimedOut").toBool()) {
            done(false, QStringLiteral("JWKS fetch timed out"));
            return;
        }
        if (reply->property("synqtTooLarge").toBool()) {
            done(false, QStringLiteral("JWKS response is larger than a key set can be"));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done(false, QStringLiteral("JWKS fetch failed: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray body{reply->readAll()};
        // A key set without keys is refused; caching it would block the refetch for five
        // minutes, as a timeout would.
        if (QJsonDocument::fromJson(body).object().value(QStringLiteral("keys")).toArray()
                .isEmpty()) {
            done(false, QStringLiteral("JWKS response carried no keys"));
            return;
        }
        m_jwksCache.insert(jwksUrl.toString(),
                           CachedJwks{body, QDateTime::currentMSecsSinceEpoch()});
        done(true, QString{});
    });
}

void JwksVerifier::verifyAsync(const QString &idToken, const IdentityProviderConfig &provider,
                               const QString &expectedNonce, VerifyCallback done)
{
    // A JWT is three non-empty base64url segments. header.payload.signature.
    const QStringList parts{idToken.split(QLatin1Char('.'))};
    if (parts.size() != 3 || parts.at(0).isEmpty() || parts.at(1).isEmpty()
        || parts.at(2).isEmpty()) {
        done({}, QStringLiteral("malformed ID token"));
        return;
    }

    const QJsonObject header{jsonSegment(parts.at(0))};
    if (header.value(QStringLiteral("alg")).toString() != QLatin1String("RS256")) {
        done({}, QStringLiteral("unsupported ID-token algorithm"));
        return;
    }
    const QString kid{header.value(QStringLiteral("kid")).toString()};
    const QString cacheKey{provider.jwksUrl.toString()};
    const auto missingKey{[kid]() {
        return kid.isEmpty()
                   ? QStringLiteral("the ID token names no signing key and the provider "
                                    "publishes more than one, so which key signed it "
                                    "cannot be told")
                   : QStringLiteral("no signing key in the JWKS matches this ID token's "
                                    "kid");
    }};
    const auto answer{[this, parts, provider, expectedNonce, done](const QJsonObject &jwk) {
        QString error;
        const QVariantMap claims{checkToken(parts, jwk, provider, expectedNonce, &error)};
        done(claims, error);
    }};

    fetchJwks(provider.jwksUrl, false,
              [this, cacheKey, kid, provider, missingKey, answer, done](
                  bool ok, const QString &error) {
        if (!ok) {
            done({}, error);
            return;
        }
        const QJsonObject jwk{selectKey(m_jwksCache.value(cacheKey).json, kid)};
        if (!jwk.isEmpty()) {
            answer(jwk);
            return;
        }
        // The cached set lacks this token's key, usually because of a rotation. Fetch once
        // more (rate-limited in fetchJwks) and look again, or the first rotation would
        // break every login until a restart.
        fetchJwks(provider.jwksUrl, true,
                  [this, cacheKey, kid, missingKey, answer, done](bool refreshed,
                                                                  const QString &) {
            const QJsonObject again{refreshed
                                        ? selectKey(m_jwksCache.value(cacheKey).json, kid)
                                        : QJsonObject{}};
            if (again.isEmpty()) {
                done({}, missingKey());
                return;
            }
            answer(again);
        });
    });
}

QVariantMap JwksVerifier::checkToken(const QStringList &parts, const QJsonObject &jwk,
                                     const IdentityProviderConfig &provider,
                                     const QString &expectedNonce, QString *error) const
{
    const auto fail{[error](const QString &message) -> QVariantMap {
        if (error) {
            *error = message;
        }
        return {};
    }};
    if (jwk.value(QStringLiteral("kty")).toString() != QLatin1String("RSA")) {
        return fail(QStringLiteral("the ID token's signing key is not RSA"));
    }
    // A key that states its purpose must state this one (RFC 7517 sections 4.2 and 4.4): a
    // key published to encrypt, or for another algorithm, does not vouch for an ID token.
    if (jwk.contains(QStringLiteral("use"))
        && jwk.value(QStringLiteral("use")).toString() != QLatin1String("sig")) {
        return fail(QStringLiteral("the ID token's key is not published for signatures"));
    }
    if (jwk.contains(QStringLiteral("alg"))
        && jwk.value(QStringLiteral("alg")).toString() != QLatin1String("RS256")) {
        return fail(QStringLiteral("the ID token's key is not published for RS256"));
    }

    // Build the RSA public key from the JWK modulus and exponent, and verify the RS256
    // signature over the exact signing input (base64url header "." base64url payload).
    std::error_code ec;
    const std::string pem{jwt::helper::create_public_key_from_rsa_components(
        jwk.value(QStringLiteral("n")).toString().toStdString(),
        jwk.value(QStringLiteral("e")).toString().toStdString(), ec)};
    if (ec) {
        return fail(QStringLiteral("could not build signing key: %1")
                        .arg(QString::fromStdString(ec.message())));
    }

    const std::string signingInput{(parts.at(0) + QLatin1Char('.') + parts.at(1)).toStdString()};
    const QByteArray signature{decodeBase64Url(parts.at(2))};
    const jwt::algorithm::rs256 algorithm{pem, "", "", ""};
    algorithm.verify(signingInput,
                     std::string{signature.constData(),
                                 static_cast<size_t>(signature.size())},
                     ec);
    if (ec) {
        return fail(QStringLiteral("ID-token signature invalid"));
    }

    // Claim checks (parsed with Qt, so a missing claim never throws).
    const QJsonObject payload{jsonSegment(parts.at(1))};
    if (!provider.issuer.isEmpty()
        && payload.value(QStringLiteral("iss")).toString() != provider.issuer) {
        return fail(QStringLiteral("ID-token issuer mismatch"));
    }
    const QString audience{provider.audience.isEmpty() ? provider.clientId : provider.audience};
    if (!audienceMatches(payload.value(QStringLiteral("aud")), audience)) {
        return fail(QStringLiteral("ID-token audience mismatch"));
    }
    // OpenID Connect Core 3.1.3.7: a token for several audiences names the party it was
    // issued to, and an `azp` present must be this client. Otherwise a token another
    // relying party asked for, listing this one too, would sign the visitor in here.
    const QJsonValue audiences{payload.value(QStringLiteral("aud"))};
    const bool several{audiences.isArray() && audiences.toArray().size() > 1};
    const QJsonValue party{payload.value(QStringLiteral("azp"))};
    if ((several || !party.isUndefined()) && party.toString() != provider.clientId) {
        return fail(QStringLiteral("ID-token authorized party is not this client"));
    }
    // `exp` is required by OpenID Connect and required here. A token without one has no
    // bounded lifetime, so a copy would sign in forever. The 60 seconds cover clock
    // skew with the provider.
    const qint64 now{QDateTime::currentSecsSinceEpoch()};
    const QJsonValue expiry{payload.value(QStringLiteral("exp"))};
    if (!expiry.isDouble()) {
        return fail(QStringLiteral("ID token carries no expiry"));
    }
    if (static_cast<qint64>(expiry.toDouble()) + 60 < now) {
        return fail(QStringLiteral("ID token expired"));
    }
    // Every session is keyed on the subject (the scope mapping, device enrolment). A token
    // without one would sign everyone in as the same nobody.
    if (payload.value(QStringLiteral("sub")).toString().isEmpty()) {
        return fail(QStringLiteral("ID token carries no subject"));
    }
    if (!expectedNonce.isEmpty()
        && payload.value(QStringLiteral("nonce")).toString() != expectedNonce) {
        return fail(QStringLiteral("ID-token nonce mismatch"));
    }

    return payload.toVariantMap();
}

} // namespace SynQt
