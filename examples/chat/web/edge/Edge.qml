// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

Edge {
    id: room

    property var said: []

    function say(body) {
        if (!Caller.hasScope("user")) {
            Caller.emitRefused("Sign in to say something.");
            return;
        }
        Store.append(Caller.identity.login, body).then(rows => room.said = rows);
    }

    topic: "Anything goes"
    // There is one of this entity, so there is one of this list: it is the room. Every
    // browser holds a mirror of this Source, so one person saying something redraws all
    // of them, and the only thing anybody wrote is the line below.
    messagesRows: room.said

    Component.onCompleted: Store.recent().then(rows => room.said = rows)
}
