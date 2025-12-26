// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "identityservice.h"

#include "oauthbackend.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace SynQt {

namespace {

QString identityToJson(const QVariantMap &identity)
{
    if (identity.isEmpty()) {
        return QString{};
    }
    return QString::fromUtf8(
        QJsonDocument{QJsonObject::fromVariantMap(identity)}.toJson(QJsonDocument::Compact));
}

} // namespace

IdentityService::IdentityService(const IdentityConfig &config, QObject *parent)
    : QObject{parent}
    , m_backend{new OAuthBackend{config, this}}
    , m_claimTtlMs{claimTtlMsFrom(config.claimTtlSeconds)}
{
    // The auth entity is the single place tokens live, so it runs the refresh sweep.
    m_backend->setAutoRefresh(config.refreshIntervalSeconds, config.refreshMarginSeconds);
}

IdentityService::~IdentityService() = default;

OAuthBackend *IdentityService::backend() const
{
    return m_backend;
}

QVariantMap IdentityService::beginLogin(const QString &provider, const QString &redirectUri,
                                        const QString &binding, const QString &context)
{
    const OAuthBackend::BeginResult result{
        m_backend->begin(provider, redirectUri, binding, context)};
    return QVariantMap{
        {QStringLiteral("state"), result.state},
        {QStringLiteral("authorizeUrl"), result.authorizeUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("error"), result.error}};
}

QVariantMap IdentityService::exchangeCode(const QString &state, const QString &code,
                                          const QString &redirectUri,
                                          const QString &presentedBinding)
{
    const OAuthBackend::ExchangeResult result{
        m_backend->exchange(state, code, redirectUri, presentedBinding)};
    return QVariantMap{
        {QStringLiteral("identityJson"), identityToJson(result.identity)},
        {QStringLiteral("context"), result.context},
        {QStringLiteral("error"), result.error}};
}

void IdentityService::bindSession(const QString &state, const QString &sessionId)
{
    m_backend->rekeyTokens(state, sessionId);
}

void IdentityService::holdClaim(const QString &code, const QString &sessionId,
                                const QString &challenge)
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    m_claims.expire(now, m_claimTtlMs);
    m_claims.hold(code, sessionId.toLatin1(), challenge, now);
}

QString IdentityService::takeClaim(const QString &code, const QString &verifier)
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    return QString::fromLatin1(m_claims.take(code, verifier, now, m_claimTtlMs));
}

void IdentityService::releaseSession(const QString &sessionId)
{
    m_backend->releaseTokens(sessionId);
}

} // namespace SynQt
