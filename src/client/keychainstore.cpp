// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "keychainstore.h"

#include <QCoreApplication>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace SynQt {

namespace {

/// A CFStringRef that releases itself. The Keychain API returns owned CoreFoundation
/// references, and manual CFRelease calls are one early return away from a leak.
class CfString
{
public:
    explicit CfString(const QString &value)
    {
        const QByteArray utf8{value.toUtf8()};
        m_ref = CFStringCreateWithBytes(
            kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(utf8.constData()),
            static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    }
    ~CfString()
    {
        if (m_ref != nullptr) {
            CFRelease(m_ref);
        }
    }
    CfString(const CfString &) = delete;
    CfString &operator=(const CfString &) = delete;

    CFStringRef get() const { return m_ref; }

private:
    CFStringRef m_ref{nullptr};
};

/// The service every SynQt item is filed under, so a person can find and delete it in
/// Keychain Access. One per application, so two SynQt apps never collide.
QString serviceName()
{
    const QString application{QCoreApplication::applicationName()};
    return application.isEmpty() ? QStringLiteral("SynQt")
                                 : QStringLiteral("SynQt ") + application;
}

/// The query for exactly one item: this app's service, this edge's account.
/// `dataProtection` selects the keychain: an application boundary or a user one.
CFMutableDictionaryRef itemQuery(const CfString &service, const CfString &account,
                                 bool dataProtection)
{
    CFMutableDictionaryRef query{CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks)};
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, service.get());
    CFDictionarySetValue(query, kSecAttrAccount, account.get());
    if (dataProtection) {
        CFDictionarySetValue(query, kSecUseDataProtectionKeychain, kCFBooleanTrue);
    }
    return query;
}

/// Read one item from one of the two keychains. Separate, because a read, unlike a write,
/// must be able to try both.
OSStatus copyItem(const CfString &service, const CfString &account, bool dataProtection,
                  CFTypeRef *found)
{
    CFMutableDictionaryRef query{itemQuery(service, account, dataProtection)};
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    const OSStatus status{SecItemCopyMatching(query, found)};
    CFRelease(query);
    return status;
}

QString describe(OSStatus status)
{
    CFStringRef message{SecCopyErrorMessageString(status, nullptr)};
    if (message == nullptr) {
        return QStringLiteral("Keychain error %1").arg(static_cast<long>(status));
    }
    const CFIndex length{CFStringGetMaximumSizeForEncoding(CFStringGetLength(message),
                                                           kCFStringEncodingUTF8) + 1};
    QByteArray buffer(static_cast<qsizetype>(length), '\0');
    // CoreFoundation's Boolean is an unsigned char, so this narrows and braces would refuse
    // it; the explicit cast is the C boundary conversion.
    const bool converted{static_cast<bool>(CFStringGetCString(message, buffer.data(), length,
                                                              kCFStringEncodingUTF8))};
    CFRelease(message);
    return converted ? QString::fromUtf8(buffer.constData())
                     : QStringLiteral("Keychain error %1").arg(static_cast<long>(status));
}

} // namespace

bool KeychainStore::isAvailable(QString *reason) const
{
    // Always: a Mac has a keychain. Which one this build may use, and whether it is
    // unlocked, is answered by the call that needs it.
    Q_UNUSED(reason);
    return true;
}

