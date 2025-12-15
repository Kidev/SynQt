// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "securestore.h"

#include "nullstore.h"

#if defined(Q_OS_MACOS)
#  include "keychainstore.h"
#elif defined(Q_OS_WIN)
#  include "credentialmanagerstore.h"
#elif defined(SYNQT_HAVE_LIBSECRET)
#  include "secretservicestore.h"
#endif

namespace SynQt {

QString secureStoreBindingName(SecureStore::Binding binding)
{
    switch (binding) {
    case SecureStore::Binding::None:
        return QStringLiteral("none");
    case SecureStore::Binding::User:
        return QStringLiteral("user");
    case SecureStore::Binding::Application:
        return QStringLiteral("application");
    case SecureStore::Binding::Hardware:
        return QStringLiteral("hardware");
    }
    return QStringLiteral("none");
}

std::unique_ptr<SecureStore> makeSecureStore()
{
    // One store per platform, chosen at compile time, and NullStore wherever there is not
    // one. There is deliberately no configuration here: which store an app uses is a
    // property of the machine it is running on, and a project that could select a different
    // one could select a worse one.
#if defined(Q_OS_MACOS)
    return std::make_unique<KeychainStore>();
#elif defined(Q_OS_WIN)
    return std::make_unique<CredentialManagerStore>();
#elif defined(SYNQT_HAVE_LIBSECRET)
    return std::make_unique<SecretServiceStore>();
#else
    return std::make_unique<NullStore>();
#endif
}

} // namespace SynQt
