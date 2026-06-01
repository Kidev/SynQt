// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The row of links Articles 13 and 14 of the GDPR want reachable from every page, plus the
// legal notice most EU member states require of a commercial site. It reads the project's
// `privacy:` block through the `Privacy` accessor, so the text lives in the configuration
// rather than in every page that shows it.
//
// A link whose URL the project did not declare is left out rather than shown broken, which
// means an app that configures nothing renders an empty row. `synqt check` is what reports
// that, at build time, where it can still be fixed.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    // The route to open, for an app whose policy is one of its own pages. An app that
    // points at an external URL leaves this alone and the link opens it directly.
    signal navigate(string url)

    spacing: 16

    Component.onCompleted: {
        if (!Privacy.policyUrl && !Privacy.legalNoticeUrl) {
            console.warn("SynQt: LegalFooter has nothing to show; set privacy.policy and "
                         + "privacy.legal_notice in synqt.yaml");
        }
    }

    Label {
        text: qsTr("Privacy policy")
        visible: Privacy.policyUrl !== ""
        font.underline: true

        TapHandler {
            onTapped: root.navigate(Privacy.policyUrl)
        }
    }

    Label {
        text: qsTr("Legal notice")
        visible: Privacy.legalNoticeUrl !== ""
        font.underline: true

        TapHandler {
            onTapped: root.navigate(Privacy.legalNoticeUrl)
        }
    }

    Label {
        text: qsTr("Contact: %1").arg(Privacy.contact)
        visible: Privacy.contact !== ""
    }

    Label {
        text: qsTr("Cookie settings")
        visible: Privacy.consentRequired && Privacy.consentAnswered
        font.underline: true

        // Withdrawing has to be as easy as consenting was (Article 7(3)), so the way back
        // to the banner is in the same footer as the policy and needs no page of its own.
        TapHandler {
            onTapped: Privacy.withdrawConsent()
        }
    }
}
