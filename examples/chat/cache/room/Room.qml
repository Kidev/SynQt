// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// What a signed-in user is handed to. Nothing here asks about scope, and that is not an
// omission: the edge in front of it hands nobody but a `user` here, so `Caller` is the only
// question there is to ask, and the members a moderator reaches are not on this surface for
// anybody to call.
//
// The room itself is not here either. `store` holds it, this mirrors it, and the moderator's
// entity mirrors the same one, which is what makes them two views of one conversation.
Room {
    id: room

    function say(body) {
        const who = Caller.identity.login;
        // How much this person has said in the last minute, kept in the one place worth
        // keeping it: a bounded store that forgets. The window starts when the first message
        // of it lands, so this is a fixed minute rather than a minute from the last thing
        // said, which would never expire for somebody typing steadily.
        const said = Cache.incr("said:" + who);
        if (said === 1) {
            Cache.expire("said:" + who, 60);
        }
        if (said > 20) {
            Caller.emitRefused("Twenty lines a minute is the limit. Give it a moment.");
            return;
        }
        Store.say(who, body, false);
    }

    topic: Store.topic
    messagesRows: Store.lines
}
