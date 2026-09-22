// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The QML test runner for the harness suite. It is this small because everything an
// application author writes is in qml/, and the only C++ is the registrations that make
// `SynQt.Test` and the generated types importable. `synqt test` generates the same file
// for an application (maingen.render_tests_main), which is why this one must not grow
// logic.

#include "entitytest.h"

#include "database_consumer.h"
#include "database_sourcehelper.h"

#include <QtQuickTest/quicktest.h>

#include <QtQml/qqml.h>
#include <QtQml/qqmlengine.h>

void synqtRegisterLedgerSources();
void synqtRegisterDatabaseSources();

// `Database` is the root type of Database.qml and the attached type behind
// `Database.on<Signal>:` in a file that consumes it.
class DatabaseTestType : public DatabaseSourceHelper
{
    Q_OBJECT
    QML_ATTACHED(DatabaseConsumer)

public:
    using DatabaseSourceHelper::DatabaseSourceHelper;

    static DatabaseConsumer *qmlAttachedProperties(QObject *object)
    {
        return DatabaseConsumer::qmlAttachedProperties(object);
    }
};

class Setup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        // The generated Sources, so `import SynQt` resolves the type each Source under test
        // derives from. The Ledger consumes the database, so the database's type carries
        // both sides and is registered last.
        synqtRegisterLedgerSources();
        synqtRegisterDatabaseSources();
        synqtRegisterDatabaseConsumers();
        qmlRegisterType<DatabaseTestType>("SynQt", 1, 0, "Database");
        SynQt::declareConsumedPoint(QStringLiteral("Ledger"), QStringLiteral("Database"),
                                    QStringLiteral("Database"), QStringLiteral("database"));
        SynQt::registerTestTypes();
    }
};

QUICK_TEST_MAIN_WITH_SETUP(synqt_entity_test, Setup)

#include "tst_entitytest.moc"
