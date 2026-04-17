// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "secretservicestore.h"

#include <QCoreApplication>

// GLib's gdbusintrospection.h has a struct member called `signals`, which Qt's keyword
// macro would turn into `public`. Undefining the keyword around this include is Qt's own
// workaround for GTK; it is restored afterwards.
#undef signals
#include <libsecret/secret.h>
#define signals Q_SIGNALS

namespace SynQt {

namespace {

// The schema every SynQt device credential uses. `edge` is the account key (the edge
// origin); `app` keeps two SynQt apps on one edge apart. The trailing entry terminates the
// array, as libsecret requires.
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

// The label shown in Seahorse or KWalletManager, readable so a person can find and delete
// the entry.
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
    // Cheap check first: no session bus is common (SSH, containers, minimal window
    // managers, CI) and must not cost a D-Bus round trip.
    if (qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) {
        if (reason) {
            *reason = QStringLiteral("there is no session bus, so no keyring to talk to");
        }
        return false;
    }
    // A sandboxed app reaches secrets through org.freedesktop.portal.Secret, which SynQt
    // does not implement. Report it now.
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
    // login. Storing replaces the item with the same attributes, as rotation needs: one
    // item per edge.
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
    // SECRET_SEARCH_LOAD_SECRETS, not SECRET_SEARCH_UNLOCK: unlocking a locked collection
    // shows a password dialog, which at startup would hang an unattended app. A locked item
    // comes back without a secret and the client signs in normally.
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
        // The value the search already loaded, so the service is not asked again (where a
        // prompt could appear).
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
    // FALSE without an error means nothing was removed, which is success: sign-out must not
    // depend on the keyring.
    Q_UNUSED(removed);
    return true;
}

SecureStore::Binding SecretServiceStore::binding() const
{
    // User, no more: any process on the session bus can read any item, so there is no
    // application boundary.
    return Binding::User;
}

QString SecretServiceStore::name() const
{
    return QStringLiteral("Secret Service (libsecret)");
}

} // namespace SynQt
