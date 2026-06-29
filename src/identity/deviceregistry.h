// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DEVICEREGISTRY_H
#define SYNQT_DEVICEREGISTRY_H

#include "identityconfig.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

namespace SynQt {

class IPersistenceProvider;

/// The durable half of staying signed in on the desktop: one row per enrolled device, holding
/// what it takes to mint a fresh session and nothing that is itself a session.
///
///  1. **Single use with rotation.** Every redemption issues the next generation and retires
///     the one presented.
///  2. **Reuse detection.** A retired generation presented past the overlap window means two
///     copies exist, so the family and every session descended from it are revoked (RFC 6819
///     s.5.2.2.3).
///  3. **A short session at the end.** A redemption buys a session of the browser's length.
///
/// A row holds a SHA-256 of the secret, never the secret; the credential is 256 random bits,
/// so no KDF is needed.
class DeviceRegistry : public QObject
{
    Q_OBJECT

public:
    /// A credential to hand to a client, and the only moment its secret exists in the clear
    /// on this side. Empty family or secret means none was issued.
    struct Credential
    {
        QString family;    ///< the opaque id the client presents alongside the secret
        QByteArray secret;
        qint64 expiresMs{0};

        bool isValid() const { return !family.isEmpty() && !secret.isEmpty(); }
    };

    /// What a successful redemption yields. Who the visitor is (so the caller can re-derive
    /// their scope through the mapping hook rather than trusting a month-old one), and the
    /// credential that replaces the one spent.
    struct Redemption
    {
        bool ok{false};
        QString sub;
        QVariantMap identity;
        Credential next;
    };

    explicit DeviceRegistry(DeviceConfig config, QObject *parent = nullptr);
    ~DeviceRegistry() override;

    /// Open the configured store and make sure the table is there. False plus *error when
    /// the provider will not open. The caller decides whether that is fatal.
    bool open(QString *error);
    bool isOpen() const;

    /// Enrol a device and issue its first credential. An empty return means nothing was
    /// enrolled (the store is not open, or `binding` is below the configured floor), which
    /// is never a reason to refuse the sign-in itself.
    Credential enrol(const QString &sub, const QVariantMap &identity, const QString &edgeOrigin,
                     DeviceBinding binding, const QString &label);

    /// Spend a credential for the next one. `ok` is false for every way this can fail, and
    /// the caller must answer all of them identically. The distinction between unknown,
    /// expired, revoked and wrong is exactly the oracle an attacker wants.
    Redemption redeem(const QString &family, const QByteArray &secret,
                      const QString &edgeOrigin);

    /// Delete a family outright (logging out, or the owner retiring a device). Idempotent.
    void forget(const QString &family);

    /// Remember which family a session was minted from, so signing that session out ends the
    /// credential too. Stored rather than kept in memory, since another edge process or the auth
    /// entity may handle the sign-out. Rebinding replaces the row, because the session id rotates
    /// on elevation. A back-reference only, authorizing nothing. It holds the session's key
    /// (SessionManager::keyFor), never the id.
    void bindSession(const QByteArray &sessionId, const QString &family);
    QString familyOf(const QByteArray &sessionId) const;
    void unbindSession(const QByteArray &sessionId);
    /// The same, for a key read out of the table rather than an id held in memory.
    void unbindSessionKey(const QString &sessionKey);

    /// Every session minted from one family, as session keys. What reuse detection
    /// revokes. Two copies of a credential are in play and there is no telling which
    /// holder is the visitor, so everything the family opened goes, wherever it was
    /// opened from (SessionManager::revokeByKey).
    QStringList sessionsOfFamily(const QString &family) const;

    /// Delete every family belonging to a visitor. This is what signing out means for
    /// somebody who signed in on more than one machine.
    void forgetSub(const QString &sub);

signals:
    /// A family was revoked because a retired generation of it was presented past the
    /// overlap window. Whoever holds the sessions descended from it must end them: two
    /// copies of the credential are in play, and one of them is not the visitor's.
    void reuseDetected(const QString &family);

private:
    QString hashOf(const QByteArray &secret) const;
    Credential issue(const QString &family, const QString &currentHash, int generation,
                     qint64 nowMs, qint64 expiresMs, bool retireCurrent);
    void purgeExpired();

    DeviceConfig m_config;
    std::unique_ptr<IPersistenceProvider> m_store;
};

} // namespace SynQt

#endif // SYNQT_DEVICEREGISTRY_H
