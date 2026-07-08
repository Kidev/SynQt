// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sessionmanager.h"

#include "secrets.h"
#include "tracer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>

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

namespace {

/// How often the expiry sweep runs: well under any sane TTL. The sweep only drains the
/// front of an ordered queue, so an idle edge pays one comparison a minute.
constexpr int kSweepIntervalMs{60 * 1000};

} // namespace

SessionManager::SessionManager(QString defaultScope, int ttlMinutes, QObject *parent)
    : QObject{parent}
    , m_defaultScope{std::move(defaultScope)}
    , m_ttlMs{static_cast<qint64>(ttlMinutes) * 60 * 1000}
{
    // Always on: even with no TTL, rotations expire on their own clock.
    m_sweepTimer = new QTimer{this};
    connect(m_sweepTimer, &QTimer::timeout, this, [this]() { purgeExpired(); });
    m_sweepTimer->start(kSweepIntervalMs);
}

void SessionManager::emitUpsert(const SessionRecord &record)
{
    emit sessionUpserted(QString::fromLatin1(record.id), record.scope,
                         identityToJson(record.identity), static_cast<double>(record.createdMs));
}

QString SessionManager::keyFor(const QByteArray &id)
{
    if (id.isEmpty()) {
        return QString{};
    }
    const QByteArray digest{QCryptographicHash::hash(id, QCryptographicHash::Sha256)};
    return QString::fromLatin1(digest.toHex().left(32));
}

void SessionManager::setMaximumSessions(int maximum)
{
    m_maximumSessions = qMax(0, maximum);
}

int SessionManager::maximumSessions() const
{
    return m_maximumSessions;
}

void SessionManager::setInUseCheck(std::function<bool(const QByteArray &)> inUse)
{
    m_inUse = std::move(inUse);
}

bool SessionManager::isEvictable(const SessionRecord &record) const
{
    if (!record.identity.isEmpty() || record.scope != m_defaultScope) {
        return false;
    }
    return !m_inUse || !m_inUse(record.id);
}

bool SessionManager::hasRoom() const
{
    if (m_maximumSessions <= 0 || m_sessions.size() < m_maximumSessions) {
        return true;
    }
    int looked{0};
    for (const auto &[createdMs, id] : m_expiryQueue) {
        if (++looked > EvictionSearchDepth) {
            break;
        }
        const auto it{m_sessions.constFind(id)};
        if (it != m_sessions.constEnd() && it->createdMs == createdMs && isEvictable(*it)) {
            return true;
        }
    }
    return false;
}

bool SessionManager::evictOne()
{
    // Oldest first, from the front of the expiry queue. Entries are hints: one whose id was
    // rotated or overwritten no longer matches its record and is skipped, as in
    // purgeExpired.
    int looked{0};
    for (auto hint{m_expiryQueue.begin()}; hint != m_expiryQueue.end(); ++hint) {
        if (++looked > EvictionSearchDepth) {
            return false;
        }
        const auto it{m_sessions.find(hint->second)};
        if (it == m_sessions.end() || it->createdMs != hint->first || !isEvictable(*it)) {
            continue;
        }
        const QByteArray id{it->id};
        dropRotationTo(it.value());
        m_sessions.erase(it);
        m_expiryQueue.erase(hint);
        // Announced like a revocation, so everything keyed on it lets go, here and on every
        // replica sharing the table.
        emit sessionRemoved(QString::fromLatin1(id));
        if (m_remote) {
            QMetaObject::invokeMethod(m_remote, "removeSession",
                                      Q_ARG(QString, QString::fromLatin1(id)));
        }
        trace(Category::Authorization, Severity::Warning, QStringLiteral("session evicted"),
              {{QStringLiteral("session"), keyFor(id)},
               {QStringLiteral("held"), static_cast<qint64>(m_sessions.size())}});
        return true;
    }
    return false;
}

QByteArray SessionManager::createSession(const QString &scope, const QVariantMap &identity)
{
    purgeExpired();
    if (m_maximumSessions > 0 && m_sessions.size() >= m_maximumSessions && !evictOne()) {
        trace(Category::Authorization, Severity::Warning,
              QStringLiteral("session refused: the table is full"),
              {{QStringLiteral("held"), static_cast<qint64>(m_sessions.size())}});
        return QByteArray{};
    }
    SessionRecord record{};
    record.id = newToken();
    record.scope = scope.isEmpty() ? m_defaultScope : scope;
    record.identity = identity;
    record.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_sessions.insert(record.id, record);
    trackExpiry(record);
    emitUpsert(record);
    // In remote (edge) mode, send the new session to the authoritative store so other edges
    // see it. The token is minted here.
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
    }
    // The handle, never the credential: a monitor must not be a place to read sessions
    // from.
    trace(Category::Authorization, Severity::Info, QStringLiteral("session created"),
          {{QStringLiteral("session"), keyFor(record.id)},
           {QStringLiteral("scope"), record.scope},
           {QStringLiteral("identified"), !record.identity.isEmpty()}});
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

