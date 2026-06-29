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

/// How often the expiry sweep runs. Well under any sane TTL, and idle work either way:
/// the sweep only drains the front of an ordered queue, so a quiet edge pays one
/// comparison a minute.
constexpr int kSweepIntervalMs{60 * 1000};

} // namespace

SessionManager::SessionManager(QString defaultScope, int ttlMinutes, QObject *parent)
    : QObject{parent}
    , m_defaultScope{std::move(defaultScope)}
    , m_ttlMs{static_cast<qint64>(ttlMinutes) * 60 * 1000}
{
    // Unconditional: with no TTL there are no sessions to reclaim, but there are still
    // rotations, which expire on a clock of their own.
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
    // Oldest first, which is the front of the expiry queue. The queue is hints: an entry
    // whose id was rotated or overwritten no longer matches the record and is skipped, as
    // purgeExpired skips it.
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
        // Told the way a revocation is told, so everything keyed on it lets go, on this
        // process and on every replica sharing the table.
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
    // In edge (remote) mode, propagate the new session to the authoritative store so other
    // edges see it too. The token is minted here and carried across.
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
    }
    // The handle, never the credential. A monitor that recorded the id would be a place a
    // visitor's session can be read out of.
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
                                    const QVariantMap &identity)
{
    // Copied before anything is emitted, and the whole function reads the copy.
    //
    // The id an elevation names is almost always a Caller's own `m_sessionId`, passed here
    // by reference, and the first thing `sessionRotated` does is set that member to the new
    // credential. A parameter still referring to it changes underneath the signals that
    // come after. Every Caller past the first was told the session had rotated from itself
    // and so left its own id behind, and `sessionRemoved` named the credential that had
    // been issued rather than the one being replaced, which is the edge being told to
    // end the session it had this moment created. `Caller.setScope` in a slot is the one
    // way an application elevates anybody, so this was on the path of every sign-in.
    const QByteArray previous{wasId};
    const auto it{m_sessions.find(previous)};
    if (it == m_sessions.end()) {
        return QByteArray{};
    }
    SessionRecord record{it.value()};
    // What this session was reached from before, if anything. Read before the line below
    // overwrites it, because it is the head of a chain that has to be moved along too.
    const QByteArray chained{record.rotatedFrom};
    m_sessions.erase(it);
    record.id = newToken();  // rotate the credential on privilege change
    record.scope = scope.isEmpty() ? m_defaultScope : scope;
    if (!identity.isEmpty()) {
        record.identity = identity;
    }
    record.createdMs = QDateTime::currentMSecsSinceEpoch();
    // The browser is still holding the id this one replaced, in a cookie nothing on the
    // live connection can rewrite. Remember what it became, so the next page load hands
    // the visitor their new credential instead of a fresh anonymous session, and remember
    // it on the record too so that reclaiming the record reclaims the hand-off with it.
    record.rotatedFrom = previous;
    m_sessions.insert(record.id, record);
    trackExpiry(record);
    m_rotations.insert(previous, Rotation{record.id, record.createdMs});
    // Elevating twice before the visitor's next page load used to sign them out. Their
    // cookie still holds the credential the FIRST rotation replaced, and that hand-off
    // pointed at the id the second rotation has just erased; `rotationOf` refuses a
    // hand-off whose target is gone, so the browser was handed a fresh anonymous session
    // instead of the elevated one it had earned. A slot that raises scope and then raises
    // it again (signing somebody in and then granting them a role) is an ordinary
    // thing to write, so the chain is followed rather than broken.
    //
    // The window keeps the clock it started on. A hand-off is for the visitor's next page
    // load, and refreshing `atMs` here would let a session that rotates every few minutes
    // keep one alive indefinitely.
    if (const auto head{m_rotations.find(chained)}; head != m_rotations.end()) {
        head->to = record.id;
    }
    emitUpsert(record);
    // First, so that everything still naming the old credential is holding the new one
    // before anybody acts on the removal below.
    emit sessionRotated(previous, record.id);
    emit rotationRecorded(QString::fromLatin1(previous), QString::fromLatin1(record.id));
    emit sessionRemoved(QString::fromLatin1(previous));
    if (m_remote) {
        // In this order, because the other replicas apply them in it: the new session
        // first, so the hand-off has something live to point at. The hand-off next, so
        // the removal that follows is read there as a rotation and not as the end of a
        // session. The removal last.
        QMetaObject::invokeMethod(m_remote, "putSession",
                                  Q_ARG(QString, QString::fromLatin1(record.id)),
                                  Q_ARG(QString, record.scope),
                                  Q_ARG(QString, identityToJson(record.identity)),
                                  Q_ARG(double, static_cast<double>(record.createdMs)));
        QMetaObject::invokeMethod(m_remote, "rotateSession",
                                  Q_ARG(QString, QString::fromLatin1(previous)),
                                  Q_ARG(QString, QString::fromLatin1(record.id)));
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(previous)));
    }
    // An elevation is the one session event worth finding in a hurry, so it names both
    // handles. What an operator is chasing is which session became which, and when.
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
    // Only while the session it points at is still there. A rotation to a session that
    // has since expired or been revoked is not a credential to hand anyone.
    return isLive(entry->to) ? entry->to : QByteArray{};
}

