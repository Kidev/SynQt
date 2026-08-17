// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "deviceregistry.h"

#include "constanttime.h"
#include "ipersistenceprovider.h"
#include "persistencefactory.h"
#include "secrets.h"
#include "sessionmanager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace SynQt {

namespace {

// A device label for a future device list. Bounded and stripped of control characters,
// since the client supplies it.
QString sanitizedLabel(const QString &label)
{
    constexpr qsizetype kMaxLabel{64};
    QString clean;
    clean.reserve(qMin(label.size(), kMaxLabel));
    for (const QChar character : label) {
        if (clean.size() >= kMaxLabel) {
            break;
        }
        if (!character.isPrint()) {
            continue;
        }
        clean.append(character);
    }
    return clean;
}

qint64 daysToMs(int days)
{
    return static_cast<qint64>(days) * 24 * 60 * 60 * 1000;
}

// The columns, once, so a SELECT and the code that reads its row cannot drift apart.
const QString kColumns{QStringLiteral(
    "family, sub, edge_origin, identity_json, current_hash, current_gen, prev_hash, "
    "prev_issued_ms, created_ms, last_used_ms, expires_ms, revoked")};

} // namespace

DeviceRegistry::DeviceRegistry(DeviceConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
{
}

DeviceRegistry::~DeviceRegistry()
{
    if (m_store) {
        m_store->disconnect();
    }
}

bool DeviceRegistry::open(QString *error)
{
    m_store = makePersistenceProvider(m_config.store, error);
    if (m_store == nullptr) {
        return false;
    }
    if (!m_store->connect(error)) {
        m_store.reset();
        return false;
    }

    // CREATE TABLE IF NOT EXISTS, not migrate(): the forward-only migration counter is one
    // number per database, and this table may share a database whose counter belongs to the
    // app.
    //
    // Portable types (TEXT for the hex digests, not BLOB/BYTEA), so every persistence
    // provider accepts the statement; a multi-edge deployment needs a shared store.
    const QString schema{QStringLiteral(
        "CREATE TABLE IF NOT EXISTS synqt_devices ("
        "  family TEXT PRIMARY KEY,"
        "  sub TEXT NOT NULL,"
        "  edge_origin TEXT NOT NULL,"
        "  identity_json TEXT NOT NULL,"
        "  current_hash TEXT NOT NULL,"
        "  current_gen INTEGER NOT NULL,"
        "  prev_hash TEXT,"
        "  prev_issued_ms BIGINT,"
        "  created_ms BIGINT NOT NULL,"
        "  last_used_ms BIGINT NOT NULL,"
        "  expires_ms BIGINT NOT NULL,"
        "  label TEXT,"
        "  revoked INTEGER NOT NULL DEFAULT 0)")};
    const DbResult created{m_store->exec(schema, {})};
    if (!created.ok) {
        if (error) {
            *error = created.error;
        }
        m_store.reset();
        return false;
    }

    // Which family a session came from, stored here rather than in the edge's memory: with
    // a shared session table, the sign-out that must end the credential can reach a process
    // that never saw the enrolment (see bindSession()). Same portable types and CREATE
    // TABLE IF NOT EXISTS as above.
    const QString sessions{QStringLiteral(
        "CREATE TABLE IF NOT EXISTS synqt_session_family ("
        "  session_id TEXT PRIMARY KEY,"
        "  family TEXT NOT NULL,"
        "  created_ms BIGINT NOT NULL)")};
    const DbResult sessionsCreated{m_store->exec(sessions, {})};
    if (!sessionsCreated.ok) {
        if (error) {
            *error = sessionsCreated.error;
        }
        m_store.reset();
        return false;
    }

    purgeExpired();
    return true;
}

bool DeviceRegistry::isOpen() const
{
    return m_store != nullptr && m_store->isHealthy();
}

QString DeviceRegistry::hashOf(const QByteArray &secret) const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(secret, QCryptographicHash::Sha256).toHex());
}

DeviceRegistry::Credential DeviceRegistry::enrol(const QString &sub, const QVariantMap &identity,
                                                 const QString &edgeOrigin,
                                                 DeviceBinding binding, const QString &label)
{
    if (!isOpen() || sub.isEmpty()) {
        return Credential{};
    }
    // The floor, applied here, not at build time: the level a machine reaches depends on
    // the machine. Refusing to enrol leaves the visitor signed in with nothing on disk, as
    // with `desktop_session: memory`.
    if (binding < m_config.minBinding) {
        return Credential{};
    }
    purgeExpired();

    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const QByteArray secret{randomSecret()};
    Credential issued;
    issued.family = QString::fromLatin1(randomSecret());
    issued.secret = secret;
    issued.expiresMs = now + daysToMs(m_config.lifetimeDays);

    const QString identityJson{
        QString::fromUtf8(QJsonDocument{QJsonObject::fromVariantMap(identity)}
                              .toJson(QJsonDocument::Compact))};
    const DbResult inserted{m_store->exec(
        QStringLiteral("INSERT INTO synqt_devices (family, sub, edge_origin, identity_json, "
                       "current_hash, current_gen, prev_hash, prev_issued_ms, created_ms, "
                       "last_used_ms, expires_ms, label, revoked) "
                       "VALUES (?, ?, ?, ?, ?, ?, NULL, NULL, ?, ?, ?, ?, 0)"),
        {issued.family, sub, edgeOrigin, identityJson, hashOf(secret), 1, now, now,
         issued.expiresMs, sanitizedLabel(label)})};
    if (!inserted.ok) {
        return Credential{};
    }
    return issued;
}

