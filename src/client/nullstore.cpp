// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "nullstore.h"

namespace SynQt {

bool NullStore::isAvailable(QString *reason) const
{
    if (reason) {
        *reason = QStringLiteral("this platform has no secure store SynQt can use");
    }
    return false;
}

bool NullStore::store(const QString &account, const QByteArray &secret, QString *error)
{
    Q_UNUSED(account);
    Q_UNUSED(secret);
    if (error) {
        *error = QStringLiteral("there is nowhere to store a credential on this machine");
    }
    return false;
}

bool NullStore::load(const QString &account, QByteArray *secret, QString *error)
{
    Q_UNUSED(account);
    Q_UNUSED(error);
    if (secret) {
        secret->clear();
    }
    // Not an error: nothing was stored, and the client signs in as on a first launch.
    return false;
}

bool NullStore::erase(const QString &account, QString *error)
{
    Q_UNUSED(account);
    Q_UNUSED(error);
    // Erasing nothing succeeds; sign-out never depends on the store.
    return true;
}

SecureStore::Binding NullStore::binding() const
{
    return Binding::None;
}

QString NullStore::name() const
{
    return QStringLiteral("none");
}

} // namespace SynQt
