// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SECURESTORE_H
#define SYNQT_SECURESTORE_H

#include <QByteArray>
#include <QString>

#include <memory>

namespace SynQt {

/// Where a native client keeps the device credential that lets a visitor stay signed in
/// between launches: the OS secure store, and nowhere else.
///
/// One narrow interface, one implementation per platform, errors reported through the
/// interface and never thrown across it. **There is no file backend**, in any mode. A machine
/// with no store persists nothing, and its visitor signs in once per launch.
///
/// The platform APIs are synchronous and can block on a UI prompt, so DeviceCredential runs
/// them on a worker with a deadline and reports a timeout as no store.
class SecureStore
{
public:
    /// What the store binds an item to, reported to the edge at enrolment so a deployment
    /// can decline to persist below a level it set. Ordered, so a floor is a comparison.
    enum class Binding {
        None = 0,        ///< nothing can be persisted here
        User = 1,        ///< at rest under the OS user. Any process running as them can read it
        Application = 2, ///< also bound to this application's code signature
        Hardware = 3     ///< a non-exportable key in a secure element
    };

    virtual ~SecureStore() = default;

    /// Whether this store can be used at all right now, with a short human-readable reason
    /// when it cannot (no session bus, no entitlement, a service logon). Cheap and silent:
    /// being unavailable is an ordinary outcome on a headless or SSH session.
    virtual bool isAvailable(QString *reason) const = 0;

    /// Write, replacing whatever was under `account`. Rotation goes through here on every
    /// relaunch, so it must overwrite rather than accumulate.
    virtual bool store(const QString &account, const QByteArray &secret, QString *error) = 0;

    /// Read. False with no `*error` means nothing is stored, which is the ordinary
    /// first-launch case and never something to report to the visitor.
    virtual bool load(const QString &account, QByteArray *secret, QString *error) = 0;

    /// Remove. Idempotent: erasing an account that is not there is a success, because
    /// signing out must not depend on the store agreeing about what it held.
    virtual bool erase(const QString &account, QString *error) = 0;

    virtual Binding binding() const = 0;
    /// The store's name, for the one line said at startup about where the credential lives.
    virtual QString name() const = 0;
};

/// The name the edge knows a binding by. The vocabulary is shared with the server's
/// DeviceBinding (src/identity/identityconfig.h) and travels between them as these strings.
QString secureStoreBindingName(SecureStore::Binding binding);

/// The store for this platform: Keychain on macOS, Credential Manager on Windows, the
/// Secret Service on Linux, and one that holds nothing anywhere else (including the browser,
/// which has no OS store and needs none). Never returns nullptr.
std::unique_ptr<SecureStore> makeSecureStore();

} // namespace SynQt

#endif // SYNQT_SECURESTORE_H