DeviceRegistry::Credential DeviceRegistry::issue(const QString &family,
                                                 const QString &currentHash, int generation,
                                                 qint64 nowMs, qint64 expiresMs,
                                                 bool retireCurrent)
{
    const QByteArray secret{randomSecret()};
    // One statement for two cases. Redeeming the live generation retires it into prev_hash
    // and starts the overlap window. Redeeming the retired one inside the window leaves
    // prev_hash and its timestamp alone, so a client that crashes between the answer and
    // its store keeps working without extending the window.
    //
    // Written only while the row still holds the generation read in redeem(). Edges sharing
    // the store can both read one generation before either writes; the one whose update
    // finds the row moved on changed nothing and issues nothing.
    const QString sql{retireCurrent
        ? QStringLiteral("UPDATE synqt_devices SET current_hash = ?, current_gen = ?, "
                         "prev_hash = ?, prev_issued_ms = ?, last_used_ms = ? "
                         "WHERE family = ? AND current_hash = ?")
        : QStringLiteral("UPDATE synqt_devices SET current_hash = ?, current_gen = ?, "
                         "last_used_ms = ? WHERE family = ? AND current_hash = ?")};
    QVariantList params;
    params.append(hashOf(secret));
    params.append(generation + 1);
    if (retireCurrent) {
        params.append(currentHash);
        params.append(nowMs);
    }
    params.append(nowMs);
    params.append(family);
    params.append(currentHash);

    const DbResult updated{m_store->exec(sql, params)};
    if (!updated.ok || updated.affected != 1) {
        return Credential{};
    }
    Credential next;
    next.family = family;
    next.secret = secret;
    next.expiresMs = expiresMs;
    return next;
}

DeviceRegistry::Redemption DeviceRegistry::redeem(const QString &family,
                                                  const QByteArray &secret,
                                                  const QString &edgeOrigin)
{
    Redemption outcome;
    if (!isOpen() || family.isEmpty() || secret.isEmpty()) {
        return outcome;
    }
    const DbResult found{m_store->query(
        QStringLiteral("SELECT %1 FROM synqt_devices WHERE family = ?").arg(kColumns),
        {family})};
    if (!found.ok || found.rows.isEmpty()) {
        return outcome;
    }
    const QVariantMap row{found.rows.first().toMap()};
    if (row.value(QStringLiteral("revoked")).toInt() != 0) {
        return outcome;
    }
    // A credential enrolled for one edge origin is not valid for another, so a deployment
    // serving two origins keeps two separate device pools.
    if (row.value(QStringLiteral("edge_origin")).toString() != edgeOrigin) {
        return outcome;
    }

    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 expiresMs{row.value(QStringLiteral("expires_ms")).toLongLong()};
    const qint64 lastUsedMs{row.value(QStringLiteral("last_used_ms")).toLongLong()};
    if (now >= expiresMs || now - lastUsedMs > daysToMs(m_config.inactivityDays)) {
        forget(family);
        return outcome;
    }

    const QString presented{hashOf(secret)};
    const QString currentHash{row.value(QStringLiteral("current_hash")).toString()};
    const QString prevHash{row.value(QStringLiteral("prev_hash")).toString()};
    const int generation{row.value(QStringLiteral("current_gen")).toInt()};

    bool retireCurrent{true};
    if (!constantTimeEquals(presented, currentHash)) {
        if (prevHash.isEmpty() || !constantTimeEquals(presented, prevHash)) {
            // Not this family's at any generation: refused, nothing more. Revoking here
            // would let anyone with a family id sign its owner out by guessing.
            return outcome;
        }
        const qint64 retiredMs{row.value(QStringLiteral("prev_issued_ms")).toLongLong()};
        if (now - retiredMs > static_cast<qint64>(m_config.overlapSeconds) * 1000) {
            // Past the window: the live generation was collected by someone, and this is a
            // second copy of the retired one. The visitor cannot be told apart, so the
            // family and every session from it are revoked.
            forget(family);
            emit reuseDetected(family);
            return outcome;
        }
        // Inside the window: the client missed the answer that retired this. It gets the
        // next generation, and the window is not extended.
        retireCurrent = false;
    }

    const Credential next{issue(family, currentHash, generation, now, expiresMs, retireCurrent)};
    if (!next.isValid()) {
        return outcome;
    }
    outcome.ok = true;
    outcome.sub = row.value(QStringLiteral("sub")).toString();
    outcome.identity = QJsonDocument::fromJson(
                           row.value(QStringLiteral("identity_json")).toString().toUtf8())
                           .object()
                           .toVariantMap();
    outcome.next = next;
    return outcome;
}

