// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    property string notice: ""

    visible: true
    // A property the edge pushes. Change it there and every window open on this room
    // retitles itself, with nothing here asking and nothing polling.
    title: Server.topic

    Edge.onRefused: reason => window.notice = reason

    ColumnLayout {
        anchors.fill: parent

        ListView {
            Layout.fillHeight: true
            Layout.fillWidth: true
            model: Server.messages

            delegate: Text {
                required property var model

                text: `${model.who}: ${model.body}`
            }
        }

        TextField {
            id: line

            Layout.fillWidth: true
            placeholderText: window.notice || qsTr("Say something")
            onAccepted: {
                Server.say(line.text);
                line.clear();
            }
        }
    }
}
