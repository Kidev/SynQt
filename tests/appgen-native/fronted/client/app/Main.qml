// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls

// One accessor, whatever is behind it. This file is the same for a visitor handed to the
// lobby and one handed to the back office, which of them answered is not the browser's to
// know, and there is nothing here that could tell.
ApplicationWindow {
    id: window

    width: 480
    height: 320
    visible: true
    title: qsTr("Fronted")

    Label {
        anchors.centerIn: parent
        text: Server.headline
    }
}
