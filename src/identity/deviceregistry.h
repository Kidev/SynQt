// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DEVICEREGISTRY_H
#define SYNQT_DEVICEREGISTRY_H

#include "identityconfig.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <memory>

namespace SynQt {

class IPersistenceProvider;

/// The durable half of staying signed in on the desktop. One row per enrolled device,
/// holding what it takes to mint that visitor a fresh session and nothing that is itself a
/// session.
///
/// Three properties are the whole design, and each one exists because the obvious
/// alternative (persist the session id) fails at it:
///
///  1. **Single use with rotation.** Every redemption issues the next generation and retires
///     the one presented, so a credential read off a disk is only good until the machine it
///     was taken from next starts up.
///  2. **Reuse detection.** A retired generation presented past the overlap window means two
///     copies of it exist, so the family and every session descended from it are revoked.
///     This is the property no file permission gives. Theft stops being silent and becomes
///     an event the edge sees. (RFC 6819 s.5.2.2.3, applied to SynQt's own credential rather
///     than to the provider's refresh token.)
///  3. **A short session at the end of it.** What a redemption buys is a session of exactly
///     the length a browser gets, so "stay signed in for a month" never becomes "a stolen
///     file is good for a month".
///
/// The secret is never stored, here or anywhere on the edge. What a row holds is a SHA-256
/// of it. A single hash rather than a password KDF, because there is no low-entropy secret
/// to stretch (the credential is 256 random bits) and a KDF would only add latency to every
/// relaunch.
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

    /// Remember which family a session was minted from, so signing that session out ends
    /// the credential too.
    ///
    /// It lives in the store rather than in the edge's memory because a session can now
    /// outlive the process that minted it: the session table is shared as soon as identity
    /// is promoted to its own entity, and the edge is replicable as soon as it is. A
    /// visitor enrolling through one process and signing out against another would
    /// otherwise keep the credential the sign-out was meant to end, with nothing about the
    /// sign-out looking wrong. Rebinding a session replaces its row, because a session id
    /// rotates on elevation and the old id must not go on naming the family.
    ///
    /// Nothing authorizes off this: it is a back-reference, and the authority for what a
    /// credential can do is the family row itself.
    void bindSession(const QByteArray &sessionId, const QString &family);
    QString familyOf(const QByteArray &sessionId) const;
    void unbindSession(const QByteArray &sessionId);

    /// Every session minted from one family. What reuse detection revokes: two copies of a
    /// credential are in play and there is no telling which holder is the visitor, so
    /// everything the family opened goes, wherever it was opened from.
    QList<QByteArray> sessionsOfFamily(const QString &family) const;

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
