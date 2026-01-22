// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The authoritative Source on the database entity. It authorizes the CALLING ENTITY, not
// a user. Only the web edge (Caller.entity === "web") may write. Any other entity, even
// one on the connect point's consumer allowlist, is refused here in the slot. Each calling
// entity gets an instance of its own, over mutual TLS, so Caller.entity is the name on the
// certificate the handshake verified.
Items {
    id: items

    property var store: []
    property int nextId: 1

    function insert(text, author, ownerSub) {
        if (Caller.entity !== "web") {
            return;   // the database refuses any caller other than the edge
        }
        // The call is the edge's, and the edge is answering somebody: the session travels
        // down the chain, so the person behind a write is knowable here without the edge
        // having to spell them out in an argument. Still an assertion by the edge, worth
        // exactly the certificate that got it through the handshake above.
        items.actingFor = Caller.hasSession ? Caller.identity.sub : "";
        const id = nextId++;
        store.push({ id: id, text: text, author: author, ownerSub: ownerSub });
        items.count = store.length;
        items.itemAdded(id, text, author, ownerSub);   // announce to the edge
    }

    function remove(id) {
        if (Caller.entity !== "web") {
            return;
        }
        store = store.filter(function(row) { return row.id !== id; });
        items.count = store.length;
        items.itemRemoved(id);
    }
}
