// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A jobs entity's owner-side Source. It calls the `Jobs` helper only: scheduling and the
// bounded work queue belong to the runtime, so this file has no timer of its own to manage
// and nothing to deploy. The enqueue runs when the runtime creates this Source, which is
// what lets the test see that the injection reached QML.
QtObject {
    Component.onCompleted: Jobs.enqueue(function() { /* the rollup */ })
}
