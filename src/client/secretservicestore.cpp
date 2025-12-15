// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "secretservicestore.h"

#include <QCoreApplication>

// GLib's gdbusintrospection.h declares a struct member called `signals`, and Qt's keyword
// macros turn that into `public` halfway through a struct definition. Dropping the keyword
// across this one include is Qt's own workaround for the same collision with GTK; it is put
// back afterwards so nothing else in the translation unit is affected.
#undef signals
#include <libsecret/secret.h>
#define signals Q_SIGNALS

namespace SynQt {

namespace {

// The schema every SynQt device credential is filed under. `edge` is the account key (the
// edge origin), `app` keeps two SynQt apps against one edge from finding each other's item.
// The trailing entry terminates the array, as libsecret requires.
const SecretSchema *deviceSchema()
{
    static const SecretSchema schema{
        "org.synqt.DeviceCredential", SECRET_SCHEMA_NONE,
        {{"edge", SECRET_SCHEMA_ATTRIBUTE_STRING},
         {"app", SECRET_SCHEMA_ATTRIBUTE_STRING},
         {nullptr, static_cast<SecretSchemaAttributeType>(0)}},
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return &schema;
}

QByteArray applicationKey()
{
    const QString name{QCoreApplication::applicationName()};
    return name.isEmpty() ? QByteArrayLiteral("SynQt") : name.toUtf8();
}

// The label a human sees in Seahorse or KWalletManager. It is readable because being able to
// find this entry and delete it is one of the reasons for using the OS store at all.
QByteArray itemLabel(const QString &account)
{
    return applicationKey() + " sign-in for " + account.toUtf8();
}

// Take the message out of a GError and free it, so no call site has to remember to.
QString takeError(GError **error)
{
    if (error == nullptr || *error == nullptr) {
        return QString{};
    }
    const QString message{QString::fromUtf8((*error)->message)};
    g_error_free(*error);
    *error = nullptr;
    return message;
}

} // namespace

bool SecretServiceStore::isAvailable(QString *reason) const
{
    // Cheap and silent first. No session bus is the common case (SSH, a container, a
    // minimal window manager, CI), and it must not cost a D-Bus round trip to find out.
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
        if (reason) {
            *reason = QStringLiteral("there is no session bus, so no keyring to talk to");
        }
        return false;
    }
    // A sandboxed app reaches secrets through org.freedesktop.portal.Secret instead, which
    // SynQt does not implement. Saying so beats failing later in a way that reads as a bug.
    if (!qEnvironmentVariableIsEmpty("FLATPAK_ID") || !qEnvironmentVariableIsEmpty("SNAP")) {
        if (reason) {
            *reason = QStringLiteral("a sandboxed app reaches secrets through the desktop "
                                     "portal, which this version does not use");
        }
        return false;
    }

    GError *failure{nullptr};
    SecretService *service{secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &failure)};
    const QString message{takeError(&failure)};
    if (service == nullptr) {
        if (reason) {
            *reason = message.isEmpty()
                ? QStringLiteral("no secret service is running on the session bus")
                : message;
        }
        return false;
    }
    g_object_unref(service);
    return true;
}

bool SecretServiceStore::store(const QString &account, const QByteArray &secret, QString *error)
{
    GError *failure{nullptr};
    // SECRET_COLLECTION_DEFAULT is the `login` keyring, which pam_gnome_keyring unlocks at
    // login. Storing replaces any item with the same attributes, which is what rotation
    // needs. One item per edge, overwritten on every relaunch.
    const gboolean stored{secret_password_store_sync(
        deviceSchema(), SECRET_COLLECTION_DEFAULT, itemLabel(account).constData(),
        secret.constData(), nullptr, &failure,
        "edge", account.toUtf8().constData(),
        "app", applicationKey().constData(), nullptr)};
    const QString message{takeError(&failure)};
    if (stored == FALSE) {
        if (error) {
            *error = message.isEmpty() ? QStringLiteral("the keyring refused the write")
                                       : message;
        }
        return false;
    }
    return true;
}

bool SecretServiceStore::load(const QString &account, QByteArray *secret, QString *error)
{
    GError *failure{nullptr};
    // SECRET_SEARCH_LOAD_SECRETS and deliberately not SECRET_SEARCH_UNLOCK. With UNLOCK a
    // locked collection is unlocked, which means a password dialog, which at startup means
    // an app that hangs on a machine with nobody in front of it. Without it a locked item
    // comes back with no secret and the client signs in normally.
    GList *found{secret_password_search_sync(
        deviceSchema(), SECRET_SEARCH_LOAD_SECRETS, nullptr, &failure,
        "edge", account.toUtf8().constData(),
        "app", applicationKey().constData(), nullptr)};
    const QString message{takeError(&failure)};
    if (found == nullptr) {
        if (error && !message.isEmpty()) {
            *error = message;
        }
        return false;  // nothing stored, or a locked keyring: the ordinary first launch
    }

    bool ok{false};
    if (SECRET_IS_ITEM(found->data)) {
        // The already-loaded value, so this reads what the search brought back rather than
        // asking the service again (which is where a prompt could still appear).
        SecretValue *value{secret_item_get_secret(SECRET_ITEM(found->data))};
        if (value != nullptr) {
            gsize length{0};
            const gchar *bytes{secret_value_get(value, &length)};
            if (bytes != nullptr && secret != nullptr) {
                *secret = QByteArray{bytes, static_cast<qsizetype>(length)};
                ok = true;
            }
            secret_value_unref(value);
        }
    }
    g_list_free_full(found, g_object_unref);
    return ok;
}

bool SecretServiceStore::erase(const QString &account, QString *error)
{
    GError *failure{nullptr};
    const gboolean removed{secret_password_clear_sync(
        deviceSchema(), nullptr, &failure,
        "edge", account.toUtf8().constData(),
        "app", applicationKey().constData(), nullptr)};
    const QString message{takeError(&failure)};
    if (!message.isEmpty()) {
        if (error) {
            *error = message;
        }
        return false;
    }
    // FALSE with no error means there was nothing to remove, which is a success: signing out
    // must not depend on the keyring agreeing about what it held.
    Q_UNUSED(removed);
    return true;
}

SecureStore::Binding SecretServiceStore::binding() const
{
    // User, and not more. Any process on the session bus can read any item, so claiming an
    // application boundary here would be claiming one that does not exist.
    return Binding::User;
}

QString SecretServiceStore::name() const
{
    return QStringLiteral("Secret Service (libsecret)");
}

} // namespace SynQt
