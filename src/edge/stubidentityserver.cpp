// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "stubidentityserver.h"

#include <QCryptographicHash>
#include <QFuture>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QPromise>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>

#include <jwt-cpp/jwt.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <memory>
#include <system_error>

namespace SynQt {

namespace {

/// How /authorize is told which of the configured people the browser picked.
const QString kUserParameter{QStringLiteral("synqt_user")};

QString randomId()
{
    return QString::fromLatin1(
        QByteArray::number(QRandomGenerator::system()->generate64(), 16));
}

QString base64Url(const QByteArray &data)
{
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::string bioToString(BIO *bio)
{
    BUF_MEM *mem{nullptr};
    BIO_get_mem_ptr(bio, &mem);
    return std::string{mem->data, mem->length};
}

QString bigNumBase64Url(const BIGNUM *value)
{
    QByteArray bytes(BN_num_bytes(value), Qt::Uninitialized);
    BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data()));
    return base64Url(bytes);
}

} // namespace

StubIdentityServer::StubIdentityServer(DevOnly, QObject *parent)
    : QObject{parent}
{
    m_users.append(QVariantMap{{QStringLiteral("id"), 1001},
                               {QStringLiteral("login"), QStringLiteral("octocat")},
                               {QStringLiteral("name"), QStringLiteral("The Octocat")},
                               {QStringLiteral("email"),
                                QStringLiteral("octocat@example.com")}});
}

StubIdentityServer::~StubIdentityServer() = default;

void StubIdentityServer::setClientCredentials(const QString &clientId, const QString &clientSecret)
{
    m_clientId = clientId;
    m_clientSecret = clientSecret;
}

void StubIdentityServer::setUser(const QVariantMap &user)
{
    m_users = {user};
}

void StubIdentityServer::addUser(const QVariantMap &user)
{
    m_users.append(user);
}

int StubIdentityServer::userCount() const
{
    return static_cast<int>(m_users.size());
}

void StubIdentityServer::setIssuer(const QString &issuer)
{
    m_issuer = issuer;
}

quint16 StubIdentityServer::port() const
{
    return m_port;
}

QString StubIdentityServer::baseUrl() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
}

void StubIdentityServer::ensureKeys()
{
    if (!m_publicKeyPem.empty()) {
        return;
    }
    EVP_PKEY *pkey{EVP_RSA_gen(2048)};
    if (!pkey) {
        return;
    }
    BIO *publicBio{BIO_new(BIO_s_mem())};
    PEM_write_bio_PUBKEY(publicBio, pkey);
    m_publicKeyPem = bioToString(publicBio);
    BIO_free(publicBio);

    BIO *privateBio{BIO_new(BIO_s_mem())};
    PEM_write_bio_PrivateKey(privateBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    m_privateKeyPem = bioToString(privateBio);
    BIO_free(privateBio);

    BIGNUM *modulus{nullptr};
    BIGNUM *exponent{nullptr};
    EVP_PKEY_get_bn_param(pkey, "n", &modulus);
    EVP_PKEY_get_bn_param(pkey, "e", &exponent);
    m_jwkModulus = bigNumBase64Url(modulus);
    m_jwkExponent = bigNumBase64Url(exponent);
    BN_free(modulus);
    BN_free(exponent);
    EVP_PKEY_free(pkey);

    m_kid = QStringLiteral("stub-key-1");
}

namespace {

/// `email_verified` as the person states it: a boolean, or the text some providers send.
/// A person who says nothing is one the developer configured, so verified.
jwt::claim verifiedClaim(const QVariantMap &user)
{
    const QVariant stated{user.value(QStringLiteral("email_verified"))};
    if (!stated.isValid()) {
        return jwt::claim(picojson::value(true));
    }
    if (stated.typeId() == QMetaType::Bool) {
        return jwt::claim(picojson::value(stated.toBool()));
    }
    return jwt::claim(stated.toString().toStdString());
}

} // namespace

std::string StubIdentityServer::signIdToken(const QString &nonce,
                                            const QVariantMap &user) const
{
    auto builder{jwt::create()};
    picojson::array audiences{picojson::value{m_clientId.toStdString()}};
    for (const QString &audience : m_extraAudiences) {
        audiences.emplace_back(audience.toStdString());
    }
    builder.set_issuer(m_issuer.toStdString())
        .set_issued_at(std::chrono::system_clock::now())
        .set_key_id(m_kid.toStdString())
        .set_payload_claim("email",
            jwt::claim(user.value(QStringLiteral("email")).toString().toStdString()))
        .set_payload_claim("name",
            jwt::claim(user.value(QStringLiteral("name")).toString().toStdString()))
        .set_payload_claim("preferred_username",
            jwt::claim(user.value(QStringLiteral("login")).toString().toStdString()));
    // Both are required claims, added here so a test can make the stub omit one
    // (omitIdTokenClaim).
    if (!m_omittedClaims.contains(QStringLiteral("sub"))) {
        // `sub` from a configured user, or `id` from a GitHub-shaped profile.
        const QVariant subject{user.contains(QStringLiteral("sub"))
                                   ? user.value(QStringLiteral("sub"))
                                   : user.value(QStringLiteral("id"))};
        builder.set_subject(subject.toString().toStdString());
    }
    if (audiences.size() == 1) {
        builder.set_audience(audiences.front().get<std::string>());
    } else {
        builder.set_audience(audiences);
    }
    if (!m_authorizedParty.isEmpty()) {
        builder.set_payload_claim("azp", jwt::claim(m_authorizedParty.toStdString()));
    }
    if (!m_omittedClaims.contains(QStringLiteral("email_verified"))) {
        builder.set_payload_claim("email_verified", verifiedClaim(user));
    }
    if (!m_omittedClaims.contains(QStringLiteral("exp"))) {
        builder.set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(3600));
    }
    if (!nonce.isEmpty()) {
        builder.set_payload_claim("nonce", jwt::claim(nonce.toStdString()));
    }
    std::error_code ec;
    const std::string token{
        builder.sign(jwt::algorithm::rs256(m_publicKeyPem, m_privateKeyPem, "", ""), ec)};
    return ec ? std::string{} : token;
}

