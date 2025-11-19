// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A jobs entity's own file: the entity itself. It calls the `Jobs` helper only, so
// scheduling and the bounded work queue belong to the runtime and this file has no timer of
// its own to manage. The enqueue runs when the entity comes up, which is what lets the test
// see that the injection reached QML.
pragma Singleton

QtObject {
    Component.onCompleted: Jobs.enqueue(function() { /* the rollup */ })
}
