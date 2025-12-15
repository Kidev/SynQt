// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DEVICECREDENTIAL_H
#define SYNQT_DEVICECREDENTIAL_H

#include "securestore.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

namespace SynQt {

/// The client's half of staying signed in. What is kept in the OS secure store, under what
/// name, and with a deadline on every call so an app never hangs on a keyring.
///
/// It knows nothing about the protocol. What it holds is an opaque pair the edge issued and
/// will want back at the next launch, and its whole job is that the pair survives the
/// process and nothing else does.
class DeviceCredential : public QObject
{
    Q_OBJECT

public:
    /// The pair as the edge issues it. The family it names, and the one live secret for it.
    struct Held
    {
        QString id;
        QByteArray secret;

        bool isValid() const { return !id.isEmpty() && !secret.isEmpty(); }
    };

    /// One credential per edge origin. Not per signed-in visitor, which would read better in
    /// a keychain listing and cannot work: at the launch that matters nothing knows who the
    /// visitor is yet, which is the entire question the stored credential answers. Signing in
    /// as somebody else on the same machine replaces it, which is what a desktop app should
    /// do anyway.
    explicit DeviceCredential(const QUrl &edgeUrl, QObject *parent = nullptr);
    ~DeviceCredential() override;

    /// Whether anything can be persisted on this machine. False is an ordinary answer (a
    /// headless session, a locked keyring, a platform with no store) and means the visitor
    /// signs in once per launch.
    bool isAvailable() const;
    SecureStore::Binding binding() const;
    /// The binding in the vocabulary the edge reads, for the enrolment request.
    QString bindingName() const;
    QString storeName() const;

    /// What was stored at the last launch, or an invalid Held when there is nothing (which
    /// is the ordinary first-launch case and not an error).
    Held load();
    /// Replace what is stored. Every redemption rotates, so this overwrites.
    bool save(const Held &held);
    /// Signing out. Idempotent, and it must be. Leaving a redeemable credential behind after
    /// a logout is worse than not having one, because the visitor believes it worked.
    void erase();

    /// A short name for this machine, so a future "your devices" list can say which one this
    /// is. Nothing depends on it and the edge treats it as opaque text.
    static QString machineLabel();

private:
    /// Shared, not owned outright. A call that runs past its deadline is abandoned rather
    /// than waited on, and the thread still inside the platform API has to find the store
    /// alive when it comes back.
    std::shared_ptr<SecureStore> m_store;
    QString m_account;
    bool m_available{false};
};

} // namespace SynQt

#endif // SYNQT_DEVICECREDENTIAL_H