void StubIdentityServer::setTokenDelayMs(int milliseconds)
{
    m_tokenDelayMs = milliseconds;
}

void StubIdentityServer::setRefreshOmitsExpiry(bool omits)
{
    m_refreshOmitsExpiry = omits;
}

void StubIdentityServer::omitIdTokenClaim(const QString &claim)
{
    m_omittedClaims.insert(claim);
}

void StubIdentityServer::restoreIdTokenClaim(const QString &claim)
{
    m_omittedClaims.remove(claim);
}

void StubIdentityServer::setExtraAudiences(const QStringList &audiences)
{
    m_extraAudiences = audiences;
}

void StubIdentityServer::setAuthorizedParty(const QString &party)
{
    m_authorizedParty = party;
}

void StubIdentityServer::setPublishedKeyUse(const QString &use, const QString &alg)
{
    m_keyUse = use;
    m_keyAlg = alg;
}

bool StubIdentityServer::start(quint16 port)
{
    ensureKeys();
    m_server = new QHttpServer{this};
    m_server->route(QStringLiteral("/authorize"), [this](const QHttpServerRequest &request) {
        return handleAuthorize(request);
    });
    m_server->route(QStringLiteral("/token"),
                    [this](const QHttpServerRequest &request) -> QFuture<QHttpServerResponse> {
        // Computed now, delivered after the delay; only the timing changes.
        auto promise{std::make_shared<QPromise<QHttpServerResponse>>()};
        QFuture<QHttpServerResponse> future{promise->future()};
        promise->start();
        auto answer{std::make_shared<QHttpServerResponse>(handleToken(request))};
        const auto deliver{[promise, answer]() {
            promise->addResult(std::move(*answer));
            promise->finish();
        }};
        if (m_tokenDelayMs <= 0) {
            deliver();
        } else {
            QTimer::singleShot(m_tokenDelayMs, this, deliver);
        }
        return future;
    });
    m_server->route(QStringLiteral("/userinfo"), [this](const QHttpServerRequest &request) {
        return handleUserinfo(request);
    });
    m_server->route(QStringLiteral("/jwks"), [this](const QHttpServerRequest &request) {
        return handleJwks(request);
    });

    m_tcp = new QTcpServer{this};
    if (!m_tcp->listen(QHostAddress::LocalHost, port)) {
        return false;
    }
    m_port = m_tcp->serverPort();
    if (m_issuer.isEmpty()) {
        m_issuer = baseUrl();
    }
    return m_server->bind(m_tcp);
}

