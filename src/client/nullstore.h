// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_NULLSTORE_H
#define SYNQT_NULLSTORE_H

#include "securestore.h"

namespace SynQt {

/// The store for a machine that has none: it reports itself unavailable and holds nothing.
///
/// This is not a degraded mode to be improved on later. It is the answer for the browser
/// (which has no OS store and needs none, since it keeps the session cookie itself), for a
/// platform with no supported store, and for a Linux session with no keyring, and on every
/// one of those the credential simply lives for the life of the process. The temptation it
/// exists to refuse is a file written "just for now": what makes the device credential safe
/// to hand out at all is that a copy of it cannot be taken without taking the OS store's
/// protection with it.
class NullStore : public SecureStore
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

#endif // SYNQT_NULLSTORE_H