void SessionManager::revoke(const QByteArray &id)
{
    const auto it{m_sessions.constFind(id)};
    if (it != m_sessions.constEnd()) {
        dropRotationTo(it.value());
        m_sessions.erase(it);
        emit sessionRemoved(QString::fromLatin1(id));
        trace(Category::Authorization, Severity::Info, QStringLiteral("session revoked"),
              {{QStringLiteral("session"), keyFor(id)}});
    }
    if (m_remote) {
        QMetaObject::invokeMethod(m_remote, "removeSession",
                                  Q_ARG(QString, QString::fromLatin1(id)));
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
    // The authoritative store's changes flow back into the local read cache. A dynamic
    // Replica frees its runtime-built metaobject on destruction, so the manager it feeds must
    // be destroyed while the Replica is still alive (the owner tears down its consumer links
    // before the caches they feed). The mesh test teardown orders exactly that.
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
    // Already held. This is the rotation this manager made, echoed back by the store, or a
    // repeat. The entry keeps the clock it started on either way (see setScope on why a
    // refreshed clock would keep a hand-off alive indefinitely), and nothing here is told
    // twice.
    if (m_rotations.contains(previous)) {
        return;
    }
    m_rotations.insert(previous, Rotation{next, QDateTime::currentMSecsSinceEpoch()});
    if (const auto record{m_sessions.find(next)}; record != m_sessions.end()) {
        record->rotatedFrom = previous;
    }
    // Everything on this process still naming the old credential moves to the new one:
    // the Callers of a tab that happened to be connected here, and the edge's own tables
    // (WebEdge::followRotation), exactly as they do for a rotation made here.
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

/// The same 256 bits from the system generator that every other secret here is made of
/// (SynQt::randomSecret). It was a v4 UUID, which is 122 random bits: nothing anybody
/// would guess, but not what secrets.h says a session credential is, and one place minting
/// its own is one place a weaker generator could go unnoticed.
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
    // Rotations expire on their own clock, whether or not sessions have a TTL: they are a
    // hand-off for one page load, not a session, and each one is a few dozen bytes kept
    // for a visitor who may never come back. One whose target is gone is dropped whatever
    // its age, which is what clears a chain. Rotating twice before the visitor reloads
    // leaves the first hand-off pointing at the id the second one replaced.
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
    // Drain only the front of the insertion-ordered queue while it is past the TTL. Locally
    // created records are appended in non-decreasing createdMs order, so the first entry that
    // is still live means the rest are too and the drain stops, amortized O(1) per create instead of
    // an O(N) full-table scan. Each drained hint is reconciled against the map: it reclaims a
    // record only when the id is still present with the same createdMs. A mismatch (the id was
    // rotated by setScope or overwritten by applyUpsert) or an absent id means a stale hint,
    // which is dropped. purge stays silent (no sessionRemoved) to match the prior
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
            dropRotationTo(it.value());
            m_sessions.erase(it);
            // Local only (see sessionExpired), what an expired session was still holding
            // here goes with it, and nothing is told about it anywhere else.
            emit sessionExpired(QString::fromLatin1(id));
            trace(Category::Authorization, Severity::Info, QStringLiteral("session expired"),
                  {{QStringLiteral("session"), keyFor(id)}});
        }
    }
}

} // namespace SynQt
