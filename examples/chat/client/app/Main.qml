// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick.Layouts

// The room. One client for users and moderators both: what a moderator can do extra is
// decided at the edge and by the entity behind it, never by which files a browser was given.
// The Erase button below is a courtesy, not a gate -- an ordinary user's session was handed
// to an entity whose surface has no `erase` on it at all.
ApplicationWindow {
    id: window

    property string notice: ""

    visible: true
    // A property the owner pushes. It is set once, on the database, and arrives here through
    // the entity serving this caller and then the edge: three hops, no polling, one value.
    title: Server.topic

    // What the owner says back when it says no, to the caller that asked and to nobody else
    // in the room.
    Edge.onRefused: reason => window.notice = reason

    ColumnLayout {
        anchors.fill: parent

        ListView {
            id: messages

            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            model: Server.messages

            // Simple x/width bindings rather than a layout, which is what a delegate wants:
            // it is created and destroyed as the view scrolls, and `model` is read through a
            // required property because a role called `id` cannot be one of its own.
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
                    // A moderator's name is red, and nothing in this browser decided that.
                    // `staff` is stamped on the row by the entity a moderator is handed to,
                    // which is the only place in the system that can set it.
                    color: line.model.staff ? "#d0342c" : window.palette.windowText
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

                Button {
                    x: parent.width - 84
                    y: 1
                    width: 76
                    height: parent.height - 2
                    // Shown to a moderator because there is no sense offering it to anybody
                    // else. It is not what stops anybody else: `erase` is not a member of the
                    // surface an ordinary session acquired, so a console call finds nothing.
                    visible: Session.hasScope("admin")
                    text: qsTr("Erase")
                    onClicked: Server.erase(line.model.id)
                }
            }
        }

        TextField {
            id: draft

            Layout.fillWidth: true
            placeholderText: window.notice || qsTr("Say something")
            onAccepted: {
                Server.say(draft.text);
                window.notice = "";
                draft.clear();
            }
        }
    }
}
