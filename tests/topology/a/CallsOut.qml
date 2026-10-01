// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// An owner that calls its declared upstream once it is completed, so a test can read what
// the runtime put on the request: a header the topology wrote as `env:NAME` arrives as the
// value of NAME, and the QML that made the call never held it.
import SynQt

Thing {
    Component.onCompleted: Http.api("upstream").get("ping")
}
