// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The view route "/" points at. It names itself so the run can tell which component the
// Loader really instantiated, not merely that some component loaded.

import QtQuick

Item {
    id: home

    readonly property string viewName: "Home"

    Text {
        anchors.centerIn: parent
        text: qsTr("Home")
    }
}
