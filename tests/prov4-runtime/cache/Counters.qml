// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A cache entity's owner-side Source. It calls the `Cache` helper only; the runtime injects
// Cache automatically from the entity's blueprint + provider config (PROV-4). The write in
// Component.onCompleted runs when the runtime creates this Source, so the test can prove the
// injection reached QML and not only the C++ side: without it the write never lands.
QtObject {
    Component.onCompleted: Cache.set("from-qml", "written-at-source-creation", 300)

    function put(key, value) {
        Cache.set(key, value, 300);
    }

    function fetch(key) {
        return Cache.get(key);
    }
}
