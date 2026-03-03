// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls

ApplicationWindow {
    id: window

    visible: true
    title: qsTr("Sign in")

    // This bundle is the whole of what a signed-out visitor downloads. The room's own
    // client is a different bundle on the same edge, and a session without `user` cannot
    // fetch a file of it: not a redirect, not a 403, simply not there.
    Column {
        anchors.centerIn: parent
        spacing: 16

        Label {
            text: qsTr("Sign in to join the room.")
        }

        Button {
            text: qsTr("Sign in")
            onClicked: Session.login()
        }
    }
}
