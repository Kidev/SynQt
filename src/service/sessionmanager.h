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
#include <utility>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// One browser user's session on the edge: the opaque credential the browser presents
/// (the httpOnly cookie), the scope it was granted, and the normalized identity
/// (empty while anonymous). The identity and scope are set by the login flow (M8); until
/// then a session is created anonymous and may be elevated by the dev/test path.
struct SessionRecord
{
    QByteArray id;
    QString scope;
    QVariantMap identity; ///< sub/login/name/email; empty == anonymous
    qint64 createdMs{0};
};

/// Owns the live sessions on the edge: creation, cookie/token lookup, scope elevation
/// with credential rotation, revocation, and time-to-live expiry. One per edge; it is
/// the single source of truth the upgrade verifier and every Caller read from.
class SessionManager : public QObject
{
    Q_OBJECT

public:
    explicit SessionManager(QString defaultScope, int ttlMinutes, QObject *parent = nullptr);

    /// Create a fresh session. An empty scope means the configured default (anonymous).
    QByteArray createSession(const QString &scope = QString(),
                             const QVariantMap &identity = QVariantMap());

    /// Look up a live (unexpired) session by its credential; nullptr if unknown/expired.
    const SessionRecord *lookup(const QByteArray &id) const;
    bool isLive(const QByteArray &id) const;

    /// Elevate a session after login and rotate its credential (defeats fixation).
    /// Returns the new id; an empty return means the old id was unknown.
    QByteArray setScope(const QByteArray &id, const QString &scope,
                        const QVariantMap &identity = QVariantMap());

    /// The id a rotated-away credential became, while that is still worth knowing.
    ///
    /// Rotating on elevation is only half an exchange: the new id reaches the connection
    /// that asked for it, and the browser goes on holding the old one in a cookie no
    /// slot call can rewrite (it is httpOnly, and a WebSocket sets no headers). Without
    /// this, `Caller.setScope()` in a slot would sign the visitor out on their next page
    /// load, losing the elevation and the sign-in with it.
    ///
    /// What is returned is a redirection, never an authorization: the old id is gone from
    /// the table, isLive() is false for it, and no upgrade, call or lookup accepts it.
    /// The only thing it can still do is tell the edge which cookie to put in its place,
    /// which is exactly what the browser that was holding it needs. Kept for
    /// RotationGraceMs, since a page load is the event that consumes it.
    QByteArray rotationOf(const QByteArray &id) const;

    void revoke(const QByteArray &id);

    QString defaultScope() const;

    /// Promote this manager to a dedicated auth entity: writes here (create/setScope/revoke)
    /// are forwarded to the authoritative store behind the given Session Replica, and the
    /// authoritative store's echoed changes are applied back into the local read cache. The
    /// cache keeps lookup()/isLive() synchronous (as the upgrade verifier needs). See
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

signals:
    /// Emitted on every table change, so the auth entity's Session Sources forward it to the
    /// edges that consume the session connect point.
    void sessionUpserted(const QString &token, const QString &scope,
                         const QString &identityJson, double createdMs);
    void sessionRemoved(const QString &token);

    /// A session reclaimed because its time-to-live ran out, on this entity.
    ///
    /// Deliberately not sessionRemoved: that one is the table change the auth entity
    /// forwards to every edge, and an expiry is not a change anyone needs to be told
    /// about (every holder of the record reaches the same verdict from the same
    /// createdMs). What it is good for is releasing what an expired session was still
    /// holding here, which is how the edge stops keeping a signed-out user's OAuth
    /// tokens for as long as the process runs.
    void sessionExpired(const QString &token);

private:
    /// How long a rotated-away id can still name its replacement. Long enough that a
    /// visitor whose scope changed mid-session gets the new cookie on their next page
    /// load or refresh, short enough that it is not a second credential anyone can hold
    /// on to. It authorizes nothing on its own either way (see rotationOf).
    static constexpr qint64 RotationGraceMs{10 * 60 * 1000};

    QByteArray newToken() const;
    void trackExpiry(const SessionRecord &record);
    void purgeExpired();
    void emitUpsert(const SessionRecord &record);

    QHash<QByteArray, SessionRecord> m_sessions;
    /// Insertion-ordered {createdMs, id} hints that make purgeExpired() amortized O(1): a
    /// fixed TTL means sessions expire in creation order, so the purge only drains the front
    /// while it is past the TTL instead of scanning the whole table on every create. Entries
    /// are hints, not truth; an id that was rotated (setScope) or overwritten (applyUpsert)
    /// leaves a stale entry whose recorded createdMs no longer matches the map, and the purge
    /// simply drops it. Only maintained when m_ttlMs > 0 (with no TTL there is nothing to
    /// reclaim). lookup()/isLive() re-check the TTL themselves, so a lagging queue never
    /// returns an expired session; it only defers reclaiming its memory.
    std::deque<std::pair<qint64, QByteArray>> m_expiryQueue;
    /// Sweeps the queue on a timer as well as on every create.
    ///
    /// Creating a session is the natural moment to reclaim one, but it is not a moment
    /// that arrives on a quiet edge: a browser that already holds a live session is not
    /// given another one (webedge.cpp), so an edge whose visitors have all gone home
    /// would keep every expired record, and whatever hangs off it, until someone new
    /// arrives. lookup() never returns an expired session either way; this is about the
    /// memory, and about the tokens released with the record.
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
    QPointer<QObject> m_remote; ///< the Session Replica when this is an edge cache
};

} // namespace SynQt

#endif // SYNQT_SESSIONMANAGER_H
