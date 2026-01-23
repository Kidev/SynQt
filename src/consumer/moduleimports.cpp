// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "moduleimports.h"

#include <QQmlEngine>

namespace SynQt {

void registerModuleImports()
{
    // Runs once per process, however often it is called: the entity main and every
    // generated contract registration call it, and a repeated import would leave duplicate
    // entries for every `import SynQt`.
    static bool registered{false};
    if (registered) {
        return;
    }
    registered = true;

    // QQmlModuleImportLatest, what a versionless qmldir `import` means.
    // QQmlModuleImportAuto would ask for QtQuick at SynQt's own version (1.0), which
    // QtQuick does not have.
    qmlRegisterModuleImport("SynQt", 1, "QtQuick",
                            QQmlModuleImportLatest, QQmlModuleImportLatest);
}

} // namespace SynQt
