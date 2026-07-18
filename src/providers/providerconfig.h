// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PROVIDERCONFIG_H
#define SYNQT_PROVIDERCONFIG_H

#include <QString>

namespace SynQt {

/// The `provider` block of an entity, as read from synqt.yaml + the entity env. One struct
/// serves every family; a provider reads only the fields it needs. Secrets (password, uri)
/// are resolved from the entity environment only and are never logged or exposed on a
/// connect point.
struct ProviderConfig
{
    QString name;              // sqlite | postgres | memory | mongodb | redis | custom:X

    /// Embedded relational (sqlite).
    QString file;              // database file path
    QString journalMode{QStringLiteral("wal")};
    int busyTimeoutMs{5000};

    /// External relational (postgres, ...).
    QString host;
    int port{0};
    QString database;
    QString user;
    QString password;          // env: only
    QString sslMode{QStringLiteral("verify-full")};
    QString caCert;            // CA that signed the engine certificate
    int poolSize{4};

    /// Cache/document connection (redis/mongodb).
    QString uri;               // env: only (full connection string)
    bool tls{true};

    /// Whether the entity is built for release. In release, an external provider refuses a
    /// plaintext or unverified connection; dev on localhost may allow it.
    bool release{true};

    /// True when host is a loopback address (dev-plaintext allowance).
    bool isLoopbackHost() const
    {
        return host == QLatin1String("127.0.0.1") || host == QLatin1String("::1")
               || host == QLatin1String("localhost");
    }
};

} // namespace SynQt

#endif // SYNQT_PROVIDERCONFIG_H
