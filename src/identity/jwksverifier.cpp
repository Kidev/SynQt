// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "jwksverifier.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTimer>

#include <jwt-cpp/jwt.h>

#include <system_error>

namespace SynQt {

namespace {

/// The floor on refetching one provider's key set. A rotation is a rare event and a
/// stream of tokens naming keys that do not exist is not, so the refetch a rotation needs
/// must not be a request an unverified token can ask for at will.
constexpr qint64 kMinRefetchMs{5 * 60 * 1000};

/// How large a key set may be before this refuses to hold it. A JWKS is a handful of public
/// keys. A megabyte is orders of magnitude above the largest real one and well below what
/// the process can spend on a document it is about to parse as JSON.
constexpr qint64 kMaxJwksBytes{1024 * 1024};

/// How deep a JWKS fetch may nest inside another one.
///
/// The wait below is a nested event loop, which keeps serving requests while it spins, so a
/// second callback arriving during a fetch runs its own exchange inside this stack frame.
/// The identity routes bound their own nesting for exactly this reason
/// (kMaxConcurrentWaits in identityprovider.cpp); this is the same bound for the one wait
/// that sits below them, so a provider that goes slow cannot turn a queue of callbacks into
/// a stack that runs out.
constexpr int kMaxNestedFetches{16};

QByteArray decodeBase64Url(const QString &segment)
{
    return QByteArray::fromBase64(segment.toUtf8(), QByteArray::Base64UrlEncoding);
}

QJsonObject jsonSegment(const QString &segment)
{
    return QJsonDocument::fromJson(decodeBase64Url(segment)).object();
}

// The JWK whose kid matches, or the sole key when the token carries no kid.
//
// "Sole" is the whole no-kid case. Returning the first key of however many there are
// is not the same thing. A provider publishes two keys
// for the length of a rotation, and a token with no kid would then verify or not depending
// on which of them the provider happened to list first. Failing is the correct answer, and
// failing with a reason that names the ambiguity beats failing with "signature invalid",
// which sends whoever is reading the log looking for the wrong thing.
QJsonObject selectKey(const QByteArray &jwks, const QString &kid)
{
    // Copy-init, not brace-init: QJsonArray{anArray} would wrap the array as a single
    // element (its initializer_list is of QJsonValue), not copy it.
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

bool JwksVerifier::ensureJwks(const QUrl &jwksUrl, QString *error, bool force)
{
    const auto cached{m_jwksCache.constFind(jwksUrl.toString())};
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    if (cached != m_jwksCache.constEnd()
        && (!force || now - cached->fetchedMs < kMinRefetchMs)) {
        // Held, and either good enough or refetched too recently to try again. The rate
        // limit is what stops a stream of tokens naming keys that do not exist from
        // turning into a stream of requests to the provider.
        return !force;
    }
    if (!isSecureIdentityEndpoint(jwksUrl)) {
        // The keys every ID token is trusted against; over http, whoever is on the path
        // chooses who your users are.
        if (error) {
            *error = QStringLiteral("refusing to fetch JWKS over a plaintext connection");
        }
        return false;
    }
    if (m_fetching >= kMaxNestedFetches) {
        if (error) {
            *error = QStringLiteral("too many JWKS fetches are already waiting");
        }
        return false;
    }
    ++m_fetching;
    const auto released{qScopeGuard([this]() { --m_fetching; })};

    QNetworkReply *reply{m_network->get(QNetworkRequest{jwksUrl})};
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // The size ceiling, checked as the body arrives. These are the keys every ID token is
    // trusted against, so the endpoint is one an attacker would like to control; a document
    // this size is not a key set whatever it is, and reading it to the end to find that out
    // is the part worth refusing.
    connect(reply, &QNetworkReply::downloadProgress, &loop,
            [reply, &loop](qint64 received, qint64 total) {
        if (received > kMaxJwksBytes || total > kMaxJwksBytes) {
            reply->abort();
            loop.quit();
        }
    });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
    // isFinished() before error(), and this is the whole of the deadline. A reply the
    // timer above walked out on has no error on it yet, so asking error() alone reads a
    // half-arrived body as a good one, and this is the one place that would then be
    // cached as the key set, with `fetchedMs` set to now, which the refetch floor holds
    // for five minutes. A provider that went slow once would refuse every login for the
    // rest of that window.
    if (!reply->isFinished()) {
        reply->abort();
        if (error) {
            *error = QStringLiteral("JWKS fetch timed out");
        }
        reply->deleteLater();
        return false;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (error) {
            *error = QStringLiteral("JWKS fetch failed: %1").arg(reply->errorString());
        }
        reply->deleteLater();
        return false;
    }
    const QByteArray body{reply->readAll()};
    reply->deleteLater();
    // A key set with no keys in it is not a key set. Caching one would put the refetch
    // floor in front of the real answer for five minutes, exactly as a timeout would.
    if (QJsonDocument::fromJson(body).object().value(QStringLiteral("keys")).toArray()
            .isEmpty()) {
        if (error) {
            *error = QStringLiteral("JWKS response carried no keys");
        }
        return false;
    }
    m_jwksCache.insert(jwksUrl.toString(), CachedJwks{body, now});
    return true;
}

QVariantMap JwksVerifier::verify(const QString &idToken, const IdentityProviderConfig &provider,
                                 const QString &expectedNonce, QString *error)
{
    const auto fail{[error](const QString &message) -> QVariantMap {
        if (error) {
            *error = message;
        }
        return {};
    }};

    // A JWT is three non-empty base64url segments: header.payload.signature.
    const QStringList parts{idToken.split(QLatin1Char('.'))};
    if (parts.size() != 3 || parts.at(0).isEmpty() || parts.at(1).isEmpty()
        || parts.at(2).isEmpty()) {
        return fail(QStringLiteral("malformed ID token"));
    }

    const QJsonObject header{jsonSegment(parts.at(0))};
    if (header.value(QStringLiteral("alg")).toString() != QLatin1String("RS256")) {
        return fail(QStringLiteral("unsupported ID-token algorithm"));
    }

    if (!ensureJwks(provider.jwksUrl, error)) {
        return {};
    }
    const QString kid{header.value(QStringLiteral("kid")).toString()};
    QJsonObject jwk{selectKey(m_jwksCache.value(provider.jwksUrl.toString()).json, kid)};
    if (jwk.isEmpty() && ensureJwks(provider.jwksUrl, error, true)) {
        // The key set on hand does not contain this token's key. The ordinary reason is a
        // rotation: the provider signed with a key it published after this set was
        // fetched. Fetch once more (rate limited inside ensureJwks) and look again, or the
        // first rotation would end every login until the edge restarts.
        jwk = selectKey(m_jwksCache.value(provider.jwksUrl.toString()).json, kid);
    }
    if (jwk.isEmpty()) {
        return fail(kid.isEmpty()
                        ? QStringLiteral("the ID token names no signing key and the provider "
                                         "publishes more than one, so which key signed it "
                                         "cannot be told")
                        : QStringLiteral("no signing key in the JWKS matches this ID token's "
                                         "kid"));
    }
    if (jwk.value(QStringLiteral("kty")).toString() != QLatin1String("RSA")) {
        return fail(QStringLiteral("the ID token's signing key is not RSA"));
    }

    // Build the RSA public key from the JWK modulus/exponent and verify the RS256
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
    // `exp` is required by OpenID Connect and required here, rather than checked only
    // when present. A token that carries none is not a token that never expires. It is a
    // token whose lifetime nothing bounds, and accepting it means a copy taken today is
    // still a valid sign-in years from now. The 60 seconds is clock skew between this
    // edge and the provider, and nothing more.
    const qint64 now{QDateTime::currentSecsSinceEpoch()};
    const QJsonValue expiry{payload.value(QStringLiteral("exp"))};
    if (!expiry.isDouble()) {
        return fail(QStringLiteral("ID token carries no expiry"));
    }
    if (static_cast<qint64>(expiry.toDouble()) + 60 < now) {
        return fail(QStringLiteral("ID token expired"));
    }
    // A subject is what the whole session is keyed on downstream (the scope mapping reads
    // it, and a device credential is enrolled against it). A token with none would sign
    // somebody in as nobody, and every such visitor would be the same nobody.
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