QHttpServerResponse StubIdentityServer::handleAuthorize(const QHttpServerRequest &request)
{
    // A real provider authenticates the user here. With one configured person the stub
    // approves and redirects back; with several it asks which.
    const QUrlQuery query{request.url().query()};
    const QString picked{query.queryItemValue(kUserParameter)};
    if (m_users.size() > 1 && picked.isEmpty()) {
        return chooser(request);
    }
    bool numeric{false};
    const int index{picked.toInt(&numeric)};
    return grant(request, (numeric && index >= 0 && index < m_users.size()) ? index : 0);
}

QHttpServerResponse StubIdentityServer::chooser(const QHttpServerRequest &request) const
{
    // Every value here comes from the project configuration and is escaped anyway.
    QString body{QStringLiteral(
        "<!doctype html><meta charset=\"utf-8\">"
        "<title>Sign in (development)</title>"
        "<style>body{font:16px system-ui;margin:3rem auto;max-width:28rem}"
        "a{display:block;padding:.75rem 1rem;margin:.5rem 0;border:1px solid #ccc;"
        "border-radius:.5rem;text-decoration:none;color:inherit}"
        "small{color:#666}</style>"
        "<h1>Sign in</h1>"
        "<p><small>The development sign-in. Nothing here exists outside "
        "<code>synqt dev</code>.</small></p>")};
    for (qsizetype index{0}; index < m_users.size(); ++index) {
        const QVariantMap user{m_users.at(index)};
        QUrl choice{request.url()};
        QUrlQuery query{choice.query()};
        query.removeAllQueryItems(kUserParameter);
        query.addQueryItem(kUserParameter, QString::number(index));
        choice.setQuery(query);
        const QString label{user.value(QStringLiteral("name")).toString().isEmpty()
                                ? user.value(QStringLiteral("login")).toString()
                                : user.value(QStringLiteral("name")).toString()};
        body += QStringLiteral("<a href=\"%1\">%2<br><small>%3</small></a>")
                    .arg(choice.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                         label.toHtmlEscaped(),
                         user.value(QStringLiteral("email")).toString().toHtmlEscaped());
    }
    return QHttpServerResponse{QByteArrayLiteral("text/html; charset=utf-8"),
                               body.toUtf8()};
}

QHttpServerResponse StubIdentityServer::grant(const QHttpServerRequest &request, int user)
{
    const QUrlQuery query{request.url().query()};
    const QString redirectUri{query.queryItemValue(QStringLiteral("redirect_uri"),
                                                    QUrl::FullyDecoded)};
    const QString state{query.queryItemValue(QStringLiteral("state"))};

    const QString code{randomId()};
    PendingCode pending;
    pending.codeChallenge = query.queryItemValue(QStringLiteral("code_challenge"));
    pending.nonce = query.queryItemValue(QStringLiteral("nonce"));
    pending.user = user;
    m_codes.insert(code, pending);

    QUrl location{redirectUri};
    QUrlQuery back;
    back.addQueryItem(QStringLiteral("code"), code);
    if (!state.isEmpty()) {
        back.addQueryItem(QStringLiteral("state"), state);
    }
    location.setQuery(back);

    QHttpServerResponse response{QHttpServerResponse::StatusCode::Found};
    QHttpHeaders headers{response.headers()};
    headers.append(QHttpHeaders::WellKnownHeader::Location,
                   location.toString(QUrl::FullyEncoded).toUtf8());
    response.setHeaders(std::move(headers));
    return response;
}

