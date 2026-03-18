// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    ListView {
        id: messages

        Layout.fillHeight: true
        Layout.fillWidth: true
        clip: true
        model: Server.messages

        delegate: Item {
            id: line

            required property var model

            width: messages.width
            height: 26

            Label {
                x: 8
                width: 132
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                color: line.model.staff ? "#d0342c" : line.palette.windowText
                font.bold: line.model.staff
                text: line.model.who
            }

            Label {
                x: 148
                width: parent.width - 148 - 88
                height: parent.height
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                text: line.model.body
            }

            Admin {
                x: parent.width - 84
                y: 1
                width: 76
                height: parent.height - 2
                messageId: line.model.id
            }
        }
    }

    TextField {
        id: draft

        Layout.fillWidth: true
        placeholderText: qsTr("Say something")
        onAccepted: {
            Server.say(draft.text);
            draft.clear();
        }
    }
}
