// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative Source on the database entity. It authorizes the CALLING ENTITY, not
// a user: only the web edge (Caller.entity === "web") may write. Any other entity; even
// one on the connect point's consumer allowlist; is refused here in the slot. This is a
// per_peer instance over mutual TLS, so Caller.entity is certificate-verified: the slot
// requires Caller.isEntityVerified so a colocation-trusted (local-socket) peer could never
// pass this gate by presenting the name "web".
ItemsSource {
    id: items

    property var store: []
    property int nextId: 1

    function insert(text, author, ownerSub) {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return;   // the database refuses any caller other than the verified edge
        }
        const id = nextId++;
        store.push({ id: id, text: text, author: author, ownerSub: ownerSub });
        items.count = store.length;
        items.itemAdded(id, text, author, ownerSub);   // announce to the edge
    }

    function remove(id) {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return;
        }
        store = store.filter(function(row) { return row.id !== id; });
        items.count = store.length;
        items.itemRemoved(id);
    }
}