QHttpServerResponse StubIdentityServer::handleToken(const QHttpServerRequest &request)
{
    const QUrlQuery form{QString::fromUtf8(request.body())};
    const QString grantType{form.queryItemValue(QStringLiteral("grant_type"))};
    const QString clientSecret{form.queryItemValue(QStringLiteral("client_secret"))};

    // RFC 6749 section 6: the refresh grant issues a new access token (and rotates the
    // refresh token) server-side, without a browser or authorization code. The client
    // secret is still required.
    if (grantType == QLatin1String("refresh_token")) {
        const QString presented{form.queryItemValue(QStringLiteral("refresh_token"))};
        if (clientSecret != m_clientSecret) {
            return QHttpServerResponse{QByteArrayLiteral("application/json"),
                                       QByteArrayLiteral("{\"error\":\"invalid_client\"}"),
                                       QHttpServerResponse::StatusCode::Unauthorized};
        }
        if (!m_refreshTokens.contains(presented)) {
            return QHttpServerResponse{QByteArrayLiteral("application/json"),
                                       QByteArrayLiteral("{\"error\":\"invalid_grant\"}"),
                                       QHttpServerResponse::StatusCode::BadRequest};
        }
        const int user{m_refreshTokens.take(presented)};  // rotate: the old one is spent
        const QString accessToken{randomId() + randomId()};
        const QString rotatedRefresh{randomId()};
        m_accessTokens.insert(accessToken, user);
        m_refreshTokens.insert(rotatedRefresh, user);

        QJsonObject refreshed;
        refreshed.insert(QStringLiteral("access_token"), accessToken);
        refreshed.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
        if (!m_refreshOmitsExpiry) {
            refreshed.insert(QStringLiteral("expires_in"), 3600);
        }
        refreshed.insert(QStringLiteral("refresh_token"), rotatedRefresh);
        return QHttpServerResponse{QJsonObject{refreshed}};
    }

    const QString code{form.queryItemValue(QStringLiteral("code"))};
    const QString verifier{form.queryItemValue(QStringLiteral("code_verifier"))};

    if (!m_codes.contains(code)) {
        return QHttpServerResponse{QByteArrayLiteral("application/json"),
                                   QByteArrayLiteral("{\"error\":\"invalid_grant\"}"),
                                   QHttpServerResponse::StatusCode::BadRequest};
    }
    const PendingCode pending{m_codes.take(code)};

    // Verify the client secret (edge-held) and the PKCE S256 verifier.
    if (clientSecret != m_clientSecret) {
        return QHttpServerResponse{QByteArrayLiteral("application/json"),
                                   QByteArrayLiteral("{\"error\":\"invalid_client\"}"),
                                   QHttpServerResponse::StatusCode::Unauthorized};
    }
    if (!pending.codeChallenge.isEmpty()) {
        const QString computed{
            base64Url(QCryptographicHash::hash(verifier.toUtf8(), QCryptographicHash::Sha256))};
        if (computed != pending.codeChallenge) {
            return QHttpServerResponse{
                QByteArrayLiteral("application/json"),
                QByteArrayLiteral(R"({"error":"invalid_grant","error_description":"PKCE"})"),
                QHttpServerResponse::StatusCode::BadRequest};
        }
    }

    const QString accessToken{randomId() + randomId()};
    const QString refreshToken{randomId()};
    m_accessTokens.insert(accessToken, pending.user);
    m_refreshTokens.insert(refreshToken, pending.user);

    QJsonObject tokens;
    tokens.insert(QStringLiteral("access_token"), accessToken);
    tokens.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
    tokens.insert(QStringLiteral("expires_in"), 3600);
    tokens.insert(QStringLiteral("refresh_token"), refreshToken);
    const std::string idToken{signIdToken(pending.nonce, m_users.at(pending.user))};
    if (!idToken.empty()) {
        tokens.insert(QStringLiteral("id_token"), QString::fromStdString(idToken));
    }
    return QHttpServerResponse{QJsonObject{tokens}};
}

QHttpServerResponse StubIdentityServer::handleUserinfo(const QHttpServerRequest &request)
{
    const QByteArray authorization{request.value("Authorization")};
    const QByteArray prefix{QByteArrayLiteral("Bearer ")};
    const QString presented{QString::fromUtf8(authorization.mid(prefix.size()))};
    if (!authorization.startsWith(prefix) || !m_accessTokens.contains(presented)) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::Unauthorized};
    }
    return QHttpServerResponse{
        QJsonObject::fromVariantMap(m_users.at(m_accessTokens.value(presented)))};
}

QHttpServerResponse StubIdentityServer::handleJwks(const QHttpServerRequest &)
{
    QJsonObject key{{QStringLiteral("kty"), QStringLiteral("RSA")},
                    {QStringLiteral("kid"), m_kid},
                    {QStringLiteral("n"), m_jwkModulus},
                    {QStringLiteral("e"), m_jwkExponent}};
    if (!m_keyUse.isEmpty()) {
        key.insert(QStringLiteral("use"), m_keyUse);
    }
    if (!m_keyAlg.isEmpty()) {
        key.insert(QStringLiteral("alg"), m_keyAlg);
    }
    return QHttpServerResponse{QJsonObject{{QStringLiteral("keys"), QJsonArray{key}}}};
}

} // namespace SynQt
