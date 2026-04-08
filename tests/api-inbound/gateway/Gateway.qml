// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// A gateway entity's own file, declaring its public surface the way a real one does: on
// `Api`, in `Component.onCompleted`, with the handlers as ordinary JavaScript. Nothing
// here touches a socket, and nothing here decides who may call: the topology's
// `network.inbound` did that before this file was loaded. No `import SynQt`, because
// `Api` is a context object the runtime installs rather than a type to import. An entity
// that also used a SynQt QML type would import it for that.
QtObject {
    id: root

    property int settled: 0

    Component.onCompleted: {
        // The synchronous shape: return a value and it is the 200.
        Api.get("/lots", () => { return { lots: ["a", "b"] }; });

        // A captured placeholder, and the more literal route declared second, because
        // /lots/open must still win over /lots/:id.
        Api.get("/lots/:id", request => { return { id: request.params.id }; });
        Api.get("/lots/open", () => { return { open: true }; });

        // A body, and a rejection the handler decides.
        Api.post("/lots", request => {
            if (!request.body || !request.body.name) {
                request.fail(422, "a lot needs a name");
                return;
            }
            settled += 1;
            return { created: request.body.name, query: request.query.dry || "" };
        });

        // Who the framework decided is calling. A handler that logs or rations by client
        // reads this rather than the forwarding header beside it in `headers`.
        Api.get("/whoami", request => { return { client: request.client }; });

        // A handler that throws must not take the process with it.
        Api.get("/broken", () => { throw new Error("deliberate"); });

        // The deferred shape, which is what a handler reaching a connect point or an
        // upstream does: take the request, return nothing, answer on a later turn.
        Api.get("/slow", request => {
            Qt.callLater(() => { request.reply({ late: true }); });
        });

        // And the one that never answers, which must cost a 504 and not a held socket.
        Api.get("/silent", () => {});
    }
}
