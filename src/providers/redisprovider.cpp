// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "redisprovider.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>

#include <hiredis/hiredis.h>

#ifdef SYNQT_HAVE_HIREDIS_SSL
#  include <hiredis/hiredis_ssl.h>
#  include <openssl/ssl.h>
#  include <openssl/x509v3.h>
#endif

#include <memory>
#include <utility>
#include <vector>

namespace SynQt {

namespace {

// hiredis TLS is in the separate hiredis_ssl library; CMake defines this when its header
// exists. Without it the provider cannot secure a link and refuses an exposed one.
#ifdef SYNQT_HAVE_HIREDIS_SSL
constexpr bool kTlsSupported{true};
#else
constexpr bool kTlsSupported{false};
#endif

// Run one command through the binary-safe argv form so keys/values may hold any bytes.
redisReply *runCommand(redisContext *context, const QList<QByteArray> &args)
{
    std::vector<const char *> argv;
    std::vector<size_t> argvLen;
    argv.reserve(args.size());
    argvLen.reserve(args.size());
    for (const QByteArray &arg : args) {
        argv.push_back(arg.constData());
        argvLen.push_back(static_cast<size_t>(arg.size()));
    }
    return static_cast<redisReply *>(
        redisCommandArgv(context, static_cast<int>(args.size()), argv.data(), argvLen.data()));
}

// A value as it is stored: its JSON text, so a number, an object or a list reads back as
// what was set, as it does from the memory provider. JSON for an integer is its decimal
// digits, which is what INCRBY counts on. A document holds an array or an object and a value
// is often neither, so the value is written as the one element of an array and the brackets
// are dropped.
QByteArray encodedValue(const QVariant &value)
{
    const QByteArray wrapped{QJsonDocument{QJsonArray{QJsonValue::fromVariant(value)}}.toJson(
        QJsonDocument::Compact)};
    return wrapped.mid(1, wrapped.size() - 2);
}

// The reverse. Text that is not JSON (written by another client, or by a build that stored
// values as text) is returned as that text.
QVariant decodedValue(const QByteArray &stored)
{
    QJsonParseError error{};
    const QJsonDocument document{QJsonDocument::fromJson('[' + stored + ']', &error)};
    if (error.error != QJsonParseError::NoError || document.array().size() != 1) {
        return QString::fromUtf8(stored);
    }
    return document.array().first().toVariant();
}

} // namespace

RedisCacheProvider::RedisCacheProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

RedisCacheProvider::~RedisCacheProvider()
{
    disconnect();
}

QString RedisCacheProvider::name() const
{
    return QStringLiteral("redis");
}

bool RedisCacheProvider::refusesInsecure() const
{
    // An unencrypted off-host cache link is refused in release; only dev on localhost may
    // relax it. Without hiredis_ssl there is no TLS, so every off-host release link is
    // refused.
    return m_config.release && !m_config.isLoopbackHost() && (!m_config.tls || !kTlsSupported);
}

// Wrap the open connection in TLS, or fail. Redis upgrades the socket right after connect
// (no in-protocol STARTTLS), and nothing has been sent yet, so the AUTH below goes out
// encrypted.
//
// The OpenSSL context is built here, not with redisCreateSSLContext, which verifies the
// chain but not the name: hiredis sends the host as SNI and never checks it against the
// certificate, and without `ca_cert` every public CA is trusted. The peer is verified
// against `ca_cert` when named, the system trust store otherwise, and against the
// configured host: an address against the certificate's IP entries, a name against its DNS
// entries.
bool RedisCacheProvider::startTls(QString *error)
{
#ifdef SYNQT_HAVE_HIREDIS_SSL
    const auto fail{[this, error](const QString &reason) {
        if (error != nullptr) {
            // Name the fix: the usual cause is a server that does not speak TLS, which for
            // a local development engine is declared in the config with `tls: false`.
            *error = QStringLiteral(
                "Redis TLS handshake with %1 failed: %2. A server that offers no TLS needs "
                "tls: false, and only a development engine on this machine may have it")
                         .arg(m_config.host, reason);
        }
        return false;
    }};

    redisInitOpenSSL();
    const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{
        SSL_CTX_new(TLS_client_method()), &SSL_CTX_free};
    if (context == nullptr) {
        return fail(QStringLiteral("no TLS context could be created"));
    }
    SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    const QByteArray caCert{m_config.caCert.toUtf8()};
    const int anchored{caCert.isEmpty()
                           ? SSL_CTX_set_default_verify_paths(context.get())
                           : SSL_CTX_load_verify_locations(context.get(), caCert.constData(),
                                                           nullptr)};
    if (anchored != 1) {
        return fail(QStringLiteral("the CA at %1 could not be read").arg(m_config.caCert));
    }

    QByteArray host{m_config.host.toUtf8()};
    X509_VERIFY_PARAM *verify{SSL_CTX_get0_param(context.get())};
    X509_VERIFY_PARAM_set_hostflags(verify, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    // set1_ip_asc accepts only an address literal, so it doubles as the test for one.
    const bool isAddress{X509_VERIFY_PARAM_set1_ip_asc(verify, host.constData()) == 1};
    if (!isAddress
        && X509_VERIFY_PARAM_set1_host(verify, host.constData(),
                                       static_cast<size_t>(host.size())) != 1) {
        return fail(QStringLiteral("the host name cannot be checked against a certificate"));
    }

    SSL *ssl{SSL_new(context.get())};
    if (ssl == nullptr) {
        return fail(QStringLiteral("no TLS session could be created"));
    }
    // SNI names a host, never an address. SSL_ctrl is what SSL_set_tlsext_host_name expands
    // to, called directly to avoid that macro's cast.
    if (!isAddress) {
        SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, host.data());
    }
    // On success the connection owns and frees the session; on failure it does not, so the
    // result is read before the session is freed.
    if (redisInitiateSSL(m_context, ssl) != REDIS_OK) {
        const long verdict{SSL_get_verify_result(ssl)};
        const QString reason{verdict != X509_V_OK
                                 ? QString::fromUtf8(X509_verify_cert_error_string(verdict))
                                 : QString::fromUtf8(m_context->errstr)};
        SSL_free(ssl);
        return fail(reason);
    }
    return true;
#else
    if (error != nullptr) {
        *error = QStringLiteral(
            "this build has no Redis TLS: SynQt was compiled without hiredis_ssl, so a "
            "connection to %1 could only be plaintext. Install hiredis_ssl, or write "
            "tls: false for a development engine on this machine (see "
            "https://synqt.org/providers/)").arg(m_config.host);
    }
    return false;
#endif
}

bool RedisCacheProvider::connect(QString *error)
{
    if (refusesInsecure()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "refusing an unverified connection to %1 in release: Redis TLS requires "
                "hiredis_ssl and a verified CA (see docs/security.md)").arg(m_config.host);
        }
        return false;
    }

    const timeval timeout{2, 0};
    m_context = redisConnectWithTimeout(m_config.host.toUtf8().constData(),
                                        m_config.port > 0 ? m_config.port : 6379, timeout);
    if (m_context == nullptr || m_context->err != 0) {
        if (error != nullptr) {
            *error = m_context != nullptr ? QString::fromUtf8(m_context->errstr)
                                          : QStringLiteral("out of memory connecting to Redis");
        }
        disconnect();
        return false;
    }

    // The connect timeout applies to every command. hiredis is synchronous and runs on the
    // entity's event loop, so a server that stops answering would block the entity. After
    // two seconds the command fails, the provider reports a miss, and the entity keeps
    // answering.
    if (redisSetTimeout(m_context, timeout) != REDIS_OK) {
        if (error != nullptr) {
            *error = QStringLiteral("could not bound Redis commands: %1")
                         .arg(QString::fromUtf8(m_context->errstr));
        }
        disconnect();
        return false;
    }

    // `tls: true` means TLS or nothing on every link, including dev loopback, so a
    // production config is exercised in development.
    if (m_config.tls && !startTls(error)) {
        disconnect();
        return false;
    }

    if (!m_config.password.isEmpty()) {
        QList<QByteArray> auth{QByteArrayLiteral("AUTH")};
        if (!m_config.user.isEmpty()) {
            auth.append(m_config.user.toUtf8());
        }
        auth.append(m_config.password.toUtf8());  // from the entity env only. Never logged
        redisReply *reply{runCommand(m_context, auth)};
        const bool ok{reply != nullptr && reply->type != REDIS_REPLY_ERROR};
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
        if (!ok) {
            if (error != nullptr) {
                *error = QStringLiteral("Redis authentication failed");
            }
            disconnect();
            return false;
        }
    }
    return true;
}

