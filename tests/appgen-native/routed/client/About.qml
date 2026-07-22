// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The view route "/about" points at, named in synqt.yaml without its .qml extension so
// the run also covers that spelling.

import QtQuick

Item {
    id: about

    readonly property string viewName: "About"

    Text {
        anchors.centerIn: parent
        text: qsTr("About")
    }
}
