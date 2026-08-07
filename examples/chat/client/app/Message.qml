// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls

Item {
    id: line

    required property var model

    height: 26

    Label {
        x: 8
        y: (line.height - height) / 2
        width: 132
        elide: Text.ElideRight
        color: line.model.staff ? "#d0342c" : line.palette.windowText
        text: line.model.who
    }

    Label {
        x: 148
        y: (line.height - height) / 2
        width: line.width - 148 - 88
        elide: Text.ElideRight
        text: line.model.body
    }

    Admin {
        x: line.width - width - 8
        height: line.height
        messageId: line.model.id
    }
}
