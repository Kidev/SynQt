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
    title: "SynQt transport spike"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: "Connection: " + spike.state
            font.bold: true
        }

        Label {
            text: "PROP counter (push): " + spike.counter
        }

        Label {
            text: "SIGNAL payload: " + spike.lastSignal
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                text: "Call echo slot"
                onClicked: spike.callEcho("from-ui")
            }

            Label {
                Layout.fillWidth: true
                text: "SLOT reply: " + spike.lastReply
            }
        }

        Label {
            text: "MODEL rows: " + spike.modelRows
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: spike.rowsModel

            delegate: ItemDelegate {
                required property var model

                width: ListView.view.width
                text: model.display !== undefined ? model.display : ""
            }
        }
    }
}
