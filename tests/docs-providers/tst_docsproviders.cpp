// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The providers docs/tutorial-advanced-cache.md and docs/tutorial-advanced-database.md
// build, compiled from the pages' own C++ (extract.py). That they compile is half the test.
// The other half is what each page promises once the file is in an entity: it registers
// under its name, and a release build refuses an unverified connection to a real address
// before opening a socket.

#include "icacheprovider.h"
#include "ipersistenceprovider.h"
#include "providerconfig.h"
#include "providerregistry.h"

#include <QTest>

using namespace SynQt;

class TestDocsProviders : public QObject
{
    Q_OBJECT

private slots:
    void theCacheProviderRegistersUnderItsName()
    {
        QVERIFY(ProviderRegistry::cacheNames().contains(QStringLiteral("Memcached")));
        QVERIFY(ProviderRegistry::createCache(QStringLiteral("Memcached"), ProviderConfig{})
                != nullptr);
    }

    void theCacheProviderRefusesPlaintextToARealAddressInRelease()
    {
        ProviderConfig config;
        config.host = QStringLiteral("cache.internal");
        config.port = 11211;
        config.tls = false;
        config.release = true;
        const std::unique_ptr<ICacheProvider> provider{
            ProviderRegistry::createCache(QStringLiteral("Memcached"), config)};
        QVERIFY(provider != nullptr);
        QString error;
        QVERIFY(!provider->connect(&error));
        QVERIFY2(error.contains(QStringLiteral("refusing a plaintext cache connection")),
                 qPrintable(error));
    }

    void theDatabaseProviderRegistersUnderItsName()
    {
        QVERIFY(ProviderRegistry::persistenceNames().contains(QStringLiteral("SqlServer")));
        QVERIFY(ProviderRegistry::createPersistence(QStringLiteral("SqlServer"),
                                                    ProviderConfig{})
                != nullptr);
    }

    void theDatabaseProviderRefusesAnUnverifiedConnectionInRelease()
    {
        ProviderConfig config;
        config.host = QStringLiteral("sql.internal");
        config.port = 1433;
        config.sslMode = QStringLiteral("prefer");
        config.release = true;
        const std::unique_ptr<IPersistenceProvider> provider{
            ProviderRegistry::createPersistence(QStringLiteral("SqlServer"), config)};
        QVERIFY(provider != nullptr);
        QString error;
        QVERIFY(!provider->connect(&error));
        QVERIFY2(error.contains(QStringLiteral("refusing an unverified connection")),
                 qPrintable(error));
    }
};

QTEST_GUILESS_MAIN(TestDocsProviders)

#include "tst_docsproviders.moc"
