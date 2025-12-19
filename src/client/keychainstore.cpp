// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "keychainstore.h"

#include <QCoreApplication>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace SynQt {

namespace {

/// A CFStringRef that releases itself. The Keychain API takes CoreFoundation types and
/// returns owned references, and this file would otherwise be a list of CFRelease calls with
/// one early return away from a leak.
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

/// The service every SynQt item is filed under, so a human can find it in Keychain Access
/// and delete it. One per application, so two SynQt apps never collide.
QString serviceName()
{
    const QString application{QCoreApplication::applicationName()};
    return application.isEmpty() ? QStringLiteral("SynQt")
                                 : QStringLiteral("SynQt ") + application;
}

/// The query that identifies exactly one item. This app's service, this edge's account.
/// `dataProtection` picks which keychain it lives in, which is the difference between an
/// application boundary and a user one.
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

QString describe(OSStatus status)
{
    CFStringRef message{SecCopyErrorMessageString(status, nullptr)};
    if (message == nullptr) {
        return QStringLiteral("Keychain error %1").arg(static_cast<long>(status));
    }
    const CFIndex length{CFStringGetMaximumSizeForEncoding(CFStringGetLength(message),
                                                           kCFStringEncodingUTF8) + 1};
    QByteArray buffer(static_cast<qsizetype>(length), '\0');
    // CoreFoundation's Boolean is an unsigned char, so this is a narrowing conversion and
    // the brace would refuse it. The cast is the conversion this file is full of at the C
    // boundary, written out rather than hidden behind a parenthesis.
    const bool converted{static_cast<bool>(CFStringGetCString(message, buffer.data(), length,
                                                              kCFStringEncodingUTF8))};
    CFRelease(message);
    return converted ? QString::fromUtf8(buffer.constData())
                     : QStringLiteral("Keychain error %1").arg(static_cast<long>(status));
}

} // namespace

bool KeychainStore::isAvailable(QString *reason) const
{
    // Always. A Mac has a keychain. What varies is which one this build may use and whether
    // it is unlocked, and both of those are answered by the call that needs them rather than
    // by a probe that would have to guess.
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
    // After first unlock, so a relaunch works without the visitor typing anything, and
    // ThisDeviceOnly so the item never leaves this machine through iCloud or a backup.
    CFDictionarySetValue(query, kSecAttrAccessible,
                         kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);

    OSStatus status{SecItemAdd(query, nullptr)};
    if (status == errSecMissingEntitlement && m_dataProtection) {
        // An unsigned or ad-hoc-signed build cannot reach the data-protection keychain. Fall
        // back to the file-based one and remember, so binding() reports what this build
        // got instead of what a signed one would have.
        m_dataProtection = false;
        CFRelease(query);
        query = itemQuery(service, accountRef, false);
        CFDictionarySetValue(query, kSecValueData, data);
        CFDictionarySetValue(query, kSecAttrAccessible,
                             kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly);
        status = SecItemAdd(query, nullptr);
    }
    if (status == errSecDuplicateItem) {
        // Rotation: an item is already there, so this replaces its value in place rather
        // than accumulating one item per launch.
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
    CFMutableDictionaryRef query{itemQuery(service, accountRef, m_dataProtection)};
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef found{nullptr};
    OSStatus status{SecItemCopyMatching(query, &found)};
    CFRelease(query);
    if (status == errSecMissingEntitlement && m_dataProtection) {
        m_dataProtection = false;
        query = itemQuery(service, accountRef, false);
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        status = SecItemCopyMatching(query, &found);
        CFRelease(query);
    }

    if (status == errSecItemNotFound) {
        return false;  // nothing stored: the ordinary first launch, and not an error
    }
    if (status != errSecSuccess || found == nullptr) {
        if (error) {
            // errSecUserCanceled and a locked keychain both land here, and both mean the
            // same thing to the client. Sign in the way a first launch does.
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
    // Both keychains, because which one holds the item depends on how this build was signed,
    // and a logout that removed it from only one of them would leave a redeemable credential
    // behind on exactly the build that is hardest to notice it on.
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
    // The data-protection keychain binds an item to this application's code signature, which
    // is a real per-application boundary and the only one of the three platforms that has
    // one. The file-based fallback does not, so it is reported as User.
    return m_dataProtection ? Binding::Application : Binding::User;
}

QString KeychainStore::name() const
{
    return QStringLiteral("Keychain");
}

} // namespace SynQt
