// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SESSIONMANAGER_H
#define SYNQT_SESSIONMANAGER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <deque>
#include <functional>
#include <utility>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// One browser user's session on the edge: the opaque credential the browser presents (the
/// httpOnly cookie), the scope it was granted, and the normalized identity (empty while
/// anonymous).
struct SessionRecord
{
    QByteArray id;
    QString scope;
    QVariantMap identity; ///< sub/login/name/email. Empty == anonymous
    qint64 createdMs{0};
    /// The credential this record replaced, when a scope change rotated it (see
    /// rotationOf). Kept so that reclaiming this record also reclaims the hand-off
    /// pointing at it, which otherwise outlives by minutes the session it names.
    QByteArray rotatedFrom;
};

/// Owns the live sessions on the edge: creation, cookie/token lookup, scope elevation
/// with credential rotation, revocation, and time-to-live expiry. One per edge. It is
/// the single source of truth the upgrade verifier and every Caller read from.
class SessionManager : public QObject
{
    Q_OBJECT

public:
    explicit SessionManager(QString defaultScope, int ttlMinutes, QObject *parent = nullptr);

    /// The name one session answers to across a system: half of the credential's SHA-256, never
    /// the credential itself.
    ///
    /// The id is the credential, so it never leaves the edge. Downstream entities key their
    /// per-session state on this instead, and it cannot be replayed at the edge. It changes when
    /// the credential rotates on a scope change: an elevated session is a different session.
    static QString keyFor(const QByteArray &id);

    /// Create a fresh session. An empty scope means the configured default (anonymous).
    ///
    /// Empty when the table is full and nothing in it can be released (see setMaximumSessions);
    /// every caller must check.
    QByteArray createSession(const QString &scope = QString(),
                             const QVariantMap &identity = QVariantMap());

    /// How many sessions this manager holds, and what it does at the ceiling.
    ///
    /// A page load with no live cookie mints a session, so this bounds the one table a stranger
    /// can grow. At the ceiling, the oldest session that is anonymous, at the default scope and
    /// without a live connection (see setInUseCheck) is released; under a flood those are the
    /// flood's own. A signed-in session is never evicted. When nothing can be released,
    /// createSession() returns empty. Zero disables the ceiling.
    void setMaximumSessions(int maximum);
    int maximumSessions() const;

    /// Tell the manager which sessions are attached to something, so eviction never takes
    /// a session out from under a live connection. The edge answers from its socket table;
    /// with no check installed, every session counts as unattached.
    void setInUseCheck(std::function<bool(const QByteArray &)> inUse);

    /// Whether createSession() would succeed now, for a route that spends something before it
    /// mints (the device credential route rotates the credential first).
    bool hasRoom() const;

    /// Look up a live (unexpired) session by its credential. Nullptr if unknown/expired.
    const SessionRecord *lookup(const QByteArray &id) const;
    bool isLive(const QByteArray &id) const;

    /// Whether a rotation leaves a hand-off for the browser still holding the replaced id (see
    /// rotationOf).
    ///
    /// A slot (`Caller.setScope`) cannot set the browser's cookie, so the next page load hands it
    /// the new credential. A route that answers with the new cookie itself needs no hand-off, and
    /// leaving one would let a planted pre-sign-in cookie redeem the elevated session.
    enum class Handoff {
        Keep,  ///< the caller cannot set the cookie. The old id may still name its successor
        None,  ///< the caller's response carries the cookie. The old id is dead outright
    };

    /// Elevate a session after login and rotate its credential (defeats fixation). Returns the
    /// new id, or empty when the old id was unknown.
    ///
    /// `wasId` is often a `Caller`'s own `m_sessionId`, which the rotation moves, so the
    /// implementation copies it first.
    QByteArray setScope(const QByteArray &wasId, const QString &scope,
                        const QVariantMap &identity = QVariantMap(),
                        Handoff handoff = Handoff::Keep);

    /// The id a rotated-away credential became, for RotationGraceMs.
    ///
    /// A slot's `Caller.setScope()` cannot rewrite the browser's httpOnly cookie, so the browser
    /// keeps the old id until its next page load. This returns a redirection, never an
    /// authorization: the old id is out of the table and nothing accepts it; it only tells the
    /// edge which cookie to set in its place.
    QByteArray rotationOf(const QByteArray &id) const;

    void revoke(const QByteArray &id);

    /// Revoke the session a key names (see keyFor), for the device store, whose rows must not
    /// carry the credential. A walk over the table: fine for a detected credential reuse, not for
    /// a request path.
    void revokeByKey(const QString &key);

    QString defaultScope() const;

