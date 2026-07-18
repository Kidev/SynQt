// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sessionmanager.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QUuid>

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

QVariantMap identityFromJson(const QString &json)
{
    if (json.isEmpty()) {
        return QVariantMap{};
    }
    return QJsonDocument::fromJson(json.toUtf8()).object().toVariantMap();
}

} // namespace

SessionManager::SessionManager(QString defaultScope, int ttlMinutes, QObject *parent)
    : QObject{parent}
    , m_defaultScope{std::move(defaultScope)}
    , m_ttlMs{static_cast<qint64>(ttlMinutes) * 60 * 1000}
{
}

void SessionManager::emitUpsert(const SessionRecord &record)
{
    emit sessionUpserted(QString::fromLatin1(record.id), record.scope,
                         identityToJson(record.identity), static_cast<double>(record.createdMs));
}

QByteArray SessionManager::createSession(const QString &scope, const QVariantMap &identity)
{
    purgeExpired();
    SessionRecord record{};
    record.id = newToken();
    record.scope = scope.isEmpty() ? m_defaultScope : scope;
    record.identity = identity;
    record.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_sessions.insert(record.id, record);
    trackExpiry(record);
    emitUpsert(record);
    // In edge (remote) mode, propagate the new session to the authoritative store so other
    // edges see it too; the token is minted here and carried across.
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
    }
    return record.id;
}

const SessionRecord *SessionManager::lookup(const QByteArray &id) const
{
    const auto it{m_sessions.constFind(id)};
    if (it == m_sessions.constEnd()) {
        return nullptr;
    }
    if (m_ttlMs > 0 && QDateTime::currentMSecsSinceEpoch() - it->createdMs > m_ttlMs) {
        return nullptr;
    }
    return &it.value();
}

bool SessionManager::isLive(const QByteArray &id) const
{
    return lookup(id) != nullptr;
}

QByteArray SessionManager::setScope(const QByteArray &id, const QString &scope,
                                    const QVariantMap &identity)
{
    const auto it{m_sessions.find(id)};
    if (it == m_sessions.end()) {
        return QByteArray{};
    }
    SessionRecord record{it.value()};
    m_sessions.erase(it);
    record.id = newToken();  // rotate the credential on privilege change
    record.scope = scope.isEmpty() ? m_defaultScope : scope;
    if (!identity.isEmpty()) {
        record.identity = identity;
    }
    record.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_sessions.insert(record.id, record);
    trackExpiry(record);
    emitUpsert(record);
    emit sessionRemoved(QString::fromLatin1(id));
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(id)));
    }
    return record.id;
}

void SessionManager::revoke(const QByteArray &id)
{
    if (m_sessions.remove(id)) {
        emit sessionRemoved(QString::fromLatin1(id));
    }
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(id)));
    }
}

void SessionManager::attachRemote(QObject *sessionReplica)
{
    m_remote = sessionReplica;
    // The authoritative store's changes flow back into the local read cache. A dynamic
    // Replica frees its runtime-built metaobject on destruction, so the manager it feeds must
    // be destroyed while the Replica is still alive (the owner tears down its consumer links
    // before the caches they feed); the mesh test teardown orders exactly that.
    connect(sessionReplica,
            SIGNAL(sessionUpserted(QString, QString, QString, double)),
            this, SLOT(applyUpsert(QString, QString, QString, double)));
    connect(sessionReplica, SIGNAL(sessionRemoved(QString)),
            this, SLOT(applyRemove(QString)));
}

void SessionManager::applyUpsert(const QString &token, const QString &scope,
                                 const QString &identityJson, double createdMs)
{
    SessionRecord record{};
    record.id = token.toLatin1();
    record.scope = scope;
    record.identity = identityFromJson(identityJson);
    record.createdMs = static_cast<qint64>(createdMs);
    m_sessions.insert(record.id, record);  // authoritative: overwrite the local copy
    trackExpiry(record);
    emitUpsert(record);  // let the auth entity's Sources forward it; edges have no observer
}

void SessionManager::applyRemove(const QString &token)
{
    if (m_sessions.remove(token.toLatin1())) {
        emit sessionRemoved(token);
    }
}

QVariantList SessionManager::snapshot() const
{
    QVariantList rows;
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    for (auto it{m_sessions.constBegin()}; it != m_sessions.constEnd(); ++it) {
        if (m_ttlMs > 0 && now - it->createdMs > m_ttlMs) {
            continue;
        }
        rows.append(QVariantMap{{QStringLiteral("token"), QString::fromLatin1(it->id)},
                                {QStringLiteral("scope"), it->scope},
                                {QStringLiteral("identityJson"), identityToJson(it->identity)},
                                {QStringLiteral("createdMs"), static_cast<double>(it->createdMs)}});
    }
    return rows;
}

QString SessionManager::defaultScope() const
{
    return m_defaultScope;
}

QByteArray SessionManager::newToken() const
{
    return QUuid::createUuid().toRfc4122().toHex();
}

void SessionManager::trackExpiry(const SessionRecord &record)
{
    if (m_ttlMs <= 0) {
        return;  // no TTL means nothing ever expires; keep the queue empty
    }
    m_expiryQueue.emplace_back(record.createdMs, record.id);
}

void SessionManager::purgeExpired()
{
    if (m_ttlMs <= 0) {
        return;
    }
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    // Drain only the front of the insertion-ordered queue while it is past the TTL. Locally
    // created records are appended in non-decreasing createdMs order, so the first entry that
    // is still live means the rest are too and we stop; amortized O(1) per create instead of
    // an O(N) full-table scan. Each drained hint is reconciled against the map: it reclaims a
    // record only when the id is still present with the same createdMs; a mismatch (the id was
    // rotated by setScope or overwritten by applyUpsert) or an absent id means a stale hint,
    // which is simply dropped. purge stays silent (no sessionRemoved) to match the prior
    // behaviour: TTL expiry is observed lazily by lookup(), not broadcast.
    while (!m_expiryQueue.empty()) {
        const std::pair<qint64, QByteArray> &front{m_expiryQueue.front()};
        if (now - front.first <= m_ttlMs) {
            break;
        }
        const qint64 createdMs{front.first};
        const QByteArray id{front.second};
        m_expiryQueue.pop_front();
        const auto it{m_sessions.find(id)};
        if (it != m_sessions.end() && it->createdMs == createdMs) {
            m_sessions.erase(it);
        }
    }
}

} // namespace SynQt
