// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CREDENTIALMANAGERSTORE_H
#define SYNQT_CREDENTIALMANAGERSTORE_H

#include "securestore.h"

namespace SynQt {

/// The Windows store: a generic credential in Credential Manager, persisted to this machine
/// only.
///
/// `CRED_PERSIST_LOCAL_MACHINE`, never `CRED_PERSIST_ENTERPRISE`, which roams the credential
/// with the user profile to every machine the user logs into.
///
/// At rest the blob is DPAPI-protected under the user account, which protects against another
/// user of the machine and an offline disk, not against another process running as that user.
/// [Desktop](https://synqt.org/desktop/) states that boundary. The edge's reuse detection is
/// what answers a copied credential.
class CredentialManagerStore : public SecureStore
{
public:
    bool isAvailable(QString *reason) const override;
    bool store(const QString &account, const QByteArray &secret, QString *error) override;
    bool load(const QString &account, QByteArray *secret, QString *error) override;
    bool erase(const QString &account, QString *error) override;
    Binding binding() const override;
    QString name() const override;
};

} // namespace SynQt

#endif // SYNQT_CREDENTIALMANAGERSTORE_H
