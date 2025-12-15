// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SECRETSERVICESTORE_H
#define SYNQT_SECRETSERVICESTORE_H

#include "securestore.h"

namespace SynQt {

/// The Linux store: the freedesktop Secret Service (`org.freedesktop.secrets`), which
/// gnome-keyring-daemon implements and KWallet bridges, reached through libsecret, which
/// negotiates the session key that keeps a secret off the bus in the clear.
///
/// - **No per-application boundary.** Any process on the session bus can read any item, so
///   this reports Binding::User.
/// - **Nothing here prompts.** A read against a locked collection returns no secret instead
///   of asking for a password, because the read happens before the first frame, possibly on a
///   headless or SSH session. The visitor then signs in normally.
class SecretServiceStore : public SecureStore
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

#endif // SYNQT_SECRETSERVICESTORE_H
