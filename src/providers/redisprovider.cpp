// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "redisprovider.h"

#include <QByteArray>
#include <QList>

#include <hiredis/hiredis.h>

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