void DeviceRegistry::forget(const QString &family)
{
    if (!isOpen() || family.isEmpty()) {
        return;
    }
    // The rows naming it stay: reuse detection forgets the family and then asks
    // sessionsOfFamily() which sessions it opened. purgeExpired() reclaims the orphans.
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE family = ?"), {family});
}

void DeviceRegistry::forgetSub(const QString &sub)
{
    if (!isOpen() || sub.isEmpty()) {
        return;
    }
    // As in forget(): purgeExpired() reclaims the rows naming these families.
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE sub = ?"), {sub});
}

void DeviceRegistry::bindSession(const QByteArray &sessionId, const QString &family)
{
    if (!isOpen() || sessionId.isEmpty() || family.isEmpty()) {
        return;
    }
    // Delete then insert instead of an upsert, which the two engines spell differently; the
    // table is small and written once per sign-in.
    //
    // The key, never the id. This table outlives the process as a file or a shared
    // database, and a backup holding ids would hold every live session. The key names a
    // session everywhere else (SessionManager::keyFor) and buys nobody a session.
    const QString token{SessionManager::keyFor(sessionId)};
    m_store->exec(QStringLiteral("DELETE FROM synqt_session_family WHERE session_id = ?"),
                  {token});
    m_store->exec(QStringLiteral("INSERT INTO synqt_session_family "
                                 "(session_id, family, created_ms) VALUES (?, ?, ?)"),
                  {token, family, QDateTime::currentMSecsSinceEpoch()});
}

QString DeviceRegistry::familyOf(const QByteArray &sessionId) const
{
    if (!isOpen() || sessionId.isEmpty()) {
        return QString{};
    }
    const DbResult found{m_store->query(
        QStringLiteral("SELECT family FROM synqt_session_family WHERE session_id = ?"),
        {SessionManager::keyFor(sessionId)})};
    if (!found.ok || found.rows.isEmpty()) {
        return QString{};
    }
    return found.rows.first().toMap().value(QStringLiteral("family")).toString();
}

void DeviceRegistry::unbindSession(const QByteArray &sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }
    unbindSessionKey(SessionManager::keyFor(sessionId));
}

void DeviceRegistry::unbindSessionKey(const QString &sessionKey)
{
    if (!isOpen() || sessionKey.isEmpty()) {
        return;
    }
    m_store->exec(QStringLiteral("DELETE FROM synqt_session_family WHERE session_id = ?"),
                  {sessionKey});
}

QStringList DeviceRegistry::sessionsOfFamily(const QString &family) const
{
    QStringList sessions;
    if (!isOpen() || family.isEmpty()) {
        return sessions;
    }
    const DbResult found{m_store->query(
        QStringLiteral("SELECT session_id FROM synqt_session_family WHERE family = ?"),
        {family})};
    if (!found.ok) {
        return sessions;
    }
    sessions.reserve(found.rows.size());
    for (const QVariant &row : found.rows) {
        sessions.append(row.toMap().value(QStringLiteral("session_id")).toString());
    }
    return sessions;
}

void DeviceRegistry::purgeExpired()
{
    if (m_store == nullptr) {
        return;
    }
    // Both clocks at once, on the rare operations (opening, enrolling) rather than every
    // redemption. Expired rows are refused inline anyway; this keeps a visitor's sub and
    // identity from staying on disk for years.
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 idleBefore{now - daysToMs(m_config.inactivityDays)};
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE expires_ms <= ? "
                                 "OR last_used_ms < ?"),
                  {now, idleBefore});
    // Then rows pointing at a family that is gone. A row is written per stored sign-in and
    // deleted by sign-out or session expiry, so the survivors are sign-ins nobody ended,
    // most of them on a store shared by a replicated edge. Without this the table, which
    // sign-out reads, grows forever.
    //
    // Only rows old enough that nothing still writes them: reuse detection forgets a family
    // and then reads its sessions, and a session may be bound just after its family is
    // created. A day is far past both. NOT IN rather than a join, so every persistence
    // provider accepts it.
    m_store->exec(QStringLiteral(
        "DELETE FROM synqt_session_family WHERE created_ms < ? AND family NOT IN "
        "(SELECT family FROM synqt_devices)"), {now - daysToMs(1)});
}

} // namespace SynQt