void RedisCacheProvider::disconnect()
{
    if (m_context != nullptr) {
        redisFree(m_context);
        m_context = nullptr;
    }
}

bool RedisCacheProvider::isHealthy() const
{
    return m_context != nullptr && m_context->err == 0;
}

QVariant RedisCacheProvider::get(const QString &key)
{
    if (m_context == nullptr) {
        return QVariant{};
    }
    redisReply *reply{runCommand(m_context, {QByteArrayLiteral("GET"), key.toUtf8()})};
    QVariant value;
    if (reply != nullptr && reply->type == REDIS_REPLY_STRING) {
        value = decodedValue(QByteArray{reply->str, static_cast<qsizetype>(reply->len)});
    }
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return value;  // invalid on a miss (NIL) or error, matching the memory provider
}

void RedisCacheProvider::set(const QString &key, const QVariant &value, int ttlSeconds)
{
    if (m_context == nullptr) {
        return;
    }
    QList<QByteArray> command;
    if (ttlSeconds > 0) {
        command = {QByteArrayLiteral("SETEX"), key.toUtf8(),
                   QByteArray::number(ttlSeconds), encodedValue(value)};
    } else {
        command = {QByteArrayLiteral("SET"), key.toUtf8(), encodedValue(value)};
    }
    redisReply *reply{runCommand(m_context, command)};
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

void RedisCacheProvider::del(const QString &key)
{
    if (m_context == nullptr) {
        return;
    }
    redisReply *reply{runCommand(m_context, {QByteArrayLiteral("DEL"), key.toUtf8()})};
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

qint64 RedisCacheProvider::incr(const QString &key, qint64 by)
{
    if (m_context == nullptr) {
        return 0;
    }
    redisReply *reply{runCommand(
        m_context, {QByteArrayLiteral("INCRBY"), key.toUtf8(), QByteArray::number(by)})};
    qint64 result{0};
    if (reply != nullptr && reply->type == REDIS_REPLY_INTEGER) {
        result = static_cast<qint64>(reply->integer);
    }
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
    return result;
}

void RedisCacheProvider::expire(const QString &key, int ttlSeconds)
{
    if (m_context == nullptr) {
        return;
    }
    // A TTL of zero or less means no expiry in this family, as on `set`, and PERSIST is how
    // Redis says it. `EXPIRE key 0` would delete the key, so the same QML would keep a
    // value with the memory provider and drop it with Redis.
    const QList<QByteArray> command{
        ttlSeconds > 0
            ? QList<QByteArray>{QByteArrayLiteral("EXPIRE"), key.toUtf8(),
                                QByteArray::number(ttlSeconds)}
            : QList<QByteArray>{QByteArrayLiteral("PERSIST"), key.toUtf8()}};
    redisReply *reply{runCommand(m_context, command)};
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

} // namespace SynQt
