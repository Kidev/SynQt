// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_KEYCHAINSTORE_H
#define SYNQT_KEYCHAINSTORE_H

#include "securestore.h"

namespace SynQt {

/// The macOS store: Keychain Services, a generic-password item under this app's service
/// name.
///
/// Two flags carry the whole security position, and neither is a default:
///
///  - `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`. The `ThisDeviceOnly` half is the
///    load-bearing one: it keeps the item out of iCloud Keychain and out of an encrypted
///    backup restored onto another machine. Without it, "a credential for this device"
///    quietly becomes "a credential on every device signed into that Apple ID", which is the
///    one thing a device credential must not be.
///  - `kSecUseDataProtectionKeychain`, which is what gives the item a real per-application
///    boundary rather than a per-user one.
///
/// The second one has a consequence. It needs a
/// keychain-access-group entitlement, so an unsigned or ad-hoc-signed build gets
/// `errSecMissingEntitlement` and this store falls back to the file-based keychain, reporting
/// Binding::User instead of Binding::Application. That is also the build whose code signature
/// changes on every rebuild, so macOS asks the developer for permission on every run. Both
/// are properties of an unsigned build, not faults. `synqt build --deploy --sign` is what
/// buys the application boundary, and until now that flag was a Gatekeeper concern only.
class KeychainStore : public SecureStore
{
public:
    bool isAvailable(QString *reason) const override;
    bool store(const QString &account, const QByteArray &secret, QString *error) override;
    bool load(const QString &account, QByteArray *secret, QString *error) override;
    bool erase(const QString &account, QString *error) override;
    Binding binding() const override;
    QString name() const override;

private:
    /// Set the first time an item is written or read through the data-protection keychain,
    /// and cleared when that answers errSecMissingEntitlement. It is what binding() reports,
    /// so the level sent to the edge is what this build got rather than what the
    /// platform could give a signed one.
    mutable bool m_dataProtection{true};
};

} // namespace SynQt

#endif // SYNQT_KEYCHAINSTORE_H
