// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    visible: true
    width: 480
    height: 640
    title: qsTr("SynQt M0 transport spike")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Connection: %1").arg(m0.state)
            font.bold: true
        }

        Label {
            text: qsTr("PROP counter (push): %1").arg(m0.counter)
        }

        Label {
            text: qsTr("SIGNAL payload: %1").arg(m0.lastSignal)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: qsTr("Call echo slot")
                onClicked: m0.callEcho("from-ui")
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("SLOT reply: %1").arg(m0.lastReply)
            }
        }

        Label {
            text: qsTr("MODEL rows: %1").arg(m0.modelRows)
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: m0.rowsModel

            delegate: ItemDelegate {
                required property var model

                width: ListView.view.width
                text: model.display !== undefined ? model.display : ""
            }
        }
    }
}
