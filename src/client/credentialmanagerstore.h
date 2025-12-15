// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CREDENTIALMANAGERSTORE_H
#define SYNQT_CREDENTIALMANAGERSTORE_H

#include "securestore.h"

namespace SynQt {

/// The Windows store. A generic credential in Credential Manager, persisted to this machine
/// and this machine only.
///
/// `CRED_PERSIST_LOCAL_MACHINE`, never `CRED_PERSIST_ENTERPRISE`. The enterprise flag roams
/// the credential with the user profile to every machine they log into, which is precisely
/// what "device credential" means not to do; it is worth knowing that the wrong constant here
/// is a one-word change that silently turns this into a credential that follows the visitor
/// around a domain.
///
/// At rest the blob is DPAPI-protected under the user account, which is real protection
/// against another user of the machine and against an offline disk. It is **not** protection
/// against another process running as that user: any of them can read it back. That is the
/// honest boundary, and [Desktop](https://synqt.org/desktop/) states it rather than implying
/// one that is not there. What answers a copied credential in this design is the edge's reuse
/// detection, not the file permission.
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
