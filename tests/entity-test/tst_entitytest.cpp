// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The QML test runner for the harness suite. It is deliberately this small: everything an
// application author writes is in qml/, and the only C++ is the two registrations that
// make `SynQt.Test` and the generated Source type importable. `synqt test` generates the
// same file for an application, which is why this one must not grow logic.

#include "entitytest.h"

#include <QtQuickTest/quicktest.h>

#include <QtQml/qqmlengine.h>

void synqtRegisterLedgerSources();

class Setup : public QObject
{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {
        // The generated LedgerSource, so `import SynQt` resolves the type the Source
        // under test derives from, and the harness itself.
        synqtRegisterLedgerSources();
        SynQt::registerTestTypes();
    }
};

QUICK_TEST_MAIN_WITH_SETUP(synqt_entity_test, Setup)

#include "tst_entitytest.moc"
