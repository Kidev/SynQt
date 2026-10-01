// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// The edge entity itself. One of it, for as long as the entity runs. The counter is one
// number for everybody, and a Source is created per browser session, so the number lives
// here and each session's Counter Source binds to it. That is what makes two clients see
// the same value while every slot still has a Caller.
QtObject {
    id: root

    property int value: 0

    function bump(by: int) {
        root.value = root.value + by;
    }
}
