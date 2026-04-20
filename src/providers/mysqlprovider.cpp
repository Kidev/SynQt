// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "mysqlprovider.h"

#include <QSqlDatabase>
#include <QStringList>

#include <utility>

namespace SynQt {

// Qt's QMYSQL driver has no usable ssl-mode option in the build SynQt requires
// (qtbase/src/plugins/sqldrivers/mysql/qsql_mysql.cpp):
//
// 1. The option table has no "SSL_MODE" entry; the key is "MYSQL_OPT_SSL_MODE", and an
//    unknown key is reported as "Illegal connect option value" and ignored.
// 2. That key is compiled out against MariaDB Connector/C (`#if ... &&
//    !defined(MARIADB_VERSION_ID)`), the only build SynQt may convey (see the class comment
//    and docs/licensing.md).
//
// What Connector/C does expose: naming a CA (SSL_CA) enables TLS, and
// MYSQL_OPT_SSL_VERIFY_SERVER_CERT checks the server certificate, host name included, so
// verify-ca is honoured at least as strictly as asked. Anything else is refused, not
// approximated.
QString MysqlProvider::connectOptions(const ProviderConfig &config, QString *error)
{
    const QString mode{config.sslMode};
    if (mode == QLatin1String("disable")) {
        return QString{};
    }
    if (mode != QLatin1String("require") && !isVerifiedSslMode(mode)) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "sslmode '%1' is not one the mysql provider can enforce: use disable, "
                "require, verify-ca or verify-full").arg(mode);
        }
        return QString{};
    }
    if (config.caCert.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "sslmode '%1' needs a ca_cert: the QMYSQL driver built against MariaDB "
                "Connector/C turns TLS on by being given a CA, and has no other option that "
                "would (see docs/providers.md)").arg(mode);
        }
        return QString{};
    }

    QStringList options;
    options.append(QStringLiteral("SSL_CA=%1").arg(config.caCert));
    options.append(QStringLiteral("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=%1")
                       .arg(isVerifiedSslMode(mode) ? QStringLiteral("TRUE")
                                                    : QStringLiteral("FALSE")));
    return options.join(QLatin1Char(';'));
}

MysqlProvider::MysqlProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

MysqlProvider::~MysqlProvider()
{
    disconnect();
}

QString MysqlProvider::name() const
{
    return QStringLiteral("mysql");
}

bool MysqlProvider::refusesInsecure() const
{
    // A plaintext or unverified connection to an external engine is allowed only in dev on
    // localhost; a release build refuses it.
    return m_config.release && !m_config.isLoopbackHost()
           && (!m_config.tls || !isVerifiedSslMode(m_config.sslMode));
}

bool MysqlProvider::connect(QString *error)
{
    if (refusesInsecure()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "refusing an unverified connection to %1 in release: set sslmode to "
                "verify-full with a ca_cert (see docs/security.md)").arg(m_config.host);
        }
        return false;
    }

    // Resolved before connecting, not in the pool's factory, so an unenforceable mode is
    // refused with a reason.
    QString optionsError;
    const QString options{connectOptions(m_config, &optionsError)};
    if (!optionsError.isEmpty()) {
        if (error != nullptr) {
            *error = optionsError;
        }
        return false;
    }

    const ProviderConfig config{m_config};
    return openPool(QStringLiteral("QMYSQL"), [config, options](QSqlDatabase &db) {
        db.setHostName(config.host);
        if (config.port > 0) {
            db.setPort(config.port);
        }
        db.setDatabaseName(config.database);
        db.setUserName(config.user);
        db.setPassword(config.password);  // from the entity env only. Never logged

        db.setConnectOptions(options);
    }, m_config.poolSize, error);
}

} // namespace SynQt
