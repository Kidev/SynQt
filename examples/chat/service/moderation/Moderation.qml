// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// What a moderator is handed to, in a binary of its own. `erase` is compiled into this
// entity and into nothing else, and neither is the line below that marks a message as staff:
// the process serving ordinary users does not contain either of them.
//
// Like the entity next door, it never asks about scope. The edge decides who arrives here.
Moderation {
    id: desk

    // A moderator is not rate limited, which is why this entity has no cache and the one
    // serving users does. What is different about the two surfaces is what each of them
    // holds, not a flag either of them reads.
    function say(body) {
        Store.say(Caller.identity.login, body, true);
    }

    // Read against the room as it stands rather than against a query of its own: the caller
    // is looking at these rows, so this is the question they think they are asking. The
    // answer goes back to the one caller who asked it and to nobody else in the room.
    function erase(id) {
        if (!Store.lines.some(line => line.id === id)) {
            Caller.emitRefused("That message is not in the room any more.");
            return;
        }
        Store.erase(id);
    }

    topic: Store.topic
    messagesRows: Store.lines
}
