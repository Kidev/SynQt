// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls

ApplicationWindow {
    id: window

    height: 200
    title: qsTr("monitored")
    visible: true
    width: 320

    Label {
        anchors.centerIn: parent
        text: Server.headline
    }
}
