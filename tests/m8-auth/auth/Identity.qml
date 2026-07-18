// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative identity Source on the auth entity (a per_peer instance: one per
// consuming edge, so each edge's answer reaches only that edge). It bridges the connect
// point to a single shared IdentityService (the `IdentityEngine` context object), which owns
// client secret, the token exchange and the stored tokens. The service methods are
// synchronous, so this Source emits each result on itself and answers only the edge that
// asked; a user's identity never crosses to another edge.
IdentitySource {
    id: source

    function beginLogin(requestId, provider, redirectUri) {
        const result = IdentityEngine.beginLogin(provider, redirectUri);
        source.emitBeginResult(requestId, result.state, result.authorizeUrl, result.error);
    }

    function exchangeCode(requestId, state, code, redirectUri) {
        const result = IdentityEngine.exchangeCode(state, code, redirectUri);
        source.emitExchangeResult(requestId, result.identityJson, result.error);
    }

    function bindSession(state, sessionId) {
        IdentityEngine.bindSession(state, sessionId);
    }

    function releaseSession(sessionId) {
        IdentityEngine.releaseSession(sessionId);
    }
}
