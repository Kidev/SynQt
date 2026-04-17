// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_KEYCHAINSTORE_H
#define SYNQT_KEYCHAINSTORE_H

#include "securestore.h"

namespace SynQt {

/// The macOS store: Keychain Services, a generic-password item under this app's service
/// name.
///
/// Two flags carry the security position, and neither is a default:
///
///  - `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`. `ThisDeviceOnly` keeps the item out
///    of iCloud Keychain and out of an encrypted backup restored onto another machine.
///  - `kSecUseDataProtectionKeychain`, which gives the item a per-application boundary rather
///    than a per-user one.
///
/// The second needs a keychain-access-group entitlement, so an unsigned or ad-hoc-signed build
/// gets `errSecMissingEntitlement` and this store falls back to the file-based keychain,
/// reporting Binding::User instead of Binding::Application. Such a build's code signature
/// changes on every rebuild, so macOS also asks for permission on every run. `synqt build
/// --deploy --sign` gives the application boundary.
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