QByteArray SessionManager::setScope(const QByteArray &wasId, const QString &scope,
                                    const QVariantMap &identity, Handoff handoff)
{
    // Copied before anything is emitted, and only the copy is read.
    //
    // The id is usually a Caller's own `m_sessionId`, passed by reference, and
    // `sessionRotated` immediately sets that member to the new credential. Reading the
    // parameter after that would give every later receiver the wrong id, and
    // `sessionRemoved` would name the new credential. `Caller.setScope` in a slot is how
    // every sign-in elevates, so this is on that path.
    const QByteArray previous{wasId};
    const auto it{m_sessions.find(previous)};
    if (it == m_sessions.end()) {
        return QByteArray{};
    }
    SessionRecord record{it.value()};
    // The id this session was previously reached from, read before the line below
    // overwrites it: the head of a chain that moves too.
    const QByteArray chained{record.rotatedFrom};
    m_sessions.erase(it);
    record.id = newToken();  // rotate the credential on privilege change
    record.scope = scope.isEmpty() ? m_defaultScope : scope;
    if (!identity.isEmpty()) {
        record.identity = identity;
    }
    record.createdMs = QDateTime::currentMSecsSinceEpoch();
    const bool handOff{handoff == Handoff::Keep};
    if (handOff) {
        // The browser still holds the replaced id in a cookie the live connection cannot
        // rewrite. Remember what it became, so the next page load returns the new
        // credential instead of a fresh anonymous session. Stored on the record too, so
        // reclaiming the record reclaims the hand-off.
        record.rotatedFrom = previous;
    }
    m_sessions.insert(record.id, record);
    trackExpiry(record);
    if (handOff) {
        m_rotations.insert(previous, Rotation{record.id, record.createdMs});
        // Two elevations before the next page load must not sign the visitor out: the
        // cookie holds the credential the first rotation replaced, and that hand-off points
        // at the id the second rotation erased. `rotationOf` refuses a hand-off to a
        // missing target, so the chain is followed. Signing someone in and then granting a
        // role in one slot is ordinary.
        //
        // The window keeps its original clock. The hand-off is for the next page load;
        // refreshing `atMs` would let a session that rotates often keep one alive
        // indefinitely.
        if (const auto head{m_rotations.find(chained)}; head != m_rotations.end()) {
            head->to = record.id;
        }
    }
    // With no hand-off, a chain that led to the old id leads nowhere, `rotationOf` refuses
    // it, and the sweep drops it. The response that asked for this carries the new cookie,
    // and no earlier credential may still buy the session.
    emitUpsert(record);
    // First, so everything holding the old credential holds the new one before the removal
    // below is acted on.
    emit sessionRotated(previous, record.id);
    if (handOff) {
        emit rotationRecorded(QString::fromLatin1(previous), QString::fromLatin1(record.id));
    }
    emit sessionRemoved(QString::fromLatin1(previous));
    if (m_remote) {
        // In the order the other replicas apply them: the new session first, so the
        // hand-off has a live target; the hand-off next, so the following removal is read
        // as a rotation; the removal last.
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
        if (handOff) {
            QMetaObject::invokeMethod(m_remote, "rotateSession",
                                      Q_ARG(QString, QString::fromLatin1(previous)),
                                      Q_ARG(QString, QString::fromLatin1(record.id)));
        }
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(previous)));
    }
    // An elevation names both handles: which session became which, and when.
    trace(Category::Authorization, Severity::Info, QStringLiteral("session scope changed"),
          {{QStringLiteral("session"), keyFor(record.id)},
           {QStringLiteral("previousSession"), keyFor(previous)},
           {QStringLiteral("scope"), record.scope}});
    return record.id;
}

QByteArray SessionManager::rotationOf(const QByteArray &id) const
{
    const auto entry{m_rotations.constFind(id)};
    if (entry == m_rotations.constEnd()
        || QDateTime::currentMSecsSinceEpoch() - entry->atMs > RotationGraceMs) {
        return QByteArray{};
    }
    // Only while its target session still exists; a rotation to an expired or revoked
    // session hands out nothing.
    return isLive(entry->to) ? entry->to : QByteArray{};
}

void SessionManager::revoke(const QByteArray &id)
{
    // Copied, because `id` may refer into the node this erases: revokeByKey passes
    // `it.key()`, and QHash::erase destroys that key before the signal, the trace and the
    // remote hand-off read it (a use-after-free, invisible to AddressSanitizer inside Qt).
    // IdentityProvider's rate-window prune has the same hazard. The QByteArray copy is
    // shared, so it only costs a reference count when the caller already owns one.
    const QByteArray token{id.constData(), id.size()};
    const auto it{m_sessions.constFind(token)};
    if (it != m_sessions.constEnd()) {
        dropRotationTo(it.value());
        m_sessions.erase(it);
        emit sessionRemoved(QString::fromLatin1(token));
        trace(Category::Authorization, Severity::Info, QStringLiteral("session revoked"),
              {{QStringLiteral("session"), keyFor(token)}});
    }
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(token)));
    }
}

