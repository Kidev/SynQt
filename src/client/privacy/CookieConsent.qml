// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The consent banner, which renders nothing until the project declares a cookie category
// that needs one.
//
// SynQt sets one cookie, the session credential. Article 5(3) of the ePrivacy Directive
// exempts storage that is strictly necessary to provide the service the visitor asked for,
// and a session the app cannot work without is that, so a project that adds no other cookie
// has nothing to ask about. Showing a banner anyway is not the safe choice it looks like:
// it trains visitors to dismiss a dialog that was asking nothing, and regulators have said
// so. So `privacy.cookies` starts empty, this stays invisible while it is, and the banner
// appears when somebody adds the category that made it necessary.
//
// Refusing is one tap, in the same place and the same weight as accepting, because consent
// under Article 4(11) has to be freely given and a banner where refusing is harder than
// accepting does not collect it.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    // What the app acts on. `granted` is the categories this visitor allowed, and it is a
    // subset of `privacy.cookies`. A visitor who took only what is necessary leaves it
    // empty. Also emitted when an earlier answer is withdrawn, so anything switched on for
    // a category gets told to switch off again.
    signal answered(var granted)

    // Nothing to ask, or already asked. Both are the ordinary case. The first is a project
    // with no non-essential cookies, the second is every visit after the first.
    visible: Privacy.consentRequired && !Privacy.consentAnswered

    Connections {
        target: Privacy

        function onConsentChanged(): void {
            root.answered(Privacy.granted);
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("This site would like to use cookies that are not required to run it. "
                       + "You can accept them, or continue with only what is necessary.")
        }

        Repeater {
            id: boxes

            model: Privacy.categories

            CheckBox {
                required property string modelData

                text: modelData
                // Off until this visitor turns it on. A box ticked in advance is not
                // consent, which is what Article 4(11) means by an unambiguous indication.
                checked: false
            }
        }

        RowLayout {
            spacing: 12

            Button {
                text: qsTr("Accept selected")
                onClicked: Privacy.accept(root.selectedCategories())
            }

            Button {
                text: qsTr("Only what is necessary")
                onClicked: Privacy.acceptNecessaryOnly()
            }

            Button {
                text: qsTr("Accept all")
                onClicked: Privacy.acceptAll()
            }
        }

        Label {
            visible: Privacy.policyUrl !== ""
            text: qsTr("What is collected and for how long is in the privacy policy.")
            font.italic: true
        }
    }

    // Read off the repeater rather than off the layout's children. The boxes are the
    // repeater's items, and walking a parent's children would find the labels and the
    // buttons as well and depend on the order they were declared in.
    function selectedCategories(): var {
        let chosen = [];
        for (let index = 0; index < boxes.count; ++index) {
            let box = boxes.itemAt(index);
            if (box && box.checked) {
                chosen.push(box.modelData);
            }
        }
        return chosen;
    }
}
