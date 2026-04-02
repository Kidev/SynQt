// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "deviceregistry.h"

#include "constanttime.h"
#include "ipersistenceprovider.h"
#include "persistencefactory.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>

#include <utility>

namespace SynQt {

namespace {

// A cryptographically random credential, hex-encoded: 256 bits, from the system generator.
// The same shape the claim codes use, and for the same reason. It is the only thing standing
// between a file on somebody's disk and a session.
QByteArray randomSecret()
{
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / static_cast<int>(sizeof(quint32)));
    return raw.toHex();
}

// The label a device carries for a future "your devices" list. Bounded and stripped of
// control characters on the way in, because it arrives from a client and is stored.
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

    // CREATE TABLE IF NOT EXISTS rather than migrate(): the forward-only migration counter is
    // one number per database, and this table is expected to share a database with an app
    // whose own schema owns that counter. Going through migrate() would mean the table is
    // silently never created on any store the app has already migrated further than one step.
    //
    // Portable types (TEXT for the hex digests rather than BLOB/BYTEA), because the same
    // statement has to be accepted by every persistence provider a deployment might point
    // this at, and a multi-edge deployment has to point it at a shared one.
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

    // Which family a session came from. Here rather than in the edge's memory for the
    // reason bindSession() gives: a session outlives the process that minted it as soon as
    // the session table is shared, and the sign-out that must end the credential can land
    // on a process that never saw the enrolment. Same portable types, same reasoning about
    // CREATE TABLE IF NOT EXISTS against a database whose migration counter is the app's.
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
    // The floor, applied here and not at build time. Which level a machine can reach is a
    // property of that machine, and a project may ship all three platforms under a policy
    // only two of them meet. Refusing to enrol leaves the visitor signed in with the session
    // they claimed and nothing on disk, which is precisely `desktop_session: memory`.
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
    // Two callers, one statement. Redeeming the live generation retires it into prev_hash and
    // starts the overlap window. Redeeming an already-retired one inside that window leaves
    // prev_hash and its timestamp alone, so a client that keeps crashing between the answer
    // and its store keeps working without extending the window a single time.
    const QString sql{retireCurrent
        ? QStringLiteral("UPDATE synqt_devices SET current_hash = ?, current_gen = ?, "
                         "prev_hash = ?, prev_issued_ms = ?, last_used_ms = ? WHERE family = ?")
        : QStringLiteral("UPDATE synqt_devices SET current_hash = ?, current_gen = ?, "
                         "last_used_ms = ? WHERE family = ?")};
    QVariantList params;
    params.append(hashOf(secret));
    params.append(generation + 1);
    if (retireCurrent) {
        params.append(currentHash);
        params.append(nowMs);
    }
    params.append(nowMs);
    params.append(family);

    const DbResult updated{m_store->exec(sql, params)};
    if (!updated.ok) {
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
    // A credential enrolled against one edge origin is not a credential for another. This is
    // what keeps a deployment that fronts two origins from being one pool of devices, and it
    // costs nothing.
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
            // Not this family's, at any generation. Refused, and nothing else: revoking here
            // would let anyone holding a family id sign its owner out by guessing at secrets.
            return outcome;
        }
        const qint64 retiredMs{row.value(QStringLiteral("prev_issued_ms")).toLongLong()};
        if (now - retiredMs > static_cast<qint64>(m_config.overlapSeconds) * 1000) {
            // Past the window, so this is the case the whole design exists for: the live
            // generation was collected by somebody, and this presentation came from a second
            // copy of the retired one. Which of the two is the visitor is unknowable, so both
            // lose the family and every session descended from it.
            forget(family);
            emit reuseDetected(family);
            return outcome;
        }
        // Inside the window. The client never got the answer that retired this. It gets the
        // next generation instead of the one it missed (which nothing holds and nothing can
        // now present), and the window it is inside is not extended.
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
    // The rows naming it are deliberately left where they are: reuse detection forgets the
    // family and then asks sessionsOfFamily() which sessions it opened, so deleting them
    // here would answer "none" at the one moment the answer matters. They are reclaimed by
    // purgeExpired() instead, which is where an orphan belongs.
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE family = ?"), {family});
}

void DeviceRegistry::forgetSub(const QString &sub)
{
    if (!isOpen() || sub.isEmpty()) {
        return;
    }
    // As in forget(): the rows naming these families are reclaimed by purgeExpired(), not
    // here, so nothing that wants to walk them loses them out from under it.
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE sub = ?"), {sub});
}

void DeviceRegistry::bindSession(const QByteArray &sessionId, const QString &family)
{
    if (!isOpen() || sessionId.isEmpty() || family.isEmpty()) {
        return;
    }
    // Delete then insert rather than an upsert: the two engines behind this interface
    // spell an upsert differently, and this table is small and written once per sign-in.
    const QString token{QString::fromLatin1(sessionId)};
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
        {QString::fromLatin1(sessionId)})};
    if (!found.ok || found.rows.isEmpty()) {
        return QString{};
    }
    return found.rows.first().toMap().value(QStringLiteral("family")).toString();
}

void DeviceRegistry::unbindSession(const QByteArray &sessionId)
{
    if (!isOpen() || sessionId.isEmpty()) {
        return;
    }
    m_store->exec(QStringLiteral("DELETE FROM synqt_session_family WHERE session_id = ?"),
                  {QString::fromLatin1(sessionId)});
}

QList<QByteArray> DeviceRegistry::sessionsOfFamily(const QString &family) const
{
    QList<QByteArray> sessions;
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
        sessions.append(
            row.toMap().value(QStringLiteral("session_id")).toString().toLatin1());
    }
    return sessions;
}

void DeviceRegistry::purgeExpired()
{
    if (m_store == nullptr) {
        return;
    }
    // Both clocks at once, and on the way in to the rare operations (opening, enrolling)
    // rather than on every redemption. A row past either one is refused inline anyway, so
    // this is about not keeping a visitor's sub and identity on disk for years after the
    // credential naming them stopped working.
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    const qint64 idleBefore{now - daysToMs(m_config.inactivityDays)};
    m_store->exec(QStringLiteral("DELETE FROM synqt_devices WHERE expires_ms <= ? "
                                 "OR last_used_ms < ?"),
                  {now, idleBefore});
    // Then whatever is left pointing at a family that is no longer there. A row here is
    // written once per stored sign-in and deleted by a sign-out or a session expiring, so
    // the ones that survive are the sign-ins nobody ever ended -- which, on a store shared
    // by a replicated edge, is most of them. Without this the table grows for the life of
    // the store, and it is the table a sign-out reads to find the family it must end.
    //
    // Only rows old enough that nothing can still be writing them, and that is not
    // fastidiousness. Reuse detection forgets a family and then asks which sessions it
    // opened, and a caller may bind a session a moment after the family it names was
    // created. A day is far past either, and a row that waits one more day to be reclaimed
    // costs nothing. Written as NOT IN rather than a join, because the same statement has
    // to be accepted by every persistence provider a deployment may point this at.
    m_store->exec(QStringLiteral(
        "DELETE FROM synqt_session_family WHERE created_ms < ? AND family NOT IN "
        "(SELECT family FROM synqt_devices)"), {now - daysToMs(1)});
}

} // namespace SynQt
