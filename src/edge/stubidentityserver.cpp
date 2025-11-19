// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "stubidentityserver.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QUrlQuery>

#include <jwt-cpp/jwt.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <system_error>

namespace SynQt {

namespace {

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
    m_user = QVariantMap{{QStringLiteral("id"), 1001},
                         {QStringLiteral("login"), QStringLiteral("octocat")},
                         {QStringLiteral("name"), QStringLiteral("The Octocat")},
                         {QStringLiteral("email"), QStringLiteral("octocat@example.com")}};
}

StubIdentityServer::~StubIdentityServer() = default;

void StubIdentityServer::setClientCredentials(const QString &clientId, const QString &clientSecret)
{
    m_clientId = clientId;
    m_clientSecret = clientSecret;
}

void StubIdentityServer::setUser(const QVariantMap &user)
{
    m_user = user;
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

std::string StubIdentityServer::signIdToken(const QString &nonce) const
{
    auto builder{jwt::create()};
    builder.set_issuer(m_issuer.toStdString())
        .set_subject(m_user.value(QStringLiteral("id")).toString().toStdString())
        .set_audience(m_clientId.toStdString())
        .set_issued_at(std::chrono::system_clock::now())
        .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(3600))
        .set_key_id(m_kid.toStdString())
        .set_payload_claim("email",
            jwt::claim(m_user.value(QStringLiteral("email")).toString().toStdString()))
        .set_payload_claim("name",
            jwt::claim(m_user.value(QStringLiteral("name")).toString().toStdString()))
        .set_payload_claim("preferred_username",
            jwt::claim(m_user.value(QStringLiteral("login")).toString().toStdString()));
    if (!nonce.isEmpty()) {
        builder.set_payload_claim("nonce", jwt::claim(nonce.toStdString()));
    }
    std::error_code ec;
    const std::string token{
        builder.sign(jwt::algorithm::rs256(m_publicKeyPem, m_privateKeyPem, "", ""), ec)};
    return ec ? std::string{} : token;
}

bool StubIdentityServer::start(quint16 port)
{
    ensureKeys();
    m_server = new QHttpServer{this};
    m_server->route(QStringLiteral("/authorize"), [this](const QHttpServerRequest &request) {
        return handleAuthorize(request);
    });
    m_server->route(QStringLiteral("/token"), [this](const QHttpServerRequest &request) {
        return handleToken(request);
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
    // A real provider authenticates the user here; the stub approves the preconfigured
    // user immediately and redirects back with an authorization code.
    const QUrlQuery query{request.url().query()};
    const QString redirectUri{query.queryItemValue(QStringLiteral("redirect_uri"),
                                                    QUrl::FullyDecoded)};
    const QString state{query.queryItemValue(QStringLiteral("state"))};

    const QString code{randomId()};
    PendingCode pending;
    pending.codeChallenge = query.queryItemValue(QStringLiteral("code_challenge"));
    pending.nonce = query.queryItemValue(QStringLiteral("nonce"));
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

    // RFC 6749 section 6: the refresh grant issues a fresh access token (and rotates the refresh
    // token) server-side, with no browser and no authorization code. The client secret is
    // still required.
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
        const QString subject{m_refreshTokens.take(presented)};  // rotate: the old one is spent
        const QString accessToken{randomId() + randomId()};
        const QString rotatedRefresh{randomId()};
        m_accessTokens.insert(accessToken, subject);
        m_refreshTokens.insert(rotatedRefresh, subject);

        QJsonObject refreshed;
        refreshed.insert(QStringLiteral("access_token"), accessToken);
        refreshed.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
        refreshed.insert(QStringLiteral("expires_in"), 3600);
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

    const QString subject{m_user.value(QStringLiteral("login")).toString()};
    const QString accessToken{randomId() + randomId()};
    const QString refreshToken{randomId()};
    m_accessTokens.insert(accessToken, subject);
    m_refreshTokens.insert(refreshToken, subject);

    QJsonObject tokens;
    tokens.insert(QStringLiteral("access_token"), accessToken);
    tokens.insert(QStringLiteral("token_type"), QStringLiteral("Bearer"));
    tokens.insert(QStringLiteral("expires_in"), 3600);
    tokens.insert(QStringLiteral("refresh_token"), refreshToken);
    const std::string idToken{signIdToken(pending.nonce)};
    if (!idToken.empty()) {
        tokens.insert(QStringLiteral("id_token"), QString::fromStdString(idToken));
    }
    return QHttpServerResponse{QJsonObject{tokens}};
}

QHttpServerResponse StubIdentityServer::handleUserinfo(const QHttpServerRequest &request)
{
    const QByteArray authorization{request.value("Authorization")};
    const QByteArray prefix{QByteArrayLiteral("Bearer ")};
    if (!authorization.startsWith(prefix)
        || !m_accessTokens.contains(QString::fromUtf8(authorization.mid(prefix.size())))) {
        return QHttpServerResponse{QHttpServerResponse::StatusCode::Unauthorized};
    }
    return QHttpServerResponse{QJsonObject::fromVariantMap(m_user)};
}

QHttpServerResponse StubIdentityServer::handleJwks(const QHttpServerRequest &)
{
    QJsonObject key{{QStringLiteral("kty"), QStringLiteral("RSA")},
                    {QStringLiteral("use"), QStringLiteral("sig")},
                    {QStringLiteral("alg"), QStringLiteral("RS256")},
                    {QStringLiteral("kid"), m_kid},
                    {QStringLiteral("n"), m_jwkModulus},
                    {QStringLiteral("e"), m_jwkExponent}};
    return QHttpServerResponse{QJsonObject{{QStringLiteral("keys"), QJsonArray{key}}}};
}

} // namespace SynQt