    /// Promote this manager to a dedicated auth entity: writes (create/setScope/revoke) are
    /// forwarded to the authoritative store behind the given SessionStore Replica, and its echoed
    /// changes are applied to the local read cache, which keeps lookup()/isLive() synchronous. See
    /// "Where identity runs" in [Authentication](https://synqt.org/authentication/).
    void attachRemote(QObject *sessionReplica);

    /// The full live session table, for replaying to a newly-connected consumer (late join).
    Q_INVOKABLE QVariantList snapshot() const;

public slots:
    /// Apply an authoritative change received from the store (no re-propagation). Also the
    /// auth entity's Session Source writes an edge's putSession/removeSession through these.
    /// createdMs is a double to match the mesh contract (real) and QML numbers.
    void applyUpsert(const QString &token, const QString &scope,
                     const QString &identityJson, double createdMs);
    void applyRemove(const QString &token);
    /// A rotation another process made, or this one's echoed back. Records the hand-off
    /// (rotationOf) and moves everything here still naming `from` to `to`, the way a
    /// local setScope does. An entry already held is left on the clock it started on.
    void applyRotation(const QString &from, const QString &to);

signals:
    /// Emitted on every table change, so the auth entity's Session Sources forward it to the
    /// edges that consume the session connect point.
    void sessionUpserted(const QString &token, const QString &scope,
                         const QString &identityJson, double createdMs);
    void sessionRemoved(const QString &token);
    /// A hand-off recorded on this manager, in the SessionStore contract's form, so the auth
    /// entity can forward it to every edge. The originating edge ignores its own echo.
    void rotationRecorded(const QString &from, const QString &to);

    /// The same session under a new credential, after a scope change rotated it. Every Caller
    /// still naming the old id follows it. Not the same event as sessionRemoved followed by
    /// sessionUpserted, which is how the pair reaches another entity.
    void sessionRotated(const QByteArray &from, const QByteArray &to);

    /// A session reclaimed because its time-to-live ran out, on this entity. Unlike
    /// sessionRemoved, it is not forwarded: every holder reaches the same verdict. It releases
    /// what the session held here, such as its OAuth tokens.
    void sessionExpired(const QString &token);

private:
    /// How long a rotated-away id can name its replacement: long enough for the next page load,
    /// short enough not to be a second credential. It authorizes nothing (see rotationOf).
    static constexpr qint64 RotationGraceMs{10 * 60 * 1000};

    /// The default ceiling. A hundred thousand records is a few tens of megabytes, far
    /// above the concurrent visitors of a single edge and far below what would take one
    /// down.
    static constexpr int DefaultMaximumSessions{100000};
    /// How far into the table eviction looks. Walking the whole table on every refused mint
    /// would give a flood another lever.
    static constexpr int EvictionSearchDepth{4096};

    QByteArray newToken() const;
    void trackExpiry(const SessionRecord &record);
    /// Whether this record is one eviction may take: anonymous, at the default scope, and
    /// attached to nothing.
    bool isEvictable(const SessionRecord &record) const;
    /// Let go of the oldest evictable session. False when none was found within
    /// EvictionSearchDepth of the front.
    bool evictOne();
    /// Drop the hand-off pointing at this record, which is going.
    void dropRotationTo(const SessionRecord &record);
    void purgeExpired();
    void emitUpsert(const SessionRecord &record);

    QHash<QByteArray, SessionRecord> m_sessions;
    /// Insertion-ordered {createdMs, id} hints that make purgeExpired() amortized O(1): with a
    /// fixed TTL, sessions expire in creation order. A rotated or overwritten id leaves a stale
    /// hint the purge drops. Only maintained when m_ttlMs > 0. lookup()/isLive() re-check the TTL,
    /// so a lagging queue only defers reclaiming memory.
    std::deque<std::pair<qint64, QByteArray>> m_expiryQueue;
    /// Sweeps the queue on a timer as well as on every create, so a quiet edge still releases
    /// expired records and the tokens hanging off them.
    QTimer *m_sweepTimer{nullptr};
    /// Rotated-away id -> what it became, and when. Not sessions: entries here are
    /// consulted only to re-cookie the browser still holding the old id (see rotationOf),
    /// and are dropped by the same sweep that reclaims expired sessions.
    struct Rotation
    {
        QByteArray to;
        qint64 atMs{0};
    };
    QHash<QByteArray, Rotation> m_rotations;
    QString m_defaultScope;
    qint64 m_ttlMs;
    int m_maximumSessions{DefaultMaximumSessions};
    std::function<bool(const QByteArray &)> m_inUse;
    QPointer<QObject> m_remote; ///< the SessionStore Replica when this is an edge cache
};

} // namespace SynQt

#endif // SYNQT_SESSIONMANAGER_H
