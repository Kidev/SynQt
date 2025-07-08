// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative session Source on the auth entity (a per_peer instance: one per
// consuming edge, so each edge's emit reaches only that edge). It bridges the connect
// point to a single shared SessionManager (the `Sessions` context object): edge writes go
// into the store, and the store's changes are forwarded to every edge. A newly connected
// edge is replayed the current table (late join).
SessionSource {
    id: session

    function putSession(token, scope, identityJson, createdMs) {
        Sessions.applyUpsert(token, scope, identityJson, createdMs);
    }

    function removeSession(token) {
        Sessions.applyRemove(token);
    }

    function onUpserted(token, scope, identityJson, createdMs) {
        session.emitSessionUpserted(token, scope, identityJson, createdMs);
    }

    function onRemoved(token) {
        session.emitSessionRemoved(token);
    }

    Component.onCompleted: {
        Sessions.sessionUpserted.connect(onUpserted);
        Sessions.sessionRemoved.connect(onRemoved);
        const rows = Sessions.snapshot();
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i];
            session.emitSessionUpserted(row.token, row.scope, row.identityJson, row.createdMs);
        }
    }
}