void SessionManager::revokeByKey(const QString &key)
{
    if (key.isEmpty()) {
        return;
    }
    for (auto it{m_sessions.constBegin()}; it != m_sessions.constEnd(); ++it) {
        if (keyFor(it.key()) == key) {
            revoke(it.key());
            return;
        }
    }
}

void SessionManager::attachRemote(QObject *sessionReplica)
{
    m_remote = sessionReplica;
    // The authoritative store's changes flow back into this read cache. A dynamic Replica
    // frees its metaobject when destroyed, so this manager must be destroyed while the
    // Replica is alive (the owner tears down its consumer links before the caches they
    // feed; the mesh test teardown follows that order).
    connect(sessionReplica,
            SIGNAL(sessionUpserted(QString, QString, QString, double)),
            this, SLOT(applyUpsert(QString, QString, QString, double)));
    connect(sessionReplica, SIGNAL(sessionRemoved(QString)),
            this, SLOT(applyRemove(QString)));
    connect(sessionReplica, SIGNAL(sessionRotated(QString, QString)),
            this, SLOT(applyRotation(QString, QString)));
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
    emitUpsert(record);  // let the auth entity's Sources forward it. Edges have no observer
}

void SessionManager::applyRotation(const QString &from, const QString &to)
{
    const QByteArray previous{from.toLatin1()};
    const QByteArray next{to.toLatin1()};
    if (previous.isEmpty() || next.isEmpty() || previous == next) {
        return;
    }
    // Already held: this manager's own rotation echoed back by the store, or a repeat. The
    // entry keeps its original clock (see setScope), and nothing is announced twice.
    if (m_rotations.contains(previous)) {
        return;
    }
    m_rotations.insert(previous, Rotation{next, QDateTime::currentMSecsSinceEpoch()});
    if (const auto record{m_sessions.find(next)}; record != m_sessions.end()) {
        record->rotatedFrom = previous;
    }
    // Everything here still naming the old credential moves to the new one: the Callers of
    // a tab connected to this process, and the edge's own tables (WebEdge::followRotation),
    // as for a local rotation.
    emit sessionRotated(previous, next);
    emit rotationRecorded(from, to);
}

void SessionManager::applyRemove(const QString &token)
{
    const auto it{m_sessions.constFind(token.toLatin1())};
    if (it != m_sessions.constEnd()) {
        dropRotationTo(it.value());
        m_sessions.erase(it);
        emit sessionRemoved(token);
    }
}

void SessionManager::dropRotationTo(const SessionRecord &record)
{
    if (!record.rotatedFrom.isEmpty()) {
        m_rotations.remove(record.rotatedFrom);
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

/// 256 bits from the system generator, like every other secret here (SynQt::randomSecret),
/// as secrets.h requires for a session credential.
QByteArray SessionManager::newToken() const
{
    return randomSecret();
}

void SessionManager::trackExpiry(const SessionRecord &record)
{
    if (m_ttlMs <= 0) {
        return;  // no TTL means nothing ever expires. Keep the queue empty
    }
    m_expiryQueue.emplace_back(record.createdMs, record.id);
}

void SessionManager::purgeExpired()
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    // Rotations expire on their own clock, with or without a session TTL: each is a
    // hand-off for one page load. One whose target is gone is dropped at any age, which
    // clears a chain (two rotations before a reload leave the first pointing at the id the
    // second replaced).
    for (auto it{m_rotations.begin()}; it != m_rotations.end();) {
        if (now - it->atMs > RotationGraceMs || !m_sessions.contains(it->to)) {
            it = m_rotations.erase(it);
        } else {
            ++it;
        }
    }
    if (m_ttlMs <= 0) {
        return;
    }
    // Drain only the front of the insertion-ordered queue while it is past the TTL. Local
    // records are appended in non-decreasing createdMs order, so the first live entry ends
    // the drain: amortized O(1) per create instead of a full scan. Each hint reclaims a
    // record only if the id is still present with the same createdMs; otherwise (rotated by
    // setScope, overwritten by applyUpsert, or gone) the hint is stale and dropped. Expiry
    // emits no sessionRemoved; lookup() observes it.
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
            dropRotationTo(it.value());
            m_sessions.erase(it);
            // Local only (see sessionExpired): what the expired session held here is
            // released without notifying anyone.
            emit sessionExpired(QString::fromLatin1(id));
            trace(Category::Authorization, Severity::Info, QStringLiteral("session expired"),
                  {{QStringLiteral("session"), keyFor(id)}});
        }
    }
}

} // namespace SynQt
