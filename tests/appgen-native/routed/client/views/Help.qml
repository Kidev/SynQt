// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The view route "/help" points at, in a subdirectory of the client entity: it is aliased
// into the module at that same relative path, so the route's qrc URL still names it.

import QtQuick

Item {
    id: help

    readonly property string viewName: "Help"

    Text {
        anchors.centerIn: parent
        text: qsTr("Help")
    }
}
