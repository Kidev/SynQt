// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A helper component a view instantiates. No route names it, so it reaches the QML module
// only because every QML file under the client entity is compiled in.

import QtQuick

Item {
    id: panel

    readonly property string tag: "panel"
}
