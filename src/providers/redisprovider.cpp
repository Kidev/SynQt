// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "redisprovider.h"

#include <QByteArray>
#include <QList>

#include <hiredis/hiredis.h>

#ifdef SYNQT_HAVE_HIREDIS_SSL
#  include <hiredis/ssl.h>
#endif

#include <utility>
#include <vector>

namespace SynQt {

namespace {

// hiredis TLS lives in the separate hiredis_ssl library; CMake sets this when its header
// is present. Without it the provider cannot secure a link and must refuse an exposed one.
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
    // Exposing an unencrypted cache link off-host in release is refused; only dev on
    // localhost may relax it. Without hiredis_ssl the provider cannot offer TLS at all, so
    // any off-host release link is refused.
    return m_config.release && !m_config.isLoopbackHost() && (!m_config.tls || !kTlsSupported);
}

// Wrap the open connection in TLS, or fail. Redis speaks TLS by upgrading the socket
// immediately after connect (there is no in-protocol STARTTLS), which is what
// redisInitiateSSLWithContext does; nothing has been sent on the wire before this runs, so
// the AUTH below is the first thing that goes out and it goes out encrypted.
//
// This exists because the config flag alone is not a setting: without it, `tls: true` was a
// claim the guard above believed while the socket stayed plaintext, and the cache password
// went out in the clear. The certificate is verified against `ca_cert` when one is named and
// against the system trust store otherwise; hiredis sets SSL_VERIFY_PEER either way, so a
// server this client cannot verify fails here rather than being trusted.
bool RedisCacheProvider::startTls(QString *error)
{
#ifdef SYNQT_HAVE_HIREDIS_SSL
    redisInitOpenSSL();
    const QByteArray caCert{m_config.caCert.toUtf8()};
    const QByteArray serverName{m_config.host.toUtf8()};
    redisSSLContextError contextError{REDIS_SSL_CTX_NONE};
    redisSSLContext *context{redisCreateSSLContext(
        caCert.isEmpty() ? nullptr : caCert.constData(), nullptr, nullptr, nullptr,
        serverName.isEmpty() ? nullptr : serverName.constData(), &contextError)};
    if (context == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("could not build the Redis TLS context: %1")
                         .arg(QString::fromUtf8(redisSSLContextGetError(contextError)));
        }
        return false;
    }
    const bool ok{redisInitiateSSLWithContext(m_context, context) == REDIS_OK};
    if (!ok && error != nullptr) {
        *error = QStringLiteral("Redis TLS handshake failed: %1")
                     .arg(QString::fromUtf8(m_context->errstr));
    }
    // The context is per connection here (one provider holds one connection), and the
    // connection keeps what it needs from it, so it is freed as soon as the handshake is
    // decided rather than held for a reconnect this provider does not do.
    redisFreeSSLContext(context);
    return ok;
#else
    if (error != nullptr) {
        *error = QStringLiteral(
            "this build has no Redis TLS: SynQt was compiled without hiredis_ssl, so a "
            "connection to %1 could only be plaintext (see https://synqt.org/providers/)")
                     .arg(m_config.host);
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

    // Asked for TLS means TLS or nothing, on every link and not only the ones the guard
    // above refuses: a dev loopback link that says `tls: true` and silently gets plaintext
    // is how a production config that means it ends up untested.
    if (m_config.tls && !startTls(error)) {
        disconnect();
        return false;
    }

    if (!m_config.password.isEmpty()) {
        QList<QByteArray> auth{QByteArrayLiteral("AUTH")};
        if (!m_config.user.isEmpty()) {
            auth.append(m_config.user.toUtf8());
        }
        auth.append(m_config.password.toUtf8());  // from the entity env only; never logged
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
        value = QString::fromUtf8(reply->str, static_cast<int>(reply->len));
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
                   QByteArray::number(ttlSeconds), value.toString().toUtf8()};
    } else {
        command = {QByteArrayLiteral("SET"), key.toUtf8(), value.toString().toUtf8()};
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
    redisReply *reply{runCommand(
        m_context, {QByteArrayLiteral("EXPIRE"), key.toUtf8(), QByteArray::number(ttlSeconds)})};
    if (reply != nullptr) {
        freeReplyObject(reply);
    }
}

} // namespace SynQt