bool KeychainStore::store(const QString &account, const QByteArray &secret, QString *error)
{
    const CfString service{serviceName()};
    const CfString accountRef{account};

    CFDataRef data{CFDataCreate(kCFAllocatorDefault,
                                reinterpret_cast<const UInt8 *>(secret.constData()),
                                static_cast<CFIndex>(secret.size()))};
    CFMutableDictionaryRef query{itemQuery(service, accountRef, m_dataProtection)};
    CFDictionarySetValue(query, kSecValueData, data);
    // After first unlock, so a relaunch needs no input, and ThisDeviceOnly so the item
    // never leaves this machine through iCloud or a backup.
    CFDictionarySetValue(query, kSecAttrAccessible,
                         kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    OSStatus status{SecItemAdd(query, nullptr)};
    if (status == errSecMissingEntitlement && m_dataProtection) {
        // An unsigned or ad-hoc-signed build cannot use the data-protection keychain. Fall
        // back to the file-based one and remember it, so binding() reports what this build
        // actually got.
        m_dataProtection = false;
        CFRelease(query);
        query = itemQuery(service, accountRef, false);
        CFDictionarySetValue(query, kSecValueData, data);
        CFDictionarySetValue(query, kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        status = SecItemAdd(query, nullptr);
    }
    if (status == errSecDuplicateItem) {
        // Rotation: the item exists, so its value is replaced in place.
        CFMutableDictionaryRef search{itemQuery(service, accountRef, m_dataProtection)};
        CFMutableDictionaryRef update{CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks)};
        CFDictionarySetValue(update, kSecValueData, data);
        status = SecItemUpdate(search, update);
        CFRelease(search);
        CFRelease(update);
    }
    CFRelease(query);
    CFRelease(data);

    if (status != errSecSuccess) {
        if (error) {
            *error = describe(status);
        }
        return false;
    }
    return true;
}

bool KeychainStore::load(const QString &account, QByteArray *secret, QString *error)
{
    const CfString service{serviceName()};
    const CfString accountRef{account};

    CFTypeRef found{nullptr};
    OSStatus status{copyItem(service, accountRef, m_dataProtection, &found)};
    if (m_dataProtection && found == nullptr) {
        // A read is not told which keychain this build may use. SecItemAdd refuses with
        // errSecMissingEntitlement, which moves store() to the file-based keychain, but
        // SecItemCopyMatching on the unreachable keychain answers errSecItemNotFound, like
        // a first launch. So the other keychain is checked too, and adopted only if the
        // item is there. A signed build with nothing stored keeps the keychain it is
        // entitled to.
        const OSStatus fallback{copyItem(service, accountRef, false, &found)};
        if (found != nullptr || status == errSecMissingEntitlement) {
            m_dataProtection = false;
            status = fallback;
        }
    }

    if (status == errSecItemNotFound) {
        return false;  // nothing stored: the ordinary first launch, and not an error
    }
    if (status != errSecSuccess || found == nullptr) {
        if (error) {
            // errSecUserCanceled and a locked keychain both mean: sign in as on a first
            // launch.
            *error = describe(status);
        }
        return false;
    }

    bool ok{false};
    if (CFGetTypeID(found) == CFDataGetTypeID() && secret != nullptr) {
        CFDataRef data{static_cast<CFDataRef>(found)};
        *secret = QByteArray{reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                             static_cast<qsizetype>(CFDataGetLength(data))};
        ok = true;
    }
    CFRelease(found);
    return ok;
}

bool KeychainStore::erase(const QString &account, QString *error)
{
    const CfString service{serviceName()};
    const CfString accountRef{account};
    // Both keychains, because which one holds the item depends on the build's signature,
    // and removing it from only one would leave a redeemable credential behind.
    OSStatus worst{errSecSuccess};
    for (const bool dataProtection : {true, false}) {
        CFMutableDictionaryRef query{itemQuery(service, accountRef, dataProtection)};
        const OSStatus status{SecItemDelete(query)};
        CFRelease(query);
        if (status != errSecSuccess && status != errSecItemNotFound
            && status != errSecMissingEntitlement) {
            worst = status;
        }
    }
    if (worst != errSecSuccess) {
        if (error) {
            *error = describe(worst);
        }
        return false;
    }
    return true;  // nothing to delete is a success
}

SecureStore::Binding KeychainStore::binding() const
{
    // The data-protection keychain binds an item to this application's code signature, a
    // real per-application boundary and the only one of the three platforms. The file-based
    // fallback does not, so it reports User.
    return m_dataProtection ? Binding::Application : Binding::User;
}

QString KeychainStore::name() const
{
    return QStringLiteral("Keychain");
}

} // namespace SynQt
