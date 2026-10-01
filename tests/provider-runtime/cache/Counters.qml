// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A cache entity's own file: the entity itself, alive for as long as the entity runs. It
// calls the `Cache` helper only. The runtime injects Cache automatically from the entity's
// type + provider config. The write in Component.onCompleted runs when the entity
// comes up, so the test can prove the injection reached QML and not only the C++ side.
pragma Singleton

QtObject {
    Component.onCompleted: Cache.set("from-qml", "written-at-source-creation", 300)

    function put(key, value) {
        Cache.set(key, value, 300);
    }

    function fetch(key) {
        return Cache.get(key);
    }
}
