// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative Source on the edge (a per_session instance: one per browser user, so
// Caller is that user). It authorizes the USER, keeps an owner id per row for that
// authorization, and publishes to the browser a model with no ownerSub role. The shared
// item list lives in the database; every session's instance mirrors it from the database
// signals, so all users see all items.
TodoSource {
    id: todo

    property var itemList: []       // [{id,text,author,ownerSub}]; server-side only
    property var owners: ({})       // id -> ownerSub, for the removal check; never published

    function publish() {
        // set<Model> keeps only declared roles (id, text, author, done): ownerSub is
        // dropped at this boundary and never reaches the client.
        todo.setItems(itemList);
        todo.count = itemList.length;
        const map = {};
        for (let i = 0; i < itemList.length; ++i) {
            map[itemList[i].id] = itemList[i].ownerSub;
        }
        todo.owners = map;
    }

    function onItemAdded(id, text, author, ownerSub) {
        itemList.push({ id: id, text: text, author: author, ownerSub: ownerSub });
        publish();
    }

    function onItemRemoved(id) {
        itemList = itemList.filter(function(row) { return row.id !== id; });
        publish();
    }

    function add(text) {
        const clean = ("" + text).trim();
        if (clean.length === 0 || clean.length > 280) {
            Caller.emitSignal("rejected", "Item must be 1 to 280 characters.");
            return;
        }
        // Persist via the database entity; it authorizes that the caller is the edge.
        Database.items.insert(clean, Caller.identity.email, Caller.identity.sub);
    }

    function remove(id) {
        // A user may remove only their own item; a moderator may remove any.
        if (todo.owners[id] !== Caller.identity.sub && !Caller.hasScope("moderator")) {
            Caller.emitSignal("rejected", "You can only remove your own items.");
            return;
        }
        Database.items.remove(id);
    }

    // A generated Source is a QObject (no default child list), so subscribe to the shared
    // database's change signals imperatively.
    Component.onCompleted: {
        Database.items.itemAdded.connect(onItemAdded);
        Database.items.itemRemoved.connect(onItemRemoved);
    }
}
