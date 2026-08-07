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

        delegate: Message {
            width: messages.width
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
