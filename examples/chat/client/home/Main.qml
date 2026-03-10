// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick.Layouts

// The landing page, and the whole of what a visitor who has signed in as nobody downloads.
// The room's client is a different bundle on the same edge, and a session without `user`
// cannot fetch a file of it: not a redirect, not a 403, simply not there.
//
// There is no `Server` here and nothing to reach for. This entity consumes no connect point,
// so the only thing this application can do is start the sign-in, which is the only thing
// somebody who is nobody yet has any business doing.
ApplicationWindow {
    id: window

    visible: true
    title: qsTr("The chat room")

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 24

        Label {
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 32
            text: qsTr("One room. Everybody in it sees the same thing.")
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 480
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Somebody types a line and it is on every screen that has the room "
                       + "open, including the ones on other machines. Sign in to join.")
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            // The edge runs the whole exchange. This browser never sees a token and never
            // holds a secret; what it ends up with is a session cookie, and the next page
            // it is served is the room.
            text: qsTr("Sign in with GitHub")
            onClicked: Session.login()
        }
    }
}
