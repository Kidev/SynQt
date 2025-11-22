// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls

// A client entity's root must be a window, or the build is green and the page is blank.
ApplicationWindow {
    visible: true
    title: qsTr("Promoted identity")

    Label {
        anchors.centerIn: parent
        text: Server.greeting ? Server.greeting.message : qsTr("Connecting")
    }
}
