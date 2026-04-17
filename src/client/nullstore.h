// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_NULLSTORE_H
#define SYNQT_NULLSTORE_H

#include "securestore.h"

namespace SynQt {

/// The store for a machine that has none. It reports itself unavailable and holds nothing.
///
/// Used by the browser (which keeps the session cookie itself), by a platform with no
/// supported store, and by a Linux session with no keyring. On each, the credential lives for
/// the life of the process. The device credential is safe to hand out because a copy cannot
/// be taken without the OS store's protection.
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
