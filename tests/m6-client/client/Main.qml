// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    visible: true
    width: 320
    height: 220
    title: qsTr("SynQt Counter")

    // Telemetry for the end-to-end browser test: surfaces connection state and the
    // counter value to the browser console. Invisible; harmless in the shipped app.
    Item {
        property string status: "state=" + Session.state + " counter="
                                + (Server.counter ? Server.counter.value : -1)
        onStatusChanged: console.log("M6 " + status)
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16

        Label {
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 24
            text: Session.state === "connected"
                  ? qsTr("Value: %1").arg(Server.counter.value)
                  : qsTr("Connecting...")
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button { text: qsTr("-"); onClicked: Server.counter.decrement() }
            Button { text: qsTr("+"); onClicked: Server.counter.increment() }
        }
    }
}
