// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Article 17 request ("right to erasure"), as a button and a confirmation.
//
// It carries the request no further than the app. `confirmed` is the signal, and what an
// app connects it to is a slot on one of its own connect points, because the app is what
// knows where its data is. The framework will not invent a route that answers "accepted"
// and erases nothing.
//
// Visible only to a signed-in visitor, and only when the project set `privacy.erasure`.
// Both halves matter. An anonymous visitor's request names no data to erase, and an app
// that has not turned the key on has nobody undertaking to act on what the button sends.

import QtQuick
import QtQuick.Controls

Item {
    id: root

    // Connect this to the slot that erases the caller's data, for example
    // `onConfirmed: Server.eraseMe()`.
    signal confirmed()

    // What the app calls when its slot has answered, so the visitor is told rather than
    // left looking at a button that did something invisible.
    function reportAccepted(): void {
        root.state = "accepted";
    }

    function reportFailed(message: string): void {
        root.failure = message;
        root.state = "failed";
    }

    property string failure: ""

    implicitWidth: content.implicitWidth
    implicitHeight: content.implicitHeight
    visible: Privacy.erasureOffered && Session.isAuthenticated
    state: "idle"

    states: [
        State { name: "idle" },
        State { name: "accepted" },
        State { name: "failed" }
    ]

    Column {
        id: content

        spacing: 8

        Button {
            text: qsTr("Request erasure of my data")
            enabled: root.state === "idle"
            onClicked: confirmDialog.open()
        }

        Label {
            visible: root.state === "accepted"
            text: qsTr("Your request has been received.")
        }

        Label {
            visible: root.state === "failed"
            text: qsTr("The request could not be sent: %1").arg(root.failure)
        }
    }

    Dialog {
        id: confirmDialog

        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Erase your data")
        standardButtons: Dialog.Ok | Dialog.Cancel

        onAccepted: root.confirmed()

        Label {
            width: 320
            wrapMode: Text.WordWrap
            // The warning is here rather than left to the app because the consequence is
            // the same everywhere and it is the part a visitor has to be told before they
            // agree. Erasure ends the account, and it cannot be undone.
            text: Privacy.retentionDays > 0
                ? qsTr("This ends your account and erases the personal data held for it. "
                       + "It cannot be undone. Records the law requires us to keep are held "
                       + "for up to %1 days and then erased.").arg(Privacy.retentionDays)
                : qsTr("This ends your account and erases the personal data held for it. "
                       + "It cannot be undone.")
        }
    }
}
