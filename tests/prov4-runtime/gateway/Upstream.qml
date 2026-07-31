// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A gateway entity's owner-side Source. Outbound only: it reaches a third party through the
// `Http` helper the runtime injects, which verifies TLS and refuses plaintext in release, so
// this file never touches a socket.
QtObject {
    property var cached: []

    function refresh(url) {
        Http.get(url).then(res => cached = res.body, err => cached = []);
    }
}
